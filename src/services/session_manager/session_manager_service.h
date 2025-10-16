#pragma once
#include <security/pam_appl.h>
#include <string>
#include <cstring>
#include <pwd.h>
#include <sys/types.h>
#include <cstdint>
#include <chrono>
#include <atomic>

#include "../service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::SessionManager
{
    struct PamConversationCtx {
        std::string password;
    };

    struct SessionInfo
    {
        std::string username;
        std::string scopeName;
        uid_t uid;
        pam_handle_t* pam_handle;
        std::string seat;
    };

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
        explicit SessionManagerService(ServiceManager* serviceManager);
        ~SessionManagerService() override;

        bool HandleMethodCall(DBusConnection* conn, DBusMessage* msg) override;
        std::string GetName() override { return "SessionManagerService"; }

    public:
        const char* PAM_GREETER_SERVICE = "jappeos-greeter";
        const char* JOS_DESKTOP_BINARY = "/jappeos/desktop";
        const char* JOS_DESKTOP_NAME = "JappeOS Desktop";
        const char* JOS_GREETER_BINARY = "/jappeos/greeter";
        const char* JOS_GREETER_USER = "jos-greeter";

    private:
        bool CreateLoginSession();
        bool StopLoginSession();
        bool CreateSession(DBusConnection* conn, DBusMessage* msg, uid_t callerUid, const std::string& username, const std::string& password);
        void SendDBusReply_CreateSession(DBusConnection* conn,
                                         DBusMessage* msg,
                                         const std::string& sessionId,
                                         uid_t uid,
                                         const std::string& seat) const;
        bool AuthenticateAndOpenPAMSession(const std::string& username, const std::string& password, pam_handle_t** out_pamh) const;
        bool SpawnUserSessionProcesses(bool isLoginSession, const std::string& username, const std::string& sessionId, const std::string& seat, std::string& outServiceName) const;
        bool StopSession(DBusConnection* conn, DBusMessage* msg, uid_t callerUid, const std::string& sessionId);
        bool TerminateUserSessionProcesses(const std::string& sessionId);
        bool PolkitAuthorizeStop(uid_t callerUid, const std::string& sessionId);
        bool ListSessions(DBusConnection* conn, DBusMessage* msg, uid_t callerUid) const;
        [[nodiscard]] bool IsGreeter(uid_t callerUid) const;
        [[nodiscard]] std::string QueryLogindSessionForUid(uid_t uid) const;
        [[nodiscard]] std::string QueryLogindSeatForSession(const std::string& sessionId) const;
        [[nodiscard]] bool ActivateLogindSession(const std::string& sessionId) const;
        [[nodiscard]] std::string GenerateFallbackSessionId() const;

    private:
        std::map<std::string, pam_handle_t*> _activePAMHandles;
        std::map<std::string, SessionInfo> _sessions;
        std::string _greeterSessionID;
        SessionInfo _greeterSession;
        bool _isGreeterActive = false;
    };
}
