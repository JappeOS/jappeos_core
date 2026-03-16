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
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <pwd.h>
#include <glib.h>
#include <linux/vt.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "../../utils/dbus_utils.h"
#include "../../utils/scope_guard.h"

using namespace JappeStudios::JappeOS::JappeOSCore::Utils;

namespace JappeStudios::JappeOS::JappeOSCore::Services::SessionManager
{

    SessionManagerService::SessionManagerService(ServiceManager* serviceManager, Connection* conn) :
                                                       Service(serviceManager, conn),
                                                       _object(*_conn, GetBaseObjectPath()),
                                                       _iface(_object.CreateInterface(GetBaseInterface()))
    {
        _iface.RegisterMethod("CreateSession", [&](const auto& m) { OnCreateSession(m); });
        _iface.RegisterMethod("StopSession",   [&](const auto& m) { OnStopSession(m); });
        _iface.RegisterMethod("ListSessions",  [&](const auto& m) { OnListSessions(m); });

        _subJobRemoved = std::make_unique<SignalSubscription>(_conn->SubscribeSignal(
            "org.freedesktop.systemd1",
            ObjectPath("/org/freedesktop/systemd1"),
            InterfaceName("org.freedesktop.systemd1.Manager"),
            "JobRemoved",
            [&](const Message& msg) { HandleJobRemoved(msg); }
        ));

        _isLiveEnvironment = false;
        CreateLoginSession();
    }

    SessionManagerService::~SessionManagerService()
    {
        // Stop all sessions
        for (auto &sess: _sessions | std::views::values)
        {
            try
            {
                TerminateUserSessionProcesses(sess.id);
            }
            catch (const std::exception& e)
            {
                // Continue cleanup anyway, but client will know something went wrong
                Log().Warn(std::format(
                    "Failed to terminate processes for session `{}`: {}",
                    sess.id,
                    e.what()
                ));
            }

            try
            {
                TerminatePAMForSession(sess);
            }
            catch (const std::exception& e)
            {
                Log().Warn(std::format(
                    "Failed to terminate PAM for session `{}`: {}",
                    sess.id,
                    e.what()
                ));
            }
        }

        _activePAMHandles.clear();
        _sessions.clear();
        _greeterSessionID = {};
        _pendingUnits.clear();
        _isGreeterActive = false;
    }

    bool SessionManagerService::IsManagedUserSession(uid_t uid) const
    {
        return std::ranges::any_of(_sessions, [&](auto const& e){ return e.second.uid == uid; });
    }

