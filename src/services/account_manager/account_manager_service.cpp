#include "account_manager_service.h"
#include "../../utils/dbus_utils.h"
#include "../logger/logger_service.h"
#include "../session_manager/session_manager_service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::AccountManager
{

    AccountManagerService::AccountManagerService(ServiceManager* serviceManager, DBusConnection* conn) : Service(serviceManager, conn)
    {

    }

    AccountManagerService::~AccountManagerService()
    {

    }

    bool AccountManagerService::HandleMethodCall(DBusMessage* msg)
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

        const auto sessionMgr = _serviceManager->Get<SessionManager::SessionManagerService>();
        if (!sessionMgr->IsManagedUserSession(senderUid))
        {
            _serviceManager->Get<Logger::LoggerService>()->Err("PowerOff message sent from unknown user");
            SendErrorReply(_conn, msg, DBUS_ERROR_ACCESS_DENIED, "Unknown user");
            return true;
        }

        if (!sessionMgr->IsPrivilegedClientProcess(senderPid, true))
        {
            _serviceManager->Get<Logger::LoggerService>()->Err("PowerOff message sent from unauthorized process");
            SendErrorReply(_conn, msg, DBUS_ERROR_ACCESS_DENIED, "Unauthorized process");
            return true;
        }

        // Dispatch based on method name
        if (strcmp(member, "AddUser") == 0)
        {
            SendErrorReply(_conn, msg, DBUS_ERROR_UNKNOWN_METHOD, "Not implemented");
            return true;

            DBusMessageIter iter;
            if (dbus_message_iter_init(msg, &iter))
            {
                SendErrorReply(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Shutdown expects no arguments");
                return true;
            }

            //Shutdown(msg, senderUid, senderPid);
            return true;
        }
        else if (strcmp(member, "RemoveUser") == 0)
        {
            SendErrorReply(_conn, msg, DBUS_ERROR_UNKNOWN_METHOD, "Not implemented");
            return true;

            DBusMessageIter iter;
            if (dbus_message_iter_init(msg, &iter))
            {
                SendErrorReply(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Reboot expects no arguments");
                return true;
            }

            //Reboot(msg, senderUid, senderPid);
            return true;
        }
        else if (strcmp(member, "ListUsers") == 0)
        {
            DBusMessageIter iter;
            if (dbus_message_iter_init(msg, &iter))
            {
                SendErrorReply(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Suspend expects no arguments");
                return true;
            }

            ListUsers(msg);
            return true;
        }
        else if (strcmp(member, "GetUserProperty") == 0)
        {
            DBusMessageIter args;
            if (!dbus_message_iter_init(msg, &args))
            {
                _serviceManager->Get<Logger::LoggerService>()->Err("GetUserProperty called without arguments");
                SendErrorReply(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Expected arguments: userObject, property");
                return true;
            }

            if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
            {
                SendErrorReply(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Expected string userObject");
                return true;
            }
            const char* userObject = nullptr;
            dbus_message_iter_get_basic(&args, &userObject);

            if (!dbus_message_iter_next(&args) ||
                dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
            {
                SendErrorReply(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Expected string property");
                return true;
            }
            const char* property = nullptr;
            dbus_message_iter_get_basic(&args, &property);

            if (!userObject || !property)
            {
                SendErrorReply(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Null argument(s)");
                return true;
            }

            GetUserProperty(msg, userObject, property);
            return true;
        }

        _serviceManager->Get<Logger::LoggerService>()->Warn(std::string("Unknown DBus method called: ") + member);
        SendErrorReply(_conn, msg, DBUS_ERROR_UNKNOWN_METHOD, "Unknown method");
        return false;
    }

    void AccountManagerService::AddUser(DBusMessage* pmsg, uid_t senderUid, pid_t senderPid)
    {

    }

    void AccountManagerService::RemoveUser(DBusMessage* pmsg, uid_t senderUid, pid_t senderPid)
    {

    }

    void AccountManagerService::ListUsers(DBusMessage* pmsg) const
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        DBusError err;
        dbus_error_init(&err);

        DBusMessage* msg = dbus_message_new_method_call(
            "org.freedesktop.Accounts",
            "/org/freedesktop/Accounts",
            "org.freedesktop.Accounts",
            "ListCachedUsers"
        );

        if (!msg)
        {
            const auto errmsg = "Failed to allocate D-Bus message for ListCachedUsers";
            logger->Err(errmsg);
            SendErrorReply(_conn, pmsg, DBUS_ERROR_NO_MEMORY, errmsg);
            return;
        }

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(_conn, msg, DBUS_DEFAULT_SAFE_TIMEOUT, &err);
        dbus_message_unref(msg);

        if (!reply || dbus_error_is_set(&err))
        {
            const auto errmsg = "D-Bus call failed: " + std::string(err.message ? err.message : "unknown");
            logger->Err(errmsg);
            dbus_error_free(&err);
            SendErrorReply(_conn, pmsg, DBUS_ERROR_FAILED, errmsg);
            return;
        }

        DBusMessageIter args, arrayIter;
        dbus_message_iter_init(reply, &args);
        dbus_message_iter_recurse(&args, &arrayIter);

        std::vector<std::string> userObjects;

        while (dbus_message_iter_get_arg_type(&arrayIter) == DBUS_TYPE_OBJECT_PATH)
        {
            const char* obj_path;
            dbus_message_iter_get_basic(&arrayIter, &obj_path);
            userObjects.emplace_back(obj_path);
            dbus_message_iter_next(&arrayIter);
        }

        dbus_message_unref(reply);
        dbus_error_free(&err);

        // Create the reply message to send back to the caller
        DBusMessage* replyMsg = dbus_message_new_method_return(pmsg);
        if (!replyMsg)
        {
            const auto errmsg = "Failed to allocate reply message";
            logger->Err(errmsg);
            SendErrorReply(_conn, pmsg, DBUS_ERROR_NO_MEMORY, errmsg);
            return;
        }

        DBusMessageIter iter;
        dbus_message_iter_init_append(replyMsg, &iter);

        // Append an array of strings
        DBusMessageIter arrayIterOut;
        if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "o", &arrayIterOut))
        {
            const auto errmsg = "Failed to open array container for reply";
            logger->Err(errmsg);
            dbus_message_unref(replyMsg);
            SendErrorReply(_conn, pmsg, DBUS_ERROR_NO_MEMORY, errmsg);
            return;
        }

        for (const auto& userPath : userObjects)
        {
            const char* str = userPath.c_str();
            dbus_message_iter_append_basic(&arrayIterOut, DBUS_TYPE_OBJECT_PATH, &str);
        }

        dbus_message_iter_close_container(&iter, &arrayIterOut);

        // Send the reply back to the caller
        if (!dbus_connection_send(_conn, replyMsg, nullptr))
        {
            const auto errmsg = "Failed to send reply message";
            logger->Err(errmsg);
            dbus_message_unref(replyMsg);
            SendErrorReply(_conn, pmsg, DBUS_ERROR_FAILED, errmsg);
            return;
        }

        dbus_connection_flush(_conn);
        dbus_message_unref(replyMsg);
    }

    void AccountManagerService::GetUserProperty(DBusMessage* pmsg,
                                            const std::string& userObject,
                                            const std::string& property) const
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        DBusError err;
        dbus_error_init(&err);

        DBusMessage* msg = dbus_message_new_method_call(
            "org.freedesktop.Accounts",            // destination bus name
            userObject.c_str(),                    // object path (e.g. /org/freedesktop/Accounts/User1000)
            "org.freedesktop.DBus.Properties",     // interface
            "Get"                                  // method
        );

        if (!msg)
        {
            const auto errmsg = "Failed to allocate D-Bus message for GetUserProperty";
            logger->Err(errmsg);
            SendErrorReply(_conn, pmsg, DBUS_ERROR_NO_MEMORY, errmsg);
            return;
        }

        // Append params: (s interface_name, s property_name)
        const char* iface = "org.freedesktop.Accounts.User";
        const char* prop  = property.c_str();

        if (!dbus_message_append_args(msg,
                                      DBUS_TYPE_STRING, &iface,
                                      DBUS_TYPE_STRING, &prop,
                                      DBUS_TYPE_INVALID))
        {
            dbus_message_unref(msg);
            const auto errmsg = "Failed to append arguments to D-Bus message";
            logger->Err(errmsg);
            SendErrorReply(_conn, pmsg, DBUS_ERROR_NO_MEMORY, errmsg);
            return;
        }

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(_conn, msg, 5000, &err);
        dbus_message_unref(msg);

        if (!reply || dbus_error_is_set(&err))
        {
            const auto errmsg = "D-Bus Get call failed: " +
                                std::string(err.message ? err.message : "unknown");
            logger->Err(errmsg);
            dbus_error_free(&err);
            SendErrorReply(_conn, pmsg, DBUS_ERROR_FAILED, errmsg);
            if (reply)
                dbus_message_unref(reply);
            return;
        }

        // Extract the returned variant (the first argument in the reply)
        DBusMessageIter replyIter;
        if (!dbus_message_iter_init(reply, &replyIter))
        {
            const auto errmsg = "D-Bus reply has no arguments";
            logger->Err(errmsg);
            dbus_message_unref(reply);
            SendErrorReply(_conn, pmsg, DBUS_ERROR_INVALID_ARGS, errmsg);
            return;
        }

        if (dbus_message_iter_get_arg_type(&replyIter) != DBUS_TYPE_VARIANT)
        {
            const auto errmsg = "Expected a variant in D-Bus reply";
            logger->Err(errmsg);
            dbus_message_unref(reply);
            SendErrorReply(_conn, pmsg, DBUS_ERROR_INVALID_SIGNATURE, errmsg);
            return;
        }

        // Recurse into the variant
        DBusMessageIter variantIter;
        dbus_message_iter_recurse(&replyIter, &variantIter);
        int innerType = dbus_message_iter_get_arg_type(&variantIter);
        char innerSig[2] = { static_cast<char>(innerType), '\0' };

        // Create a new reply to our own caller
        DBusMessage* replyMsg = dbus_message_new_method_return(pmsg);
        if (!replyMsg)
        {
            const auto errmsg = "Failed to allocate reply message";
            logger->Err(errmsg);
            dbus_message_unref(reply);
            SendErrorReply(_conn, pmsg, DBUS_ERROR_NO_MEMORY, errmsg);
            return;
        }

        DBusMessageIter outIter;
        dbus_message_iter_init_append(replyMsg, &outIter);

        // Rewrap the variant and copy the basic data inside
        DBusMessageIter variantOut;
        if (!dbus_message_iter_open_container(&outIter, DBUS_TYPE_VARIANT, innerSig, &variantOut))
        {
            const auto errmsg = "Failed to open variant container for reply";
            logger->Err(errmsg);
            dbus_message_unref(replyMsg);
            dbus_message_unref(reply);
            SendErrorReply(_conn, pmsg, DBUS_ERROR_NO_MEMORY, errmsg);
            return;
        }

        if (dbus_type_is_basic(innerType))
        {
            const void* data;
            dbus_message_iter_get_basic(&variantIter, &data);
            dbus_message_iter_append_basic(&variantOut, innerType, &data);
        }
        else
        {
            const auto errmsg = "Non-basic variant type not supported in forwarding";
            logger->Warn(errmsg);
        }

        dbus_message_iter_close_container(&outIter, &variantOut);

        // Send the reply
        if (!dbus_connection_send(_conn, replyMsg, nullptr))
        {
            const auto errmsg = "Failed to send reply message";
            logger->Err(errmsg);
            dbus_message_unref(replyMsg);
            dbus_message_unref(reply);
            SendErrorReply(_conn, pmsg, DBUS_ERROR_FAILED, errmsg);
            return;
        }

        dbus_connection_flush(_conn);

        dbus_message_unref(replyMsg);
        dbus_message_unref(reply);
        dbus_error_free(&err);
    }

}
