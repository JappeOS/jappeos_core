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

#include "session_host_service.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "../../application.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::SessionHost
{
    static void CloseFd(int& fd)
    {
        if (fd >= 0) close(fd);
        fd = -1;
    }

    static void DrainPipe(int fd)
    {
        if (fd < 0) return;
        for (;;)
        {
            char buf[64];
            const auto n = read(fd, buf, sizeof(buf));
            if (n <= 0) break;
        }
    }

    static void SetNonBlocking(int fd)
    {
        if (fd < 0) return;
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) return;
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    static bool WriteAll(int fd, const char* buf, size_t len)
    {
        size_t off = 0;
        while (off < len)
        {
            const auto n = write(fd, buf + off, len - off);
            if (n > 0)
            {
                off += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        return true;
    }

    SessionHostService::SessionHostService(ServiceManager* serviceManager, Connection* conn)
        : Service(serviceManager, conn)
    {
        const auto type = Application::GetInstance()->GetSessionTarget();
        _path = (type == SessionTarget::Greeter) ? JOS_GREETER_BINARY : JOS_DESKTOP_BINARY;
        Start();
    }

    SessionHostService::~SessionHostService()
    {
        StopAndJoin();
        CleanupChildFds();
        CloseFd(_controlPipe[0]);
        CloseFd(_controlPipe[1]);
    }

    void SessionHostService::Start()
    {
        if (pipe(_controlPipe) != 0)
        {
            Log().Err(std::string("Failed to create control pipe: ") + std::strerror(errno));
            _controlPipe[0] = -1;
            _controlPipe[1] = -1;
        }
        else
        {
            SetNonBlocking(_controlPipe[0]);
            SetNonBlocking(_controlPipe[1]);
        }

        _supervisorThread = std::thread([this] { SupervisorThreadFunc(); });
    }

    void SessionHostService::StopAndJoin()
    {
        if (_stopRequested.exchange(true))
            return;

        RequestStop();

        if (_supervisorThread.joinable())
            _supervisorThread.join();
    }

    void SessionHostService::RequestStop() const
    {
        if (_controlPipe[1] < 0) return;
        constexpr char b = 1;
        (void)write(_controlPipe[1], &b, 1);
    }

    void SessionHostService::CleanupChildFds()
    {
        CloseFd(_pidfd);
        CloseFd(_stdoutReadFd);
        CloseFd(_stderrReadFd);
    }

    bool SessionHostService::SpawnChild()
    {
        int outPipe[2]{-1, -1};
        int errPipe[2]{-1, -1};

        if (pipe(outPipe) != 0 || pipe(errPipe) != 0)
        {
            Log().Err(std::string("pipe() failed: ") + std::strerror(errno));
            CloseFd(outPipe[0]);
            CloseFd(outPipe[1]);
            CloseFd(errPipe[0]);
            CloseFd(errPipe[1]);
            return false;
        }

        const pid_t pid = fork();
        if (pid < 0)
        {
            Log().Err(std::string("fork() failed: ") + std::strerror(errno));
            CloseFd(outPipe[0]);
            CloseFd(outPipe[1]);
            CloseFd(errPipe[0]);
            CloseFd(errPipe[1]);
            return false;
        }

        if (pid == 0)
        {
            // Child: become its own process group leader so we can signal the whole tree via -PID.
            (void)setpgid(0, 0);

            (void)dup2(outPipe[1], STDOUT_FILENO);
            (void)dup2(errPipe[1], STDERR_FILENO);

            CloseFd(outPipe[0]);
            CloseFd(outPipe[1]);
            CloseFd(errPipe[0]);
            CloseFd(errPipe[1]);

            execl(_path.c_str(), _path.c_str(), nullptr);
            _exit(127);
        }

        // Parent
        _pid = pid;
        (void)setpgid(_pid, _pid);

        CloseFd(outPipe[1]);
        CloseFd(errPipe[1]);

        _stdoutReadFd = outPipe[0];
        _stderrReadFd = errPipe[0];
        SetNonBlocking(_stdoutReadFd);
        SetNonBlocking(_stderrReadFd);

#ifdef SYS_pidfd_open
        _pidfd = static_cast<int>(syscall(SYS_pidfd_open, _pid, 0));
        if (_pidfd < 0)
        {
            // pidfd is optional; fallback to waitpid polling.
            if (errno != ENOSYS)
                Log().Warn(std::string("pidfd_open failed, falling back to waitpid polling: ") + std::strerror(errno));
            _pidfd = -1;
        }
#else
        _pidfd = -1;
#endif

        Log().Info("Spawned session process pid=" + std::to_string(_pid) + " path=" + _path);
        return true;
    }

    void SessionHostService::ForwardFdTo(int fd, int outFd, const std::function<void(std::string)>& hook) const
    {
        if (fd < 0) return;
        char buf[4096];
        for (;;)
        {
            const auto n = read(fd, buf, sizeof(buf));
            if (n > 0)
            {
                (void)WriteAll(outFd, buf, static_cast<size_t>(n));
                if (hook) hook(std::string(buf, static_cast<size_t>(n)));
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            break;
        }
    }

    bool SessionHostService::WaitForChildExitWithTimeout(const int timeoutMs, int* outStatus)
    {
        if (_pid <= 0) return true;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline)
        {
            pollfd fds[3]{};
            int nfds = 0;
            int stdoutIndex = -1;
            int stderrIndex = -1;
            int pidfdIndex = -1;

            if (_stdoutReadFd >= 0)
            {
                stdoutIndex = nfds;
                fds[nfds].fd = _stdoutReadFd;
                fds[nfds].events = POLLIN | POLLHUP;
                nfds++;
            }
            if (_stderrReadFd >= 0)
            {
                stderrIndex = nfds;
                fds[nfds].fd = _stderrReadFd;
                fds[nfds].events = POLLIN | POLLHUP;
                nfds++;
            }
            if (_pidfd >= 0)
            {
                pidfdIndex = nfds;
                fds[nfds].fd = _pidfd;
                fds[nfds].events = POLLIN;
                nfds++;
            }

            int remainingMs = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                    .count());
            if (remainingMs < 0) remainingMs = 0;

            const int sliceMs = (remainingMs > 250) ? 250 : remainingMs;
            if (nfds > 0)
                (void)poll(fds, nfds, sliceMs);
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(sliceMs));

            if (stdoutIndex >= 0 && (fds[stdoutIndex].revents & (POLLIN | POLLHUP)))
            {
                ForwardFdTo(_stdoutReadFd, STDOUT_FILENO, _onStdout);
                if (fds[stdoutIndex].revents & POLLHUP) CloseFd(_stdoutReadFd);
            }
            if (stderrIndex >= 0 && (fds[stderrIndex].revents & (POLLIN | POLLHUP)))
            {
                ForwardFdTo(_stderrReadFd, STDERR_FILENO, _onStderr);
                if (fds[stderrIndex].revents & POLLHUP) CloseFd(_stderrReadFd);
            }

            int st = 0;
            if (_pidfd >= 0)
            {
                if (pidfdIndex >= 0 && (fds[pidfdIndex].revents & POLLIN))
                {
                    for (;;)
                    {
                        const pid_t w = waitpid(_pid, &st, 0);
                        if (w == _pid) break;
                        if (w < 0 && errno == EINTR) continue;
                        return false;
                    }
                    if (outStatus) *outStatus = st;
                    return true;
                }
            }
            else
            {
                const pid_t w = waitpid(_pid, &st, WNOHANG);
                if (w == _pid)
                {
                    if (outStatus) *outStatus = st;
                    return true;
                }
                if (w < 0 && errno != EINTR)
                    return false;
            }
        }
        return false;
    }

    void SessionHostService::TerminateChildBlocking()
    {
        if (_pid <= 0) return;

        Log().Info("Stopping session process pid=" + std::to_string(_pid));
        (void)kill(-_pid, SIGTERM);

        int status = 0;
        if (!WaitForChildExitWithTimeout(5000, &status))
        {
            Log().Warn("Session process did not exit in time; sending SIGKILL pid=" + std::to_string(_pid));
            (void)kill(-_pid, SIGKILL);
            (void)WaitForChildExitWithTimeout(5000, &status);
        }

        if (_onExit) _onExit(status);

        _pid = -1;
        CleanupChildFds();
    }

    void SessionHostService::SupervisorThreadFunc()
    {
        while (!_stopRequested.load())
        {
            if (_pid <= 0)
            {
                if (!SpawnChild())
                {
                    // Retry periodically, but wake quickly on stop.
                    pollfd ctrl{};
                    ctrl.fd = _controlPipe[0];
                    ctrl.events = POLLIN;
                    (void)poll(&ctrl, (_controlPipe[0] >= 0) ? 1 : 0, 1000);
                    if (ctrl.revents & POLLIN) DrainPipe(_controlPipe[0]);
                    continue;
                }
            }

            pollfd fds[4]{};
            int nfds = 0;
            int ctrlIndex = -1;
            int stdoutIndex = -1;
            int stderrIndex = -1;

            if (_controlPipe[0] >= 0)
            {
                ctrlIndex = nfds;
                fds[nfds].fd = _controlPipe[0];
                fds[nfds].events = POLLIN;
                nfds++;
            }
            if (_stdoutReadFd >= 0)
            {
                stdoutIndex = nfds;
                fds[nfds].fd = _stdoutReadFd;
                fds[nfds].events = POLLIN | POLLHUP;
                nfds++;
            }
            if (_stderrReadFd >= 0)
            {
                stderrIndex = nfds;
                fds[nfds].fd = _stderrReadFd;
                fds[nfds].events = POLLIN | POLLHUP;
                nfds++;
            }
            if (_pidfd >= 0)
            {
                fds[nfds].fd = _pidfd;
                fds[nfds].events = POLLIN;
                nfds++;
            }

            const int timeoutMs = (ctrlIndex >= 0) ? -1 : 250;
            const int r = poll(fds, nfds, timeoutMs);
            if (r < 0)
            {
                if (errno == EINTR) continue;
                Log().Err(std::string("poll() failed: ") + std::strerror(errno));
                continue;
            }
            if (_stopRequested.load())
                break;

            // Stop request
            if (ctrlIndex >= 0 && (fds[ctrlIndex].revents & POLLIN))
            {
                DrainPipe(_controlPipe[0]);
                _stopRequested.store(true);
                break;
            }

            // stdout/stderr forwarding
            if (stdoutIndex >= 0 && (fds[stdoutIndex].revents & (POLLIN | POLLHUP)))
            {
                ForwardFdTo(_stdoutReadFd, STDOUT_FILENO, _onStdout);
                if (fds[stdoutIndex].revents & POLLHUP) CloseFd(_stdoutReadFd);
            }
            if (stderrIndex >= 0 && (fds[stderrIndex].revents & (POLLIN | POLLHUP)))
            {
                ForwardFdTo(_stderrReadFd, STDERR_FILENO, _onStderr);
                if (fds[stderrIndex].revents & POLLHUP) CloseFd(_stderrReadFd);
            }

            // Process exit detection
            bool exited = false;
            int status = 0;
            if (_pidfd >= 0)
            {
                for (int i = 0; i < nfds; i++)
                {
                    if (fds[i].fd == _pidfd && (fds[i].revents & POLLIN))
                    {
                        exited = true;
                        break;
                    }
                }
            }
            else
            {
                const pid_t w = waitpid(_pid, &status, WNOHANG);
                if (w == _pid) exited = true;
            }

            if (exited)
            {
                if (_pidfd >= 0)
                    (void)waitpid(_pid, &status, 0);

                if (_onExit) _onExit(status);

                if (WIFEXITED(status))
                    Log().Info("Session process exited code=" + std::to_string(WEXITSTATUS(status)));
                else if (WIFSIGNALED(status))
                    Log().Info("Session process killed signal=" + std::to_string(WTERMSIG(status)));

                _pid = -1;
                CleanupChildFds();

                if (!_stopRequested.load())
                {
                    // Avoid a tight respawn loop if the binary immediately exits (e.g. exec failure).
                    pollfd ctrl{};
                    ctrl.fd = _controlPipe[0];
                    ctrl.events = POLLIN;
                    (void)poll(&ctrl, (_controlPipe[0] >= 0) ? 1 : 0, 1000);
                    if (ctrl.revents & POLLIN) DrainPipe(_controlPipe[0]);
                }
            }
        }

        // Final shutdown path: terminate child and wait for it to exit before returning.
        TerminateChildBlocking();
    }
}