    bool SessionManagerService::IsPrivilegedClientProcess(const pid_t pid, const bool allowChildProcesses) const
    {
        if (allowChildProcesses)
        {
            Log().Warn(
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

    // TODO: Polkit
    void SessionManagerService::SharedPolicy(const Message& message) const
    {
        const auto sender = message.GetSender();
        const auto senderUid = _conn->GetUnixUser(sender);
        const pid_t senderPid = Utils::DBusUtils::GetSenderPID(_rawConn, message.GetRawMessage());

        if (!IsManagedUserSession(senderUid))
        {
            throw DBusException(DBUS_ERROR_ACCESS_DENIED, "Unknown user");
        }

        if (!IsPrivilegedClientProcess(senderPid, true))
        {
            throw DBusException(DBUS_ERROR_ACCESS_DENIED, "Unauthorized process");
        }
    }

    void SessionManagerService::OnCreateSession(const Message& message)
    {
        SharedPolicy(message);
        const auto [username, password] = message.GetArgs<std::string, std::string>();
        if (username.empty() || password.empty())
        {
            throw DBusException(
                DBUS_ERROR_INVALID_ARGS,
                "Missing username or password"
            );
        }

        const auto sender = message.GetSender();
        const auto senderUid = _conn->GetUnixUser(sender);
        if (senderUid != 0 && !IsGreeter(senderUid))
        {
            Log().Warn("Unauthorized CreateSession attempt by UID " + std::to_string(senderUid));
            throw DBusException(
                DBUS_ERROR_ACCESS_DENIED,
                "Only greeter may call CreateSession"
            );
        }

        std::string sessionId;
        uid_t uid;
        std::string seat;
        CreateSession(username, password, sessionId, uid, seat, false);

        auto reply = Message::CreateMethodReturn(message);
        reply.SetArgs(sessionId, static_cast<uint32_t>(uid), seat);
        reply.Send(*_conn);
    }

    void SessionManagerService::OnStopSession(const Message& message)
    {
        SharedPolicy(message);
        const auto [sessionId] = message.GetArgs<std::string>();
        if (sessionId.empty())
        {
            throw DBusException(
                DBUS_ERROR_INVALID_ARGS,
                "Missing sessionId"
            );
        }

        const auto it = _sessions.find(sessionId);
        if (it == _sessions.end())
        {
            throw DBusException(
                DBUS_ERROR_INVALID_ARGS,
                "No such session"
            );
        }

        const auto& session = it->second;
        const auto sender = message.GetSender();
        const auto senderUid = _conn->GetUnixUser(sender);
        if (senderUid != session.uid /*&& !PolkitAuthorizeStop(senderUid, sessionId)*/)
        {
            Log().Warn("Unauthorized StopSession attempt by UID " + std::to_string(senderUid)
                         + " for session " + sessionId);

            throw DBusException(
                DBUS_ERROR_ACCESS_DENIED,
                "Not authorized"
            );
        }

        StopSession(sessionId, false);
        Message::CreateMethodReturn(message).Send(*_conn);
    }

    void SessionManagerService::OnListSessions(const Message& message) const
    {
        SharedPolicy(message);
        const auto sender = message.GetSender();
        const auto senderUid = _conn->GetUnixUser(sender);
        if (senderUid != 0)
        {
            throw DBusException(
                DBUS_ERROR_ACCESS_DENIED,
                "Only root may list sessions"
            );
        }

        std::vector<std::tuple<
            std::string,
            std::string,
            uint32_t,
            std::string,
            std::string>> list{};

        for (const auto&[id, sessInfo] : _sessions)
        {
            const std::string& sessionId = id;
            const SessionInfo& s = sessInfo;

            const char* sid = sessionId.c_str();
            const char* user = s.username.c_str();
            dbus_uint32_t uid = s.uid;
            const char* seat = s.seat.c_str();
            const char* scope = s.scopeName.c_str();

            list.push_back(std::make_tuple(
                sid,
                user,
                uid,
                seat,
                scope
            ));
        }

        auto reply = Message::CreateMethodReturn(message);
        reply.SetArgs(list);
        reply.Send(*_conn);
    }

    void SessionManagerService::CreateLoginSession()
    {
        if (_isGreeterActive)
        {
            Log().Notice("Tried to spawn greeter while greeter was already active");
            return;
        }

        std::string sessionId;
        std::string _0;
        uid_t _1;
        CreateSession(JOS_GREETER_USER, "", sessionId, _1, _0, true);
        _greeterSessionID = sessionId;
        _isGreeterActive = true;
        Log().Info("Greeter session initialized successfully");
    }

    void SessionManagerService::StopLoginSession()
    {
        if (!_isGreeterActive)
        {
            Log().Notice("StopLoginSession called but greeter is not active");
            return;
        }

        StopSession(_greeterSessionID, true);
        _greeterSessionID = {};
        _isGreeterActive = false;
        Log().Info("Greeter session stopped successfully");
    }

    void SessionManagerService::CreateSession(const std::string& username,
                                                 const std::string& password,
                                                 std::string& outSessionId,
                                                 uid_t& outUid,
                                                 std::string& outSeat,
                                                 const bool isLoginSession)
    {
        // --- 1) Lookup UID ---
        const passwd* pwd = getpwnam(username.c_str());
        if (!pwd)
        {
            Log().Err("CreateSession: unknown user: " + username);
            throw DBusException(DBUS_ERROR_FAILED, "Unknown user");
        }
        const uid_t uid = pwd->pw_uid;

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

        ScopeGuard guard0(cleanupPam);

        try
        {
            AuthenticateAndOpenPAMSession(
                isLoginSession ? PAM_GREETER_SERVICE : PAM_LOGIN_SERVICE,
                username,
                password,
                controlFd,
                leaderPid
            );
        }
        catch (const std::exception& e)
        {
            Log().Notice(std::format("AuthenticateAndOpenPAMSession failed for `{}`: {}", username, e.what()));
            throw DBusException(DBUS_ERROR_AUTH_FAILED, "Authentication failed");
        }

        // --- 3) Session ID ---
        ObjectPath objectPath;
        const std::string sessionId = QueryLogindSessionForUid(pwd->pw_uid, objectPath);
        if (sessionId.empty())
        {
            Log().Err("No logind session found for user UID: " + std::to_string(pwd->pw_uid));
            throw DBusException(DBUS_ERROR_FAILED, "Could not determine session ID");
        }

        if (_sessions.contains(sessionId))
        {
            Log().Err("Duplicate sessionId detected: " + sessionId);
            throw DBusException(DBUS_ERROR_FAILED, "Duplicate session ID");
        }

        // --- 4) Query seat ---
        std::string seat = QueryLogindSeatForSession(sessionId, objectPath);
        if (seat.empty())
        {
            Log().Warn("No seat found for session: " + sessionId);
            seat = "seat0"; // safe fallback
        }

        // --- 5) Spawn compositor/desktop ---
        std::string scopeName;
        SpawnUserSessionProcesses(isLoginSession, username, sessionId, seat, scopeName);
        ScopeGuard guard1([&]{ TerminateUserSessionProcesses(sessionId); });
        if (scopeName.empty())
        {
            Log().Err("SpawnUserSessionProcesses returned empty scopeName for: " + username);
            throw DBusException(DBUS_ERROR_FAILED, "Invalid session scope");
        }

        // --- 6) Insert into session map ---
        _sessions[sessionId] =
        {
            sessionId,
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

        // --- 7) Schedule VT switch and login session stop ---
        try
        {
            ActivateLogindSession(sessionId, seat);
        }
        catch (const std::exception& e)
        {
            Log().Err(std::format(
                "Failed to activate logind session for user `{}`: {}",
                username,
                e.what()
            ));
        }

        if (!isLoginSession)
        {
            try
            {
                StopLoginSession();
            }
            catch (const std::exception& e)
            {
                Log().Err(std::format(
                    "Failed to stop login session when starting session for user `{}`: {}",
                    username,
                    e.what()
                ));
            }
        }

        /*struct ScheduleData
        {
            SessionManagerServiceNew* self;
            std::string username;
            std::string sessionId;
            std::string seat;
            bool isLoginSession;
        };
        auto* userData = new ScheduleData{this, username, sessionId, seat, isLoginSession};
        g_timeout_add_once(
            USE_SESSION_MANAGER_CREATE_VT_SWITCH_DELAY ? 1000 : 0,
            [](const gpointer d) {
                const std::unique_ptr<ScheduleData> data(static_cast<ScheduleData*>(d));
                if (!data || !data->self) return;

                try
                {
                    data->self->ActivateLogindSession(data->sessionId, data->seat);
                }
                catch (const std::exception& e)
                {
                    Log().Err(std::format(
                        "Failed to activate logind session for user `{}`: {}",
                        data->username,
                        e.what()
                    ));
                }

                if (data->isLoginSession) return;

                try
                {
                    data->self->StopLoginSession();
                }
                catch (const std::exception& e)
                {
                    Log().Err(std::format(
                        "Failed to stop login session for user `{}`: {}",
                        data->username,
                        e.what()
                    ));
                }
            },
            userData
        );*/

        // --- 8) Return data ---
        outSessionId = sessionId;
        outUid = uid;
        outSeat = seat;
        Log().Info("Created session `" + sessionId + "` for user `" + username + "`.");
        guard0.Dismiss();
        guard1.Dismiss();
    }

    void SessionManagerService::StopSession(const std::string& sessionId, bool isLoginSession)
    {
        const auto it = _sessions.find(sessionId);
        if (it == _sessions.end())
        {
            throw DBusException(
                DBUS_ERROR_INVALID_ARGS,
                "No such session"
            );
        }

        auto& session = it->second;

        try
        {
            TerminateUserSessionProcesses(sessionId);
        }
        catch (const std::exception& e)
        {
            // Continue cleanup anyway, but client will know something went wrong
            Log().Warn(std::format(
                "Failed to terminate processes for session `{}`: {}",
                sessionId,
                e.what()
            ));
        }

        try
        {
            TerminatePAMForSession(session);
        }
        catch (const std::exception& e)
        {
            Log().Warn(std::format(
                "Failed to terminate PAM for session `{}`: {}",
                sessionId,
                e.what()
            ));
        }

        _sessions.erase(sessionId);

        // TODO: Switch to other session or greeter when logged out, if there are no sessions, just create a login session
        if (!isLoginSession && !_isGreeterActive)
        {
            if (_sessions.empty())
            {
                try
                {
                    CreateLoginSession();
                }
                catch (const std::exception& e)
                {
                    Log().Warn(std::format(
                        "Failed to spawn greeter after stopping session `{}`: {}",
                        sessionId,
                        e.what()
                    ));
                }
            }
            else
            {
                const auto& target = _sessions.begin();
                ActivateLogindSession(target->second.id, target->second.seat);
            }
        }

        Log().Info("Stopped session `" + sessionId + "` of user `" + session.username + "`.");
    }

    void SessionManagerService::AuthenticateAndOpenPAMSession(const std::string& service,
                                                                 const std::string& username,
                                                                 const std::string& password,
                                                                 int& outControlFd,
                                                                 pid_t& outChildPid)
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
            throw std::runtime_error("Failed to create pipes for session management");
        }

        const pid_t pid = fork();
        if (pid == -1)
        {
            close(status_pipe[0]); close(status_pipe[1]);
            close(control_pipe[0]); close(control_pipe[1]);
            throw std::runtime_error("fork() failed");
        }

        if (pid == 0)
        {
            // CHILD PROCESS (Session Leader)
            close(status_pipe[0]);   // Close read end of status pipe
            close(control_pipe[1]);  // Close write end of control pipe

            pam_handle_t* pamh = nullptr;
            int retval;
            char status = '0';

            int ttyfd = ActivateTTY(tty_num);
            if (ttyfd < 0)
            {
                write(status_pipe[1], &status, 1);
                close(status_pipe[1]);
                close(control_pipe[0]);
                _exit(1);
            }

            // Initialize PAM in child
            retval = pam_start(service.c_str(), username.c_str(), &conv, &pamh);
            if (retval != PAM_SUCCESS)
            {
                write(status_pipe[1], &status, 1);
                close(status_pipe[1]);
                close(control_pipe[0]);
                close(ttyfd);
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
                close(ttyfd);
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
                close(ttyfd);
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
                close(ttyfd);
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
            close(ttyfd);

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
            throw std::runtime_error("Child process failed to create PAM session");
        }

        // Store the control pipe for later use
        outControlFd = control_pipe[1];
        outChildPid = pid;

        explicit_bzero(convctx.password.data(), convctx.password.size());
    }

    void SessionManagerService::SpawnUserSessionProcesses(const bool isLoginSession,
                                                             const std::string& username,
                                                             const std::string& sessionId,
                                                             const std::string& seat,
                                                             std::string& outServiceName) const
    {
        const auto pwd = getpwnam(username.c_str());
        if (!pwd)
        {
            throw std::runtime_error(std::string("Failed to get user: ") + username);
        }

        outServiceName = "session@" + sessionId + ".service";
        auto msg = Message::CreateMethodCall(
            "org.freedesktop.systemd1",
            ObjectPath("/org/freedesktop/systemd1"),
            InterfaceName("org.freedesktop.systemd1.Manager"),
            "StartTransientUnit"
        );

        std::vector<std::tuple<std::string, DBusVariant>> properties{};
        properties.emplace_back("Description", DBusVariant::make<std::string>("Desktop Session"));
        properties.emplace_back("Slice",       DBusVariant::make<std::string>("session.slice"));
        properties.emplace_back("User",        DBusVariant::make<std::string>(std::string(username)));

        const std::string path = isLoginSession ? JOS_GREETER_BINARY : JOS_DESKTOP_BINARY;
        auto lib = path.substr(0, path.find_last_of('/')) + "/lib";
        auto bin = path.substr(0, path.find_last_of('/')) + "/bin";
        properties.emplace_back("Environment", DBusVariant::make<std::vector<std::string>>({
            "XDG_SESSION_TYPE=wayland",
            "XDG_CURRENT_DESKTOP=" + std::string(JOS_DESKTOP_NAME),
            "XDG_RUNTIME_DIR=/run/user/" + std::to_string(pwd->pw_uid),
            "XDG_SEAT=" + seat,
            "XDG_SESSION_CLASS=user",
            "ZENITH_MULTI_MONITOR_MODE=extend",
            "LIBSEAT_BACKEND=logind",
            std::vformat("LD_LIBRARY_PATH=/usr/lib:/lib:{}", std::make_format_args(lib)),
            std::vformat(
                "PATH={}:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
                std::make_format_args(bin)
            ),
        }));

        std::vector<std::tuple<
            std::string,
            std::vector<std::string>,
            bool>> execStart{};

        execStart.push_back(std::make_tuple(
            path,                 // path
            std::vector{path}, // args
            false                 // isShell
        ));

        properties.emplace_back("ExecStart", DBusVariant::make(execStart));

        msg.SetArgs(
            outServiceName,            // name
            std::string("replace"),  // mode
            properties,                // properties
            std::vector<std::tuple<std::string, std::vector<std::tuple<std::string, DBusVariant>>>>{} // aux units
        );

        msg.SendWithReplyIgnore(*_conn);
    }

    void SessionManagerService::ActivateLogindSession(const std::string& sessionId, const std::string& seat) const
    {
        if (sessionId.empty())
        {
            throw std::runtime_error("ActivateLogindSession called with empty sessionId");
        }

        auto msg = Message::CreateMethodCall(
            "org.freedesktop.login1",
            ObjectPath("/org/freedesktop/login1"),
            InterfaceName("org.freedesktop.login1.Manager"),
            "ActivateSessionOnSeat"
        );

        msg.SetArgs(sessionId, seat);
        msg.SendWithReplyIgnore(*_conn);
        Log().Debug("Activated logind session: " + sessionId);
    }

    void SessionManagerService::TerminateUserSessionProcesses(const std::string& sessionId)
    {
        const auto it = _sessions.find(sessionId);
        if (it == _sessions.end() || it->second.scopeName.empty())
        {
            Log().Warn("TerminateUserSessionProcesses called for unknown or empty session: " + sessionId);
            return;
        }

        const std::string& scope = it->second.scopeName;
        auto msg = Message::CreateMethodCall(
            "org.freedesktop.systemd1",
            ObjectPath("/org/freedesktop/systemd1"),
            InterfaceName("org.freedesktop.systemd1.Manager"),
            "StopUnit"
        );

        msg.SetArgs(scope, std::string("fail"));
        msg.SendWithReplyIgnore(*_conn);
        Log().Debug("Successfully stopped processes for session `" + sessionId + "` (scope: " + scope + ")");
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
                    Log().Warn(
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

    void SessionManagerService::HandleJobRemoved(const Message& msg)
    {
        const auto [id, jobPath, unitName, result]
                = msg.GetArgs<uint32_t, ObjectPath, std::string, std::string>();

        const auto it = std::ranges::find(_pendingUnits, unitName);
        if (it == _pendingUnits.end()) return;
        _pendingUnits.erase(it);

        try
        {
            if (const pid_t pid = GetUnitMainPID(unitName); pid > 0)
            {
                Log().Debug("Got MainPID for " + unitName + ": " + std::to_string(pid));

                if (std::string sessionId; TryGetSessionIdByUnitName(unitName, sessionId))
                {
                    const auto it = _sessions.find(sessionId);
                    if (it != _sessions.end())
                    {
                        auto& session = it->second;
                        session.privilegedClientProcesses.push_back(pid);
                    }
                    else
                    {
                        Log().Err("Failed to get session info by unit name: " + unitName);
                    }
                }
                else
                {
                    Log().Err("Failed to get session info by unit name: " + unitName);
                }
            }
            else
            {
                Log().Warn("Failed to get MainPID for " + unitName);
            }
        }
        catch (const std::exception& e)
        {
            Log().Warn("Failed to get MainPID for `" + unitName + "` because of exception: " + e.what());
        }
    }

    bool SessionManagerService::IsGreeter(const uid_t callerUid) const
    {
        const auto it = _sessions.find(_greeterSessionID);
        if (it == _sessions.end())
            return false;

        if (callerUid == it->second.uid) return true;
        return false;
    }

    std::string SessionManagerService::QueryLogindSessionForUid(const uid_t uid, ObjectPath& outObjectPath) const noexcept
    {
        try
        {
            const auto msg = Message::CreateMethodCall(
                "org.freedesktop.login1",
                ObjectPath("/org/freedesktop/login1"),
                InterfaceName("org.freedesktop.login1.Manager"),
                "ListSessions"
            );

            const auto reply = msg.SendWithReply(*_conn);
            const auto [sessions] = reply.GetArgs<
                std::vector<
                    std::tuple<
                        std::string,
                        uint32_t,
                        std::string,
                        std::string,
                        ObjectPath
                    >
                >
            >();

            if (sessions.empty())
                throw std::runtime_error("No sessions");

            std::string foundSessionId;
            for (const auto& session : sessions)
            {
                const auto [
                    sessionId,
                    sessionUid,
                    username,
                    seatId,
                    objPath
                ] = session;

                if (objPath == ObjectPath())
                    Log().Err("Invalid object path");

                if (sessionUid == uid && !sessionId.empty() && !seatId.empty())
                {
                    foundSessionId = sessionId;
                    outObjectPath = objPath;
                    break;
                }
            }

            if (!foundSessionId.empty())
                Log().Debug("Found session " + foundSessionId + " for UID " + std::to_string(uid));
            else
                Log().Notice("No session found for UID " + std::to_string(uid));

            return foundSessionId;
        }
        catch (const std::exception& e)
        {
            Log().Err("QueryLogindSessionForUid error: " + std::string(e.what()));
            return "";
        }
    }

    std::string SessionManagerService::QueryLogindSeatForSession(const std::string& sessionId,
                                                                    const ObjectPath& sessionObjectPath) const noexcept
    {
        try
        {
            if (sessionObjectPath == ObjectPath())
                throw std::runtime_error("Invalid object path");

            auto msg = Message::CreateMethodCall(
                "org.freedesktop.login1",
                sessionObjectPath,
                InterfaceName("org.freedesktop.DBus.Properties"),
                "Get"
            );

            msg.SetArgs(std::string("org.freedesktop.login1.Session"), std::string("Seat"));
            const auto reply = msg.SendWithReply(*_conn);
            const auto [seatv] = reply.GetArgs<DBusVariant>();

            // Newer systemd (>= 251) returns (string, object_path)
            const auto [seatId, seatPath] = seatv.get<std::tuple<std::string, ObjectPath>>();

            if (seatPath == ObjectPath())
                throw std::runtime_error("Invalid object path");

            const auto seat = std::string(seatPath.Leaf());
            Log().Debug("Session " + sessionId + " is on seat " + seat);
            return seat;
        }
        catch (const std::exception& e)
        {
            Log().Warn("QueryLogindSeatForSession defaulting to seat0, because of error: " + std::string(e.what()));
            return "seat0";
        }
    }

    pid_t SessionManagerService::GetUnitMainPID(const std::string& unitName) const
    {
        if (unitName.empty())
            throw std::runtime_error("Empty unitName in GetUnitMainPID");

        auto msg = Message::CreateMethodCall(
            "org.freedesktop.systemd1",
            ObjectPath("/org/freedesktop/systemd1"),
            InterfaceName("org.freedesktop.systemd1.Manager"),
            "GetUnit"
        );

        msg.SetArgs(unitName);
        const auto reply = msg.SendWithReply(*_conn);
        const auto [unitPath] = reply.GetArgs<ObjectPath>();

        if (unitPath == ObjectPath())
            throw std::runtime_error("Invalid unit path in GetUnitMainPID");

        auto msg1 = Message::CreateMethodCall(
            "org.freedesktop.systemd1",
            unitPath,
            InterfaceName("org.freedesktop.DBus.Properties"),
            "Get"
        );

        msg1.SetArgs(std::string("org.freedesktop.systemd1.Service"), std::string("MainPID"));
        const auto reply1 = msg1.SendWithReply(*_conn);
        const auto [mainPidV] = reply1.GetArgs<DBusVariant>();
        const auto mainPid = mainPidV.get<uint32_t>();
        return static_cast<pid_t>(mainPid);
    }

    bool SessionManagerService::TryGetSessionIdByUnitName(const std::string& unitName, std::string& outSessionId)
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

    std::string SessionManagerService::GenerateFallbackSessionId()
    {
        static std::atomic<uint64_t> counter{0};

        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

        const uint64_t id = counter.fetch_add(1, std::memory_order_relaxed);

        std::string sessionId = "local-" + std::to_string(millis) + "-" + std::to_string(id);
        Log().Debug("Generated fallback session ID: " + sessionId);
        return sessionId;
    }

    int SessionManagerService::FindFreeTTY()
    {
        const int fd = open("/dev/tty0", O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return -1;

        int next = -1;
        if (ioctl(fd, VT_OPENQRY, &next) < 0)
            next = -1;

        close(fd);
        return next;
    }

    int SessionManagerService::ActivateTTY(int vt)
    {
        if (vt <= 0)
            return -1;

        char path[32];
        snprintf(path, sizeof(path), "/dev/tty%d", vt);

        const int fd = open(path, O_RDWR | O_CLOEXEC | O_NOCTTY);
        if (fd < 0)
        {
            fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
            return -1;
        }

        if (ioctl(fd, VT_ACTIVATE, vt) < 0)
        {
            fprintf(stderr, "VT_ACTIVATE failed for %s: %s\n", path, strerror(errno));
            close(fd);
            return -1;
        }

        if (ioctl(fd, VT_WAITACTIVE, vt) < 0)
        {
            fprintf(stderr, "VT_WAITACTIVE failed for %s: %s\n", path, strerror(errno));
            close(fd);
            return -1;
        }

        return fd; // keep open; caller owns fd
    }

    std::optional<pid_t> SessionManagerService::GetParentPid(pid_t pid)
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
