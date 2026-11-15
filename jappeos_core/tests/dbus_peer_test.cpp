/*
 * jappeos_core, Core system management daemon for JappeOS.
 * Copyright (C) 2026  The JappeOS team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "utils/dbus_peer.h"
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace JappeStudios::JappeOS::JappeOSCore;

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// An isolated systemd-like peer. Never talks to the host's bus or service manager.
class FakeManager
{
public:
    enum class PingBehavior { Normal, DelayFirst, Silent, Denied };
    std::string address;
    std::atomic<unsigned> pingCalls = 0;
    std::atomic<unsigned> applicationCalls = 0;
    explicit FakeManager(bool authenticate = true, PingBehavior pingBehavior = PingBehavior::Normal)
        : _authenticate(authenticate), _pingBehavior(pingBehavior)
    {
        char directory[] = "/tmp/jappeos-peer-test-XXXXXX";
        const auto result = mkdtemp(directory);
        Require(result, "mkdtemp failed");
        _directory = result;
        _path = _directory + "/socket";
        address = "unix:path=" + _path;
        _listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        Require(_listener >= 0, "socket failed");
        sockaddr_un location{};
        location.sun_family = AF_UNIX;
        std::copy(_path.begin(), _path.end(), location.sun_path);
        Require(bind(_listener, reinterpret_cast<sockaddr*>(&location), sizeof(location)) == 0, "bind failed");
        Require(listen(_listener, 1) == 0, "listen failed");
        _worker = std::thread([this] { Run(); });
    }
    ~FakeManager()
    {
        _stop = true;
        shutdown(_listener, SHUT_RDWR);
        if (_worker.joinable()) _worker.join();
        close(_listener);
        unlink(_path.c_str());
        rmdir(_directory.c_str());
    }
private:
    int _listener = -1;
    std::string _directory, _path;
    std::atomic<bool> _stop = false;
    bool _authenticate;
    PingBehavior _pingBehavior;
    sd_bus_message* _delayedPing = nullptr;
    std::thread _worker;
    static int Filter(sd_bus_message* message, void* userdata, sd_bus_error*)
    {
        auto& manager = *static_cast<FakeManager*>(userdata);
        if (sd_bus_message_is_method_call(message, "org.freedesktop.DBus.Peer", "Ping"))
        {
            const auto count = ++manager.pingCalls;
            Require(!sd_bus_message_get_destination(message), "readiness Ping must address the peer directly");
            if (manager._pingBehavior == PingBehavior::Silent) return 1;
            if (manager._pingBehavior == PingBehavior::Denied)
                return sd_bus_reply_method_errorf(message, SD_BUS_ERROR_ACCESS_DENIED, "Ping denied");
            if (manager._pingBehavior == PingBehavior::DelayFirst)
            {
                if (count == 1)
                {
                    manager._delayedPing = sd_bus_message_ref(message);
                    return 1;
                }
                if (manager._delayedPing)
                {
                    sd_bus_reply_method_return(manager._delayedPing, "");
                    manager._delayedPing = sd_bus_message_unref(manager._delayedPing);
                }
            }
        }
        else if (sd_bus_message_is_method_call(message, "org.jappeos.Test", "Burst"))
            ++manager.applicationCalls;
        return 0;
    }
    static int Burst(sd_bus_message* message, void*, sd_bus_error*)
    {
        auto* bus = sd_bus_message_get_bus(message);
        // These signals arrive before the synchronous reply. A fd-only event
        // source would lose them until unrelated traffic wakes it again.
        sd_bus_emit_signal(bus, "/test", "org.jappeos.Test", "Changed", "u", 1U);
        sd_bus_emit_signal(bus, "/test", "org.jappeos.Test", "Changed", "u", 2U);
        return sd_bus_reply_method_return(message, "s", "ok");
    }
    static int Denied(sd_bus_message* message, void*, sd_bus_error*)
    {
        return sd_bus_reply_method_errorf(message, SD_BUS_ERROR_ACCESS_DENIED, "Test denial");
    }
    static int Subscribe(sd_bus_message* message, void*, sd_bus_error*)
    {
        return sd_bus_reply_method_return(message, "");
    }
    void Run()
    {
        const int client = accept4(_listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) return;
        if (!_authenticate)
        {
            // Accept the transport but never answer authentication.
            while (!_stop) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            close(client);
            return;
        }
        sd_bus* bus = nullptr;
        sd_id128_t id;
        sd_id128_randomize(&id);
        sd_bus_new(&bus);
        sd_bus_set_fd(bus, client, client);
        sd_bus_set_server(bus, true, id);
        sd_bus_set_sender(bus, "org.jappeos.Test");
        sd_bus_add_filter(bus, nullptr, Filter, this);
        static const sd_bus_vtable vtable[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("Burst", "", "s", Burst, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("Denied", "s", "", Denied, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("Subscribe", "", "", Subscribe, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_SIGNAL("Changed", "u", 0),
            SD_BUS_VTABLE_END
        };
        sd_bus_add_object_vtable(bus, nullptr, "/test", "org.jappeos.Test", vtable, nullptr);
        // Match systemd's sd-event integration. A hand-written process/wait
        // loop hides the authentication-buffer race seen in the real manager.
        sd_bus_negotiate_fds(bus, true);
        sd_event* event = nullptr;
        sd_event_new(&event);
        sd_bus_attach_event(bus, event, 0);
        sd_bus_start(bus);
        while (!_stop)
        {
            if (sd_event_run(event, 10000) < 0) break;
        }
        sd_bus_detach_event(bus);
        sd_bus_message_unref(_delayedPing);
        sd_bus_flush_close_unref(bus);
        sd_event_unref(event);
    }
};

static void CallBurst(Utils::DBusPeer& peer)
{
    auto message = Message::CreateMethodCall("org.jappeos.Test", ObjectPath("/test"),
                                             InterfaceName("org.jappeos.Test"), "Burst");
    const auto reply = message.SendWithReply(peer.GetConnection(), 2000);
    Require(std::get<0>(reply.GetArgs<std::string>()) == "ok", "unexpected peer reply");
    Require(std::string(dbus_message_get_destination(message.GetRawMessage())) == "org.jappeos.Test",
            "peer send mutated the original call destination");
}

static void PumpUntil(const std::function<bool()>& done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done() && std::chrono::steady_clock::now() < deadline)
    {
        g_main_context_iteration(nullptr, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(done(), "GLib did not dispatch queued peer signals");
}

int main()
{
    try
    {
        dbus_threads_init_default();
        {
            FakeManager manager(true, FakeManager::PingBehavior::DelayFirst);
            Utils::DBusPeer peer(manager.address);
            Require(manager.pingCalls >= 2, "readiness handshake was not retried");
            CallBurst(peer);
            Require(manager.applicationCalls == 1, "application call was replayed");
        }
        for (const auto behavior : {FakeManager::PingBehavior::Silent, FakeManager::PingBehavior::Denied})
        {
            FakeManager manager(true, behavior);
            const auto start = std::chrono::steady_clock::now();
            bool failed = false;
            try { Utils::DBusPeer peer(manager.address); }
            catch (const DBusException& error)
            {
                failed = true;
                if (behavior == FakeManager::PingBehavior::Silent)
                {
                    Require(error.GetCode() == DBUS_ERROR_TIMEOUT, "wrong readiness timeout code");
                    Require(error.GetMsg().find("readiness") != std::string::npos, "missing readiness context");
                }
                else
                {
                    Require(error.GetCode() == DBUS_ERROR_ACCESS_DENIED, "readiness error swallowed");
                }
            }
            Require(failed, "unready peer was accepted");
            Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(7),
                    "readiness did not respect its deadline");
            Require(manager.applicationCalls == 0, "application call sent before readiness");
        }
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            FakeManager manager;
            Utils::DBusPeer peer(manager.address);
            Require(dbus_connection_get_is_authenticated(peer.GetConnection().GetRawConnection()),
                    "Peer constructor returned before authentication completed");
            auto subscribe = Message::CreateMethodCall("org.jappeos.Test", ObjectPath("/test"),
                InterfaceName("org.jappeos.Test"), "Subscribe");
            subscribe.SendWithReplyIgnore(peer.GetConnection(), 2000);
            std::vector<uint32_t> received;
            auto subscription = peer.GetConnection().SubscribeSignal(
                "org.jappeos.Test", ObjectPath("/test"), InterfaceName("org.jappeos.Test"), "Changed",
                [&](const Message& message) { received.push_back(std::get<0>(message.GetArgs<uint32_t>())); });
            CallBurst(peer);
            PumpUntil([&] { return received.size() == 2; });
            Require(received == std::vector<uint32_t>({1, 2}), "signals reordered");

            auto message = Message::CreateMethodCall("org.jappeos.Test", ObjectPath("/test"),
                InterfaceName("org.jappeos.Test"), "Burst");
            message.Send(peer.GetConnection());
            peer.GetConnection().Flush();
            PumpUntil([&] { return received.size() == 4; });
            Require(std::string(dbus_message_get_destination(message.GetRawMessage())) == "org.jappeos.Test",
                    "nonblocking peer send mutated the original call");

            auto denied = Message::CreateMethodCall("org.jappeos.Test", ObjectPath("/test"),
                InterfaceName("org.jappeos.Test"), "Denied");
            denied.SetArgs(std::string("secret-test-argument"));
            bool rejected = false;
            try { denied.SendWithReplyIgnore(peer.GetConnection(), 2000); }
            catch (const DBusException& error)
            {
                rejected = true;
                Require(error.GetCode() == DBUS_ERROR_ACCESS_DENIED, "remote error code lost");
                Require(error.GetMsg().find("org.jappeos.Test.Denied at /test") != std::string::npos,
                        "failed method missing from diagnostic");
                Require(error.GetMsg().find("secret-test-argument") == std::string::npos,
                        "diagnostic exposed method arguments");
            }
            Require(rejected, "remote denial was not reported");
        }
        {
            FakeManager manager;
            auto peer = std::make_unique<Utils::DBusPeer>(manager.address);
            std::unique_ptr<SignalSubscription> subscription;
            int received = 0;
            subscription = std::make_unique<SignalSubscription>(peer->GetConnection().SubscribeSignal(
                "org.jappeos.Test", ObjectPath("/test"), InterfaceName("org.jappeos.Test"), "Changed",
                [&](const Message&)
                {
                    ++received;
                    subscription.reset();
                    peer.reset(); // Logout during signal dispatch must be safe.
                }));
            CallBurst(*peer);
            PumpUntil([&] { return !peer; });
            Require(received == 1, "dispatch continued after peer destruction");
        }
        {
            FakeManager manager;
            Utils::DBusPeer peer(manager.address);
            int calls = 0;
            std::unique_ptr<SignalSubscription> first, second;
            first = std::make_unique<SignalSubscription>(peer.GetConnection().SubscribeSignal(
                "org.jappeos.Test", ObjectPath("/test"), InterfaceName("org.jappeos.Test"), "Changed",
                [&](const Message&)
                {
                    ++calls;
                    first.reset();
                    second.reset();
                }));
            second = std::make_unique<SignalSubscription>(peer.GetConnection().SubscribeSignal(
                "org.jappeos.Test", ObjectPath("/test"), InterfaceName("org.jappeos.Test"), "Changed",
                [&](const Message&) { calls += 100; }));
            CallBurst(peer);
            PumpUntil([&] { return calls != 0; });
            Require(calls == 1, "removed handler invoked from dispatch snapshot");
        }
        {
            auto manager = std::make_unique<FakeManager>();
            Utils::DBusPeer peer(manager->address);
            bool disconnected = false;
            auto subscription = peer.GetConnection().SubscribeSignal(
                "org.freedesktop.DBus.Local", ObjectPath("/org/freedesktop/DBus/Local"),
                InterfaceName("org.freedesktop.DBus.Local"), "Disconnected",
                [&](const Message&) { disconnected = true; });
            CallBurst(peer);
            manager.reset();
            PumpUntil([&] { return disconnected; });
        }
        {
            FakeManager manager(false);
            bool timedOut = false;
            try { Utils::DBusPeer peer(manager.address); }
            catch (const DBusException& error)
            {
                timedOut = true;
                Require(error.GetCode() == DBUS_ERROR_TIMEOUT, "wrong authentication failure code");
                Require(error.GetMsg().find("authenticating") != std::string::npos,
                        "authentication timeout missing context");
            }
            Require(timedOut, "unresponsive peer did not time out");
        }
        std::cout << "PASS: authentication, readiness retries/deadlines, 100 connection cycles, peer calls, diagnostics, signal ordering, teardown, disconnect\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
