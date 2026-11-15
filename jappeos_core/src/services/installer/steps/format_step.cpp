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

#include "format_step.h"

#include <stdexcept>
#include <string>
#include <vector>

#include "../installer_def.h"
#include "../../../utils/command_runner.h"

using JappeStudios::JappeOS::JappeOSCore::Utils::CommandRunner;

namespace JappeStudios::JappeOS::JappeOSCore::Services::Installer::Steps
{

    void FormatStep::Execute(InstallContext& context)
    {
        for (const auto& target : context.partitionTargets)
        {
            if (!target.format)
                continue;

            if (target.device.empty())
                throw std::runtime_error("Cannot format an empty partition target");

            CommandRunner::RunOrThrow(BuildFormatCommand(target));
        }
    }

    std::vector<std::string> FormatStep::BuildFormatCommand(const InstallPartitionTargetData& target)
    {
        if (target.filesystem == STORAGE_FILESYSTEM_FAT32)
            return {"mkfs.fat", "-F", "32", target.device};

        if (target.filesystem == STORAGE_FILESYSTEM_EXT4)
            return {"mkfs.ext4", "-F", target.device};

        if (target.filesystem == STORAGE_FILESYSTEM_BTRFS)
            return {"mkfs.btrfs", "-f", target.device};

        if (target.filesystem == STORAGE_FILESYSTEM_XFS)
            return {"mkfs.xfs", "-f", target.device};

        throw std::runtime_error("Unsupported filesystem for formatting: " + target.filesystem);
    }

}
