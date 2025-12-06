#pragma once
#include <cstdint>

#include "../service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::AccountManager
{
    struct DBusValue
    {
        int dbusType;                 // D-Bus type (e.g. DBUS_TYPE_STRING)
        std::string signature;    // Variant signature ("s", "i", etc.)
        std::variant<std::monostate, int32_t, std::uint32_t, bool, double, std::string> value;
    };

    class AccountManagerService : public Service
    {
    public:
        explicit AccountManagerService(ServiceManager* serviceManager, DBusConnection* conn);
        ~AccountManagerService() override;

        bool HandleMethodCall(DBusMessage* msg) override;
        std::string GetName() override { return "AccountManagerService"; }

    private:
        void CreateInitialUserWithPasswordDbus(DBusMessage* pmsg,
                                           uid_t senderUid,
                                           pid_t senderPid,
                                           const std::string& username,
                                           const std::string& realName,
                                           const std::string& cryptedPassword);
        void AddUserDbus(DBusMessage* pmsg,
                     uid_t senderUid,
                     pid_t senderPid,
                     const std::string& username,
                     const std::string& realName);
        bool AddUser(const std::string& username, const std::string& realName, std::string& outObjectPath) const;
        void RemoveUser(DBusMessage* pmsg, uid_t senderUid, pid_t senderPid);
        void ListUsersDbus(DBusMessage* pmsg);
        bool ListUsers(std::vector<std::string>& outObjectPaths) const;
        void GetUserPropertyDbus(DBusMessage* pmsg, const std::string& userObject, const std::string& property);
        bool GetUserProperty(const std::string& userObject, const std::string& property, DBusValue& outValue) const;
        [[nodiscard]] bool SetUserPassword(const std::string& userObjectPath,
                     const std::string& cryptedPassword,
                     const std::string& hint) const;
    };
}
