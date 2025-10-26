#pragma once
#include <cstdint>
#include <stdexcept>
#include <dbus/dbus.h>

namespace JappeStudios::JappeOS::JappeOSCore::Utils
{
    class DBusUtils
    {
    public:
        static pid_t GetSenderPID(DBusConnection *conn, DBusMessage *msg)
        {
            const char *sender = dbus_message_get_sender(msg);
            if (!sender)
                return 0; // No sender info (e.g., message from bus itself)

            // Create a method call to org.freedesktop.DBus
            DBusMessage *method = dbus_message_new_method_call(
                "org.freedesktop.DBus",       // destination (the bus daemon)
                "/org/freedesktop/DBus",      // object path
                "org.freedesktop.DBus",       // interface
                "GetConnectionUnixProcessID"  // method name
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
            DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, method, -1, &error);
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
