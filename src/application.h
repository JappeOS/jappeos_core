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

#pragma once

#include <cassert>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <execinfo.h>
#include <cxxabi.h>
#include <iostream>
#include <bits/ostream.tcc>
#include <dbus/dbus.h>
#include <poll.h>
#include <algorithm>

#include "event.h"
#include "globals.h"
#include "dbus/dbus_wrapper.h"
#include "services/service.h"

#include "services/logger/stdout_logger.h"

namespace JappeStudios::JappeOS::JappeOSCore
{
    enum class ApplicationState {
#define X(name) name,
#include "application_state.def"
#undef X
    };

    const char* ApplicationState_ToString(ApplicationState state);

    class Application
    {
    public:
        Event<> OnMessageHandlerPre;
        Event<> OnMessageHandlerPost;

        Application() { s_instance = this; }
        ~Application() = default;
        Application(Application& other) = delete;
        void operator=(const Application&) = delete;
        uint8_t Run();
        void Quit(const uint8_t exitCode = EXIT_SUCCESS) { _shouldRun = false; _exitCode = exitCode; }
        void FailFastFatalError(const std::string& errCode, const std::string& message, const std::string& stack) const;
        [[nodiscard]] bool IsRunning() const { return _shouldRun; }

    public:
        static Application* GetInstance()
        {
            assert(s_instance != nullptr);
            return s_instance;
        }

        static std::string PrintStackTrace(unsigned int max_frames = 64); // TODO: Move to utils

    private:
        bool _shouldRun = true;
        uint8_t _exitCode = EXIT_SUCCESS;
        ApplicationState _state = ApplicationState::Initializing;
        Connection* _conn = nullptr;
        DBusError* _err = new DBusError;
        pollfd _fds[2]{};
        Services::ServiceManager* _serviceManager = nullptr;

        void Initialize();
        void InitDBus();
        void InitServices() const;
        void Update();
        void HandleDBusMessageLegacy(Services::Logger::LoggerService* logger, DBusMessage* msg) const;
        void HandleMethodCallLegacy(Services::Logger::LoggerService* logger,
                                    Services::Service* svc,
                                    DBusMessage* msg,
                                    const char* interface) const;
        void CleanUp() const;

    private:
        static Application* s_instance;
        static volatile std::sig_atomic_t s_signalCaught;
        static int s_pipeFds[2]; // pipe_fds[0] = read, pipe_fds[1] = write

        static void SignalHandler(int signal)
        {
            s_signalCaught = signal;

            constexpr char signal_byte = 1;
            write(s_pipeFds[1], &signal_byte, 1);
        }
    };
}
