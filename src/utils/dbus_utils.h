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
#include <dbus/dbus.h>

namespace JappeStudios::JappeOS::JappeOSCore::Utils
{
    class DBusUtils
    {
    public:
        /**
         * @param conn A D-Bus connection
         * @param msg The target message tp get the PID from
         * @return The process ID that the message was sent from
         */
        static pid_t GetSenderPID(DBusConnection* conn, DBusMessage* msg)
        {
            const char *sender = dbus_message_get_sender(msg);
            if (!sender)
                return 0; // No sender info (e.g., message from bus itself)

            DBusMessage* method = dbus_message_new_method_call(
                "org.freedesktop.DBus",
                "/org/freedesktop/DBus",
                "org.freedesktop.DBus",
                "GetConnectionUnixProcessID"
            );

            if (!method)
                return 0;

            // Append the sender name
            if (!dbus_message_append_args(method,
                                          DBUS_TYPE_STRING, &sender,
                                          DBUS_TYPE_INVALID))
            {
                dbus_message_unref(method);
                return 0;
            }

            // Send the method call and wait for a reply
            DBusError error;
            dbus_error_init(&error);
            DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, method, -1, &error);
            dbus_message_unref(method);

            if (dbus_error_is_set(&error))
            {
                dbus_error_free(&error);
                throw std::runtime_error(std::string("D-Bus error: ") + error.name + " - " + error.message);
            }

            // Extract the uint32 PID
            std::uint32_t pid = 0;
            if (!dbus_message_get_args(reply, &error,
                                       DBUS_TYPE_UINT32, &pid,
                                       DBUS_TYPE_INVALID))
            {
                dbus_error_free(&error);
                dbus_message_unref(reply);
                throw std::runtime_error(std::string("Failed to parse PID: ") + error.message);
            }

            dbus_message_unref(reply);
            return static_cast<pid_t>(pid);
        }
    };
}
