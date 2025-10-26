#pragma once

#include <cstring>
#include <format>

#include "../service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Logger
{
    class LoggerService : public Service
    {
    public:
        explicit LoggerService(ServiceManager* serviceManager, DBusConnection* conn) : Service(serviceManager, conn) {}

        virtual void Emerg(const std::string& str) = 0;
        virtual void Alert(const std::string& str) = 0;
        virtual void Crit(const std::string& str) = 0;
        virtual void Err(const std::string& str) = 0;
        virtual void Warn(const std::string& str) = 0;
        virtual void Notice(const std::string& str) = 0;
        virtual void Info(const std::string& str) = 0;
        virtual void Debug(const std::string& str) = 0;

        /*void InitDBus(DBusConnection* connection, DBusError* error) override
        {
            dbus_bus_add_match(connection, std::format("type='method_call',interface='{}'", GetFullInterfaceName(this)).c_str(), error);
        }*/

        // TODO: More error/safety checking
        bool HandleMethodCall(DBusMessage* msg) override
        {
            const char* member = dbus_message_get_member(msg);
            if (strcmp(member, "Log") != 0)
            {
                return false;
            }

            DBusMessageIter args;
            if (!dbus_message_iter_init(msg, &args))
            {
                SendErrorReply(_conn, msg, "Invalid arguments");
                return true;
            }

            // Extract log level
            if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
            {
                SendErrorReply(_conn, msg, "Expected log level as first argument");
                return true;
            }
            const char* level;
            dbus_message_iter_get_basic(&args, &level);
            dbus_message_iter_next(&args);

            // Extract log message
            if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
            {
                SendErrorReply(_conn, msg, "Expected message string as second argument");
                return true;
            }
            const char* message;
            dbus_message_iter_get_basic(&args, &message);

            const std::string msgStr(message);
            const std::string lvlStr(level);

            // Dispatch to correct log level handler
            // TODO: SWITCH STMT
            if (lvlStr == "emerg") Emerg(msgStr);
            else if (lvlStr == "alert") Alert(msgStr);
            else if (lvlStr == "crit") Crit(msgStr);
            else if (lvlStr == "err") Err(msgStr);
            else if (lvlStr == "warn") Warn(msgStr);
            else if (lvlStr == "notice") Notice(msgStr);
            else if (lvlStr == "info") Info(msgStr);
            else if (lvlStr == "debug") Debug(msgStr);
            else
            {
                SendErrorReply(_conn, msg, "Unknown log level: " + lvlStr);
                return true;
            }

            // Send success reply
            SendSuccessReply(_conn, msg);
            return true;
        }

        std::string GetName() override { return "LoggerService"; }

    private:
        static void SendErrorReply(DBusConnection* conn, DBusMessage* msg, const std::string& errorMsg)
        {
            DBusMessage* error = dbus_message_new_error(
                msg,
                DBUS_ERROR_INVALID_ARGS,
                errorMsg.c_str()
            );
            dbus_connection_send(conn, error, nullptr);
            dbus_connection_flush(conn);
            dbus_message_unref(error);
        }
    };
}