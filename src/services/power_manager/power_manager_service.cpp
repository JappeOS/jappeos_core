/*
 * jappeos_core, Core system management daemon for JappeOS.
 * Copyright (C) 2026  Jappe02
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

#include "power_manager_service.h"

#include "power_device.h"
#include "../../utils/dbus_utils.h"
#include "../logger/logger_service.h"
#include "../session_manager/session_manager_service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::PowerManager
{

    PowerManagerService::PowerManagerService(ServiceManager* serviceManager, Connection* conn) :
                                             Service(serviceManager, conn),
                                             _object(*_conn, GetBaseObjectPath()),
                                             _iface(_object.CreateInterface(GetBaseInterface()))
    {
        _iface.RegisterMethod("Shutdown", [&](const auto& m) { OnShutdown(m); });
        _iface.RegisterMethod("Reboot",   [&](const auto& m) { OnReboot(m); });
        _iface.RegisterMethod("Suspend",  [&](const auto& m) { OnSuspend(m); });

        _subNameOwnerChanged = std::make_unique<SignalSubscription>(_conn->SubscribeSignal(
            "org.freedesktop.DBus",
            ObjectPath("/org/freedesktop/DBus"),
            InterfaceName("org.freedesktop.DBus"),
            "NameOwnerChanged",
            [&](const Message& msg)
            {
                return OnNameOwnerChanged(msg);
            }
        ));
    }

    PowerManagerService::~PowerManagerService()
    {
        OnPowerManagerDisappeared();
    }

    std::vector<ObjectPath> PowerManagerService::ListDevices() const
    {
        auto ks = std::views::keys(_devices);
        std::vector<ObjectPath> keys{ks.begin(), ks.end()};
        return keys;
    }

    // TODO: Polkit
    void PowerManagerService::SharedPolicy(const Message& message) const
    {
        const auto sender = message.GetSender();
        const auto senderUid = _conn->GetUnixUser(sender);
        const pid_t senderPid = Utils::DBusUtils::GetSenderPID(_rawConn, message.GetRawMessage());

        const auto sessionMgr = _serviceManager->Get<SessionManager::SessionManagerService>();

        if (!sessionMgr->IsManagedUserSession(senderUid))
        {
            throw DBusException(DBUS_ERROR_ACCESS_DENIED, "Unknown user");
        }

        if (!sessionMgr->IsPrivilegedClientProcess(senderPid, true))
        {
            throw DBusException(DBUS_ERROR_ACCESS_DENIED, "Unauthorized process");
        }
    }

    void PowerManagerService::OnShutdown(const Message& message) const
    {
        SharedPolicy(message);

        auto fwdMsg = Message::CreateMethodCall(
            "org.freedesktop.login1",
            ObjectPath("/org/freedesktop/login1"),
            InterfaceName("org.freedesktop.login1.Manager"),
            "PowerOff"
        );

        fwdMsg.SetArgs(false);
        fwdMsg.SendWithReplyIgnore(*_conn);
        Message::CreateMethodReturn(message).Send(*_conn);
    }

    void PowerManagerService::OnReboot(const Message& message) const
    {
        SharedPolicy(message);

        auto fwdMsg = Message::CreateMethodCall(
            "org.freedesktop.login1",
            ObjectPath("/org/freedesktop/login1"),
            InterfaceName("org.freedesktop.login1.Manager"),
            "Reboot"
        );

        fwdMsg.SetArgs(false);
        fwdMsg.SendWithReplyIgnore(*_conn);
        Message::CreateMethodReturn(message).Send(*_conn);
    }

    void PowerManagerService::OnSuspend(const Message& message) const
    {
        SharedPolicy(message);

        auto fwdMsg = Message::CreateMethodCall(
            "org.freedesktop.login1",
            ObjectPath("/org/freedesktop/login1"),
            InterfaceName("org.freedesktop.login1.Manager"),
            "Suspend"
        );

        fwdMsg.SetArgs(false);
        fwdMsg.SendWithReplyIgnore(*_conn);
        Message::CreateMethodReturn(message).Send(*_conn);
    }

    void PowerManagerService::OnListBatteryDevices(const Message& message) const
    {
        SharedPolicy(message);
        auto msg = Message::CreateMethodReturn(message);
        const auto list = ListDevices();
        msg.SetArgs(list);
        msg.Send(*_conn);
    }

    void PowerManagerService::EmitBatteryDeviceAdded(const ObjectPath& path)
    {
        auto sig = Message::CreateSignal(
            GetBaseObjectPath(),
            GetBaseInterface(),
            "BatteryDeviceAdded"
        );

        sig.SetArgs(path);
        sig.Send(*_conn);
    }

    void PowerManagerService::EmitBatteryDeviceRemoved(const ObjectPath& path)
    {
        auto sig = Message::CreateSignal(
            GetBaseObjectPath(),
            GetBaseInterface(),
            "BatteryDeviceRemoved"
        );

        sig.SetArgs(path);
        sig.Send(*_conn);
    }

    void PowerManagerService::OnNameOwnerChanged(const Message& msg)
    {
        const auto args = msg.GetArgs<std::string, std::string, std::string>();
        const auto name = std::get<0>(args);
        const auto oldOwner = std::get<1>(args);
        const auto newOwner = std::get<2>(args);

        if (name != "org.freedesktop.UPower")
            return;

        if (!newOwner.empty())
            OnPowerManagerAppeared();
        else
            OnPowerManagerDisappeared();
    }

    void PowerManagerService::OnPowerManagerAppeared()
    {
        if (_uPower)
            return;

        DiscoverDevices();
        SubscribeToPowerManagerSignals();

        _uPower = true;
    }

    void PowerManagerService::OnPowerManagerDisappeared()
    {
        if (!_uPower)
            return;

        UnsubscribeFromPowerManagerSignals();

        for (const auto &path: _devices | std::views::keys)
        {
            EmitBatteryDeviceRemoved(path);
        }

        _devices.clear();

        _uPower = false;
    }

    void PowerManagerService::DiscoverDevices()
    {
        const auto msg = Message::CreateMethodCall(
            "org.freedesktop.UPower",
            ObjectPath("/org/freedesktop/UPower"),
            InterfaceName("org.freedesktop.UPower"),
            "EnumerateDevices"
        );

        const auto args = msg.GetArgs<std::vector<ObjectPath>>();
        auto objects = std::get<0>(args);
        for (const auto& path: objects)
        {
            AddDevice(path);
        }
    }

    void PowerManagerService::SubscribeToPowerManagerSignals()
    {
        _subDeviceAdded = std::make_unique<SignalSubscription>(_conn->SubscribeSignal(
            "org.freedesktop.UPower",
            ObjectPath("/org/freedesktop/UPower"),
            InterfaceName("org.freedesktop.UPower"),
            "DeviceAdded",
            [&](const Message& msg)
            {
                const auto device = std::get<0>(msg.GetArgs<ObjectPath>());
                AddDevice(device);
            }
        ));

        _subDeviceRemoved = std::make_unique<SignalSubscription>(_conn->SubscribeSignal(
            "org.freedesktop.UPower",
            ObjectPath("/org/freedesktop/UPower"),
            InterfaceName("org.freedesktop.UPower"),
            "DeviceRemoved",
            [&](const Message& msg)
            {
                const auto device = std::get<0>(msg.GetArgs<ObjectPath>());
                RemoveDevice(device);
            }
        ));
    }

    void PowerManagerService::UnsubscribeFromPowerManagerSignals()
    {
        _subDeviceAdded = nullptr;
        _subDeviceRemoved = nullptr;
    }

    void PowerManagerService::AddDevice(const ObjectPath& upowerPath)
    {
        const auto path = GetBaseObjectPath().Child("Battery").Child(upowerPath.Leaf());

        if (_devices.contains(path))
            return;

        auto device = std::make_unique<PowerDevice>(*this, *_conn, path, upowerPath);
        _devices.emplace(path, std::move(device));
        EmitBatteryDeviceAdded(path);
    }

    void PowerManagerService::RemoveDevice(const ObjectPath& upowerPath)
    {
        const auto path = GetBaseObjectPath().Child("Battery").Child(upowerPath.Leaf());

        const auto it = _devices.find(path);
        if (it == _devices.end())
            return;

        _devices.erase(it);
    }

}
