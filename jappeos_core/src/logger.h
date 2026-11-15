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
#include <string>

#include "services/logger/logger_service.h"

namespace JappeStudios::JappeOS::JappeOSCore
{
    class Logger
    {
    public:
        explicit Logger(std::string name);
        ~Logger();

        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

        Logger(Logger&&) = delete;
        Logger& operator=(Logger&&) = delete;

        void Emerg (const std::string& str);
        void Alert (const std::string& str);
        void Crit  (const std::string& str);
        void Err   (const std::string& str);
        void Warn  (const std::string& str);
        void Notice(const std::string& str);
        void Info  (const std::string& str);
        void Debug (const std::string& str);

    private:
        Services::Logger::LoggerService* _loggerService;
        std::string _name;

        std::string GetMessage(const std::string& str);
    };
}
