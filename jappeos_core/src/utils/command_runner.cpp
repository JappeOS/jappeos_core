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

#include "command_runner.h"

#include <cstring>
#include <unistd.h>
#include <sys/poll.h>
#include <sys/wait.h>

namespace JappeStudios::JappeOS::JappeOSCore::Utils
{

    CommandResult CommandRunner::Run(const std::vector<std::string>& command)
    {
        if (command.empty())
            throw std::invalid_argument("Command must not be empty");

        // Build argv
        std::vector<char*> argv;
        argv.reserve(command.size() + 1);
        for (const auto& arg : command)
            argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);

        // Create pipes
        int stdoutPipe[2];
        int stderrPipe[2];

        if (pipe(stdoutPipe) == -1)
            throw std::runtime_error(std::string("pipe(stdout): ") + strerror(errno));

        if (pipe(stderrPipe) == -1)
        {
            close(stdoutPipe[0]); close(stdoutPipe[1]);
            throw std::runtime_error(std::string("pipe(stderr): ") + strerror(errno));
        }

        const pid_t pid = fork();
        if (pid == -1)
        {
            close(stdoutPipe[0]); close(stdoutPipe[1]);
            close(stderrPipe[0]); close(stderrPipe[1]);
            throw std::runtime_error(std::string("fork: ") + strerror(errno));
        }

        // CHILD:
        if (pid == 0)
        {
            close(stdoutPipe[0]);
            close(stderrPipe[0]);

            if (dup2(stdoutPipe[1], STDOUT_FILENO) == -1) _exit(126);
            if (dup2(stderrPipe[1], STDERR_FILENO) == -1) _exit(126);

            close(stdoutPipe[1]);
            close(stderrPipe[1]);

            execvp(argv[0], argv.data());
            _exit(127); // execvp only returns on failure
        }

        // PARENT:

        close(stdoutPipe[1]);
        close(stderrPipe[1]);

        std::string stdoutBuf, stderrBuf;
        char buf[4096];

        pollfd fds[2];
        fds[0] = { stdoutPipe[0], POLLIN, 0 };
        fds[1] = { stderrPipe[0], POLLIN, 0 };

        int openFds = 2;

        // Drain loop
        while (openFds > 0)
        {
            int ready = poll(fds, 2, -1); // -1 = block until at least one is ready
            if (ready == -1)
            {
                if (errno == EINTR) continue; // interrupted by signal, retry
                break;
            }

            for (int i = 0; i < 2; ++i)
            {
                if (fds[i].fd == -1)
                    continue;

                if (fds[i].revents & POLLIN)
                {
                    ssize_t n = read(fds[i].fd, buf, sizeof(buf));
                    if (n > 0)
                        (i == 0 ? stdoutBuf : stderrBuf).append(buf, n);
                }

                // POLLHUP fires when the child closes its write end (i.e. exits).
                // Check after POLLIN so we drain any final bytes first.
                if (fds[i].revents & (POLLHUP | POLLERR))
                {
                    close(fds[i].fd);
                    fds[i].fd = -1;
                    --openFds;
                }
            }
        }

        // Reap child
        int status = -1;
        while (waitpid(pid, &status, 0) == -1)
        {
            if (errno != EINTR)
                throw std::runtime_error(std::string("waitpid: ") + strerror(errno));
        }

        return CommandResult {
            .exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1,
            .stdOut   = std::move(stdoutBuf),
            .stdErr   = std::move(stderrBuf),
        };
    }

    void CommandRunner::RunOrThrow(const std::vector<std::string>& command)
    {
        if (const auto result = Run(command); result.exitCode != 0)
        {
            throw CommandFailedException(
                command,
                result.exitCode,
                result.stdErr.empty() ? "<empty>" : result.stdErr
            );
        }
    }

}
