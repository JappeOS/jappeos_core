#pragma once
#include "../service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::AccountManager
{
    class AccountManagerService : public Service
    {
    public:
        explicit AccountManagerService(ServiceManager* serviceManager, DBusConnection* conn);
        ~AccountManagerService() override;

        bool HandleMethodCall(DBusMessage* msg) override;
        std::string GetName() override { return "AccountManagerService"; }

    private:
        void AddUser(DBusMessage* pmsg, uid_t senderUid, pid_t senderPid);
        void RemoveUser(DBusMessage* pmsg, uid_t senderUid, pid_t senderPid);
        void ListUsers(DBusMessage* pmsg) const;
        void GetUserProperty(DBusMessage* pmsg, const std::string& userObject, const std::string& property) const;
    };
}
