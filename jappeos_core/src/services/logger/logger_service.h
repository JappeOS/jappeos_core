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

#include <cstring>
#include <format>

#include "../service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Logger
{
    class LoggerService : public Service
    {
    public:
        explicit LoggerService(ServiceManager* serviceManager, Connection* conn) : Service(serviceManager, conn) {}

        virtual void Emerg (const std::string& str) = 0;
        virtual void Alert (const std::string& str) = 0;
        virtual void Crit  (const std::string& str) = 0;
        virtual void Err   (const std::string& str) = 0;
        virtual void Warn  (const std::string& str) = 0;
        virtual void Notice(const std::string& str) = 0;
        virtual void Info  (const std::string& str) = 0;
        virtual void Debug (const std::string& str) = 0;

        bool HandleMethodCallLegacy(DBusMessage* msg) override
        {
            if (!msg)
                return false;

            if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
                return false;

            const char* member = dbus_message_get_member(msg);
            if (!member)
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Missing method name"
                );
                return true;
            }

            if (strcmp(member, "Log") != 0)
                return false;

            DBusMessageIter args;
            if (!dbus_message_iter_init(msg, &args))
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Expected arguments (log level, message)"
                );
                return true;
            }

            // Arg 1: log level
            if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "First argument must be a string log level"
                );
                return true;
            }

            const char* level = nullptr;
            dbus_message_iter_get_basic(&args, &level);

            if (!level)
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Log level string is null"
                );
                return true;
            }

            if (!dbus_message_iter_next(&args))
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Missing message argument"
                );
                return true;
            }

            // Arg 2: log message
            if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Second argument must be a message string"
                );
                return true;
            }

            const char* message = nullptr;
            dbus_message_iter_get_basic(&args, &message);

            if (!message)
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Message string is null"
                );
                return true;
            }

            // Disallow extra args
            if (dbus_message_iter_next(&args))
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Too many arguments"
                );
                return true;
            }

            // Size limit
            constexpr size_t kMaxLogLen = 4096;
            if (strlen(level) > 32 || strlen(message) > kMaxLogLen)
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_LIMITS_EXCEEDED,
                    "Log level or message too long"
                );
                return true;
            }

            const std::string lvlStr(level);
            const std::string msgStr(message);

            // Dispatch log level
            if      (lvlStr == "emerg")  Emerg(msgStr);
            else if (lvlStr == "alert")  Alert(msgStr);
            else if (lvlStr == "crit")   Crit(msgStr);
            else if (lvlStr == "err")    Err(msgStr);
            else if (lvlStr == "warn")   Warn(msgStr);
            else if (lvlStr == "notice") Notice(msgStr);
            else if (lvlStr == "info")   Info(msgStr);
            else if (lvlStr == "debug")  Debug(msgStr);
            else
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Unknown log level: " + lvlStr
                );
                return true;
            }

            SendSuccessReplyAndLogLegacy(_rawConn, msg);
            return true;
        }

        [[nodiscard]] std::string GetName() const override { return "LoggerService"; }
    };
}
