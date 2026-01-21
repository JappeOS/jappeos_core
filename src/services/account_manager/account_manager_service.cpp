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

#include "account_manager_service.h"
#include "../../utils/dbus_utils.h"
#include "../logger/logger_service.h"
#include "../session_manager/session_manager_service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::AccountManager
{

    AccountManagerService::AccountManagerService(ServiceManager* serviceManager,
                                                 Connection* conn) :
                                                 Service(serviceManager, conn)
    {}

    AccountManagerService::~AccountManagerService() = default;

    bool AccountManagerService::HandleMethodCallLegacy(DBusMessage* msg)
    {
        const char* sender = dbus_message_get_sender(msg);
        if (!sender)
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_ACCESS_DENIED,
                "No sender"
            );
            return true;
        }

        DBusError error;
        dbus_error_init(&error);
        const uid_t senderUid = dbus_bus_get_unix_user(_rawConn, sender, &error);
        if (dbus_error_is_set(&error))
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_ACCESS_DENIED,
                std::string("Failed to get UID for sender ")
                    + sender + ": "
                    + (error.message ? error.message : "unknown")
            );
            dbus_error_free(&error);
            return true;
        }

        const pid_t senderPid = Utils::DBusUtils::GetSenderPID(_rawConn, msg);

        const char* member = dbus_message_get_member(msg);
        if (!member)
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_UNKNOWN_METHOD,
                "No method specified"
            );
            return true;
        }

        const auto sessionMgr = _serviceManager->Get<SessionManager::SessionManagerService>();
        if (!sessionMgr->IsManagedUserSession(senderUid))
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_ACCESS_DENIED,
                "Unknown user"
            );
            return true;
        }

        // TODO: Make sure to not allow child processes in the future.
        if (!sessionMgr->IsPrivilegedClientProcess(senderPid, true))
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_ACCESS_DENIED,
                "Unauthorized process"
            );
            return true;
        }

        // Dispatch based on method name
        if (strcmp(member, "CreateInitialUserWithPassword") == 0)
        {
            DBusMessageIter args;
            if (!dbus_message_iter_init(msg, &args))
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Expected arguments: username, realName, cryptedPassword"
                );
                return true;
            }

            if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Expected string username"
                );
                return true;
            }
            const char* username = nullptr;
            dbus_message_iter_get_basic(&args, &username);

            if (!dbus_message_iter_next(&args) ||
                dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Expected string realName"
                );
                return true;
            }
            const char* realName = nullptr;
            dbus_message_iter_get_basic(&args, &realName);

            if (!dbus_message_iter_next(&args) ||
                dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Expected string cryptedPassword"
                );
                return true;
            }
            const char* cryptedPassword = nullptr;
            dbus_message_iter_get_basic(&args, &cryptedPassword);

            if (!username || !realName || !cryptedPassword)
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Null argument(s)"
                );
                return true;
            }

            CreateInitialUserWithPasswordDbus(
                msg,
                senderUid,
                senderPid,
                username,
                realName,
                cryptedPassword
            );
            return true;
        }
        else if (strcmp(member, "AddUser") == 0)
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_UNKNOWN_METHOD,
                "Not implemented"
            );

            return true;
        }
        else if (strcmp(member, "RemoveUser") == 0)
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_UNKNOWN_METHOD,
                "Not implemented"
            );

            return true;
        }
        else if (strcmp(member, "ListUsers") == 0)
        {
            DBusMessageIter iter;
            if (dbus_message_iter_init(msg, &iter))
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Suspend expects no arguments"
                );
                return true;
            }

            ListUsersDbus(msg);
            return true;
        }
        else if (strcmp(member, "GetUserProperty") == 0)
        {
            DBusMessageIter args;
            if (!dbus_message_iter_init(msg, &args))
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Expected arguments: userObject, property"
                );
                return true;
            }

            if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_OBJECT_PATH)
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Expected string userObject"
                );
                return true;
            }
            const char* userObject = nullptr;
            dbus_message_iter_get_basic(&args, &userObject);

            if (!dbus_message_iter_next(&args) ||
                dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Expected string property"
                );
                return true;
            }
            const char* property = nullptr;
            dbus_message_iter_get_basic(&args, &property);

            if (!userObject || !property)
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Null argument(s)"
                );
                return true;
            }

            GetUserPropertyDbus(msg, userObject, property);
            return true;
        }

        SendErrorReplyAndLogLegacy(
            _rawConn,
            msg,
            DBUS_ERROR_UNKNOWN_METHOD,
            std::string("Unknown DBus method called: ") + member
        );
        return false;
    }

    void AccountManagerService::CreateInitialUserWithPasswordDbus(DBusMessage* pmsg,
                                                                  uid_t senderUid,
                                                                  pid_t senderPid,
                                                                  const std::string& username,
                                                                  const std::string& realName,
                                                                  const std::string& cryptedPassword)
    {
        if (empty(username) || empty(cryptedPassword))
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                pmsg,
                DBUS_ERROR_INVALID_ARGS,
                "Missing username or password."
            );
            return;
        }

        std::vector<std::string> userPaths;
        if (!ListUsers(userPaths))
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                pmsg,
                DBUS_ERROR_FAILED,
                "Failed to list users. Check logs for more information."
            );
            return;
        }

        if (!userPaths.empty())
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                pmsg,
                DBUS_ERROR_FAILED,
                "Cannot create initial user. There's more than 0 users present."
            );
            return;
        }

        std::string userPath;
        if (!AddUser(username, realName, userPath))
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                pmsg,
                DBUS_ERROR_FAILED,
                "Failed to add user. Check logs for more information."
            );
            return;
        }

        if (!SetUserPassword(userPath, cryptedPassword, ""))
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                pmsg,
                DBUS_ERROR_FAILED,
                "Failed to set user password. Check logs for more information."
            );
            return;
        }

        SendSuccessReplyAndLogLegacy(_rawConn, pmsg, "Initial user successfully created.");
    }

    void AccountManagerService::AddUserDbus(DBusMessage* pmsg,
                                            const uid_t senderUid,
                                            pid_t senderPid,
                                            const std::string& username,
                                            const std::string& realName)
    {
        if (senderUid != 0)
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                pmsg,
                DBUS_ERROR_ACCESS_DENIED,
                "Only root may add users"
            );
        }

        // TODO
    }

    bool AccountManagerService::AddUser(const std::string& username,
                                        const std::string& realName,
                                        std::string& outObjectPath,
                                        bool cache) const
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        if (username.empty())
        {
            logger->Warn("AddUser called with an empty username");
            return false;
        }

        DBusError err;
        dbus_error_init(&err);

        // Create the DBus method call to org.freedesktop.Accounts.CreateUser
        DBusMessage* msg = dbus_message_new_method_call(
            "org.freedesktop.Accounts",              // Service
            "/org/freedesktop/Accounts",             // Path
            "org.freedesktop.Accounts",              // Interface
            "CreateUser"                             // Method
        );

        if (!msg)
        {
            logger->Err("Failed to allocate D-Bus message for CreateUser");
            return false;
        }

        const char* name = username.c_str();
        const char* real = realName.c_str();

        // accountType: 1 = Standard user, 0 = System user
        int32_t accountType = 1;

        if (!dbus_message_append_args(msg,
                                      DBUS_TYPE_STRING, &name,
                                      DBUS_TYPE_STRING, &real,
                                      DBUS_TYPE_INT32, &accountType,
                                      DBUS_TYPE_INVALID))
        {
            logger->Err("Failed to append arguments to CreateUser message for username: " + username);
            dbus_message_unref(msg);
            return false;
        }

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(
            _rawConn,
            msg,
            DBUS_DEFAULT_SAFE_TIMEOUT,
            &err
        );
        dbus_message_unref(msg);

        if (!reply)
        {
            if (dbus_error_is_set(&err))
            {
                logger->Err("CreateUser failed for username '" + username + "': " +
                    std::string(err.message ? err.message : "unknown error"));
                dbus_error_free(&err);
            }
            else
            {
                logger->Err("CreateUser returned no reply for username: " + username);
            }
            return false;
        }

        const char* newUserPath = nullptr;
        if (dbus_message_get_args(reply, &err,
                                  DBUS_TYPE_OBJECT_PATH, &newUserPath,
                                  DBUS_TYPE_INVALID))
        {
            logger->Info("User '" + username + "' created successfully at path: " +
                         (newUserPath ? newUserPath : "<null>"));
            outObjectPath = newUserPath;
        }
        else
        {
            if (dbus_error_is_set(&err))
            {
                logger->Err("Failed to parse CreateUser reply for '" + username +
                            "': " + std::string(err.message ? err.message : "unknown"));
                dbus_error_free(&err);
            }
            dbus_message_unref(reply);
            return false;
        }

        dbus_message_unref(reply);

        if (cache && !CacheUser(username))
        {
            logger->Err("Failed to cache user in CreateUser for: " + username);
            return false;
        }

        return true;
    }

    void AccountManagerService::RemoveUser(DBusMessage* pmsg, uid_t senderUid, pid_t senderPid)
    {
        if (senderUid != 0)
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                pmsg,
                DBUS_ERROR_ACCESS_DENIED,
                "Only root may add users"
            );
        }

        // TODO: Implement RemoveUser
    }

    // TODO: use ListUsers
    void AccountManagerService::ListUsersDbus(DBusMessage* pmsg)
    {
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
            SendErrorReplyAndLogLegacy(
                _rawConn,
                pmsg,
                DBUS_ERROR_NO_MEMORY,
                "Failed to allocate D-Bus message"
            );
            return;
        }

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(
            _rawConn,
            msg,
            DBUS_DEFAULT_SAFE_TIMEOUT,
            &err
        );
        dbus_message_unref(msg);

        if (!reply || dbus_error_is_set(&err))
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                pmsg,
                DBUS_ERROR_FAILED,
                "D-Bus call failed: " + std::string(err.message ? err.message : "unknown")
            );
            dbus_error_free(&err);
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
            SendErrorReplyAndLogLegacy(
                _rawConn,
                pmsg,
                DBUS_ERROR_NO_MEMORY,
                "Failed to allocate reply message"
            );
            return;
        }

        DBusMessageIter iter;
        dbus_message_iter_init_append(replyMsg, &iter);

        DBusMessageIter arrayIterOut;
        if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "o", &arrayIterOut))
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                pmsg,
                DBUS_ERROR_NO_MEMORY,
                "Failed to open array container for reply"
            );
            dbus_message_unref(replyMsg);
            return;
        }

        for (const auto& userPath : userObjects)
        {
            const char* str = userPath.c_str();
            dbus_message_iter_append_basic(&arrayIterOut, DBUS_TYPE_OBJECT_PATH, &str);
        }

        dbus_message_iter_close_container(&iter, &arrayIterOut);

        // Send the reply back to the caller
        if (!dbus_connection_send(_rawConn, replyMsg, nullptr))
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                pmsg,
                DBUS_ERROR_FAILED,
                "Failed to send reply message"
            );
            dbus_message_unref(replyMsg);
            return;
        }

        dbus_message_unref(replyMsg);
    }

    bool AccountManagerService::ListUsers(std::vector<std::string>& outObjectPaths) const
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
            logger->Err("Failed to allocate D-Bus message");
            return false;
        }

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(
            _rawConn,
            msg,
            DBUS_DEFAULT_SAFE_TIMEOUT,
            &err
        );
        dbus_message_unref(msg);

        if (!reply || dbus_error_is_set(&err))
        {
            dbus_error_free(&err);
            logger->Err("D-Bus call failed: " + std::string(err.message ? err.message : "unknown"));
            return false;
        }

        DBusMessageIter args, arrayIter;
        dbus_message_iter_init(reply, &args);
        dbus_message_iter_recurse(&args, &arrayIter);

        while (dbus_message_iter_get_arg_type(&arrayIter) == DBUS_TYPE_OBJECT_PATH)
        {
            const char* obj_path;
            dbus_message_iter_get_basic(&arrayIter, &obj_path);
            outObjectPaths.emplace_back(obj_path);
            dbus_message_iter_next(&arrayIter);
        }

        dbus_message_unref(reply);
        dbus_error_free(&err);
        return true;
    }

    void AccountManagerService::GetUserPropertyDbus(DBusMessage* pmsg,
                                                    const std::string& userObject,
                                                    const std::string& property)
    {
        DBusMessage* reply = dbus_message_new_method_return(pmsg);
        DBusMessageIter iter, variantIter;
        dbus_message_iter_init_append(reply, &iter);

        DBusValue val;
        if (!GetUserProperty(userObject, property, val))
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                pmsg,
                DBUS_ERROR_FAILED,
                "Failed to get user property"
            );
        }

        dbus_message_iter_open_container(
            &iter,
            DBUS_TYPE_VARIANT,
            val.signature.c_str(),
            &variantIter
        );

        std::visit([&]<typename T0>(T0&& v)
        {
            using V = std::decay_t<T0>;
            if constexpr (std::is_same_v<V, std::string>)
            {
                const char* s = v.c_str();
                dbus_message_iter_append_basic(&variantIter, DBUS_TYPE_STRING, &s);
            }
            else if constexpr (std::is_same_v<V, int32_t>)
            {
                dbus_message_iter_append_basic(&variantIter, DBUS_TYPE_INT32, &v);
            }
            else if constexpr (std::is_same_v<V, uint32_t>)
            {
                dbus_message_iter_append_basic(&variantIter, DBUS_TYPE_UINT32, &v);
            }
            else if constexpr (std::is_same_v<V, bool>)
            {
                dbus_message_iter_append_basic(&variantIter, DBUS_TYPE_BOOLEAN, &v);
            }
            else if constexpr (std::is_same_v<V, double>)
            {
                dbus_message_iter_append_basic(&variantIter, DBUS_TYPE_DOUBLE, &v);
            }
            else
            {
                const std::string msg = "Unsupported variant type in GetUserPropertyDbus";
                SendErrorReplyAndLogLegacy(_rawConn, pmsg, DBUS_ERROR_FAILED, msg);
            }
        }, val.value);

        dbus_message_iter_close_container(&iter, &variantIter);
        dbus_connection_send(_rawConn, reply, nullptr);
        dbus_message_unref(reply);
    }

    bool AccountManagerService::GetUserProperty(const std::string& userObject,
                                                const std::string& property,
                                                DBusValue& outValue) const
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        DBusError err;
        dbus_error_init(&err);

        DBusMessage* msg = dbus_message_new_method_call(
            "org.freedesktop.Accounts",
            userObject.c_str(),
            "org.freedesktop.DBus.Properties",
            "Get"
        );

        if (!msg)
        {
            logger->Err("Failed to allocate D-Bus message");
            return false;
        }

        const char* interface = "org.freedesktop.Accounts.User";
        const char* prop = property.c_str();

        if (!dbus_message_append_args(msg,
                                      DBUS_TYPE_STRING, &interface,
                                      DBUS_TYPE_STRING, &prop,
                                      DBUS_TYPE_INVALID))
        {
            dbus_message_unref(msg);
            logger->Err("Failed to append arguments to D-Bus message");
            return false;
        }

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(
            _rawConn,
            msg,
            DBUS_DEFAULT_SAFE_TIMEOUT,
            &err
        );
        dbus_message_unref(msg);

        if (dbus_error_is_set(&err))
        {
            std::string msg = err.message;
            dbus_error_free(&err);
            logger->Err("D-Bus method call failed: " + msg);
            return false;
        }

        if (!reply)
        {
            dbus_message_unref(reply);
            logger->Err("No reply received for D-Bus call");
            return false;
        }

        // Extract variant from reply
        DBusMessageIter args;
        if (!dbus_message_iter_init(reply, &args))
        {
            dbus_message_unref(reply);
            logger->Err("Reply has no arguments");
            return false;
        }

        if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_VARIANT)
        {
            dbus_message_unref(reply);
            logger->Err("Expected variant in D-Bus reply");
            return false;
        }
        DBusMessageIter variantIter;
        dbus_message_iter_recurse(&args, &variantIter);

        DBusValue out;
        out.dbusType = dbus_message_iter_get_arg_type(&variantIter);

        char* sig_c = dbus_message_iter_get_signature(&variantIter);
        if (sig_c)
        {
            out.signature = sig_c;
            dbus_free(sig_c);
        }
        else
        {
            out.signature.clear();
        }

        switch (out.dbusType)
        {
            case DBUS_TYPE_STRING:
            {
                const char* s = nullptr;
                dbus_message_iter_get_basic(&variantIter, &s);
                out.value = std::string(s ? s : "");
                break;
            }
            case DBUS_TYPE_INT32:
            {
                int32_t i = 0;
                dbus_message_iter_get_basic(&variantIter, &i);
                out.value = i;
                break;
            }
            case DBUS_TYPE_UINT32:
            {
                uint32_t u = 0;
                dbus_message_iter_get_basic(&variantIter, &u);
                out.value = u;
                break;
            }
            case DBUS_TYPE_BOOLEAN:
            {
                dbus_bool_t b = 0;
                dbus_message_iter_get_basic(&variantIter, &b);
                out.value = static_cast<bool>(b);
                break;
            }
            case DBUS_TYPE_DOUBLE:
            {
                double d = 0.0;
                dbus_message_iter_get_basic(&variantIter, &d);
                out.value = d;
                break;
            }
            default:
                dbus_message_unref(reply);
                logger->Err("Unsupported DBus variant type: " + std::to_string(out.dbusType));
                return false;
        }

        dbus_message_unref(reply);
        outValue = out;
        return true;
    }

    bool AccountManagerService::CacheUser(const std::string& username) const
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        if (username.empty())
        {
            logger->Warn("CacheUser called with an empty username");
            return false;
        }

        DBusError err;
        dbus_error_init(&err);

        DBusMessage* msg = dbus_message_new_method_call(
            "org.freedesktop.Accounts",
            "/org/freedesktop/Accounts",
            "org.freedesktop.Accounts",
            "CacheUser"
        );

        if (!msg)
        {
            logger->Err("Failed to allocate D-Bus message for CacheUser");
            return false;
        }

        const char* name = username.c_str();

        if (!dbus_message_append_args(msg,
                                      DBUS_TYPE_STRING, &name,
                                      DBUS_TYPE_INVALID))
        {
            logger->Err("Failed to append arguments to CacheUser message for username: " + username);
            dbus_message_unref(msg);
            return false;
        }

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(
            _rawConn,
            msg,
            DBUS_DEFAULT_SAFE_TIMEOUT,
            &err
        );
        dbus_message_unref(msg);

        if (!reply)
        {
            if (dbus_error_is_set(&err))
            {
                logger->Err("CacheUser failed for username '" + username + "': " +
                    std::string(err.message ? err.message : "unknown error"));
                dbus_error_free(&err);
            }
            else
            {
                logger->Err("CacheUser returned no reply for username: " + username);
            }
            return false;
        }

        dbus_message_unref(reply);
        return true;
    }

    bool AccountManagerService::SetUserPassword(const std::string& userObjectPath,
                                                const std::string& cryptedPassword,
                                                const std::string& hint) const
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        if (userObjectPath.empty())
        {
            logger->Warn("SetUserPassword called with an empty user object path");
            return false;
        }

        DBusError err;
        dbus_error_init(&err);

        DBusMessage* msg = dbus_message_new_method_call(
            "org.freedesktop.Accounts",
            userObjectPath.c_str(),
            "org.freedesktop.Accounts.User",
            "SetPassword"
        );

        if (!msg)
        {
            logger->Err("Failed to allocate D-Bus message for SetPassword (user path " + userObjectPath + ")");
            return false;
        }

        const char* pw = cryptedPassword.c_str();
        const char* pwHint = hint.c_str();

        if (!dbus_message_append_args(msg,
                                      DBUS_TYPE_STRING, &pw,
                                      DBUS_TYPE_STRING, &pwHint,
                                      DBUS_TYPE_INVALID))
        {
            logger->Err("Failed to append arguments to SetPassword message (user path " + userObjectPath + ")");
            dbus_message_unref(msg);
            return false;
        }

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(
            _rawConn,
            msg,
            DBUS_DEFAULT_SAFE_TIMEOUT,
            &err
        );
        dbus_message_unref(msg);

        if (!reply)
        {
            if (dbus_error_is_set(&err))
            {
                logger->Err(
                    "SetPassword failed for user path " + userObjectPath + ": " +
                    std::string(err.message ? err.message : "unknown error"));
                dbus_error_free(&err);
            }
            else
            {
                logger->Err("SetPassword returned no reply for user path: " + userObjectPath);
            }
            return false;
        }

        // No return values expected for SetPassword
        dbus_message_unref(reply);

        logger->Info("Successfully updated password for user at " + userObjectPath);
        return true;
    }

}
