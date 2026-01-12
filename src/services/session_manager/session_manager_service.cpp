#include "session_manager_service.h"

#include <algorithm>
#include <csignal>
#include <thread>
#include <sys/wait.h>

#include "../logger/logger_service.h"
#include "../../utils/dbus_utils.h"

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

    SessionManagerService::SessionManagerService(ServiceManager* serviceManager, Connection* conn) : Service(serviceManager, conn)
    {
        DBusError err;
        dbus_error_init(&err);

        dbus_bus_add_match(_rawConn,
                      "type='signal',"
                           "sender='org.freedesktop.systemd1',"
                           "interface='org.freedesktop.systemd1.Manager',"
                           "member='JobRemoved'",
                           &err);

        dbus_connection_flush(_rawConn);

        if (dbus_error_is_set(&err))
        {
            _serviceManager->Get<Logger::LoggerService>()->Err(std::string("Failed to add D-Bus match: ") + err.message);
            dbus_error_free(&err);
        }

        _isLiveEnvironment = false;
        SubscribeToSignalLegacy("org.freedesktop.systemd1.Manager.JobRemoved");

        CreateLoginSession();
    }

    SessionManagerService::~SessionManagerService()
    {
        // Stop all sessions
        const auto logger = _serviceManager->Get<Logger::LoggerService>();
        for (auto& [id, sess] : _sessions)
        {
            // --- 1) Terminate processes ---
            if (!TerminateUserSessionProcesses(_greeterSessionID))
            {
                logger->Warn("Failed to terminate processes for session " + _greeterSessionID);
                // Continue cleanup anyway, but client will know something went wrong
            }

            // --- 2) Signal child to clean up PAM and exit ---
            TerminatePAMForSession(sess);
        }

        _activePAMHandles.clear();
        _sessions.clear();
        _greeterSessionID = {};
        _isGreeterActive = false;
    }

    bool SessionManagerService::CreateLoginSession()
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        if (_isGreeterActive)
        {
            logger->Warn("Tried to spawn greeter while greeter was already active");
            return false;
        }

        struct passwd* pwd = nullptr;

        // --- 1) Lookup UID ---
        logger->Debug("PRE getpwnam"); // TODO: REM
        pwd = getpwnam(JOS_GREETER_USER);
        if (!pwd)
        {
            logger->Err(std::string("Failed to get greeter user: ") + JOS_GREETER_USER);
            return false;
        }

        // --- 2) PAM Authentication ---
        int controlFd = -1;
        pid_t leaderPid = 0;
        auto cleanupPam = [&]()
        {
            // Always close the control FD if open
            if (controlFd >= 0)
            {
                constexpr char shutdown = '1';
                write(controlFd, &shutdown, 1);
                usleep(10000); // TODO PERFORMANCE: DO NOT SLEEP
                close(controlFd);
                controlFd = -1;
            }

            // Kill child process if still running
            if (leaderPid > 0)
            {
                // Wait for graceful exit (with timeout)
                int status;
                pid_t result = waitpid(leaderPid, &status, WNOHANG);
                if (result == 0)
                {
                    // Child didn't exit quickly, wait a bit longer
                    usleep(100000); // 100ms grace period
                    result = waitpid(leaderPid, &status, WNOHANG);

                    if (result == 0)
                    {
                        // Force kill if still alive
                        kill(leaderPid, SIGKILL);
                        waitpid(leaderPid, nullptr, 0);
                    }
                }
                leaderPid = 0;
            }
        };

        logger->Debug("PRE AuthenticateAndOpenPAMSession"); // TODO: REM
        if (!AuthenticateAndOpenPAMSession(PAM_GREETER_SERVICE, JOS_GREETER_USER, "", controlFd, leaderPid))
        {
            logger->Err("Failed to open PAM session for greeter");
            cleanupPam();
            return false;
        }

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

        // --- 5) Spawn compositor/login UI ---
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

        // --- 6) Set current _greeterSession value ---
        _sessions[sessionId] = { JOS_GREETER_USER, scopeName, static_cast<uid_t>(pwd->pw_uid), leaderPid, controlFd, seat, {} };
        _greeterSessionID = sessionId;
        _pendingUnits.push_back(scopeName);
        controlFd = -1;

        // --- 7) logind session ---
        std::this_thread::sleep_for(std::chrono::milliseconds(300)); // TODO: Remove sleep and wait for UI properly
        if (!ActivateLogindSession(sessionId, seat))
        {
            logger->Warn("Failed to activate logind session: " + sessionId);
        }

        // --- 8) Return success ---
        logger->Info("Greeter session initialized successfully");
        _isGreeterActive = true;
        return true;
    }

    bool SessionManagerService::StopLoginSession()
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        if (!_isGreeterActive)
        {
            logger->Warn("StopLoginSession called but greeter is not active");
            return false;
        }

        const auto it = _sessions.find(_greeterSessionID);
        if (it == _sessions.end())
        {
            logger->Crit("StopLoginSession called but session was not found");
            return false;
        }

        auto& session = it->second;

        // --- 1) Terminate processes ---
        if (!TerminateUserSessionProcesses(_greeterSessionID))
        {
            logger->Warn("Failed to terminate processes for session " + _greeterSessionID);
            // Continue cleanup anyway, but client will know something went wrong
        }

        // --- 2) Signal child to clean up PAM and exit ---
        TerminatePAMForSession(session);

        // --- 3) Cleanup and return ---
        _sessions.erase(it);
        _greeterSessionID = {};
        _isGreeterActive = false;
        logger->Info("Greeter session terminated successfully");
        return true;
    }

    bool SessionManagerService::IsManagedUserSession(uid_t uid)
    {
        return std::ranges::any_of(_sessions, [&](auto const& e){ return e.second.uid == uid; });
    }

    bool SessionManagerService::IsPrivilegedClientProcess(const pid_t pid, const bool allowChildProcesses) const
    {
        if (allowChildProcesses)
        {
            _serviceManager->Get<Logger::LoggerService>()->Warn("Use of potentially unsafe option 'allowChildProcesses' on 'IsPrivilegedClientProcess'!");
        }

        return std::ranges::any_of(_sessions, [&](auto const& e)
        {
            auto const& privilegedPids = e.second.privilegedClientProcesses;

            if (std::ranges::find(privilegedPids, pid) != privilegedPids.end())
                return true;

            if (allowChildProcesses)
            {
                auto maybeParent = GetParentPid(pid);
                if (maybeParent && std::ranges::find(privilegedPids, *maybeParent) != privilegedPids.end())
                    return true;
            }

            return false;
        });
    }

    bool SessionManagerService::HandleMethodCallLegacy(DBusMessage* msg)
    {
        if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_SIGNAL)
        {
            const char* iface = dbus_message_get_interface(msg);
            const char* member = dbus_message_get_member(msg);

            if (iface && member &&
                strcmp(iface, "org.freedesktop.systemd1.Manager") == 0 &&
                strcmp(member, "JobRemoved") == 0)
            {
                uint32_t id = 0;
                const char* jobPath = nullptr;
                const char* unitName = nullptr;
                const char* result = nullptr;

                DBusError err;
                dbus_error_init(&err);

                if (!dbus_message_get_args(
                        msg, &err,
                        DBUS_TYPE_UINT32, &id,
                        DBUS_TYPE_OBJECT_PATH, &jobPath,
                        DBUS_TYPE_STRING, &unitName,
                        DBUS_TYPE_STRING, &result,
                        DBUS_TYPE_INVALID))
                {
                    _serviceManager->Get<Logger::LoggerService>()->Err(
                        std::string("Failed to parse JobRemoved signal: ") +
                        (err.message ? err.message : "unknown"));
                    dbus_error_free(&err);
                    return true;
                }

                dbus_error_free(&err);

                OnJobRemoved(unitName, result);
                return true; // Signal handled
            }

            // Other signals can be handled here
            return true; // Ignore unknown signals
        }

        const char* sender = dbus_message_get_sender(msg);
        if (!sender)
        {
            SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_ACCESS_DENIED, "No sender");
            return true;
        }

        DBusError err;
        dbus_error_init(&err);

        const uid_t senderUid = dbus_bus_get_unix_user(_rawConn, sender, &err);
        if (dbus_error_is_set(&err))
        {
            SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_ACCESS_DENIED, std::string("Failed to get UID for sender ") + sender + ": " + (err.message ? err.message : "unknown"));
            dbus_error_free(&err);
            return true;
        }

        dbus_error_free(&err);

        const char* iface = dbus_message_get_interface(msg);
        if (!iface)
        {
            SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_UNKNOWN_INTERFACE, "No interface specified");
            return true;
        }

        const char* member = dbus_message_get_member(msg);
        if (!member)
        {
            SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_UNKNOWN_METHOD, "No method specified");
            return true;
        }

        // Dispatch based on method name
        if (strcmp(member, "CreateSession") == 0)
        {
            DBusMessageIter args;
            if (!dbus_message_iter_init(msg, &args))
            {
                SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_INVALID_ARGS, "Expected arguments: username, password");
                return true;
            }

            if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
            {
                SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_INVALID_ARGS, "Expected string username");
                return true;
            }
            const char* username = nullptr;
            dbus_message_iter_get_basic(&args, &username);

            if (!dbus_message_iter_next(&args) ||
                dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
            {
                SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_INVALID_ARGS, "Expected string password");
                return true;
            }
            const char* password = nullptr;
            dbus_message_iter_get_basic(&args, &password);

            if (!username || !password)
            {
                SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_INVALID_ARGS, "Null argument(s)");
                return true;
            }

            CreateSession(msg, senderUid, username, password);
            return true;
        }
        else if (strcmp(member, "StopSession") == 0)
        {
            DBusMessageIter args;
            if (!dbus_message_iter_init(msg, &args))
            {
                SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_INVALID_ARGS, "Expected arguments: sessionId");
                return true;
            }

            if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
            {
                SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_INVALID_ARGS, "Expected string sessionId");
                return true;
            }

            const char* sessionId = nullptr;
            dbus_message_iter_get_basic(&args, &sessionId);

            if (!sessionId)
            {
                SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_INVALID_ARGS, "Null sessionId");
                return true;
            }

            StopSessionDbus(msg, senderUid, sessionId);
            return true;
        }
        else if (strcmp(member, "ListSessions") == 0)
        {
            DBusMessageIter iter;
            if (dbus_message_iter_init(msg, &iter))
            {
                SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_INVALID_ARGS, "ListSessions expects no arguments");
                return true;
            }

            ListSessions(msg, senderUid);
            return true;
        }

        SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_UNKNOWN_METHOD, std::string("Unknown DBus method called: ") + member);
        return false;
    }

    void SessionManagerService::CreateSession(DBusMessage* msg,
                                              const uid_t callerUid,
                                              const std::string& username,
                                              const std::string& password)
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        // --- 1) Authorization ---
        if (callerUid != 0 && !IsGreeter(callerUid))
        {
            logger->Warn("Unauthorized CreateSession attempt by UID " + std::to_string(callerUid));
            SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_ACCESS_DENIED, "Only greeter may call CreateSession");
            return;
        }

        // --- 2) Lookup UID ---
        const struct passwd* pwd = getpwnam(username.c_str());
        if (!pwd)
        {
            logger->Err("CreateSession: unknown user " + username);
            SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_FAILED, "Unknown user");
            return;
        }
        const uid_t uid = pwd->pw_uid;

        // --- 3) PAM Authentication ---
        int controlFd = -1;
        pid_t leaderPid = 0;
        auto cleanupPam = [&]()
        {
            // Always close the control FD if open
            if (controlFd >= 0)
            {
                constexpr char shutdown = '1';
                write(controlFd, &shutdown, 1);
                usleep(10000); // TODO PERFORMANCE: DO NOT SLEEP
                close(controlFd);
                controlFd = -1;
            }

            // Kill child process if still running
            if (leaderPid > 0)
            {
                // Wait for graceful exit (with timeout)
                int status;
                pid_t result = waitpid(leaderPid, &status, WNOHANG);
                if (result == 0)
                {
                    // Child didn't exit quickly, wait a bit longer
                    usleep(100000); // 100ms grace period
                    result = waitpid(leaderPid, &status, WNOHANG);

                    if (result == 0)
                    {
                        // Force kill if still alive
                        kill(leaderPid, SIGKILL);
                        waitpid(leaderPid, nullptr, 0);
                    }
                }
                leaderPid = 0;
            }
        };

        if (!AuthenticateAndOpenPAMSession(PAM_LOGIN_SERVICE, username, password, controlFd, leaderPid))
        {
            logger->Warn("Authentication failed for user " + username);
            cleanupPam();
            SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_AUTH_FAILED, "Authentication failed");
            return;
        }

        // --- 4) Session ID ---
        std::string objectPath;
        const std::string sessionId = QueryLogindSessionForUid(pwd->pw_uid, objectPath);
        if (sessionId.empty())
        {
            logger->Err("No logind session found for user UID: " + std::to_string(pwd->pw_uid));
            cleanupPam();
            SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_FAILED, "Could not determine session ID");
            return;
        }

        // --- 5) Query seat ---
        std::string seat = QueryLogindSeatForSession(sessionId, objectPath);
        if (seat.empty())
        {
            logger->Warn("No seat found for session " + sessionId);
            seat = "seat0"; // safe fallback
        }

        // Ensure sessionId not already in map
        if (_sessions.count(sessionId))
        {
            logger->Err("Duplicate sessionId detected: " + sessionId);
            cleanupPam();
            SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_FAILED, "Duplicate session ID");
            return;
        }

        // --- 6) Spawn compositor/desktop ---
        std::string scopeName;
        if (!SpawnUserSessionProcesses(false, username, sessionId, seat, scopeName))
        {
            logger->Err("Failed to spawn user session for " + username);
            cleanupPam();
            SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_FAILED, "Failed to spawn desktop session");
            return;
        }

        if (scopeName.empty())
        {
            logger->Err("SpawnUserSessionProcesses returned empty scopeName for " + username);
            TerminateUserSessionProcesses(sessionId);
            cleanupPam();
            SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_FAILED, "Invalid session scope");
            return;
        }

        // --- 7) Insert into session map ---
        _sessions[sessionId] = { username, scopeName, uid, leaderPid, controlFd, seat, {} };
        _pendingUnits.push_back(scopeName);
        controlFd = -1;

        // --- 8) Send reply ---
        SendDBusReply_CreateSession(msg, sessionId, uid, seat);
        logger->Info("Created session " + sessionId + " for user " + username);

        // --- 9) logind session ---
        std::this_thread::sleep_for(std::chrono::milliseconds(300)); // TODO: Remove sleep and wait for UI properly
        if (!ActivateLogindSession(sessionId, seat))
        {
            logger->Warn("Failed to activate logind session: " + sessionId);
        }

        // --- 10) Manage greeter ---
        if (!StopLoginSession())
        {
            logger->Warn("Failed to stop login session.");
        }
    }

    void SessionManagerService::SendDBusReply_CreateSession(DBusMessage* msg,
                                                       const std::string& sessionId,
                                                       const uid_t uid,
                                                       const std::string& seat)
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
            dbus_message_unref(reply);
            SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_NO_MEMORY, "Failed to build reply");
            return;
        }

        if (!dbus_connection_send(_rawConn, reply, nullptr))
        {
            dbus_message_unref(reply);
            SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_FAILED, "Reply send failed");
            return;
        }

        dbus_connection_flush(_rawConn);
        dbus_message_unref(reply);
    }

    bool SessionManagerService::AuthenticateAndOpenPAMSession(const std::string& service,
                                                          const std::string& username,
                                                          const std::string& password,
                                                          int& out_controlFd,
                                                          pid_t& out_childPid)
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        struct pam_conv conv;
        PamConversationCtx convctx{password};
        conv.conv = &pam_conversation;
        conv.appdata_ptr = &convctx;

        const int tty_num = FindFreeTTY();
        const std::string ttyPath = tty_num > 0
            ? "/dev/tty" + std::to_string(tty_num)
            : "/dev/tty1";

        // Create communication pipes BEFORE any PAM operations
        int status_pipe[2];  // For status communication
        int control_pipe[2]; // For shutdown signaling

        if (pipe(status_pipe) == -1 || pipe(control_pipe) == -1)
        {
            logger->Err("Failed to create pipes for session management");
            return false;
        }

        const pid_t pid = fork();
        if (pid == -1)
        {
            close(status_pipe[0]); close(status_pipe[1]);
            close(control_pipe[0]); close(control_pipe[1]);
            logger->Err("fork() failed");
            return false;
        }

        if (pid == 0)
        {
            // ===== CHILD PROCESS (Session Leader) =====
            close(status_pipe[0]);   // Close read end of status pipe
            close(control_pipe[1]);  // Close write end of control pipe

            pam_handle_t* pamh = nullptr;
            int retval;
            char status = '0';

            // Initialize PAM in child
            retval = pam_start(service.c_str(), username.c_str(), &conv, &pamh);
            if (retval != PAM_SUCCESS)
            {
                write(status_pipe[1], &status, 1);
                close(status_pipe[1]);
                close(control_pipe[0]);
                _exit(1);
            }

            // Set PAM items
            pam_set_item(pamh, PAM_RUSER, username.c_str());
            pam_set_item(pamh, PAM_TTY, ttyPath.c_str());

            // Authenticate
            retval = pam_authenticate(pamh, 0);
            if (retval != PAM_SUCCESS)
            {
                write(status_pipe[1], &status, 1);
                close(status_pipe[1]);
                close(control_pipe[0]);
                pam_end(pamh, retval);
                _exit(1);
            }

            // Account management
            retval = pam_acct_mgmt(pamh, 0);
            if (retval != PAM_SUCCESS)
            {
                write(status_pipe[1], &status, 1);
                close(status_pipe[1]);
                close(control_pipe[0]);
                pam_end(pamh, retval);
                _exit(1);
            }

            // Set environment
            pam_putenv(pamh, "XDG_SESSION_TYPE=wayland");
            pam_putenv(pamh, ("XDG_VTNR=" + std::to_string(tty_num)).c_str());

            // Open session (creates logind session)
            retval = pam_open_session(pamh, 0);
            if (retval != PAM_SUCCESS)
            {
                write(status_pipe[1], &status, 1);
                close(status_pipe[1]);
                close(control_pipe[0]);
                pam_end(pamh, retval);
                _exit(1);
            }

            // Success! Notify parent
            status = '1';
            write(status_pipe[1], &status, 1);
            close(status_pipe[1]);

            // Wait for shutdown signal from parent
            char shutdown_signal;
            read(control_pipe[0], &shutdown_signal, 1);
            close(control_pipe[0]);

            // Clean up PAM session
            pam_close_session(pamh, 0);
            pam_end(pamh, PAM_SUCCESS);

            _exit(0);
        }

        // ===== PARENT PROCESS =====
        close(status_pipe[1]);   // Close write end of status pipe
        close(control_pipe[0]);  // Close read end of control pipe

        // Wait for child to complete PAM setup
        char status;
        const ssize_t bytes_read = read(status_pipe[0], &status, 1);
        close(status_pipe[0]);

        if (bytes_read != 1 || status != '1')
        {
            // Child failed - clean up
            close(control_pipe[1]);
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            explicit_bzero(convctx.password.data(), convctx.password.size());
            logger->Err("Child process failed to create PAM session");
            return false;
        }

        // Store the control pipe for later use
        out_controlFd = control_pipe[1];
        out_childPid = pid;

        explicit_bzero(convctx.password.data(), convctx.password.size());
        return true;
    }

    bool SessionManagerService::SpawnUserSessionProcesses(bool isLoginSession,
                                                     const std::string& username,
                                                     const std::string& sessionId,
                                                     const std::string& seat,
                                                     std::string& outServiceName) const
    {
        auto logger = _serviceManager->Get<Logger::LoggerService>();

        auto pwd = getpwnam(username.c_str());
        if (!pwd)
        {
            logger->Err(std::string("Failed to get user: ") + username);
            return false;
        }

        DBusError err;
        dbus_error_init(&err);

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

            if (dbus_error_is_set(&err))
                dbus_error_free(&err);
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
        reply = dbus_connection_send_with_reply_and_block(_rawConn, msg, DBUS_DEFAULT_SAFE_TIMEOUT, &err);
        dbus_message_unref(msg);
        msg = nullptr;

        if (dbus_error_is_set(&err))
        {
            logger->Err(std::string("StartTransientUnit failed: ") + (err.message ? err.message : "unknown"));
            dbus_error_free(&err);
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

    void SessionManagerService::StopSessionDbus(DBusMessage* msg,
                                        const uid_t callerUid,
                                        const std::string& sessionId)
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        const auto it = _sessions.find(sessionId);
        if (it == _sessions.end())
        {
            SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_INVALID_ARGS, "No such session");
            return;
        }

        auto& session = it->second;

        if (callerUid != session.uid && !PolkitAuthorizeStop(callerUid, sessionId))
        {
            logger->Warn("Unauthorized StopSession attempt by UID " + std::to_string(callerUid)
                         + " for session " + sessionId);
            SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_ACCESS_DENIED, "Not authorized");
            return;
        }

        if (!StopSession(sessionId, session))
        {
            SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_FAILED, "Failed to stop session. Check logs for more information.");
        }

        SendSuccessReplyAndLogLegacy(_rawConn, msg);
        logger->Info("Stopped session " + sessionId + " successfully");
    }

    bool SessionManagerService::StopSession(const std::string& sessionId, SessionInfo& session)
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        // --- Terminate user processes ---
        if (!TerminateUserSessionProcesses(sessionId))
        {
            logger->Warn("Failed to terminate processes for session " + sessionId);
            // Continue cleanup anyway, but client will know something went wrong
        }

        // --- Signal child to clean up PAM and exit ---
        TerminatePAMForSession(session);

        // --- Remove session from map ---
        _sessions.erase(sessionId);

        // --- Spawn greeter if needed ---
        if (!_isGreeterActive && _sessions.empty()) // TODO: Switch to other session or greeter when logged out, if there are no sessions, just create a login session
        {
            if (!CreateLoginSession())
                logger->Warn("Failed to spawn greeter after stopping session " + sessionId);
        }

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

        DBusError err;
        dbus_error_init(&err);

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

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(_rawConn, msg, DBUS_DEFAULT_SAFE_TIMEOUT, &err);
        dbus_message_unref(msg);

        if (!reply)
        {
            if (dbus_error_is_set(&err))
            {
                logger->Err("StopUnit failed for scope " + scope + ": " + std::string(err.message ? err.message : "unknown"));
                dbus_error_free(&err);
            }
            else logger->Err("StopUnit returned no reply for scope " + scope);
            return false;
        }

        dbus_message_unref(reply);
        logger->Info("Successfully stopped processes for session " + sessionId + " (scope: " + scope + ")");
        return true;
    }

    void SessionManagerService::TerminatePAMForSession(SessionInfo& session)
    {
        if (session.leaderPid > 0)
        {
            if (session.controlFd >= 0)
            {
                // Send shutdown signal to child via control pipe
                constexpr char shutdown = '1';
                write(session.controlFd, &shutdown, 1);
            }

            // Wait for child to exit gracefully
            int status;
            pid_t result = waitpid(session.leaderPid, &status, WNOHANG);
            if (result == 0)
            {
                // Give child a moment to clean up
                usleep(500000); // 500ms
                result = waitpid(session.leaderPid, &status, WNOHANG);

                if (result == 0)
                {
                    // Force kill if still running
                    _serviceManager->Get<Logger::LoggerService>()->Warn("Child process did not exit gracefully, forcing termination");
                    kill(session.leaderPid, SIGKILL);
                    waitpid(session.leaderPid, nullptr, 0);
                }
            }

            if (session.controlFd >= 0)
            {
                close(session.controlFd);
                session.controlFd = -1;
            }

            session.leaderPid = 0;
        }
    }

    void SessionManagerService::ListSessions(DBusMessage* msg, uid_t callerUid)
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        // --- Authorization ---
        if (callerUid != 0)
        {
            SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_ACCESS_DENIED, "Only root may list sessions");
            return;
        }

        // --- Allocate reply message ---
        DBusMessage* reply = dbus_message_new_method_return(msg);
        if (!reply)
        {
            SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_NO_MEMORY, "Failed to allocate D-Bus reply");
            return;
        }

        DBusMessageIter iter;
        dbus_message_iter_init_append(reply, &iter);

        // --- Open array container a(ssuss) ---
        DBusMessageIter arrayIter;
        if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(ssuss)", &arrayIter))
        {
            SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_NO_MEMORY, "Failed to open array container for ListSessions");
            dbus_message_unref(reply);
            return;
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
        if (!dbus_connection_send(_rawConn, reply, nullptr))
        {
            SendErrorReplyAndLogLegacy(_rawConn, msg, DBUS_ERROR_FAILED, "Failed to send ListSessions reply over D-Bus");
            dbus_message_unref(reply);
            return;
        }

        dbus_connection_flush(_rawConn);
        dbus_message_unref(reply);

        logger->Info("ListSessions reply sent successfully (total sessions: " + std::to_string(_sessions.size()) + ")");
    }

    void SessionManagerService::OnJobRemoved(const std::string& unitName, const std::string& result)
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        // Only handle our own transient units
        const auto it = std::ranges::find(_pendingUnits, unitName);
        if (it == _pendingUnits.end()) return;

        logger->Info("JobRemoved for unit " + unitName + " (result=" + result + ")");

        // Now query the MainPID for this unit
        if (const pid_t pid = GetUnitMainPID(_rawConn, unitName); pid > 0)
        {
            logger->Info("Got MainPID for " + unitName + ": " + std::to_string(pid));

            std::string sessionId;
            if (GetSessionIdByUnitName(unitName, sessionId))
            {
                const auto it = _sessions.find(sessionId);
                if (it != _sessions.end())
                {
                    auto& session = it->second;
                    session.privilegedClientProcesses.push_back(pid);
                }
                else
                {
                    logger->Err("Failed to get session info by unit name: " + unitName);
                }
            }
            else
            {
                logger->Err("Failed to get session info by unit name: " + unitName);
            }
        }
        else
        {
            logger->Warn("Failed to get MainPID for " + unitName);
        }

        // Mark unit as completed, etc.
        _pendingUnits.erase(it);
    }

    bool SessionManagerService::IsGreeter(const uid_t callerUid) const
    {
        const auto it = _sessions.find(_greeterSessionID);
        if (it == _sessions.end())
            return false;

        if (callerUid == it->second.uid) return true;
        return false;
    }

    std::string SessionManagerService::QueryLogindSessionForUid(const uid_t uid, std::string& outObjectPath) const
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

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

        DBusError err;
        dbus_error_init(&err);

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(_rawConn, msg, DBUS_DEFAULT_SAFE_TIMEOUT, &err);
        dbus_message_unref(msg);

        if (!reply)
        {
            if (dbus_error_is_set(&err))
            {
                logger->Err("ListSessions call failed: " + std::string(err.message ? err.message : "unknown"));
                dbus_error_free(&err);
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
            if (sessionUid) logger->Debug("   Session UID: " + std::to_string(sessionUid)); // TODO: REM
            else logger->Debug("   <no uid>");

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

        auto iface = "org.freedesktop.login1.Session";
        auto prop  = "Seat";
        if (!dbus_message_append_args(msg,
                                      DBUS_TYPE_STRING, &iface,
                                      DBUS_TYPE_STRING, &prop,
                                      DBUS_TYPE_INVALID))
        {
            logger->Err("Failed to append D-Bus args");
            dbus_message_unref(msg);
            return "seat0";
        }

        DBusError err;
        dbus_error_init(&err);

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(_rawConn, msg, 5000, &err);
        dbus_message_unref(msg);

        if (!reply)
        {
            logger->Err("D-Bus call failed: " + std::string(err.message ? err.message : "unknown"));
            dbus_error_free(&err);
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

        DBusError err;
        dbus_error_init(&err);

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(_rawConn, msg, DBUS_DEFAULT_SAFE_TIMEOUT, &err);
        dbus_message_unref(msg);

        if (!reply)
        {
            if (dbus_error_is_set(&err))
            {
                logger->Err("ActivateLogindSession failed: " + std::string(err.message ? err.message : "unknown"));
                dbus_error_free(&err);
            }
            else logger->Err("ActivateLogindSession: no reply received");
            return false;
        }

        dbus_message_unref(reply);
        logger->Info("Activated logind session: " + sessionId);
        return true;
    }

    pid_t SessionManagerService::GetUnitMainPID(DBusConnection* conn, const std::string& unitName)
    {
        DBusError err;
        dbus_error_init(&err);
        DBusMessage* msg = nullptr;
        DBusMessage* reply = nullptr;
        pid_t pid = 0;

        // Step 1: Get the unit object path
        msg = dbus_message_new_method_call(
            "org.freedesktop.systemd1",
            "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager",
            "GetUnit"
        );
        if (!msg)
        {
            dbus_error_free(&err);
            return 0;
        }

        const char* name = unitName.c_str();
        if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID))
        {
            dbus_message_unref(msg);
            dbus_error_free(&err);
            return 0;
        }

        reply = dbus_connection_send_with_reply_and_block(conn, msg, DBUS_DEFAULT_SAFE_TIMEOUT, &err);
        dbus_message_unref(msg);

        if (!reply)
        {
            if (dbus_error_is_set(&err))
                dbus_error_free(&err);
            return 0;
        }

        const char* unitPath = nullptr;
        if (!dbus_message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH, &unitPath, DBUS_TYPE_INVALID))
        {
            dbus_message_unref(reply);
            dbus_error_free(&err);
            return 0;
        }
        dbus_message_unref(reply);

        // Step 2: Get MainPID property
        msg = dbus_message_new_method_call(
            "org.freedesktop.systemd1",
            unitPath,
            "org.freedesktop.DBus.Properties",
            "Get"
        );
        if (!msg)
        {
            dbus_error_free(&err);
            return 0;
        }

        auto iface = "org.freedesktop.systemd1.Service";
        auto prop = "MainPID";
        if (!dbus_message_append_args(
                msg,
                DBUS_TYPE_STRING, &iface,
                DBUS_TYPE_STRING, &prop,
                DBUS_TYPE_INVALID))
        {
            dbus_message_unref(msg);
            dbus_error_free(&err);
            return 0;
        }

        reply = dbus_connection_send_with_reply_and_block(conn, msg, DBUS_DEFAULT_SAFE_TIMEOUT, &err);
        dbus_message_unref(msg);

        if (!reply)
        {
            if (dbus_error_is_set(&err))
                dbus_error_free(&err);
            return 0;
        }

        // Extract variant(uint32)
        DBusMessageIter iter, variant;
        dbus_message_iter_init(reply, &iter);
        if (DBUS_TYPE_VARIANT == dbus_message_iter_get_arg_type(&iter))
        {
            dbus_message_iter_recurse(&iter, &variant);
            dbus_uint32_t mainPid = 0;
            if (DBUS_TYPE_UINT32 == dbus_message_iter_get_arg_type(&variant))
                dbus_message_iter_get_basic(&variant, &mainPid);
            pid = static_cast<pid_t>(mainPid);
        }

        dbus_message_unref(reply);
        dbus_error_free(&err);
        return pid;
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

    bool SessionManagerService::GetSessionIdByUnitName(const std::string& unitName, std::string& outSessionId)
    {
        return std::ranges::any_of(_sessions, [&](auto const& e)
        {
            if (e.second.scopeName == unitName)
            {
                outSessionId = e.first;
                return true;
            }

            return false;
        });
    }

    std::optional<pid_t> SessionManagerService::GetParentPid(pid_t pid) const
    {
        std::ifstream statFile("/proc/" + std::to_string(pid) + "/stat");
        if (!statFile.is_open())
            return std::nullopt;

        std::string comm;
        char state;
        pid_t ppid;

        // Format: pid (comm) state ppid ...
        statFile >> pid >> comm >> state >> ppid;

        if (statFile.fail())
            return std::nullopt;

        return ppid;
    }

}