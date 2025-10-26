#include "power_manager_service.h"

#include "../../utils/dbus_utils.h"
#include "../logger/logger_service.h"
#include "../session_manager/session_manager_service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::PowerManager
{

    PowerManagerService::PowerManagerService(ServiceManager* serviceManager, DBusConnection* conn) : Service(serviceManager, conn)
    {

    }

    PowerManagerService::~PowerManagerService()
    {

    }

    bool PowerManagerService::HandleMethodCall(DBusMessage* msg)
    {
        if (!msg)
        {
            _serviceManager->Get<Logger::LoggerService>()->Err("HandleMethodCall called with null parameters");
            return false;
        }

        const char* sender = dbus_message_get_sender(msg);
        if (!sender)
        {
            _serviceManager->Get<Logger::LoggerService>()->Warn("DBus message without sender");
            SendErrorReply(_conn, msg, DBUS_ERROR_ACCESS_DENIED, "No sender");
            return true;
        }

        DBusError error;
        dbus_error_init(&error);
        uid_t senderUid = dbus_bus_get_unix_user(_conn, sender, &error);
        if (dbus_error_is_set(&error))
        {
            _serviceManager->Get<Logger::LoggerService>()->Err(
            std::string("Failed to get UID for sender ") + sender + ": " + (error.message ? error.message : "unknown"));
            dbus_error_free(&error);
            SendErrorReply(_conn, msg, DBUS_ERROR_ACCESS_DENIED, "Could not get UID");
            return true;
        }

        const pid_t senderPid = Utils::DBusUtils::GetSenderPID(_conn, msg);

        const char* member = dbus_message_get_member(msg);
        if (!member)
        {
            _serviceManager->Get<Logger::LoggerService>()->Warn("DBus message without member field");
            SendErrorReply(_conn, msg, DBUS_ERROR_UNKNOWN_METHOD, "No method specified");
            return true;
        }

        // Dispatch based on method name
        if (strcmp(member, "Shutdown") == 0)
        {
            if (dbus_message_iter_init(msg, nullptr))
            {
                SendErrorReply(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Shutdown expects no arguments");
                return true;
            }

            Shutdown(msg, senderUid, senderPid);
            return true;
        }
        else if (strcmp(member, "Reboot") == 0)
        {
            if (dbus_message_iter_init(msg, nullptr))
            {
                SendErrorReply(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Reboot expects no arguments");
                return true;
            }

            Reboot(msg, senderUid, senderPid);
            return true;
        }
        else if (strcmp(member, "Suspend") == 0)
        {
            if (dbus_message_iter_init(msg, nullptr))
            {
                SendErrorReply(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Suspend expects no arguments");
                return true;
            }

            Suspend(msg, senderUid, senderPid);
            return true;
        }

        _serviceManager->Get<Logger::LoggerService>()->Warn(std::string("Unknown DBus method called: ") + member);
        SendErrorReply(_conn, msg, DBUS_ERROR_UNKNOWN_METHOD, "Unknown method");
        return false;
    }

    // TODO: Auth if needed
    void PowerManagerService::Shutdown(DBusMessage* pmsg, const uid_t senderUid, const pid_t senderPid) const
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();
        const auto sessionMgr = _serviceManager->Get<SessionManager::SessionManagerService>();

        if (!sessionMgr->IsManagedUserSession(senderUid))
        {
            logger->Err("PowerOff message sent from unknown user");
            SendErrorReply(_conn, pmsg, DBUS_ERROR_ACCESS_DENIED, "Unknown user");
            return;
        }

        if (!sessionMgr->IsPrivilegedClientProcess(senderPid))
        {
            logger->Err("PowerOff message sent from unauthorized process");
            SendErrorReply(_conn, pmsg, DBUS_ERROR_ACCESS_DENIED, "Unauthorized process");
            return;
        }

        DBusError err;
        dbus_error_init(&err);

        DBusMessage* msg = dbus_message_new_method_call(
            "org.freedesktop.login1",
            "/org/freedesktop/login1",
            "org.freedesktop.login1.Manager",
            "PowerOff"
        );

        if (!msg)
        {
            const auto errmsg = "Failed to allocate D-Bus message for PowerOff";
            logger->Err(errmsg);
            SendErrorReply(_conn, pmsg, DBUS_ERROR_NO_MEMORY, errmsg);
            return;
        }

        if (!dbus_message_append_args(msg,
                                      DBUS_TYPE_BOOLEAN, false,
                                      DBUS_TYPE_INVALID))
        {
            const auto errmsg = "Failed to append arguments to PowerOff message";
            logger->Err(errmsg);
            dbus_message_unref(msg);
            SendErrorReply(_conn, pmsg, DBUS_ERROR_FAILED, errmsg);
            return;
        }

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(_conn, msg, -1, &err);
        dbus_message_unref(msg);

        if (!reply)
        {
            if (dbus_error_is_set(&err))
            {
                const auto errmsg = "PowerOff failed: " + std::string(err.message ? err.message : "unknown");
                logger->Err(errmsg);
                dbus_error_free(&err);
                SendErrorReply(_conn, pmsg, DBUS_ERROR_FAILED, errmsg);
            }
            else
            {
                const auto errmsg = "PowerOff returned no reply";
                logger->Err(errmsg);
                SendErrorReply(_conn, pmsg, DBUS_ERROR_FAILED, errmsg);
            }

            return;
        }

        dbus_message_unref(reply);
        logger->Info("Successfully sent PowerOff message");
    }

    // TODO: Auth if needed
    void PowerManagerService::Reboot(DBusMessage* msg, uid_t senderUid, pid_t senderPid)
    {

    }

    // TODO: Auth if needed
    void PowerManagerService::Suspend(DBusMessage* msg, uid_t senderUid, pid_t senderPid)
    {

    }

}
