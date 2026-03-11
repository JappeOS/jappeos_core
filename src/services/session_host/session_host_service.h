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

#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <sys/types.h>

#include "../service.h"
#include "../../logger.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::SessionHost
{
    class SessionHostService : public Service
    {
    public:
        explicit SessionHostService(ServiceManager* serviceManager, Connection* conn);
        ~SessionHostService() override;
        [[nodiscard]] std::string GetName() const override { return "SessionHostService"; }

    public:
        const char* JOS_DESKTOP_BINARY = "/jappeos/desktop/desktop";
        const char* JOS_GREETER_BINARY = "/jappeos/greeter/greeter";

    private:
        std::string _path;

        std::atomic<bool> _stopRequested{false};
        std::thread _supervisorThread;

        pid_t _pid = -1;
        int _pidfd = -1;
        int _stdoutReadFd = -1;
        int _stderrReadFd = -1;
        int _controlPipe[2]{-1, -1}; // [0]=read, [1]=write

        std::function<void(std::string)> _onStdout;
        std::function<void(std::string)> _onStderr;
        std::function<void(int)> _onExit;

        void Start();
        void StopAndJoin();
        void SupervisorThreadFunc();

        bool SpawnChild();
        void CleanupChildFds();
        void RequestStop() const;
        void TerminateChildBlocking();
        bool WaitForChildExitWithTimeout(int timeoutMs, int* outStatus);
        void ForwardFdTo(int fd, int outFd, const std::function<void(std::string)>& hook) const;

    private:
        static JappeOSCore::Logger& Log() {
            static JappeOSCore::Logger instance{"SessionHostService"};
            return instance;
        }
    };
}
