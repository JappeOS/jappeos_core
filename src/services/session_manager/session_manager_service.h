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

namespace JappeStudios::JappeOS::JappeOSCore::Services::SessionManager
{
    struct PamConversationCtx
    {
        std::string password;
    };

    struct SessionInfo
    {
        std::string username;
        std::string scopeName;
        uid_t uid;
        pid_t leaderPid;
        int controlFd;
        std::string seat;
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
        const bool USE_SESSION_MANAGER_CREATE_VT_SWITCH_DELAY = false; // TODO
        const char* PAM_GREETER_SERVICE = "jappeos-greeter";
        const char* PAM_LOGIN_SERVICE = "jappeos-login";
        const char* JOS_DESKTOP_BINARY = "/jappeos/desktop/desktop";
        const char* JOS_DESKTOP_NAME = "JappeOS Desktop";
        const char* JOS_GREETER_BINARY = "/jappeos/greeter/greeter";
        const char* JOS_GREETER_USER = "jos-greeter";
        const char* JOS_INSTALLER_BINARY = "/jappeos/installer/installer";

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
        void TerminatePAMForSession(SessionInfo& session);
        bool PolkitAuthorizeStop(uid_t callerUid, const std::string& sessionId);
        void ListSessions(DBusMessage* msg, uid_t callerUid);
        void OnJobRemoved(const std::string& unitName, const std::string& result);
        [[nodiscard]] bool IsGreeter(uid_t callerUid) const;
        [[nodiscard]] std::string QueryLogindSessionForUid(uid_t uid, std::string& outObjectPath) const;
        [[nodiscard]] std::string QueryLogindSeatForSession(const std::string& sessionId, const std::string& sessionObjectPath) const;
        [[nodiscard]] bool ActivateLogindSession(const std::string& sessionId, const std::string& seat) const;
        pid_t GetUnitMainPID(DBusConnection* conn, const std::string& unitName);
        [[nodiscard]] std::string GenerateFallbackSessionId() const;
        int FindFreeTTY();
        bool GetSessionIdByUnitName(const std::string& unitName, std::string& outSessionId);
        [[nodiscard]] std::optional<pid_t> GetParentPid(pid_t pid) const;

    private:
        std::map<std::string, pam_handle_t*> _activePAMHandles;
        std::map<std::string, SessionInfo> _sessions;
        std::vector<std::string> _pendingUnits;
        std::string _greeterSessionID;
        bool _isGreeterActive = false;
        bool _isLiveEnvironment;
    };

}
