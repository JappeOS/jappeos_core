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

#include "logger.h"

#include "application.h"

namespace JappeStudios::JappeOS::JappeOSCore
{

    Logger::Logger(std::string name) : _name(std::move(name))
    {
        _loggerService = Application::GetInstance()->GetServiceManager()->Get<Services::Logger::LoggerService>();
    }

    Logger::~Logger() = default;

    void Logger::Emerg (const std::string& str) { _loggerService->Emerg(GetMessage(str)); }
    void Logger::Alert (const std::string& str) { _loggerService->Alert(GetMessage(str)); }
    void Logger::Crit  (const std::string& str) { _loggerService->Crit(GetMessage(str)); }
    void Logger::Err   (const std::string& str) { _loggerService->Err(GetMessage(str)); }
    void Logger::Warn  (const std::string& str) { _loggerService->Warn(GetMessage(str)); }
    void Logger::Notice(const std::string& str) { _loggerService->Notice(GetMessage(str)); }
    void Logger::Info  (const std::string& str) { _loggerService->Info(GetMessage(str)); }
    void Logger::Debug (const std::string& str) { _loggerService->Debug(GetMessage(str)); }

    std::string Logger::GetMessage(const std::string& str) { return std::format("[{}]: {}", _name, str); }

}
