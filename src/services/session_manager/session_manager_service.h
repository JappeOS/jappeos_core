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

#include "../service.h"
#include "../../logger.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::SessionManager
{
    struct PamConversationCtx
    {
        std::string password;
    };

    struct SessionInfo
    {
        std::string        id;
        std::string        username;
        std::string        scopeName;
        uid_t              uid;
        pid_t              leaderPid;
        int                controlFd;
        std::string        seat;
        std::vector<pid_t> privilegedClientProcesses;
        std::string        activeState;
        std::string        result;
    };

    // TODO: Track session main process for crashes
    class SessionManagerService : public Service
    {
    public:
        explicit SessionManagerService(ServiceManager* serviceManager, Connection* conn);
        ~SessionManagerService() override;
        [[nodiscard]] std::string GetName() const override { return "SessionManagerService"; }

        [[nodiscard]] bool IsLiveEnvironment() const { return _isLiveEnvironment; }
        bool IsManagedUserSession(uid_t uid) const;
        [[nodiscard]] bool IsPrivilegedClientProcess(pid_t pid, bool allowChildProcesses = false) const;

    public:
        const bool USE_SESSION_MANAGER_CREATE_VT_SWITCH_DELAY = false; // TODO: (used for debugging)
        const char* JOS_CORE_SESSION_BINARY = "/jappeos/jappeos_session";
        const char* JOS_INSTALLER_BINARY    = "/jappeos/installer/installer";
        const char* JOS_DESKTOP_BINARY      = "/jappeos/desktop/desktop";
        const char* JOS_GREETER_BINARY      = "/jappeos/greeter/greeter";
        const char* PAM_GREETER_SERVICE     = "jappeos-greeter";
        const char* PAM_LOGIN_SERVICE       = "jappeos-login";
        const char* JOS_DESKTOP_NAME        = "JappeOS Desktop";
        const char* JOS_GREETER_USER        = "jos-greeter";

    private:
        Object _object;
        Interface& _iface;
        std::unique_ptr<SignalSubscription> _subUnitNew;
        std::unique_ptr<SignalSubscription> _subMainPIDChanged;
        std::unique_ptr<SignalSubscription> _subActiveStateChanged;
        std::unique_ptr<SignalSubscription> _subSubstateChanged;
        std::unique_ptr<SignalSubscription> _subResultChanged;

        std::map<std::string, pam_handle_t*> _activePAMHandles;
        std::map<std::string, SessionInfo>   _sessions;
        std::map<std::string, std::vector<std::unique_ptr<SignalSubscription>>> _sessionSubscriptions;
        std::vector<std::string>             _pendingUnits;
        std::string _greeterSessionID;
        bool        _isGreeterActive = false;
        bool        _isLiveEnvironment;

        // D-Bus interface

        void SharedPolicy(const Message& message) const;

        void OnCreateSession(const Message& message);
        void OnStopSession(const Message& message);
        void OnListSessions(const Message& message) const;

        // Internal methods

        void CreateLoginSession();
        void StopLoginSession();
        void CreateSession(const std::string& username,
                           const std::string& password,
                           std::string& outSessionId,
                           uid_t& outUid,
                           std::string& outSeat,
                           bool isLoginSession = false);
        void StopSession(const std::string& sessionId);

        void AuthenticateAndOpenPAMSession(const std::string& service,
                                           const std::string& username,
                                           const std::string& password,
                                           int& outControlFd,
                                           pid_t& outChildPid);
        void SubscribeToSessionSignals(const std::string& sessionId, const std::string& unitName);
        void SpawnUserSessionProcesses(bool isLoginSession,
                                       const std::string& username,
                                       const std::string& sessionId,
                                       const std::string& seat,
                                       std::string& outServiceName) const;
        void ActivateLogindSession(const std::string& sessionId, const std::string& seat) const;
        void TerminateUserSessionProcesses(const std::string& sessionId);
        void TerminatePAMForSession(SessionInfo& session) const;

        //void HandleSessionStop();
        void HandleUnitNew(const Message& msg); // <-- TODO: Replase with UnitStarted
        void HandleSessionMainPIDChanged(const std::string& sessionId, pid_t newPID);
        void HandleSessionActiveStateChanged(const std::string& sessionId, const std::string& activeState);
        void HandleSessionResultChanged(const std::string& sessionId, const std::string& result);
        void EvaluateSessionState(const SessionInfo& session);

        [[nodiscard]] bool IsGreeter(uid_t callerUid) const;
        [[nodiscard]] std::string QueryLogindSessionForUid(uid_t uid, ObjectPath& outObjectPath) const noexcept;
        [[nodiscard]] std::string QueryLogindSeatForSession(const std::string& sessionId,
                                                            const ObjectPath& sessionObjectPath) const noexcept;
        pid_t GetUnitMainPID(const std::string& unitName) const;
        bool TryGetSessionIdByUnitName(const std::string& unitName, std::string& outSessionId);


    private:
        static JappeOSCore::Logger& Log()
        {
            static JappeOSCore::Logger instance{"SessionManagerService"};
            return instance;
        }

        [[nodiscard]] static std::string GenerateFallbackSessionId();
        static int FindFreeTTY();
        static int ActivateTTY(int vt);
        [[nodiscard]] static std::optional<pid_t> GetParentPid(pid_t pid);

        static int PAMConversation(int num_msg,
                                   const pam_message** msg,
                                   pam_response** resp,
                                   void* appdata_ptr);
    };
}
