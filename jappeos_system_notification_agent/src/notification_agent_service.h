#pragma once
#include "daemon.h"
#include "logging/logger.h"

using namespace JappeStudios::JappeOS::DaemonCore::DBus;

namespace JappeStudios::JappeOS::SystemNotificationAgent
{
    class NotificationAgentService : public DaemonCore::DaemonClass
    {
    public:
        explicit NotificationAgentService();

    private:
        Connection& _conn;
        Proxy       _driverManagerProxy;
        std::unique_ptr<SignalSubscription> _subDriverCurrentInstallsChanged;
        std::unordered_map<std::string, uint32_t> _driverInstallations;

        void OnDriverCurrentInstallsChanged(const std::vector<std::tuple<std::string, std::string>>& properties);
        static std::string GetDriverKey(const std::string& type, const std::string& name);

        bool TryShowNotification(const std::string& content, uint32_t& outNotificationId) const;
        bool TryCloseNotification(uint32_t notificationId) const;

    private:
        CREATE_LOGGER("NotificationAgentService")
    };
}
