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

#include "validate_install_step.h"

#include <stdexcept>

namespace JappeStudios::JappeOS::JappeOSCore::Services::Installer::Steps
{

    void ValidateInstallStep::Execute(InstallContext& context)
    {
        const std::string path = "/rootfs.tar.zst";

        if (FILE *file = fopen(path.c_str(), "r"))
        {
            fclose(file);
        }
        else
        {
            throw std::runtime_error("Could not find file `" + path + "` required for install.");
        }

        context.rootFsImagePath = path;
    }

}
