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

#include "generate_fstab_step.h"

#include <fstream>

#include "../../../utils/command_runner.h"

using JappeStudios::JappeOS::JappeOSCore::Utils::CommandRunner;

namespace JappeStudios::JappeOS::JappeOSCore::Services::Installer::Steps
{

    void GenerateFstabStep::Execute(InstallContext& context)
    {
        if (context.targetRoot.empty())
            throw std::runtime_error("Cannot create fstab with empty target root path");

        const std::filesystem::path targetRoot(context.targetRoot);
        const std::filesystem::path fstabPath = targetRoot / "etc" / "fstab";

        const auto result = CommandRunner::Run({
            "genfstab",
            "-U",
            targetRoot,
        });
        if (result.exitCode != 0)
            throw Utils::CommandFailedException(
                {"genfstab", "-U", targetRoot},
                result.exitCode,
                result.stdErr.empty() ? "<empty>" : result.stdErr
            );

        std::ofstream fstab(fstabPath, std::ios::app);
        if (!fstab)
            throw std::runtime_error("Failed to open fstab for writing");

        fstab << result.stdOut;
    }

}
