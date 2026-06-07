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

#define GENERATE_ENUM_STRINGS
#include "application.h"

#include <algorithm>
#include <glib.h>
#include <sstream>
#include <vector>

#include "services/locale/locale_service.h"
#include "services/watchdog/watchdog_service.h"
#if defined(JAPPEOS_DAEMON_SESSION)
#include "services/audio_pulse/audio_service.h"
#endif
#if defined(JAPPEOS_DAEMON_SYSTEM) || !defined(JAPPEOS_DAEMON_SESSION)
#include "services/account_manager/account_manager_service.h"
#include "services/session_manager/session_manager_service.h"
#include "services/network_manager/network_manager_service.h"
#include "services/power_manager/power_manager_service.h"
#include "services/locale/locale_service.h"
#include "services/installer/installer_service.h"
#endif
#include "utils/jos_exception.h"

namespace JappeStudios::JappeOS::JappeOSCore
{

    const char* ApplicationState_ToString(const ApplicationState state) {
        switch (state) {
#define X(name) case ApplicationState::name: return #name;
#include "application_state.def"
#undef X
            default: return "Unknown";
        }
    }

#if defined(JAPPEOS_DAEMON_SESSION)
    const char* SessionTarget_ToString(const SessionTarget target)
    {
        switch (target)
        {
            case SessionTarget::Desktop: return "desktop";
            case SessionTarget::Greeter: return "greeter";
            default: return "unknown";
        }
    }
#endif

    Application* Application::s_instance = nullptr;
    volatile std::sig_atomic_t Application::s_signalCaught = 0;
    int Application::s_pipeFds[2] = { -1, -1 };

    uint8_t Application::Run()
    {
        _exitCode = EXIT_SUCCESS;
        try
        {
            _state = ApplicationState::Initializing;
            Initialize();
            _state = ApplicationState::Running;
            while (_shouldRun)
            {
                // Update the application
                Update();

                // Handle signals
                if (s_signalCaught)
                {
                    switch (s_signalCaught)
                    {
                        case SIGABRT: throw Utils::JosException("SIGABRT", "");
                        case SIGBUS:  throw Utils::JosException("SIGBUS", "");
                        case SIGFPE:  throw Utils::JosException("SIGFPE", "");
                        case SIGHUP:  throw Utils::JosException("SIGHUP", "");
                        case SIGILL:  throw Utils::JosException("SIGILL", "");
                        case SIGPIPE: throw Utils::JosException("SIGPIPE", "");
                        case SIGQUIT: throw Utils::JosException("SIGQUIT", "");
                        case SIGSEGV: throw Utils::JosException("SIGSEGV", "");
                        case SIGSYS:  throw Utils::JosException("SIGSYS", "");
                        case SIGUSR1: throw Utils::JosException("SIGUSR1", "");
                        case SIGUSR2: throw Utils::JosException("SIGUSR2", "");
                        case SIGXCPU: throw Utils::JosException("SIGXCPU", "");
                        case SIGXFSZ: throw Utils::JosException("SIGXFSZ", "");
                        default: Quit();
                    }
                }
            }
            _state = ApplicationState::Stopping;
            CleanUp();
            _state = ApplicationState::Stopped;
        }
        catch (const std::exception& e)
        {
            const auto logger = NULL_SAFE_CALL_RET(_serviceManager, Get<Services::Logger::LoggerService>());
            NULL_SAFE_CALL(logger, Alert(""));
            NULL_SAFE_CALL(logger, Alert("An unhandled exception was thrown! Printing exception information below ..."));
            NULL_SAFE_CALL(logger, Alert("+-- BEGIN EXCEPTION INFO --+"));
            NULL_SAFE_CALL(logger, Alert(""));
            NULL_SAFE_CALL(logger, Alert(std::string("Application State: ") + ApplicationState_ToString(_state)));
            NULL_SAFE_CALL(logger, Alert(""));
            NULL_SAFE_CALL(logger, Alert(std::string("Exception: ") + e.what()));
            NULL_SAFE_CALL(logger, Alert(""));
            NULL_SAFE_CALL(logger, Alert("+-- END EXCEPTION INFO --+"));
            NULL_SAFE_CALL(logger, Alert(""));

            if (_state == ApplicationState::Running)
            {
                NULL_SAFE_CALL(logger, Info("Application was running. Attempting cleanup."));
                try
                {
                    _state = ApplicationState::Stopping;
                    CleanUp();
                }
                catch (const std::exception&)
                {
                    NULL_SAFE_CALL(logger, Err("Cleanup failed."));
                }
            }

            _state = ApplicationState::Stopped;
            _exitCode = EXIT_FAILURE;

            std::string errorCode = "EXCEPTION_UNHANDLED";
            std::string stackTrace;
            if (auto* se = dynamic_cast<const std::system_error*>(&e))
            {
                errorCode = std::to_string(se->code().value());
            }
            else if (auto* je = dynamic_cast<const Utils::JosException*>(&e))
            {
                errorCode = je->ShortErrorCode();
                stackTrace = je->Stacktrace();
            }

            FailFastFatalError(
                errorCode,
                e.what(),
                stackTrace.empty() ? PrintStackTrace() : stackTrace
            );
        }

        return _exitCode;
    }

