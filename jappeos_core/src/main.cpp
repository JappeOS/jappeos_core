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

#include "application.h"

#include <cstring>
#include <iostream>

using JappeStudios::JappeOS::JappeOSCore::Application;

#if defined(JAPPEOS_DAEMON_SESSION)
using JappeStudios::JappeOS::JappeOSCore::SessionTarget;
#endif

static void PrintUsage(const char* argv0)
{
#if defined(JAPPEOS_DAEMON_SESSION)
    std::cerr << "Usage: " << argv0 << " [-t desktop|greeter]\n";
#else
    std::cerr << "Usage: " << argv0 << "\n";
#endif
}

int main(const int argc, char** argv)
{
    Application app;

#if defined(JAPPEOS_DAEMON_SESSION)
    SessionTarget target = SessionTarget::Desktop;
    for (int i = 1; i < argc; i++)
    {
        const char* arg = argv[i];
        if (std::strcmp(arg, "-t") == 0 || std::strcmp(arg, "--type") == 0)
        {
            if (i + 1 >= argc)
            {
                PrintUsage(argv[0]);
                return 2;
            }

            const char* value = argv[++i];
            if (std::strcmp(value, "desktop") == 0)      target = SessionTarget::Desktop;
            else if (std::strcmp(value, "greeter") == 0) target = SessionTarget::Greeter;
            else
            {
                std::cerr << "Invalid value for -t/--type: " << value << "\n";
                PrintUsage(argv[0]);
                return 2;
            }
            continue;
        }

        std::cerr << "Unknown argument: " << arg << "\n";
        PrintUsage(argv[0]);
        return 2;
    }

    app.SetSessionTarget(target);
#else
    for (int i = 1; i < argc; i++)
    {
        const char* arg = argv[i];
        if (std::strcmp(arg, "-t") == 0 || std::strcmp(arg, "--type") == 0)
        {
            std::cerr << "Error: -t/--type is only supported by the jappeos_session build.\n";
            PrintUsage(argv[0]);
            return 2;
        }
        std::cerr << "Unknown argument: " << arg << "\n";
        PrintUsage(argv[0]);
        return 2;
    }
#endif

    return app.Run();
}
