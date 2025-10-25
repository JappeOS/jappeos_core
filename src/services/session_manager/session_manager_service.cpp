#include "session_manager_service.h"

#include <algorithm>

#include "../logger/logger_service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::SessionManager
{

    static int pam_conversation(const int num_msg, const struct pam_message **msg,
                           struct pam_response **resp, void *appdata_ptr)
    {
        const auto* ctx = static_cast<PamConversationCtx *>(appdata_ptr);
        if (num_msg <= 0) return PAM_CONV_ERR;
        struct pam_response *r = static_cast<struct pam_response *>(calloc(num_msg, sizeof(*r)));
        for (int i = 0; i < num_msg; ++i)
        {
            if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF)
            {
                if (!r) return PAM_BUF_ERR;
                char* dup = strdup(ctx->password.c_str());
                if (!dup)
                {
                    free(r);
                    return PAM_BUF_ERR;
                }
                r[i].resp = dup;
                r[i].resp_retcode = 0;
            }
            else
            {
                // handle other message styles as needed
                r[i].resp = nullptr;
            }
        }
        *resp = r;
        return PAM_SUCCESS;
    }

    SessionManagerService::SessionManagerService(ServiceManager* serviceManager, DBusConnection* conn, DBusError* err) : Service(serviceManager, conn, err)
    {
        CreateLoginSession();
    }

    SessionManagerService::~SessionManagerService()
    {
        // Stop all sessions
        for (auto& [id, sess] : _sessions)
        {
            TerminateUserSessionProcesses(id);
            if (sess.pam_handle)
            {
                if (const int rc1 = pam_close_session(sess.pam_handle, 0); rc1 != PAM_SUCCESS)
                    _serviceManager->Get<Logger::LoggerService>()->Warn("pam_close_session returned error: " + std::to_string(rc1));

                if (const int rc2 = pam_end(sess.pam_handle, PAM_SUCCESS); rc2 != PAM_SUCCESS)
                    _serviceManager->Get<Logger::LoggerService>()->Warn("pam_end returned error: " + std::to_string(rc2));

                sess.pam_handle = nullptr;
            }
        }
        _sessions.clear();

        StopLoginSession();
    }

    bool SessionManagerService::CreateLoginSession() // TODO: NEW
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        if (_isGreeterActive)
        {
            logger->Warn("Tried to spawn greeter while greeter was already active");
            return false;
        }

        struct passwd* pwd = nullptr;
        pam_handle_t* pamh = nullptr;

        // --- 1) Lookup UID ---
        logger->Debug("PRE getpwnam"); // TODO: REM
        pwd = getpwnam(JOS_GREETER_USER);
        if (!pwd)
        {
            logger->Err(std::string("Failed to get greeter user: ") + JOS_GREETER_USER);
            return false;
        }

        // --- 2) PAM Authentication ---
        bool pamOpened = false;
        auto cleanupPam = [&]()
        {
            if (pamh)
            {
                if (pamOpened)
                {
                    if (const int rc1 = pam_close_session(pamh, 0); rc1 != PAM_SUCCESS)
                        logger->Warn("pam_close_session failed: " + std::to_string(rc1));
                }
                if (const int rc2 = pam_end(pamh, PAM_SUCCESS); rc2 != PAM_SUCCESS)
                    logger->Warn("pam_end failed: " + std::to_string(rc2));

                pamh = nullptr;
                pamOpened = false;
            }
        };

        logger->Debug("PRE AuthenticateAndOpenPAMSession"); // TODO: REM
        if (!AuthenticateAndOpenPAMSession(JOS_GREETER_USER, "", &pamh)) // pamh is nullptr
        {
            logger->Err("Failed to open PAM session for greeter");
            return false;
        }
        pamOpened = true;

        // --- 3) Session ID ---
        logger->Debug("PRE QueryLogindSessionForUid"); // TODO: REM
        std::string objectPath;
        const std::string sessionId = QueryLogindSessionForUid(pwd->pw_uid, objectPath);
        if (sessionId.empty())
        {
            logger->Err("No logind session found for greeter UID: " + std::to_string(pwd->pw_uid));
            cleanupPam();
            return false;
        }

        // --- 4) Query seat ---
        logger->Debug("PRE QueryLogindSeatForSession"); // TODO: REM
        std::string seat = QueryLogindSeatForSession(sessionId, objectPath);
        if (seat.empty())
        {
            logger->Warn("No seat found for session " + sessionId);
            seat = "seat0"; // safe fallback
        }

        // --- 5) logind session ---
        if (!ActivateLogindSession(sessionId, seat))
        {
            logger->Err("Failed to activate logind session: " + sessionId);
            cleanupPam();
            return false;
        }

        // --- 6) Spawn compositor/login UI ---
        std::string scopeName;
        if (!SpawnUserSessionProcesses(true, JOS_GREETER_USER, sessionId, seat, scopeName))
        {
            logger->Err(std::string("Failed to spawn user session for ") + JOS_GREETER_USER);
            cleanupPam();
            return false;
        }

        if (scopeName.empty())
        {
            logger->Err(std::string("SpawnUserSessionProcesses returned empty scopeName for ") + JOS_GREETER_USER);
            TerminateUserSessionProcesses(sessionId);
            cleanupPam();
            return false;
        }

        // --- 7) Set current _greeterSession value ---
        _greeterSessionID = sessionId;
        _greeterSession = { JOS_GREETER_USER, scopeName, static_cast<uid_t>(pwd->pw_uid), pamh };
        pamh = nullptr;
        pamOpened = false;

        // --- 8) Return success ---
        logger->Info("Greeter session initialized successfully");
        cleanupPam();
        _isGreeterActive = true;
        return true;
    }

    bool SessionManagerService::StopLoginSession() // TODO: NEW
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        if (!_isGreeterActive)
        {
            logger->Warn("TerminateLoginGreeterProcesses called but greeter is not active");
            return false;
        }

        // --- 1) Terminate processes ---
        if (!TerminateUserSessionProcesses(_greeterSessionID))
        {
            logger->Warn("Failed to terminate processes for session " + _greeterSessionID);
            // Continue cleanup anyway, but client will know something went wrong
        }

        // --- 2) Close PAM session ---
        if (_greeterSession.pam_handle)
        {
            if (const int rc1 = pam_close_session(_greeterSession.pam_handle, 0); rc1 != PAM_SUCCESS)
                logger->Warn("pam_close_session failed for session " + _greeterSessionID + ": " + std::to_string(rc1));

            if (const int rc2 = pam_end(_greeterSession.pam_handle, PAM_SUCCESS); rc2 != PAM_SUCCESS)
                logger->Warn("pam_end failed for session " + _greeterSessionID + ": " + std::to_string(rc2));

            _greeterSession.pam_handle = nullptr;
        }

        // --- 3) Cleanup and return ---
        _greeterSessionID = {};
        _greeterSession = {};
        _isGreeterActive = false;
        logger->Info("Greeter session terminated successfully");
        return true;
    }

    bool SessionManagerService::IsManagedUserSession(uid_t uid)
    {
        return std::ranges::any_of(_sessions, [&](auto const& e){ return e.second.uid == uid; });
    }

    bool SessionManagerService::IsPrivilegedClientProcess(pid_t pid)
    {

    }

    bool SessionManagerService::HandleMethodCall(DBusMessage* msg)
    {
        if (!_conn || !msg)
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

        const char* member = dbus_message_get_member(msg);
        if (!member)
        {
            _serviceManager->Get<Logger::LoggerService>()->Warn("DBus message without member field");
            SendErrorReply(_conn, msg, DBUS_ERROR_UNKNOWN_METHOD, "No method specified");
            return true;
        }

        // Dispatch based on method name
        if (strcmp(member, "CreateSession") == 0)
        {
            DBusMessageIter args;
            if (!dbus_message_iter_init(msg, &args))
            {
                _serviceManager->Get<Logger::LoggerService>()->Err("CreateSession called without arguments");
                SendErrorReply(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Expected arguments: username, password");
                return true;
            }

            if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
            {
                SendErrorReply(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Expected string username");
                return true;
            }
            const char* username = nullptr;
            dbus_message_iter_get_basic(&args, &username);

            if (!dbus_message_iter_next(&args) ||
                dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
            {
                SendErrorReply(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Expected string password");
                return true;
            }
            const char* password = nullptr;
            dbus_message_iter_get_basic(&args, &password);

            if (!username || !password)
            {
                SendErrorReply(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Null argument(s)");
                return true;
            }

            return CreateSession(msg, senderUid, username, password);
        }
        else if (strcmp(member, "StopSession") == 0)
        {
            DBusMessageIter args;
            if (!dbus_message_iter_init(msg, &args))
            {
                SendErrorReply(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Expected arguments: sessionId");
                return true;
            }

            if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
            {
                SendErrorReply(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Expected string sessionId");
                return true;
            }

            const char* sessionId = nullptr;
            dbus_message_iter_get_basic(&args, &sessionId);

            if (!sessionId)
            {
                SendErrorReply(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Null sessionId");
                return true;
            }

            return StopSession(msg, senderUid, sessionId);
        }
        else if (strcmp(member, "ListSessions") == 0)
        {
            // No args expected
            if (dbus_message_iter_init(msg, nullptr))
            {
                SendErrorReply(_conn, msg, DBUS_ERROR_INVALID_ARGS, "ListSessions expects no arguments");
                return true;
            }

            return ListSessions(msg, senderUid);
        }

        _serviceManager->Get<Logger::LoggerService>()->Warn(std::string("Unknown DBus method called: ") + member);
        SendErrorReply(_conn, msg, DBUS_ERROR_UNKNOWN_METHOD, "Unknown method");
        return false;
    }

    bool SessionManagerService::CreateSession(DBusMessage* msg,
                                          const uid_t callerUid,
                                          const std::string& username,
                                          const std::string& password)
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        // --- 1) Authorization ---
        if (callerUid != 0 && !IsGreeter(callerUid))
        {
            logger->Warn("Unauthorized CreateSession attempt by UID " + std::to_string(callerUid));
            SendErrorReply(_conn, msg, DBUS_ERROR_ACCESS_DENIED, "Only greeter may call CreateSession");
            return true;
        }

        // --- 2) Lookup UID ---
        const struct passwd* pwd = getpwnam(username.c_str());
        if (!pwd)
        {
            logger->Err("CreateSession: unknown user " + username);
            SendErrorReply(_conn, msg, DBUS_ERROR_FAILED, "Unknown user");
            return true;
        }
        const uid_t uid = pwd->pw_uid;

        // --- 3) PAM Authentication ---
        pam_handle_t* pamh = nullptr;
        bool pamOpened = false;
        auto cleanupPam = [&]()
        {
            if (pamh)
            {
                if (pamOpened)
                {
                    if (const int rc1 = pam_close_session(pamh, 0); rc1 != PAM_SUCCESS)
                        logger->Warn("pam_close_session failed: " + std::to_string(rc1));
                }
                if (const int rc2 = pam_end(pamh, PAM_SUCCESS); rc2 != PAM_SUCCESS)
                    logger->Warn("pam_end failed: " + std::to_string(rc2));

                pamh = nullptr;
                pamOpened = false;
            }
        };

        if (!AuthenticateAndOpenPAMSession(username, password, &pamh))
        {
            logger->Warn("Authentication failed for user " + username);
            SendErrorReply(_conn, msg, DBUS_ERROR_AUTH_FAILED, "Authentication failed");
            return true;
        }
        pamOpened = true;

        // --- 4) Session ID ---
        std::string objectPath;
        const std::string sessionId = QueryLogindSessionForUid(pwd->pw_uid, objectPath);
        if (sessionId.empty())
        {
            logger->Err("No logind session found for greeter UID: " + std::to_string(pwd->pw_uid));
            cleanupPam();
            SendErrorReply(_conn, msg, DBUS_ERROR_FAILED, "Could not determine session ID");
            return true;
        }

        // --- 5) Query seat ---
        std::string seat = QueryLogindSeatForSession(sessionId, objectPath);
        if (seat.empty())
        {
            logger->Warn("No seat found for session " + sessionId);
            seat = "seat0"; // safe fallback
        }

        // --- 6) logind session ---
        if (!ActivateLogindSession(sessionId, seat))
        {
            logger->Err("Failed to activate logind session: " + sessionId);
            cleanupPam();
            SendErrorReply(_conn, msg, DBUS_ERROR_FAILED, "Failed to activate session");
            return true;
        }

        // Ensure sessionId not already in map
        if (_sessions.count(sessionId))
        {
            logger->Err("Duplicate sessionId detected: " + sessionId);
            cleanupPam();
            SendErrorReply(_conn, msg, DBUS_ERROR_FAILED, "Duplicate session ID");
            return true;
        }

        // --- 7) Spawn compositor/desktop ---
        std::string scopeName;
        if (!SpawnUserSessionProcesses(false, username, sessionId, seat, scopeName))
        {
            logger->Err("Failed to spawn user session for " + username);
            cleanupPam();
            SendErrorReply(_conn, msg, DBUS_ERROR_FAILED, "Failed to spawn desktop session");
            return true;
        }

        if (scopeName.empty())
        {
            logger->Err("SpawnUserSessionProcesses returned empty scopeName for " + username);
            TerminateUserSessionProcesses(sessionId);
            cleanupPam();
            SendErrorReply(_conn, msg, DBUS_ERROR_FAILED, "Invalid session scope");
            return true;
        }

        // --- 8) Insert into session map ---
        _sessions[sessionId] = { username, scopeName, uid, pamh, seat };
        // ownership of pamh now belongs to _sessions entry
        pamh = nullptr;
        pamOpened = false;

        // --- 9) Manage greeter ---
        if (_sessions.size() > 1 && !_isGreeterActive)
        {
            if (!CreateLoginSession()) // TODO: Do not switch to login screen here. It's supposed to run in the background with multiple sessions.
                logger->Warn("Failed to spawn greeter while creating new session");
        }
        else
            StopLoginSession();

        // --- 10) Send reply ---
        SendDBusReply_CreateSession(msg, sessionId, uid, seat);
        logger->Info("Created session " + sessionId + " for user " + username);
        return true;
    }

    void SessionManagerService::SendDBusReply_CreateSession(DBusMessage* msg,
                                                       const std::string& sessionId,
                                                       const uid_t uid,
                                                       const std::string& seat) const
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        DBusMessage* reply = dbus_message_new_method_return(msg);
        if (!reply)
        {
            logger->Err("Failed to allocate D-Bus reply for CreateSession");
            return;
        }

        DBusMessageIter args;
        dbus_message_iter_init_append(reply, &args);

        const char* sid     = sessionId.c_str();
        const char* seatStr = seat.c_str();

        if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &sid) ||
            !dbus_message_iter_append_basic(&args, DBUS_TYPE_UINT32, &uid) ||
            !dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &seatStr))
        {
            logger->Err("Failed to append arguments to CreateSession reply");
            dbus_message_unref(reply);
            SendErrorReply(_conn, msg, DBUS_ERROR_NO_MEMORY, "Failed to build reply");
            return;
        }

        if (!dbus_connection_send(_conn, reply, nullptr))
        {
            logger->Err("Failed to send CreateSession reply over D-Bus");
            dbus_message_unref(reply);
            SendErrorReply(_conn, msg, DBUS_ERROR_FAILED, "Reply send failed");
            return;
        }

        dbus_connection_flush(_conn);
        dbus_message_unref(reply);
    }

    bool SessionManagerService::AuthenticateAndOpenPAMSession(const std::string& username,
                                                          const std::string& password,
                                                          pam_handle_t** out_pamh)
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        struct pam_conv conv;
        PamConversationCtx convctx{password};
        conv.conv = &pam_conversation;
        conv.appdata_ptr = &convctx;

        pam_handle_t* pamh = nullptr;
        logger->Debug("PRE pam_start"); // TODO: REM
        int retval = pam_start(PAM_GREETER_SERVICE, username.c_str(), &conv, &pamh);
        if (retval != PAM_SUCCESS)
        {
            logger->Err("pam_start failed for " + username + ": " + pam_strerror(pamh, retval));
            return false;
        }

        logger->Debug("PRE pam_set_item"); // TODO: REM
        retval = pam_set_item(pamh, PAM_RUSER, username.c_str());
        if (retval != PAM_SUCCESS)
        {
            logger->Err("pam_set_item failed for " + username + ": " + pam_strerror(pamh, retval));
            pam_end(pamh, retval);
            return false;
        }

        logger->Debug("PRE pam_authenticate"); // TODO: REM
        retval = pam_authenticate(pamh, 0);
        if (retval != PAM_SUCCESS)
        {
            logger->Warn("pam_authenticate failed for " + username + ": " + pam_strerror(pamh, retval));
            pam_end(pamh, retval);
            return false;
        }

        logger->Debug("PRE pam_acct_mgmt"); // TODO: REM
        retval = pam_acct_mgmt(pamh, 0);
        if (retval != PAM_SUCCESS)
        {
            logger->Warn("pam_acct_mgmt failed for " + username + ": " + pam_strerror(pamh, retval));
            pam_end(pamh, retval);
            return false;
        }

        /*logger->Debug("PRE pam_set_item"); // TODO: REM
        retval = pam_set_item(pamh, PAM_SEAT, "seat0");
        if (retval != PAM_SUCCESS)
        {
            logger->Err("pam_set_item failed for " + username + ": " + pam_strerror(pamh, retval));
            pam_end(pamh, retval);
            return false;
        }*/

        const int tty_num = FindFreeTTY();
        const std::string ttyPath = tty_num > 0
            ? "/dev/tty" + std::to_string(tty_num)
            : "/dev/tty1"; // fallback

        pam_putenv(pamh, "XDG_SESSION_TYPE=wayland"); // TODO: Error handling
        //pam_putenv(pamh, "XDG_SEAT=seat0");
        pam_putenv(pamh, ("XDG_VTNR=" + std::to_string(tty_num)).c_str()); // TODO: Error handling

        pam_set_item(pamh, PAM_TTY, ttyPath.c_str()); // TODO: Error handling

        logger->Debug("PRE pam_open_session"); // TODO: REM
        retval = pam_open_session(pamh, 0);
        if (retval != PAM_SUCCESS)
        {
            logger->Warn("pam_open_session failed for " + username + ": " + pam_strerror(pamh, retval));
            pam_end(pamh, retval);
            return false;
        }

        *out_pamh = pamh;
        return true;
    }

    bool SessionManagerService::SpawnUserSessionProcesses(bool isLoginSession,
                                                     const std::string& username,
                                                     const std::string& sessionId,
                                                     const std::string& seat,
                                                     std::string& outServiceName,
                                                     std::vector<pid_t>& outProcessIds) const
    {
        auto logger = _serviceManager->Get<Logger::LoggerService>();

        auto pwd = getpwnam(username.c_str());
        if (!pwd)
        {
            logger->Err(std::string("Failed to get user: ") + username);
            return false;
        }

        // Connect to the system bus
        if (!_conn)
        {
            logger->Err(std::string("Failed to connect to system bus: ") + (_err->message ? _err->message : "unknown"));
            if (dbus_error_is_set(_err)) dbus_error_free(_err);
            return false;
        }

        // Cleanup helpers
        DBusMessage* msg = nullptr;
        DBusMessage* reply = nullptr;
        auto cleanupDbus = [&]()
        {
            if (reply)
            {
                dbus_message_unref(reply);
                reply = nullptr;
            }
            if (msg)
            {
                dbus_message_unref(msg);
                msg = nullptr;
            }

            if (dbus_error_is_set(_err))
                dbus_error_free(_err);
        };

        // Build scope name early
        outServiceName = "session@" + sessionId + ".service";

        // Create method call
        msg = dbus_message_new_method_call(
            "org.freedesktop.systemd1",
            "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager",
            "StartTransientUnit"
        );
        if (!msg)
        {
            logger->Err("Failed to allocate D-Bus message for StartTransientUnit");
            cleanupDbus();
            return false;
        }

        const char* unitName = outServiceName.c_str();
        auto mode = "replace";

        // Append unit name and mode
        DBusMessageIter iter;
        dbus_message_iter_init_append(msg, &iter);
        if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &unitName) ||
            !dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &mode))
        {
            logger->Err("Failed to append StartTransientUnit arguments (unit/mode)");
            cleanupDbus();
            return false;
        }

        // Properties array: a(sv)
        DBusMessageIter arrayIter;
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(sv)", &arrayIter);

        // Helper: add string property (returns bool on success)
        auto addPropString = [&](const char* key, const char* val) -> bool
        {
            DBusMessageIter structIter, variantIter;
            dbus_message_iter_open_container(&arrayIter, DBUS_TYPE_STRUCT, nullptr, &structIter);
            if (!dbus_message_iter_append_basic(&structIter, DBUS_TYPE_STRING, &key))
            {
                dbus_message_iter_close_container(&arrayIter, &structIter);
                return false;
            }
            dbus_message_iter_open_container(&structIter, DBUS_TYPE_VARIANT, "s", &variantIter);
            if (!dbus_message_iter_append_basic(&variantIter, DBUS_TYPE_STRING, &val))
            {
                dbus_message_iter_close_container(&structIter, &variantIter);
                dbus_message_iter_close_container(&arrayIter, &structIter);
                return false;
            }
            dbus_message_iter_close_container(&structIter, &variantIter);
            dbus_message_iter_close_container(&arrayIter, &structIter);
            return true;
        };

        if (!addPropString("Description", "Desktop Session") ||
            !addPropString("Slice", "session.slice") ||
            !addPropString("User", username.c_str()))
        {
            logger->Err("Failed to append basic properties to StartTransientUnit");
            dbus_message_iter_close_container(&iter, &arrayIter);
            cleanupDbus();
            return false;
        }

        // Environment (string array)
        {
            auto key = "Environment";
            DBusMessageIter structIter, variantIter, arrayIter2;
            dbus_message_iter_open_container(&arrayIter, DBUS_TYPE_STRUCT, nullptr, &structIter);
            if (!dbus_message_iter_append_basic(&structIter, DBUS_TYPE_STRING, &key))
            {
                dbus_message_iter_close_container(&arrayIter, &structIter);
                logger->Err("Failed to append Environment key");
                dbus_message_iter_close_container(&iter, &arrayIter);
                cleanupDbus();
                return false;
            }
            // variant of "as"
            dbus_message_iter_open_container(&structIter, DBUS_TYPE_VARIANT, "as", &variantIter);
            dbus_message_iter_open_container(&variantIter, DBUS_TYPE_ARRAY, "s", &arrayIter2);

            // Keep C++ strings in scope until we've appended them
            auto env1 = "XDG_SESSION_TYPE=wayland";
            std::string de = "XDG_CURRENT_DESKTOP=" + std::string(JOS_DESKTOP_NAME);
            const char* env2 = de.c_str();
            auto env3 = "XDG_RUNTIME_DIR=/run/user/" + std::to_string(pwd->pw_uid);
            auto env4 = "XDG_SEAT=" + seat;
            auto env5 = "XDG_SESSION_CLASS=user";

            if (!dbus_message_iter_append_basic(&arrayIter2, DBUS_TYPE_STRING, &env1) ||
                !dbus_message_iter_append_basic(&arrayIter2, DBUS_TYPE_STRING, &env2) ||
                !dbus_message_iter_append_basic(&arrayIter2, DBUS_TYPE_STRING, &env3) ||
                !dbus_message_iter_append_basic(&arrayIter2, DBUS_TYPE_STRING, &env4) ||
                !dbus_message_iter_append_basic(&arrayIter2, DBUS_TYPE_STRING, &env5))
            {
                dbus_message_iter_close_container(&variantIter, &arrayIter2);
                dbus_message_iter_close_container(&structIter, &variantIter);
                dbus_message_iter_close_container(&arrayIter, &structIter);
                logger->Err("Failed to append Environment values");
                dbus_message_iter_close_container(&iter, &arrayIter);
                cleanupDbus();
                return false;
            }

            dbus_message_iter_close_container(&variantIter, &arrayIter2);
            dbus_message_iter_close_container(&structIter, &variantIter);
            dbus_message_iter_close_container(&arrayIter, &structIter);
        }

        // ExecStart ([ (sa(sv)) ]) [NORMAL]
        /*{
            const char* key = "ExecStart";
            DBusMessageIter structIter, variantIter, arrayIter2, innerStructIter, arrayIter3;

            dbus_message_iter_open_container(&arrayIter, DBUS_TYPE_STRUCT, nullptr, &structIter);
            if (!dbus_message_iter_append_basic(&structIter, DBUS_TYPE_STRING, &key))
            {
                dbus_message_iter_close_container(&arrayIter, &structIter);
                logger->Err("Failed to append ExecStart key");
                dbus_message_iter_close_container(&iter, &arrayIter);
                cleanupDbus();
                return false;
            }

            dbus_message_iter_open_container(&structIter, DBUS_TYPE_VARIANT, "a(sasb)", &variantIter);
            dbus_message_iter_open_container(&variantIter, DBUS_TYPE_ARRAY, "(sasb)", &arrayIter2);

            // one struct (path, args[], boolean)
            dbus_message_iter_open_container(&arrayIter2, DBUS_TYPE_STRUCT, nullptr, &innerStructIter);

            const char* path = JOS_DESKTOP_BINARY;
            if (!dbus_message_iter_append_basic(&innerStructIter, DBUS_TYPE_STRING, &path))
            {
                dbus_message_iter_close_container(&arrayIter2, &innerStructIter);
                dbus_message_iter_close_container(&variantIter, &arrayIter2);
                dbus_message_iter_close_container(&structIter, &variantIter);
                dbus_message_iter_close_container(&arrayIter, &structIter);
                logger->Err("Failed to append ExecStart path");
                dbus_message_iter_close_container(&iter, &arrayIter);
                cleanupDbus();
                return false;
            }

            // arguments array
            dbus_message_iter_open_container(&innerStructIter, DBUS_TYPE_ARRAY, "s", &arrayIter3);
            const char* arg0 = JOS_DESKTOP_BINARY;
            if (!dbus_message_iter_append_basic(&arrayIter3, DBUS_TYPE_STRING, &arg0))
            {
                dbus_message_iter_close_container(&innerStructIter, &arrayIter3);
                dbus_message_iter_close_container(&arrayIter2, &innerStructIter);
                dbus_message_iter_close_container(&variantIter, &arrayIter2);
                dbus_message_iter_close_container(&structIter, &variantIter);
                dbus_message_iter_close_container(&arrayIter, &structIter);
                logger->Err("Failed to append ExecStart args");
                dbus_message_iter_close_container(&iter, &arrayIter);
                cleanupDbus();
                return false;
            }
            dbus_message_iter_close_container(&innerStructIter, &arrayIter3);

            // boolean (not shell)
            dbus_bool_t isShell = false;
            if (!dbus_message_iter_append_basic(&innerStructIter, DBUS_TYPE_BOOLEAN, &isShell))
            {
                dbus_message_iter_close_container(&arrayIter2, &innerStructIter);
                dbus_message_iter_close_container(&variantIter, &arrayIter2);
                dbus_message_iter_close_container(&structIter, &variantIter);
                dbus_message_iter_close_container(&arrayIter, &structIter);
                logger->Err("Failed to append ExecStart boolean");
                dbus_message_iter_close_container(&iter, &arrayIter);
                cleanupDbus();
                return false;
            }

            dbus_message_iter_close_container(&arrayIter2, &innerStructIter);
            dbus_message_iter_close_container(&variantIter, &arrayIter2);
            dbus_message_iter_close_container(&structIter, &variantIter);
            dbus_message_iter_close_container(&arrayIter, &structIter);
        }*/

        // ExecStart ([ (sa(sv)) ]) [CAGE]
        {
            const char* key = "ExecStart";
            DBusMessageIter structIter, variantIter, arrayIter2, innerStructIter, arrayIter3;

            dbus_message_iter_open_container(&arrayIter, DBUS_TYPE_STRUCT, nullptr, &structIter);
            if (!dbus_message_iter_append_basic(&structIter, DBUS_TYPE_STRING, &key))
            {
                dbus_message_iter_close_container(&arrayIter, &structIter);
                logger->Err("Failed to append ExecStart key");
                dbus_message_iter_close_container(&iter, &arrayIter);
                cleanupDbus();
                return false;
            }

            dbus_message_iter_open_container(&structIter, DBUS_TYPE_VARIANT, "a(sasb)", &variantIter);
            dbus_message_iter_open_container(&variantIter, DBUS_TYPE_ARRAY, "(sasb)", &arrayIter2);

            // One ExecStart entry: ("cage", ["cage", JOS_DESKTOP_BINARY, "--locked"], false)
            dbus_message_iter_open_container(&arrayIter2, DBUS_TYPE_STRUCT, nullptr, &innerStructIter);

            auto path = "/usr/bin/cage";
            if (!dbus_message_iter_append_basic(&innerStructIter, DBUS_TYPE_STRING, &path))
            {
                logger->Err("Failed to append ExecStart path");
                dbus_message_iter_close_container(&arrayIter2, &innerStructIter);
                dbus_message_iter_close_container(&variantIter, &arrayIter2);
                dbus_message_iter_close_container(&structIter, &variantIter);
                dbus_message_iter_close_container(&arrayIter, &structIter);
                cleanupDbus();
                return false;
            }

            // arguments array: ["cage", JOS_DESKTOP_BINARY, "--locked"]
            dbus_message_iter_open_container(&innerStructIter, DBUS_TYPE_ARRAY, "s", &arrayIter3);
            auto arg0 = "/usr/bin/cage";
            auto arg1 = "-d";
            auto arg2 = "-s";
            auto arg3 = "-m last";
            const char* arg4 = isLoginSession ? JOS_GREETER_BINARY : JOS_DESKTOP_BINARY;
            if (!dbus_message_iter_append_basic(&arrayIter3, DBUS_TYPE_STRING, &arg0) ||
                !dbus_message_iter_append_basic(&arrayIter3, DBUS_TYPE_STRING, &arg1) ||
                !dbus_message_iter_append_basic(&arrayIter3, DBUS_TYPE_STRING, &arg2) ||
                !dbus_message_iter_append_basic(&arrayIter3, DBUS_TYPE_STRING, &arg3) ||
                !dbus_message_iter_append_basic(&arrayIter3, DBUS_TYPE_STRING, &arg4))
            {
                logger->Err("Failed to append ExecStart args");
                dbus_message_iter_close_container(&innerStructIter, &arrayIter3);
                dbus_message_iter_close_container(&arrayIter2, &innerStructIter);
                dbus_message_iter_close_container(&variantIter, &arrayIter2);
                dbus_message_iter_close_container(&structIter, &variantIter);
                dbus_message_iter_close_container(&arrayIter, &structIter);
                cleanupDbus();
                return false;
            }
            dbus_message_iter_close_container(&innerStructIter, &arrayIter3);

            dbus_bool_t isShell = false;
            dbus_message_iter_append_basic(&innerStructIter, DBUS_TYPE_BOOLEAN, &isShell);

            dbus_message_iter_close_container(&arrayIter2, &innerStructIter);
            dbus_message_iter_close_container(&variantIter, &arrayIter2);
            dbus_message_iter_close_container(&structIter, &variantIter);
            dbus_message_iter_close_container(&arrayIter, &structIter);
        }

        // Close top-level properties array
        dbus_message_iter_close_container(&iter, &arrayIter);

        // Aux units: empty array
        {
            DBusMessageIter auxIter;
            dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(sa(sv))", &auxIter);
            dbus_message_iter_close_container(&iter, &auxIter);
        }

        // Send message and wait for reply
        reply = dbus_connection_send_with_reply_and_block(_conn, msg, -1, _err);
        dbus_message_unref(msg);
        msg = nullptr;

        if (dbus_error_is_set(_err))
        {
            logger->Err(std::string("StartTransientUnit failed: ") + (_err->message ? _err->message : "unknown"));
            dbus_error_free(_err);
            cleanupDbus();
            return false;
        }

        if (!reply)
        {
            logger->Err("No reply from systemd for StartTransientUnit");
            cleanupDbus();
            return false;
        }

        // Success: we don't need jobPath now; free reply and connection
        dbus_message_unref(reply);
        reply = nullptr;

        return true;
    }

    bool SessionManagerService::StopSession(DBusMessage* msg,
                                        const uid_t callerUid,
                                        const std::string& sessionId)
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        const auto it = _sessions.find(sessionId);
        if (it == _sessions.end())
        {
            SendErrorReply(_conn, msg, DBUS_ERROR_INVALID_ARGS, "No such session");
            return true;
        }

        auto& session = it->second;

        // --- Authorization ---
        if (callerUid != session.uid && !PolkitAuthorizeStop(callerUid, sessionId))
        {
            logger->Warn("Unauthorized StopSession attempt by UID " + std::to_string(callerUid)
                         + " for session " + sessionId);
            SendErrorReply(_conn, msg, DBUS_ERROR_ACCESS_DENIED, "Not authorized");
            return true;
        }

        // --- Terminate user processes ---
        if (!TerminateUserSessionProcesses(sessionId))
        {
            logger->Warn("Failed to terminate processes for session " + sessionId);
            // Continue cleanup anyway, but client will know something went wrong
        }

        // --- Close PAM session safely ---
        if (session.pam_handle)
        {
            if (const int rc1 = pam_close_session(session.pam_handle, 0); rc1 != PAM_SUCCESS)
                logger->Warn("pam_close_session failed for session " + sessionId + ": " + std::to_string(rc1));

            if (const int rc2 = pam_end(session.pam_handle, PAM_SUCCESS); rc2 != PAM_SUCCESS)
                logger->Warn("pam_end failed for session " + sessionId + ": " + std::to_string(rc2));

            session.pam_handle = nullptr;
        }

        // --- Remove session from map ---
        _sessions.erase(it);

        // --- Spawn greeter if needed ---
        if (!_isGreeterActive)
        {
            if (!CreateLoginSession())
                logger->Warn("Failed to spawn greeter after stopping session " + sessionId);
        }

        // --- Send success reply ---
        SendSuccessReply(_conn, msg);
        logger->Info("Stopped session " + sessionId + " successfully");

        return true;
    }

    bool SessionManagerService::PolkitAuthorizeStop(uid_t callerUid, const std::string& sessionId)
    {
        // TODO: Use Polkit
        if (callerUid == 0) return true;
        return false;
    }

    bool SessionManagerService::TerminateUserSessionProcesses(const std::string& sessionId)
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        const auto it = _sessions.find(sessionId);
        if (it == _sessions.end() || it->second.scopeName.empty())
        {
            logger->Warn("TerminateUserSessionProcesses called for unknown or empty session: " + sessionId);
            return false;
        }

        const std::string& scope = it->second.scopeName;

        if (!_conn)
        {
            logger->Err("Failed to connect to system bus: " + std::string(_err->message ? _err->message : "unknown"));
            if (dbus_error_is_set(_err)) dbus_error_free(_err);
            return false;
        }

        DBusMessage* msg = dbus_message_new_method_call(
            "org.freedesktop.systemd1",
            "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager",
            "StopUnit"
        );

        if (!msg)
        {
            logger->Err("Failed to allocate D-Bus message for StopUnit");
            return false;
        }

        const char* scopeName = scope.c_str();
        auto mode = "fail";

        if (!dbus_message_append_args(msg,
                                      DBUS_TYPE_STRING, &scopeName,
                                      DBUS_TYPE_STRING, &mode,
                                      DBUS_TYPE_INVALID))
        {
            logger->Err("Failed to append arguments to StopUnit message for scope: " + scope);
            dbus_message_unref(msg);
            return false;
        }

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(_conn, msg, -1, _err);
        dbus_message_unref(msg);

        if (!reply)
        {
            if (dbus_error_is_set(_err))
            {
                logger->Err("StopUnit failed for scope " + scope + ": " + std::string(_err->message ? _err->message : "unknown"));
                dbus_error_free(_err);
            }
            else logger->Err("StopUnit returned no reply for scope " + scope);
            return false;
        }

        dbus_message_unref(reply);
        logger->Info("Successfully stopped processes for session " + sessionId + " (scope: " + scope + ")");
        return true;
    }

    bool SessionManagerService::ListSessions(DBusMessage* msg, uid_t callerUid) const
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        // --- Authorization ---
        if (callerUid != 0)
        {
            SendErrorReply(_conn, msg, DBUS_ERROR_ACCESS_DENIED, "Only root may list sessions");
            return true;
        }

        // --- Allocate reply message ---
        DBusMessage* reply = dbus_message_new_method_return(msg);
        if (!reply)
        {
            logger->Err("Failed to allocate D-Bus reply in ListSessions");
            return false;
        }

        DBusMessageIter iter;
        dbus_message_iter_init_append(reply, &iter);

        // --- Open array container a(ssuss) ---
        DBusMessageIter arrayIter;
        if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(ssuss)", &arrayIter))
        {
            logger->Err("Failed to open array container for ListSessions");
            dbus_message_unref(reply);
            return false;
        }

        for (const auto& kv : _sessions)
        {
            const std::string& sessionId = kv.first;
            const SessionInfo& s = kv.second;

            const char* sid = sessionId.c_str();
            const char* user = s.username.c_str();
            dbus_uint32_t uid = s.uid;
            const char* seat = s.seat.c_str();
            const char* scope = s.scopeName.c_str();

            DBusMessageIter structIter;
            if (!dbus_message_iter_open_container(&arrayIter, DBUS_TYPE_STRUCT, nullptr, &structIter))
            {
                logger->Warn("Failed to open struct container for session " + sessionId);
                continue; // skip this session but continue others
            }

            if (!dbus_message_iter_append_basic(&structIter, DBUS_TYPE_STRING, &sid) ||
                !dbus_message_iter_append_basic(&structIter, DBUS_TYPE_STRING, &user) ||
                !dbus_message_iter_append_basic(&structIter, DBUS_TYPE_UINT32, &uid) ||
                !dbus_message_iter_append_basic(&structIter, DBUS_TYPE_STRING, &seat) ||
                !dbus_message_iter_append_basic(&structIter, DBUS_TYPE_STRING, &scope))
            {
                logger->Warn("Failed to append session data for session " + sessionId);
                dbus_message_iter_close_container(&arrayIter, &structIter);
                continue;
            }

            dbus_message_iter_close_container(&arrayIter, &structIter);
        }

        dbus_message_iter_close_container(&iter, &arrayIter);

        // --- Send reply ---
        if (!dbus_connection_send(_conn, reply, nullptr))
        {
            logger->Err("Failed to send ListSessions reply over D-Bus");
            dbus_message_unref(reply);
            return false;
        }

        dbus_connection_flush(_conn);
        dbus_message_unref(reply);

        logger->Info("ListSessions reply sent successfully (total sessions: " + std::to_string(_sessions.size()) + ")");
        return true;
    }

    bool SessionManagerService::IsGreeter(const uid_t callerUid) const
    {
        if (callerUid == _greeterSession.uid) return true;
        return false;
    }

    std::string SessionManagerService::QueryLogindSessionForUid(const uid_t uid, std::string& outObjectPath) const
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        if (!_conn)
        {
            if (dbus_error_is_set(_err))
            {
                logger->Err("Failed to connect to system bus: " + std::string(_err->message ? _err->message : "unknown"));
                dbus_error_free(_err);
            }
            else
            {
                logger->Err("Failed to connect to system bus (unknown error)");
            }
            return "";
        }

        DBusMessage* msg = dbus_message_new_method_call(
            "org.freedesktop.login1",            // destination
            "/org/freedesktop/login1",           // path
            "org.freedesktop.login1.Manager",    // interface
            "ListSessions"                       // method
        );
        if (!msg)
        {
            logger->Err("Failed to allocate D-Bus message for ListSessions");
            return "";
        }

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(_conn, msg, -1, _err);
        dbus_message_unref(msg);

        if (!reply)
        {
            if (dbus_error_is_set(_err))
            {
                logger->Err("ListSessions call failed: " + std::string(_err->message ? _err->message : "unknown"));
                dbus_error_free(_err);
            }
            else
            {
                logger->Err("ListSessions returned no reply");
            }
            return "";
        }

        std::string foundSessionId;

        DBusMessageIter iter;
        if (!dbus_message_iter_init(reply, &iter))
        {
            logger->Warn("ListSessions reply is empty");
            dbus_message_unref(reply);
            return "";
        }

        if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
        {
            logger->Err("Unexpected ListSessions reply type, expected array");
            dbus_message_unref(reply);
            return "";
        }

        DBusMessageIter arrayIter;
        dbus_message_iter_recurse(&iter, &arrayIter);

        while (dbus_message_iter_get_arg_type(&arrayIter) == DBUS_TYPE_STRUCT)
        {
            DBusMessageIter structIter;
            dbus_message_iter_recurse(&arrayIter, &structIter);

            const char* sessionId = nullptr;
            uint32_t sessionUid = 0;
            const char* userName = nullptr;
            const char* seatId = nullptr;
            const char* objPath = nullptr;

            if (dbus_message_iter_get_arg_type(&structIter) == DBUS_TYPE_STRING)
                dbus_message_iter_get_basic(&structIter, &sessionId);
            dbus_message_iter_next(&structIter);

            if (dbus_message_iter_get_arg_type(&structIter) == DBUS_TYPE_UINT32)
                dbus_message_iter_get_basic(&structIter, &sessionUid);
            dbus_message_iter_next(&structIter);

            if (dbus_message_iter_get_arg_type(&structIter) == DBUS_TYPE_STRING)
                dbus_message_iter_get_basic(&structIter, &userName);
            dbus_message_iter_next(&structIter);

            if (dbus_message_iter_get_arg_type(&structIter) == DBUS_TYPE_STRING)
                dbus_message_iter_get_basic(&structIter, &seatId);
            dbus_message_iter_next(&structIter);

            if (dbus_message_iter_get_arg_type(&structIter) == DBUS_TYPE_OBJECT_PATH)
                dbus_message_iter_get_basic(&structIter, &objPath);

            if (!objPath)
                logger->Err("Object path expected");

            if (sessionId) logger->Debug("-> Session ID: " + std::string(sessionId)); // TODO: REM

            if (sessionUid == uid && sessionId && seatId && *seatId != '\0') // TODO: MIGHT RETURN WRONG SESSION (THE MANAGER ONLY ONE)
            {
                if (objPath && objPath[0] == '/')
                {
                    foundSessionId = sessionId;
                    outObjectPath = objPath;
                    break;
                }
                else
                {
                    logger->Err("Invalid object path for session " + std::string(sessionId ? sessionId : "(null)"));
                }
            }

            dbus_message_iter_next(&arrayIter);
        }

        dbus_message_unref(reply);

        if (!foundSessionId.empty())
            logger->Info("Found session " + foundSessionId + " for UID " + std::to_string(uid));
        else
            logger->Info("No session found for UID " + std::to_string(uid));

        return foundSessionId;
    }

    // TODO: Fix issues with retrieving the seat
    std::string SessionManagerService::QueryLogindSeatForSession(const std::string& sessionId, const std::string& sessionObjectPath) const
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        if (sessionObjectPath.empty() || sessionObjectPath[0] != '/')
        {
            logger->Err("Invalid session object path for session " + sessionId + ": '" + sessionObjectPath + "'");
            return "seat0";
        }

        if (!_conn)
        {
            logger->Err("Failed to connect to system bus: " + std::string(_err->message ? _err->message : "unknown"));
            dbus_error_free(_err);
            return "seat0";
        }

        // Build the correct object path for the session
        /*std::string objectPath = "/org/freedesktop/login1/session/";
        if (!sessionId.empty() && isdigit(sessionId[0]))
            objectPath += "_" + sessionId;
        else
            objectPath += sessionId;*/

        DBusMessage* msg = dbus_message_new_method_call(
            "org.freedesktop.login1",          // destination
            sessionObjectPath.c_str(),         // object path
            "org.freedesktop.DBus.Properties", // interface
            "Get"                              // method
        );

        if (!msg)
        {
            logger->Err("Failed to allocate D-Bus message");
            return "seat0";
        }

        const char* iface = "org.freedesktop.login1.Session";
        const char* prop  = "Seat";
        if (!dbus_message_append_args(msg,
                                      DBUS_TYPE_STRING, &iface,
                                      DBUS_TYPE_STRING, &prop,
                                      DBUS_TYPE_INVALID))
        {
            logger->Err("Failed to append D-Bus args");
            dbus_message_unref(msg);
            return "seat0";
        }

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(_conn, msg, 5000, _err);
        dbus_message_unref(msg);

        if (!reply)
        {
            logger->Err("D-Bus call failed: " + std::string(_err->message ? _err->message : "unknown"));
            dbus_error_free(_err);
            return "seat0";
        }

        DBusMessageIter iter;
        if (!dbus_message_iter_init(reply, &iter))
        {
            logger->Warn("Empty D-Bus reply");
            dbus_message_unref(reply);
            return "seat0";
        }

        if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
        {
            logger->Err("Unexpected D-Bus reply type (expected variant)");
            dbus_message_unref(reply);
            return "seat0";
        }

        DBusMessageIter variantIter;
        dbus_message_iter_recurse(&iter, &variantIter);

        const char* seatPath = nullptr;

        int argType = dbus_message_iter_get_arg_type(&variantIter);
        if (argType == DBUS_TYPE_OBJECT_PATH)
        {
            // Old systemd (< 251)
            dbus_message_iter_get_basic(&variantIter, &seatPath);
        }
        else if (argType == DBUS_TYPE_STRUCT)
        {
            // Newer systemd (>= 251) returns (string, object_path)
            DBusMessageIter structIter;
            dbus_message_iter_recurse(&variantIter, &structIter);

            // First element: seat ID (string)
            if (dbus_message_iter_get_arg_type(&structIter) == DBUS_TYPE_STRING)
            {
                const char* seatId = nullptr;
                dbus_message_iter_get_basic(&structIter, &seatId);
                dbus_message_iter_next(&structIter);

                // Second element: seat path (object_path)
                if (dbus_message_iter_get_arg_type(&structIter) == DBUS_TYPE_OBJECT_PATH)
                {
                    dbus_message_iter_get_basic(&structIter, &seatPath);
                }
            }
        }

        if (!seatPath)
        {
            logger->Err("QueryLogindSeatForSession: could not extract seat object path");
        }

        dbus_message_unref(reply);

        if (!seatPath)
        {
            logger->Warn("No seat found, defaulting to seat0");
            return "seat0";
        }

        std::string seatStr(seatPath);
        if (const auto pos = seatStr.find_last_of('/'); pos != std::string::npos) // TODO: Maybe log this and check value for debugging
        {
            std::string seat = seatStr.substr(pos + 1);
            logger->Info("Session " + sessionId + " is on seat " + seat);
            return seat;
        }

        logger->Warn("Malformed seat path (" + seatStr + "), defaulting");
        return "seat0";
    }

    bool SessionManagerService::ActivateLogindSession(const std::string& sessionId, const std::string& seat) const
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        if (sessionId.empty())
        {
            logger->Err("ActivateLogindSession called with empty sessionId");
            return false;
        }

        if (!_conn)
        {
            if (dbus_error_is_set(_err))
            {
                logger->Err("Failed to connect to system bus in ActivateLogindSession: " +
                            std::string(_err->message ? _err->message : "unknown"));
                dbus_error_free(_err);
            }
            else
            {
                logger->Err("Failed to connect to system bus (unknown error)");
            }
            return false;
        }

        DBusMessage* msg = dbus_message_new_method_call(
            "org.freedesktop.login1",
            "/org/freedesktop/login1",
            "org.freedesktop.login1.Manager",
            "ActivateSessionOnSeat"
        );
        if (!msg)
        {
            logger->Err("Failed to allocate D-Bus message in ActivateLogindSession");
            return false;
        }

        const char* sid = sessionId.c_str();
        if (!dbus_message_append_args(msg,
                                      DBUS_TYPE_STRING, &sid,
                                      DBUS_TYPE_STRING, &seat,
                                      DBUS_TYPE_INVALID))
        {
            logger->Err("Failed to append arguments in ActivateLogindSession");
            dbus_message_unref(msg);
            return false;
        }

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(_conn, msg, -1, _err);
        dbus_message_unref(msg);

        if (!reply)
        {
            if (dbus_error_is_set(_err))
            {
                logger->Err("ActivateLogindSession failed: " + std::string(_err->message ? _err->message : "unknown"));
                dbus_error_free(_err);
            }
            else logger->Err("ActivateLogindSession: no reply received");
            return false;
        }

        dbus_message_unref(reply);
        logger->Info("Activated logind session: " + sessionId);
        return true;
    }

    std::string SessionManagerService::GenerateFallbackSessionId() const
    {
        static std::atomic<uint64_t> counter{0};

        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

        const uint64_t id = counter.fetch_add(1, std::memory_order_relaxed);

        std::string sessionId = "local-" + std::to_string(millis) + "-" + std::to_string(id);

        _serviceManager->Get<Logger::LoggerService>()->Warn("Generated fallback session ID: " + sessionId);

        return sessionId;
    }

    int SessionManagerService::FindFreeTTY()
    {
        const int fd = open("/dev/tty0", O_RDWR);
        if (fd < 0)
            return -1;

        int next = 0;
        if (ioctl(fd, VT_OPENQRY, &next) == 0 && next > 0)
            return next; // tty number (e.g. 2 means /dev/tty2)
        close(fd);
        return -1;
    }

}