    void Application::Initialize()
    {
        if (pipe(s_pipeFds) == -1)
        {
            throw std::runtime_error("Failed to create signal pipe");
        }

        struct sigaction sa{};
        sa.sa_handler = SignalHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        // TODO
        //sigaction(SIGABRT, &sa, nullptr);
        //sigaction(SIGBUS, &sa, nullptr);
        //sigaction(SIGFPE, &sa, nullptr);
        //sigaction(SIGHUP, &sa, nullptr);
        //sigaction(SIGILL, &sa, nullptr);
        //sigaction(SIGPIPE, &sa, nullptr);
        sigaction(SIGQUIT, &sa, nullptr);
        //sigaction(SIGSEGV, &sa, nullptr);
        //sigaction(SIGSYS, &sa, nullptr);
        //sigaction(SIGUSR1, &sa, nullptr);
        //sigaction(SIGUSR2, &sa, nullptr);
        //sigaction(SIGXCPU, &sa, nullptr);
        //sigaction(SIGXFSZ, &sa, nullptr);
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);

        _serviceManager = new Services::ServiceManager();
        InitDBus();
        _serviceManager->InitDBus(_conn);

        const auto logger = _serviceManager->Register<Services::Logger::StdoutLogger>();
        NULL_SAFE_CALL(logger, Debug("Initializing..."));
        NULL_SAFE_CALL(logger, Debug(std::string("D-Bus name acquired: ") + DBUS_INTERFACE));
        InitServices();
        NULL_SAFE_CALL(logger, Debug("Initialization done!"));
    }

    void Application::InitDBus()
    {
        dbus_error_init(_err);
#if defined(JAPPEOS_DAEMON_SESSION)
        _conn = new Connection(DBUS_BUS_SESSION);
#else
        _conn = new Connection(DBUS_BUS_SYSTEM);
#endif

        if (dbus_error_is_set(_err))
        {
            const std::string message = _err->message;
            dbus_error_free(_err);
            throw std::runtime_error("D-Bus error: " + message);
        }

        _conn->AcquireName(DBUS_INTERFACE, DBUS_NAME_FLAG_DO_NOT_QUEUE);
        _conn->AddMatch("type='method_call'"); // TODO: Don't allow all method calls
        _conn->Flush();
        const int dbus_fd = _conn->GetUnixFd();

        _fds[0].fd = dbus_fd;
        _fds[0].events = POLLIN;

        _fds[1].fd = s_pipeFds[0];
        _fds[1].events = POLLIN;
    }

    void Application::InitServices() const
    {
        _serviceManager->Register<Services::Watchdog::WatchdogService>();
#if defined(JAPPEOS_DAEMON_SESSION)
        _serviceManager->Register<Services::AudioPulse::AudioService>();
#endif
#if defined(JAPPEOS_DAEMON_SYSTEM) || !defined(JAPPEOS_DAEMON_SESSION)
        _serviceManager->Register<Services::SessionManager::SessionManagerService>();
        _serviceManager->Register<Services::AccountManager::AccountManagerService>();
        _serviceManager->Register<Services::PowerManager::PowerManagerService>();
        _serviceManager->Register<Services::NetworkManager::NetworkManagerService>();
        _serviceManager->Register<Services::Locale::LocaleService>();
        _serviceManager->Register<Services::Installer::InstallerService>();
#endif
    }

    void Application::Update()
    {
        // Integrate GLib default main context fds into our poll loop so libnm
        // and other GLib sources can wake this daemon even when there is no
        // traffic on our own D-Bus Connection fd.
        auto* mainContext = g_main_context_default();

        std::vector<GPollFD> glibFds;
        gint glibMaxPriority = 0;
        gint glibTimeoutMs = -1;
        gint glibN = 0;
        bool glibAcquired = false;
        bool glibReady = false;

        if (mainContext)
        {
            glibAcquired = g_main_context_acquire(mainContext);
            if (glibAcquired)
            {
                glibReady = g_main_context_prepare(mainContext, &glibMaxPriority);

                glibFds.resize(16);
                glibN = g_main_context_query(
                    mainContext,
                    glibMaxPriority,
                    &glibTimeoutMs,
                    glibFds.data(),
                    static_cast<gint>(glibFds.size())
                );

                if (glibN > static_cast<gint>(glibFds.size()))
                {
                    glibFds.resize(static_cast<size_t>(glibN));
                    glibN = g_main_context_query(
                        mainContext,
                        glibMaxPriority,
                        &glibTimeoutMs,
                        glibFds.data(),
                        static_cast<gint>(glibFds.size())
                    );
                }

                if (glibReady)
                    glibTimeoutMs = 0;
            }
        }

        std::vector<pollfd> pollFds;
        pollFds.reserve(2 + static_cast<size_t>(std::max(0, glibN)));
        pollFds.push_back(_fds[0]);
        pollFds.push_back(_fds[1]);

        for (gint i = 0; i < glibN; ++i)
        {
            pollfd pfd{};
            pfd.fd = glibFds[i].fd;
            pfd.events = static_cast<short>(glibFds[i].events);
            pollFds.push_back(pfd);
        }

        if (poll(pollFds.data(), pollFds.size(), glibTimeoutMs) < 0)
        {
            if (glibAcquired)
                g_main_context_release(mainContext);
            return;
        }

        _fds[0].revents = pollFds[0].revents;
        _fds[1].revents = pollFds[1].revents;

        const auto logger = NULL_SAFE_CALL_RET(_serviceManager, Get<Services::Logger::LoggerService>());

        // GLib support
        if (glibAcquired)
        {
            for (gint i = 0; i < glibN; ++i)
                glibFds[i].revents = static_cast<gushort>(pollFds[2 + i].revents);

            if (g_main_context_check(mainContext, glibMaxPriority, glibFds.data(), glibN))
                g_main_context_dispatch(mainContext);

            g_main_context_release(mainContext);
        }

        // Signals
        if (_fds[1].revents & POLLIN)
        {
            char buf[64];
            read(s_pipeFds[0], buf, sizeof(buf));

            NULL_SAFE_CALL(logger, Info("Received signal, exiting..."));
            Quit();
            return;
        }

        // D-Bus messages
        if (_fds[0].revents & POLLIN)
        {
            while (_conn->ReadWrite(0))
            {
                auto msg = _conn->PopMessage();
                if (!msg.has_value() || _serviceManager == nullptr) break;

                OnMessageHandlerPre();
                if (!_conn->HandleMessage(msg.value()))
                {
                    HandleDBusMessageLegacy(logger, msg.value().GetRawMessage());
                }
                OnMessageHandlerPost();
            }
        }
    }

    void Application::HandleDBusMessageLegacy(Services::Logger::LoggerService* logger, DBusMessage* msg) const
    {
        const char* interface = dbus_message_get_interface(msg);
        if (!interface)
            return;

        auto services = _serviceManager->ListNamed();

        if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_SIGNAL)
        {
            const std::string key = std::string(interface) + "." + dbus_message_get_member(msg);
            const auto& signalSubscribers = _serviceManager->ListSignalSubscribers();
            if (const auto subscribers = signalSubscribers.find(key); subscribers != signalSubscribers.end())
            {
                for (const auto svc : subscribers->second)
                {
                    HandleMethodCallLegacy(logger, svc, msg, interface);
                }
            }
            return;
        }

        if (const auto it = services.find(interface); it != services.end())
        {
            HandleMethodCallLegacy(logger, it->second, msg, interface);
        }
    }

    void Application::HandleMethodCallLegacy(Services::Logger::LoggerService* logger,
                                             Services::Service* svc,
                                             DBusMessage* msg,
                                             const char* interface) const
    {
        if (!msg)
        {
            NULL_SAFE_CALL(
                logger,
                Err(
                    std::string("HandleMethodCall called with null parameters for service `" + svc->GetName()
                    + "` with interface: ")
                    + interface
                )
            );
            return;
        }

        try
        {
            if (!svc->HandleMethodCallLegacy(msg))
            {
                NULL_SAFE_CALL(
                    logger,
                    Debug(std::string("Method not handled in service `" + svc->GetName() + "` with interface: ")
                        + interface)
                );
            }
        }
        catch (const std::exception& e)
        {
            NULL_SAFE_CALL(
                logger,
                Crit(std::string("(non-fatal) Unhandled exception in service `" + svc->GetName()
                    + "` while handling method call: ") + e.what())
            );
        }
    }

    void Application::CleanUp() const
    {
        const auto logger = NULL_SAFE_CALL_RET(_serviceManager, Get<Services::Logger::LoggerService>());
        NULL_SAFE_CALL(logger, Debug("Cleaning up..."));
        dbus_error_free(_err);
        delete _err;
        _serviceManager->RunBeforeCleanup(logger, [&]
        {
            NULL_SAFE_CALL(logger, Debug("Cleanup done!"));
        });
        delete _serviceManager;
        delete _conn;
    }

    void Application::FailFastFatalError(const std::string& errCode,
                                         const std::string& message,
                                         const std::string& stack) const
    {
        const auto logger = NULL_SAFE_CALL_RET(_serviceManager, Get<Services::Logger::LoggerService>());

        auto newErrCode = errCode;
        for (auto& c: newErrCode) c = toupper(c);
        std::erase(newErrCode, ' ');

        const auto newStack = stack.empty() ? PrintStackTrace() : stack;

        NULL_SAFE_CALL(
            logger,
            Emerg(
                "[Application::FailFastFatalError] FailFastFatalError called! "
                "ErrCode: " + newErrCode
                + ", Message: " + message
                + ", Stack: " + newStack
                + ". This indicates that a fatal system error has occurred."
            )
        );
        NULL_SAFE_CALL(
            logger,
            Crit("[Application::FailFastFatalError] Exiting immediately because of unrecoverable fatal error.")
        );
        exit(EXIT_FAILURE);
    }

    std::string Application::PrintStackTrace(unsigned int max_frames)
    {
        std::ostringstream oss;

        // Storage array for stack trace address data
        void* addrlist[max_frames + 1];

        // Retrieve current stack addresses
        const int addrlen = backtrace(addrlist, sizeof(addrlist) / sizeof(void*));
        if (addrlen == 0)
        {
            oss << "<empty, possibly corrupt>\n";
            return oss.str();
        }

        // Resolve addresses into strings containing "filename(function+address)"
        char** symbols = backtrace_symbols(addrlist, addrlen);
        if (!symbols)
        {
            oss << "<backtrace_symbols failed>\n";
            return oss.str();
        }

        // Iterate over returned symbol lines
        for (int i = 0; i < addrlen; i++)
        {
            std::string sym(symbols[i]);
            std::string function_name = sym;

            // Try to extract the mangled name between '(' and '+'
            const size_t begin = sym.find('(');
            size_t end   = sym.find('+', begin);
            if (begin != std::string::npos && end != std::string::npos)
            {
                std::string mangled = sym.substr(begin + 1, end - begin - 1);
                int status = 0;
                char* demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
                if (status == 0 && demangled)
                {
                    function_name = sym.substr(0, begin + 1) + demangled + sym.substr(end);
                    std::free(demangled);
                }
            }

            oss << "[" << i << "] " << function_name << "\n";
        }

        free(symbols);
        return oss.str();
    }

}
