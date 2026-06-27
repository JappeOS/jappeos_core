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

#include "configure_system_step.h"

#include <fstream>

#include "../../../utils/command_runner.h"
#include "../init_boot_file.h"
#include "../installer_def.h"

using JappeStudios::JappeOS::JappeOSCore::Utils::CommandRunner;

namespace JappeStudios::JappeOS::JappeOSCore::Services::Installer::Steps
{

    void ConfigureSystemStep::Execute(InstallContext& context)
    {
        if (context.targetRoot.empty())
            throw std::runtime_error("Cannot configure system with empty target root path");

        // TODO: Maybe have some fs-overlay thing so that installed system has different packages/files from live system.

        const std::filesystem::path targetRoot(context.targetRoot);
        const std::filesystem::path initFilePath(targetRoot / STORAGE_SYSTEM_FILE_INIT_BOOT_PATH);

        auto writer = InitBootFile::Writer(initFilePath);
        writer.WriteLocale(context.data.locale);
        writer.WriteTimezone(context.data.timezone);
        writer.WriteKeyboardLayout(std::get<0>(context.data.keyboardLayout));
        writer.WriteKeyboardLayoutVariant(std::get<1>(context.data.keyboardLayout));
        writer.WriteHostname(context.data.hostname);
        writer.WriteUsername(context.data.username);
        writer.WritePassword(context.data.password);
        writer.Close();
    }

}
