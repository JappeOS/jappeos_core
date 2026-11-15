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
#include <chrono>
#include <string>
#include <vector>
#include <ranges>

namespace JappeStudios::JappeOS::JappeOSCore::Utils
{
    struct CommandResult
    {
        int exitCode;
        std::string stdOut;
        std::string stdErr;
    };

    class CommandFailedException : public std::runtime_error
    {
    public:
        explicit CommandFailedException(
            std::vector<std::string> command,
            const int exitCode,
            std::string stderrText = "<empty>")
            : std::runtime_error(
                BuildMessage(
                    command,
                    exitCode,
                    stderrText)),
              _command(std::move(command)),
              _exitCode(exitCode),
              _stdErr(std::move(stderrText))
        {}

        [[nodiscard]] const auto& Command() const noexcept { return _command; }
        [[nodiscard]] int ExitCode() const noexcept { return _exitCode; }
        [[nodiscard]] const auto& StdErr() const noexcept { return _stdErr; }

    private:
        std::vector<std::string> _command;
        int                      _exitCode;
        std::string              _stdErr;

    private:
        static std::string BuildMessage(
            const std::vector<std::string>& command,
            const int exitCode,
            const std::string& stderrText)
        {
            std::stringstream res;
            std::ranges::copy(command, std::ostream_iterator<std::string>(res, "  "));
            const auto cmd = res.str();

            return
                "Command failed: " +
                cmd +
                " (exit code " +
                std::to_string(exitCode) +
                ")\n" +
                stderrText;
        }
    };

    class CommandRunner final
    {
    public:
        CommandRunner() = delete;

        static CommandResult Run(const std::vector<std::string>& command);
        //static CommandResult Run(const std::vector<std::string>& command, std::chrono::milliseconds timeout);
        static void RunOrThrow(const std::vector<std::string>& command);
        //static void RunOrThrow(const std::vector<std::string>& command, std::chrono::milliseconds timeout);
    };
}
