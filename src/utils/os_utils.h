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
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace JappeStudios::JappeOS::JappeOSCore::Utils
{
    class OsUtils final
    {
    public:
        OsUtils() = delete;

        /**
         * @return Whether the system is a live or installation/repair environment
         */
        static bool IsLiveOrInstallationEnvironment()
        {
            return IsArchIso();
        }

    private:
        static bool IsArchIso()
        {
            std::ifstream file("/proc/cmdline");
            std::string cmdline;
            std::getline(file, cmdline);

            if (cmdline.find("archiso") != std::string::npos)
                return true;

            if (std::filesystem::exists("/run/archiso"))
                return true;

            return false;
        }
    };
}
