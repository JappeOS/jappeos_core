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

#include "mount_step.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "../installer_def.h"
#include "../../../utils/command_runner.h"

using JappeStudios::JappeOS::JappeOSCore::Utils::CommandRunner;

namespace JappeStudios::JappeOS::JappeOSCore::Services::Installer::Steps
{

    void MountStep::Execute(InstallContext& context)
    {
        const auto& rootTarget = FindTarget(context, STORAGE_MOUNTPOINT_ROOT);
        const auto& bootTarget = FindTarget(context, STORAGE_MOUNTPOINT_BOOT);

        const std::filesystem::path targetRoot(INSTALL_TARGET_ROOT);
        const std::filesystem::path targetBoot = targetRoot / "boot";

        std::vector<std::filesystem::path> mounted;
        try
        {
            MountDevice(rootTarget.device, targetRoot);
            mounted.push_back(targetRoot);

            MountDevice(bootTarget.device, targetBoot);
            mounted.push_back(targetBoot);
        }
        catch (...)
        {
            for (auto it = mounted.rbegin(); it != mounted.rend(); ++it)
            {
                try
                {
                    UnmountDevice(*it);
                }
                catch (...)
                {
                }
            }

            throw;
        }

        context.targetRoot = targetRoot.string();
    }

    const InstallPartitionTargetData& MountStep::FindTarget(const InstallContext& context, const std::string& mountpoint)
    {
        const InstallPartitionTargetData* target = nullptr;
        for (const auto& candidate : context.partitionTargets)
        {
            if (candidate.mountpoint != mountpoint)
                continue;

            if (target)
                throw std::runtime_error("Multiple partition targets for mountpoint: " + mountpoint);

            target = &candidate;
        }

        if (!target)
            throw std::runtime_error("Missing partition target for mountpoint: " + mountpoint);

        if (target->device.empty())
            throw std::runtime_error("Partition target has empty device for mountpoint: " + mountpoint);

        return *target;
    }

    void MountStep::MountDevice(const std::string& device, const std::filesystem::path& mountpoint)
    {
        std::filesystem::create_directories(mountpoint);
        CommandRunner::RunOrThrow({"mount", device, mountpoint.string()});
    }

    void MountStep::UnmountDevice(const std::filesystem::path& mountpoint)
    {
        CommandRunner::RunOrThrow({"umount", mountpoint.string()});
    }

}
