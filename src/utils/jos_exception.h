/*
 * jappeos_core, Core system management daemon for JappeOS.
 * Copyright (C) 2026  Jappe02
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
#include <stdexcept>
#include <string>

#include "../application.h"

namespace JappeStudios::JappeOS::JappeOSCore::Utils
{
    class JosException final : std::runtime_error
    {
    public:
        explicit JosException(const std::string& shortErrorCode, const std::string& msg) : std::runtime_error(msg)
        {
            auto newErrCode = shortErrorCode;
            for (auto & c: newErrCode) c = toupper(c);
            std::erase(newErrCode, ' ');
            _shortErrorCode = newErrCode;

            _stacktrace = Application::PrintStackTrace();
        }

        [[nodiscard]] const std::string& ShortErrorCode() const noexcept { return _shortErrorCode; }
        [[nodiscard]] const std::string& Stacktrace() const noexcept { return _stacktrace; }

    private:
        std::string _shortErrorCode;
        std::string _stacktrace;
    };
}
