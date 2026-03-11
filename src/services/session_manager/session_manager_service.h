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

#pragma once
#include <security/pam_appl.h>
#include <string>
#include <cstring>
#include <pwd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/vt.h>
#include <cstdint>
#include <chrono>
#include <atomic>
#include <fcntl.h>
#include <fstream>

#include "../service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Logger {
    class LoggerService;
}

namespace JappeStudios::JappeOS::JappeOSCore::Services::SessionManager
{
    struct PamConversationCtx
    {
        std::string password;
    };

    struct SessionInfo
    {
        std::string        username;
        std::string        scopeName;
        uid_t              uid;
        pid_t              leaderPid;
        int                controlFd;
        std::string        seat;
        std::vector<pid_t> privilegedClientProcesses;
    };

    class SessionManagerService : public Service
    {
    public:
        explicit SessionManagerService(ServiceManager* serviceManager, Connection* conn);
        ~SessionManagerService() override;

        bool HandleMethodCallLegacy(DBusMessage* msg) override;
        [[nodiscard]] std::string GetName() const override { return "SessionManagerService"; }

        [[nodiscard]] bool IsLiveEnvironment() const { return _isLiveEnvironment; }

                      bool IsManagedUserSession(uid_t uid);
        [[nodiscard]] bool IsPrivilegedClientProcess(pid_t pid, bool allowChildProcesses = false) const;

    public:
        const bool USE_SESSION_MANAGER_CREATE_VT_SWITCH_DELAY = false; // TODO: (used for debugging)
        const char* JOS_CORE_SESSION_BINARY = "/jappeos/jappeos_session";
        const char* PAM_GREETER_SERVICE     = "jappeos-greeter";
        const char* PAM_LOGIN_SERVICE       = "jappeos-login";
        const char* JOS_DESKTOP_NAME        = "JappeOS Desktop";
        const char* JOS_GREETER_USER        = "jos-greeter";
        const char* JOS_INSTALLER_BINARY    = "/jappeos/installer/installer";

    private:
        bool CreateLoginSession();
        bool StopLoginSession();
        void CreateSession(DBusMessage* msg, uid_t callerUid, const std::string& username, const std::string& password);
        void SendDBusReply_CreateSession(DBusMessage* msg,
                                         const std::string& sessionId,
                                         uid_t uid,
                                         const std::string& seat);
        bool AuthenticateAndOpenPAMSession(const std::string& service,
                                           const std::string& username,
                                           const std::string& password,
                                           int& out_controlFd,
                                           pid_t& out_childPid);
        bool SpawnUserSessionProcesses(bool isLoginSession,
                                       const std::string& username,
                                       const std::string& sessionId,
                                       const std::string& seat,
                                       std::string& outServiceName) const;
        void StopSessionDbus(DBusMessage* msg, uid_t callerUid, const std::string& sessionId);
        bool StopSession(const std::string& sessionId, SessionInfo& session);
        bool TerminateUserSessionProcesses(const std::string& sessionId);
        void TerminatePAMForSession(SessionInfo& session) const;
        bool PolkitAuthorizeStop(uid_t callerUid, const std::string& sessionId);
        void ListSessions(DBusMessage* msg, uid_t callerUid);
        void OnJobRemoved(const std::string& unitName);
        [[nodiscard]] bool IsGreeter(uid_t callerUid) const;
        [[nodiscard]] std::string QueryLogindSessionForUid(uid_t uid, std::string& outObjectPath) const;
        [[nodiscard]] std::string QueryLogindSeatForSession(const std::string& sessionId,
                                                            const std::string& sessionObjectPath) const;
        [[nodiscard]] bool ActivateLogindSession(const std::string& sessionId, const std::string& seat) const;
        pid_t GetUnitMainPID(DBusConnection* conn, const std::string& unitName);
        [[nodiscard]] std::string GenerateFallbackSessionId() const;
        int FindFreeTTY();
        bool GetSessionIdByUnitName(const std::string& unitName, std::string& outSessionId);
        [[nodiscard]] std::optional<pid_t> GetParentPid(pid_t pid) const;

    private:
        Logger::LoggerService* _logger;
        std::map<std::string, pam_handle_t*> _activePAMHandles;
        std::map<std::string, SessionInfo>   _sessions;
        std::vector<std::string>             _pendingUnits;
        std::string _greeterSessionID;
        bool        _isGreeterActive = false;
        bool        _isLiveEnvironment;

    private:
        static int PAMConversation(int num_msg,
                                   const pam_message** msg,
                                   pam_response** resp,
                                   void* appdata_ptr);
    };
}
