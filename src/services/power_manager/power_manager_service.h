#pragma once
#include "../service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::PowerManager
{
    class PowerManagerService : public Service
    {
    public:
        explicit PowerManagerService(ServiceManager* serviceManager, DBusConnection* conn);
        ~PowerManagerService() override;

        bool HandleMethodCall(DBusMessage* msg) override;
        std::string GetName() override { return "PowerManagerService"; }

    private:
        void Shutdown(DBusMessage* pmsg, uid_t senderUid, pid_t senderPid) const;
        void Reboot(DBusMessage* msg, uid_t senderUid, pid_t senderPid) const;
        void Suspend(DBusMessage* msg, uid_t senderUid, pid_t senderPid) const;
    };
}
