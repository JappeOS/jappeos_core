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

#include "cleanup_step.h"

#include <filesystem>
#include <stdexcept>

#include "../../../utils/command_runner.h"

using JappeStudios::JappeOS::JappeOSCore::Utils::CommandRunner;

namespace JappeStudios::JappeOS::JappeOSCore::Services::Installer::Steps
{

    void CleanupStep::Execute(InstallContext& context)
    {
        if (context.targetRoot.empty())
            return;

        const std::filesystem::path targetRoot(context.targetRoot);

        CommandRunner::Run({"sync"});

        const std::vector<std::filesystem::path> mountpoints = {
            targetRoot / "boot",
            targetRoot,
        };

        std::vector<std::string> errors;
        for (const auto& mountpoint : mountpoints)
        {
            if (!std::filesystem::exists(mountpoint))
                continue;

            const auto result = CommandRunner::Run({"umount", mountpoint.string()});
            if (result.exitCode != 0)
                errors.push_back("Failed to unmount " + mountpoint.string() + ": " +
                                 (result.stdErr.empty() ? "<empty>" : result.stdErr));
        }

        context.targetRoot.clear();
        context.rootPartition.clear();

        if (!errors.empty())
        {
            std::string message = "Cleanup encountered errors:";
            for (const auto& error : errors)
                message += "\n  " + error;
            throw std::runtime_error(message);
        }
    }

}
