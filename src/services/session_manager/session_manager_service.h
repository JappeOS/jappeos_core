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
        pam_handle_t* pam_handle;
        std::string seat;
        std::vector<pid_t> privilegedClientProcesses;
    };

    class LoginManager;

    // TODO: [NEW] GET THE PID(S) OF TRUSTED CLIENTS USING D-BUS SIGNALS!
    // TODO: Implement D-Bus signals
    // Add comprehensive error handling throughout
    // Implement Polkit authorization for session management
    // Add logging for all major operations
    // Handle edge cases: user switching, session recovery, crash handling
    // Add session state tracking (active, locked, closing, etc.)
    // Implement proper signal handling for clean shutdown
    // Session locking/unlocking support
    // Multi-seat proper support
    // Session migration between seats
    // Idle timeout handling
    // Integration with power management
    // --
    // Test rapid login/logout sequences
    // Test concurrent logins
    // Test crash recovery (what happens if compositor dies?)
    // Test with multiple users
    // Test session switching
    // Verify all processes are cleaned up on logout
    // Test with PAM modules that do actual authentication
    class SessionManagerService : public Service
    {
    public:
        explicit SessionManagerService(ServiceManager* serviceManager, DBusConnection* conn);
        ~SessionManagerService() override;

        bool HandleMethodCall(DBusMessage* msg) override;
        std::string GetName() override { return "SessionManagerService"; }

        [[nodiscard]] bool IsLiveEnvironment() const { return _isLiveEnvironment; }

                      bool IsManagedUserSession(uid_t uid);
        [[nodiscard]] bool IsPrivilegedClientProcess(pid_t pid, bool allowChildProcesses = false) const;

    public:
        const char* PAM_GREETER_SERVICE = "jappeos-greeter";
        const char* JOS_DESKTOP_BINARY = "/jappeos/desktop/desktop";
        const char* JOS_DESKTOP_NAME = "JappeOS Desktop";
        const char* JOS_GREETER_BINARY = "/jappeos/greeter/greeter";
        const char* JOS_GREETER_USER = "jos-greeter";
        const char* JOS_INSTALLER_BINARY = "/jappeos/installer/installer";

    private:
        bool CreateLoginSession();
        bool StopLoginSession();
        bool CreateSession(DBusMessage* msg, uid_t callerUid, const std::string& username, const std::string& password);
        void SendDBusReply_CreateSession(DBusMessage* msg,
                                         const std::string& sessionId,
                                         uid_t uid,
                                         const std::string& seat) const;
        bool AuthenticateAndOpenPAMSession(const std::string& username, const std::string& password, pam_handle_t** out_pamh);
        bool SpawnUserSessionProcesses(bool isLoginSession,
                                       const std::string& username,
                                       const std::string& sessionId,
                                       const std::string& seat,
                                       std::string& outServiceName) const;
        bool StopSession(DBusMessage* msg, uid_t callerUid, const std::string& sessionId);
        bool TerminateUserSessionProcesses(const std::string& sessionId);
        bool PolkitAuthorizeStop(uid_t callerUid, const std::string& sessionId);
        bool ListSessions(DBusMessage* msg, uid_t callerUid) const;
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
        LoginManager* _loginManager;
        std::string _greeterSessionID;
        bool _isGreeterActive = false;
        bool _isLiveEnvironment;
    };

    class LoginManager
    {
    public:
        explicit LoginManager(SessionManagerService* sessionManager, DBusConnection* conn);
        ~LoginManager();

        void AddUser(DBusMessage* msg, uid_t callerUid);
        void RemoveUser(DBusMessage* msg, uid_t callerUid);
        void ListUsers(DBusMessage* msg, uid_t callerUid);

    private:
        SessionManagerService* _sessionManager;
        DBusConnection* _conn;
    };
}
