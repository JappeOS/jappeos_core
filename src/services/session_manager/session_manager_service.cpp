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

#include "session_manager_service.h"

#include <algorithm>
#include <csignal>
#include <thread>
#include <sys/wait.h>

#include "../logger/logger_service.h"
#include "../../utils/dbus_utils.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::SessionManager
{

    SessionManagerService::SessionManagerService(ServiceManager* serviceManager, Connection* conn)
        : Service(serviceManager, conn)
    {
        _logger = _serviceManager->Get<Logger::LoggerService>();
        
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
            _logger->Err(
                std::string("Failed to add D-Bus match: ") + err.message
            );
            dbus_error_free(&err);
        }

        _isLiveEnvironment = false;
        SubscribeToSignalLegacy("org.freedesktop.systemd1.Manager.JobRemoved");

        CreateLoginSession();
    }

    SessionManagerService::~SessionManagerService()
    {
        // Stop all sessions
        for (auto &sess: _sessions | std::views::values)
        {
            // --- 1) Terminate processes ---
            if (!TerminateUserSessionProcesses(_greeterSessionID))
            {
                _logger->Warn("Failed to terminate processes for session " + _greeterSessionID);
                // Continue cleanup anyway, but client will know something went wrong
            }

            // --- 2) Cleanup PAM ---
            TerminatePAMForSession(sess);
        }

        _activePAMHandles.clear();
        _sessions.clear();
        _greeterSessionID = {};
        _isGreeterActive = false;
        _logger = nullptr;
    }

    bool SessionManagerService::CreateLoginSession()
    {
        if (_isGreeterActive)
        {
            _logger->Warn("Tried to spawn greeter while greeter was already active");
            return false;
        }

        const passwd* pwd = nullptr;

        // --- 1) Lookup UID ---
        pwd = getpwnam(JOS_GREETER_USER);
        if (!pwd)
        {
            _logger->Err(std::string("Failed to get greeter user: ") + JOS_GREETER_USER);
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
                if (pid_t result = waitpid(leaderPid, &status, WNOHANG); result == 0)
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

        if (!AuthenticateAndOpenPAMSession(
            PAM_GREETER_SERVICE,
            JOS_GREETER_USER,
            "",
            controlFd,
            leaderPid))
        {
            _logger->Err("Failed to open PAM session for greeter");
            cleanupPam();
            return false;
        }

        // --- 3) Session ID ---
        std::string objectPath;
        const std::string sessionId = QueryLogindSessionForUid(pwd->pw_uid, objectPath);
        if (sessionId.empty())
        {
            _logger->Err("No logind session found for greeter UID: " + std::to_string(pwd->pw_uid));
            cleanupPam();
            return false;
        }

        // --- 4) Query seat ---
        std::string seat = QueryLogindSeatForSession(sessionId, objectPath);
        if (seat.empty())
        {
            _logger->Warn("No seat found for session " + sessionId);
            seat = "seat0";
        }

        // --- 5) Spawn compositor/login UI ---
        std::string scopeName;
        if (!SpawnUserSessionProcesses(
            true,
            JOS_GREETER_USER,
            sessionId,
            seat,
            scopeName))
        {
            _logger->Err(std::string("Failed to spawn user session for ") + JOS_GREETER_USER);
            cleanupPam();
            return false;
        }

        if (scopeName.empty())
        {
            _logger->Err(
                std::string("SpawnUserSessionProcesses returned empty scopeName for ") + JOS_GREETER_USER
            );
            TerminateUserSessionProcesses(sessionId);
            cleanupPam();
            return false;
        }

        // --- 6) Set current values ---
        _sessions[sessionId] =
        {
            JOS_GREETER_USER,
            scopeName,
            static_cast<uid_t>(pwd->pw_uid),
            leaderPid,
            controlFd,
            seat,
            {}
        };

        _greeterSessionID = sessionId;
        _pendingUnits.push_back(scopeName);
        controlFd = -1;

        // --- 7) logind session ---
        if (USE_SESSION_MANAGER_CREATE_VT_SWITCH_DELAY)
            std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // TODO: Remove sleep and wait for UI properly
        if (!ActivateLogindSession(sessionId, seat))
        {
            _logger->Warn("Failed to activate logind session: " + sessionId);
        }

        // --- 8) Return success ---
        _logger->Info("Greeter session initialized successfully");
        _isGreeterActive = true;
        return true;
    }

    bool SessionManagerService::StopLoginSession()
    {
        if (!_isGreeterActive)
        {
            _logger->Warn("StopLoginSession called but greeter is not active");
            return false;
        }

        const auto it = _sessions.find(_greeterSessionID);
        if (it == _sessions.end())
        {
            _logger->Crit("StopLoginSession called but session was not found");
            return false;
        }

        auto& session = it->second;

        // --- 1) Terminate processes ---
        if (!TerminateUserSessionProcesses(_greeterSessionID))
        {
            _logger->Warn("Failed to terminate processes for session " + _greeterSessionID);
            // Continue cleanup anyway, but client will know something went wrong
        }

        // --- 2) Terminate PAM ---
        TerminatePAMForSession(session);

        // --- 3) Cleanup and return ---
        _sessions.erase(it);
        _greeterSessionID = {};
        _isGreeterActive = false;
        _logger->Info("Greeter session terminated successfully");
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
            _logger->Warn(
                "Use of potentially unsafe option 'allowChildProcesses' on 'IsPrivilegedClientProcess'!"
            );
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
                    _logger->Err(
                        std::string("Failed to parse JobRemoved signal: ") +
                        (err.message ? err.message : "unknown"));
                    dbus_error_free(&err);
                    return true;
                }

                dbus_error_free(&err);

                OnJobRemoved(unitName);
                return true;
            }

            return true;
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
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_ACCESS_DENIED,
                std::string("Failed to get UID for sender ") + sender + ": "
                    + (err.message ? err.message : "unknown")
            );
            dbus_error_free(&err);
            return true;
        }

        dbus_error_free(&err);

        const char* iface = dbus_message_get_interface(msg);
        if (!iface)
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_UNKNOWN_INTERFACE,
                "No interface specified"
            );
            return true;
        }

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

        // Dispatch based on method name
        if (strcmp(member, "CreateSession") == 0)
        {
            DBusMessageIter args;
            if (!dbus_message_iter_init(msg, &args))
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Expected arguments: username, password"
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
                    "Expected string password"
                );
                return true;
            }
            const char* password = nullptr;
            dbus_message_iter_get_basic(&args, &password);

            if (!username || !password)
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Null argument(s)"
                );
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
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Expected arguments: sessionId"
                );
                return true;
            }

            if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Expected string sessionId"
                );
                return true;
            }

            const char* sessionId = nullptr;
            dbus_message_iter_get_basic(&args, &sessionId);

            if (!sessionId)
            {
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "Null sessionId"
                );
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
                SendErrorReplyAndLogLegacy(
                    _rawConn,
                    msg,
                    DBUS_ERROR_INVALID_ARGS,
                    "ListSessions expects no arguments"
                );
                return true;
            }

            ListSessions(msg, senderUid);
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

    void SessionManagerService::CreateSession(DBusMessage* msg,
                                              const uid_t callerUid,
                                              const std::string& username,
                                              const std::string& password)
    {
        // --- 1) Authorization ---
        if (callerUid != 0 && !IsGreeter(callerUid))
        {
            _logger->Warn("Unauthorized CreateSession attempt by UID " + std::to_string(callerUid));
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_ACCESS_DENIED,
                "Only greeter may call CreateSession"
            );
            return;
        }

        // --- 2) Lookup UID ---
        const passwd* pwd = getpwnam(username.c_str());
        if (!pwd)
        {
            _logger->Err("CreateSession: unknown user " + username);
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
            _logger->Warn("Authentication failed for user " + username);
            cleanupPam();
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_AUTH_FAILED,
                "Authentication failed"
            );
            return;
        }

        // --- 4) Session ID ---
        std::string objectPath;
        const std::string sessionId = QueryLogindSessionForUid(pwd->pw_uid, objectPath);
        if (sessionId.empty())
        {
            _logger->Err("No logind session found for user UID: " + std::to_string(pwd->pw_uid));
            cleanupPam();
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_FAILED,
                "Could not determine session ID"
            );
            return;
        }

        // --- 5) Query seat ---
        std::string seat = QueryLogindSeatForSession(sessionId, objectPath);
        if (seat.empty())
        {
            _logger->Warn("No seat found for session " + sessionId);
            seat = "seat0"; // safe fallback
        }

        // Ensure sessionId not already in map
        if (_sessions.contains(sessionId))
        {
            _logger->Err("Duplicate sessionId detected: " + sessionId);
            cleanupPam();
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_FAILED,
                "Duplicate session ID"
            );
            return;
        }

        // --- 6) Spawn compositor/desktop ---
        std::string scopeName;
        if (!SpawnUserSessionProcesses(false, username, sessionId, seat, scopeName))
        {
            _logger->Err("Failed to spawn user session for " + username);
            cleanupPam();
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_FAILED,
                "Failed to spawn desktop session"
            );
            return;
        }

        if (scopeName.empty())
        {
            _logger->Err("SpawnUserSessionProcesses returned empty scopeName for " + username);
            TerminateUserSessionProcesses(sessionId);
            cleanupPam();
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_FAILED,
                "Invalid session scope"
            );
            return;
        }

        // --- 7) Insert into session map ---
        _sessions[sessionId] =
        {
            username,
            scopeName,
            uid,
            leaderPid,
            controlFd,
            seat,
            {}
        };

        _pendingUnits.push_back(scopeName);
        controlFd = -1;

        // --- 8) Send reply ---
        SendDBusReply_CreateSession(msg, sessionId, uid, seat);
        _logger->Info("Created session " + sessionId + " for user " + username);

        // --- 9) logind session ---
        if (USE_SESSION_MANAGER_CREATE_VT_SWITCH_DELAY)
            std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // TODO: Remove sleep and wait for UI properly
        if (!ActivateLogindSession(sessionId, seat))
        {
            _logger->Warn("Failed to activate logind session: " + sessionId);
        }

        // --- 10) Manage greeter ---
        if (!StopLoginSession())
        {
            _logger->Warn("Failed to stop login session.");
        }
    }

    void SessionManagerService::SendDBusReply_CreateSession(DBusMessage* msg,
                                                            const std::string& sessionId,
                                                            const uid_t uid,
                                                            const std::string& seat)
    {
        DBusMessage* reply = dbus_message_new_method_return(msg);
        if (!reply)
        {
            _logger->Err("Failed to allocate D-Bus reply for CreateSession");
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
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_NO_MEMORY,
                "Failed to build reply"
            );
            return;
        }

        if (!dbus_connection_send(_rawConn, reply, nullptr))
        {
            dbus_message_unref(reply);
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_FAILED,
                "Reply send failed"
            );
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
        pam_conv conv;
        PamConversationCtx convctx{password};
        conv.conv = &PAMConversation;
        conv.appdata_ptr = &convctx;

        const int tty_num = FindFreeTTY();
        const std::string ttyPath = tty_num > 0
            ? "/dev/tty" + std::to_string(tty_num)
            : "/dev/tty1";

        int status_pipe[2];  // Status communication
        int control_pipe[2]; // Shutdown signaling

        if (pipe(status_pipe) == -1 || pipe(control_pipe) == -1)
        {
            _logger->Err("Failed to create pipes for session management");
            return false;
        }

        const pid_t pid = fork();
        if (pid == -1)
        {
            close(status_pipe[0]); close(status_pipe[1]);
            close(control_pipe[0]); close(control_pipe[1]);
            _logger->Err("fork() failed");
            return false;
        }

        if (pid == 0)
        {
            // CHILD PROCESS (Session Leader)
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

        // PARENT PROCESS
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
            _logger->Err("Child process failed to create PAM session");
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
        auto pwd = getpwnam(username.c_str());
        if (!pwd)
        {
            _logger->Err(std::string("Failed to get user: ") + username);
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

        outServiceName = "session@" + sessionId + ".service";

        msg = dbus_message_new_method_call(
            "org.freedesktop.systemd1",
            "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager",
            "StartTransientUnit"
        );
        if (!msg)
        {
            _logger->Err("Failed to allocate D-Bus message for StartTransientUnit");
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
            _logger->Err("Failed to append StartTransientUnit arguments (unit/mode)");
            cleanupDbus();
            return false;
        }

        // Properties array: a(sv)
        DBusMessageIter arrayIter;
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(sv)", &arrayIter);

        // Helper: add string property
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
            _logger->Err("Failed to append basic properties to StartTransientUnit");
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
                _logger->Err("Failed to append Environment key");
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
                _logger->Err("Failed to append Environment values");
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
                _logger->Err("Failed to append ExecStart key");
                dbus_message_iter_close_container(&iter, &arrayIter);
                cleanupDbus();
                return false;
            }

            dbus_message_iter_open_container(
                &structIter,
                DBUS_TYPE_VARIANT,
                "a(sasb)",
                &variantIter
            );

            dbus_message_iter_open_container(
                &variantIter,
                DBUS_TYPE_ARRAY,
                "(sasb)",
                &arrayIter2
            );

            // One ExecStart entry
            dbus_message_iter_open_container(
                &arrayIter2,
                DBUS_TYPE_STRUCT,
                nullptr,
                &innerStructIter
            );

            auto path = "/usr/bin/cage";
            if (!dbus_message_iter_append_basic(&innerStructIter, DBUS_TYPE_STRING, &path))
            {
                _logger->Err("Failed to append ExecStart path");
                dbus_message_iter_close_container(&arrayIter2, &innerStructIter);
                dbus_message_iter_close_container(&variantIter, &arrayIter2);
                dbus_message_iter_close_container(&structIter, &variantIter);
                dbus_message_iter_close_container(&arrayIter, &structIter);
                cleanupDbus();
                return false;
            }

            // Arguments array
            dbus_message_iter_open_container(
                &innerStructIter,
                DBUS_TYPE_ARRAY,
                "s",
                &arrayIter3
            );

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
                _logger->Err("Failed to append ExecStart args");
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

        // Send msg and wait for reply
        reply = dbus_connection_send_with_reply_and_block(
            _rawConn,
            msg,
            DBUS_DEFAULT_SAFE_TIMEOUT,
            &err
        );
        dbus_message_unref(msg);
        msg = nullptr;

        if (dbus_error_is_set(&err))
        {
            _logger->Err(std::string("StartTransientUnit failed: ") + (err.message ? err.message : "unknown"));
            dbus_error_free(&err);
            cleanupDbus();
            return false;
        }

        if (!reply)
        {
            _logger->Err("No reply from systemd for StartTransientUnit");
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
        const auto it = _sessions.find(sessionId);
        if (it == _sessions.end())
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_INVALID_ARGS,
                "No such session"
            );
            return;
        }

        auto& session = it->second;

        if (callerUid != session.uid && !PolkitAuthorizeStop(callerUid, sessionId))
        {
            _logger->Warn("Unauthorized StopSession attempt by UID " + std::to_string(callerUid)
                         + " for session " + sessionId);
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_ACCESS_DENIED,
                "Not authorized"
            );
            return;
        }

        if (!StopSession(sessionId, session))
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_FAILED,
                "Failed to stop session. Check logs for more information."
            );
        }

        SendSuccessReplyAndLogLegacy(_rawConn, msg);
        _logger->Info("Stopped session " + sessionId + " successfully");
    }

    bool SessionManagerService::StopSession(const std::string& sessionId, SessionInfo& session)
    {
        // --- Terminate user processes ---
        if (!TerminateUserSessionProcesses(sessionId))
        {
            _logger->Warn("Failed to terminate processes for session " + sessionId);
            // Continue cleanup anyway, but client will know something went wrong
        }

        // --- Signal child to clean up PAM and exit ---
        TerminatePAMForSession(session);

        // --- Remove session from map ---
        _sessions.erase(sessionId);

        // --- Spawn greeter if needed ---
        // TODO: Switch to other session or greeter when logged out, if there are no sessions, just create a login session
        if (!_isGreeterActive && _sessions.empty())
        {
            if (!CreateLoginSession())
                _logger->Warn("Failed to spawn greeter after stopping session " + sessionId);
        }

        return true;
    }

    bool SessionManagerService::PolkitAuthorizeStop(const uid_t callerUid, const std::string& sessionId)
    {
        // TODO: Use Polkit
        if (callerUid == 0) return true;
        return false;
    }

    bool SessionManagerService::TerminateUserSessionProcesses(const std::string& sessionId)
    {
        const auto it = _sessions.find(sessionId);
        if (it == _sessions.end() || it->second.scopeName.empty())
        {
            _logger->Warn("TerminateUserSessionProcesses called for unknown or empty session: " + sessionId);
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
            _logger->Err("Failed to allocate D-Bus message for StopUnit");
            return false;
        }

        const char* scopeName = scope.c_str();
        auto mode = "fail";

        if (!dbus_message_append_args(msg,
                                      DBUS_TYPE_STRING, &scopeName,
                                      DBUS_TYPE_STRING, &mode,
                                      DBUS_TYPE_INVALID))
        {
            _logger->Err("Failed to append arguments to StopUnit message for scope: " + scope);
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
                _logger->Err(
                    "StopUnit failed for scope "
                    + scope
                    + ": "
                    + std::string(err.message ? err.message : "unknown")
                );
                dbus_error_free(&err);
            }
            else _logger->Err("StopUnit returned no reply for scope " + scope);
            return false;
        }

        dbus_message_unref(reply);
        _logger->Info("Successfully stopped processes for session " + sessionId + " (scope: " + scope + ")");
        return true;
    }

    void SessionManagerService::TerminatePAMForSession(SessionInfo& session) const
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
                    _logger->Warn(
                        "Child process did not exit gracefully, forcing termination"
                    );
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

    void SessionManagerService::ListSessions(DBusMessage* msg, const uid_t callerUid)
    {
        // --- Authorization ---
        if (callerUid != 0)
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_ACCESS_DENIED,
                "Only root may list sessions"
            );
            return;
        }

        // --- Allocate reply message ---
        DBusMessage* reply = dbus_message_new_method_return(msg);
        if (!reply)
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_NO_MEMORY,
                "Failed to allocate D-Bus reply"
            );
            return;
        }

        DBusMessageIter iter;
        dbus_message_iter_init_append(reply, &iter);

        // --- Open array container a(ssuss) ---
        DBusMessageIter arrayIter;
        if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(ssuss)", &arrayIter))
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_NO_MEMORY,
                "Failed to open array container for ListSessions"
            );
            dbus_message_unref(reply);
            return;
        }

        for (const auto&[id, sessInfo] : _sessions)
        {
            const std::string& sessionId = id;
            const SessionInfo& s = sessInfo;

            const char* sid = sessionId.c_str();
            const char* user = s.username.c_str();
            dbus_uint32_t uid = s.uid;
            const char* seat = s.seat.c_str();
            const char* scope = s.scopeName.c_str();

            DBusMessageIter structIter;
            if (!dbus_message_iter_open_container(
                &arrayIter,
                DBUS_TYPE_STRUCT,
                nullptr,
                &structIter))
            {
                _logger->Warn("Failed to open struct container for session " + sessionId);
                continue; // skip this session but continue others
            }

            if (!dbus_message_iter_append_basic(&structIter, DBUS_TYPE_STRING, &sid) ||
                !dbus_message_iter_append_basic(&structIter, DBUS_TYPE_STRING, &user) ||
                !dbus_message_iter_append_basic(&structIter, DBUS_TYPE_UINT32, &uid) ||
                !dbus_message_iter_append_basic(&structIter, DBUS_TYPE_STRING, &seat) ||
                !dbus_message_iter_append_basic(&structIter, DBUS_TYPE_STRING, &scope))
            {
                _logger->Warn("Failed to append session data for session " + sessionId);
                dbus_message_iter_close_container(&arrayIter, &structIter);
                continue;
            }

            dbus_message_iter_close_container(&arrayIter, &structIter);
        }

        dbus_message_iter_close_container(&iter, &arrayIter);

        // --- Send reply ---
        if (!dbus_connection_send(_rawConn, reply, nullptr))
        {
            SendErrorReplyAndLogLegacy(
                _rawConn,
                msg,
                DBUS_ERROR_FAILED,
                "Failed to send ListSessions reply over D-Bus"
            );
            dbus_message_unref(reply);
            return;
        }

        dbus_connection_flush(_rawConn);
        dbus_message_unref(reply);

        _logger->Info(
            "ListSessions reply sent successfully (total sessions: " + std::to_string(_sessions.size()) + ")"
        );
    }

    void SessionManagerService::OnJobRemoved(const std::string& unitName)
    {
        // Only handle our own transient units
        const auto it = std::ranges::find(_pendingUnits, unitName);
        if (it == _pendingUnits.end()) return;

        // Query the MainPID for this unit
        if (const pid_t pid = GetUnitMainPID(_rawConn, unitName); pid > 0)
        {
            _logger->Debug("Got MainPID for " + unitName + ": " + std::to_string(pid));

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
                    _logger->Err("Failed to get session info by unit name: " + unitName);
                }
            }
            else
            {
                _logger->Err("Failed to get session info by unit name: " + unitName);
            }
        }
        else
        {
            _logger->Warn("Failed to get MainPID for " + unitName);
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
        DBusMessage* msg = dbus_message_new_method_call(
            "org.freedesktop.login1",
            "/org/freedesktop/login1",
            "org.freedesktop.login1.Manager",
            "ListSessions"
        );
        if (!msg)
        {
            _logger->Err("Failed to allocate D-Bus message for ListSessions");
            return "";
        }

        DBusError err;
        dbus_error_init(&err);

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
                _logger->Err(
                    "ListSessions call failed: " + std::string(err.message ? err.message : "unknown")
                );
                dbus_error_free(&err);
            }
            else
            {
                _logger->Err("ListSessions returned no reply");
            }
            return "";
        }

        std::string foundSessionId;

        DBusMessageIter iter;
        if (!dbus_message_iter_init(reply, &iter))
        {
            _logger->Warn("ListSessions reply is empty");
            dbus_message_unref(reply);
            return "";
        }

        if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
        {
            _logger->Err("Unexpected ListSessions reply type, expected array");
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
                _logger->Err("Object path expected");

            if (sessionUid == uid && sessionId && seatId && *seatId != '\0')
            {
                if (objPath && objPath[0] == '/')
                {
                    foundSessionId = sessionId;
                    outObjectPath = objPath;
                    break;
                }
                else
                {
                    _logger->Err("Invalid object path for session " + std::string(sessionId ? sessionId : "(null)"));
                }
            }

            dbus_message_iter_next(&arrayIter);
        }

        dbus_message_unref(reply);

        if (!foundSessionId.empty())
            _logger->Info("Found session " + foundSessionId + " for UID " + std::to_string(uid));
        else
            _logger->Info("No session found for UID " + std::to_string(uid));

        return foundSessionId;
    }

    std::string SessionManagerService::QueryLogindSeatForSession(const std::string& sessionId,
                                                                 const std::string& sessionObjectPath) const
    {
        if (sessionObjectPath.empty() || sessionObjectPath[0] != '/')
        {
            _logger->Err("Invalid session object path for session " + sessionId + ": '" + sessionObjectPath + "'");
            return "seat0";
        }

        DBusMessage* msg = dbus_message_new_method_call(
            "org.freedesktop.login1",
            sessionObjectPath.c_str(),
            "org.freedesktop.DBus.Properties",
            "Get"
        );

        if (!msg)
        {
            _logger->Err("Failed to allocate D-Bus message");
            return "seat0";
        }

        auto iface = "org.freedesktop.login1.Session";
        auto prop  = "Seat";
        if (!dbus_message_append_args(msg,
                                      DBUS_TYPE_STRING, &iface,
                                      DBUS_TYPE_STRING, &prop,
                                      DBUS_TYPE_INVALID))
        {
            _logger->Err("Failed to append D-Bus args");
            dbus_message_unref(msg);
            return "seat0";
        }

        DBusError err;
        dbus_error_init(&err);

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(
            _rawConn,
            msg,
            DBUS_DEFAULT_SAFE_TIMEOUT,
            &err
        );
        dbus_message_unref(msg);

        if (!reply)
        {
            _logger->Err("D-Bus call failed: " + std::string(err.message ? err.message : "unknown"));
            dbus_error_free(&err);
            return "seat0";
        }

        DBusMessageIter iter;
        if (!dbus_message_iter_init(reply, &iter))
        {
            _logger->Warn("Empty D-Bus reply");
            dbus_message_unref(reply);
            return "seat0";
        }

        if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
        {
            _logger->Err("Unexpected D-Bus reply type (expected variant)");
            dbus_message_unref(reply);
            return "seat0";
        }

        DBusMessageIter variantIter;
        dbus_message_iter_recurse(&iter, &variantIter);

        const char* seatPath = nullptr;

        if (const int argType = dbus_message_iter_get_arg_type(&variantIter); argType == DBUS_TYPE_OBJECT_PATH)
        {
            // Old systemd (< 251)
            dbus_message_iter_get_basic(&variantIter, &seatPath);
        }
        else if (argType == DBUS_TYPE_STRUCT)
        {
            // Newer systemd (>= 251) returns (string, object_path)
            DBusMessageIter structIter;
            dbus_message_iter_recurse(&variantIter, &structIter);

            // 1:st element: seat ID (string)
            if (dbus_message_iter_get_arg_type(&structIter) == DBUS_TYPE_STRING)
            {
                const char* seatId = nullptr;
                dbus_message_iter_get_basic(&structIter, &seatId);
                dbus_message_iter_next(&structIter);

                // 2:nd element: seat path (object_path)
                if (dbus_message_iter_get_arg_type(&structIter) == DBUS_TYPE_OBJECT_PATH)
                {
                    dbus_message_iter_get_basic(&structIter, &seatPath);
                }
            }
        }

        if (!seatPath)
        {
            _logger->Err("QueryLogindSeatForSession: could not extract seat object path");
        }

        dbus_message_unref(reply);

        if (!seatPath)
        {
            _logger->Warn("No seat found, defaulting to seat0");
            return "seat0";
        }

        const std::string seatStr(seatPath);
        if (const auto pos = seatStr.find_last_of('/'); pos != std::string::npos)
        {
            std::string seat = seatStr.substr(pos + 1);
            _logger->Info("Session " + sessionId + " is on seat " + seat);
            return seat;
        }

        _logger->Warn("Malformed seat path (" + seatStr + "), defaulting");
        return "seat0";
    }

    bool SessionManagerService::ActivateLogindSession(const std::string& sessionId, const std::string& seat) const
    {
        if (sessionId.empty())
        {
            _logger->Err("ActivateLogindSession called with empty sessionId");
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
            _logger->Err("Failed to allocate D-Bus message in ActivateLogindSession");
            return false;
        }

        const char* sid = sessionId.c_str();
        if (!dbus_message_append_args(msg,
                                      DBUS_TYPE_STRING, &sid,
                                      DBUS_TYPE_STRING, &seat,
                                      DBUS_TYPE_INVALID))
        {
            _logger->Err("Failed to append arguments in ActivateLogindSession");
            dbus_message_unref(msg);
            return false;
        }

        DBusError err;
        dbus_error_init(&err);

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
                _logger->Err("ActivateLogindSession failed: " + std::string(err.message ? err.message : "unknown"));
                dbus_error_free(&err);
            }
            else _logger->Err("ActivateLogindSession: no reply received");
            return false;
        }

        dbus_message_unref(reply);
        _logger->Info("Activated logind session: " + sessionId);
        return true;
    }

    pid_t SessionManagerService::GetUnitMainPID(DBusConnection* conn, const std::string& unitName)
    {
        DBusError err;
        dbus_error_init(&err);
        DBusMessage* msg = nullptr;
        DBusMessage* reply = nullptr;
        pid_t pid = 0;

        // --- 1) Get the unit object path ---
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

        // --- 2) Get MainPID property ---
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

        _logger->Warn("Generated fallback session ID: " + sessionId);

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

    int SessionManagerService::PAMConversation(const int num_msg,
                                               const pam_message** msg,
                                               pam_response** resp,
                                               void* appdata_ptr)
    {
        const auto* ctx = static_cast<PamConversationCtx*>(appdata_ptr);
        if (num_msg <= 0) return PAM_CONV_ERR;
        pam_response* r = static_cast<struct pam_response*>(calloc(num_msg, sizeof(*r)));
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
                // TODO: Handle other message styles as needed
                r[i].resp = nullptr;
            }
        }
        *resp = r;
        return PAM_SUCCESS;
    }

}