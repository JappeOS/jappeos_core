#include "notification_agent_service.h"

#include <ranges>

namespace JappeStudios::JappeOS::SystemNotificationAgent
{

    NotificationAgentService::NotificationAgentService() :
        _conn(DaemonCore::Daemon::GetConnection()),
        _driverManagerProxy(
            _conn,
            "org.jappeos.Core.DriverManagerService",
            ObjectPath("/org/jappeos/Core/DriverManagerService"),
            InterfaceName("org.jappeos.Core.DriverManagerService"))
    {
        _subDriverCurrentInstallsChanged = std::make_unique<SignalSubscription>(
            _driverManagerProxy.SubscribePropertyChanged<const std::vector<std::tuple<std::string, std::string>>&>(
                "CurrentInstalls",
                [&](const auto& properties) { OnDriverCurrentInstallsChanged(properties); }
            )
        );
    }

    void NotificationAgentService::OnDriverCurrentInstallsChanged(const std::vector<std::tuple<std::string, std::string>>& properties)
    {
        std::set<std::string> leftovers;
        for (const auto& key: _driverInstallations | std::views::keys)
            leftovers.insert(key);

        for (const auto& property : properties)
        {
            const auto& type = std::get<0>(property);
            const auto& name = std::get<1>(property);
            const auto key = GetDriverKey(type, name);

            if (uint32_t outNotificationId; TryShowNotification("Installing " + type + " driver named " + name, outNotificationId))
            {
                _driverInstallations.insert_or_assign(key, outNotificationId);
                leftovers.erase(key);
            }
        }

        for (const auto& key : leftovers)
        {
            TryCloseNotification(_driverInstallations.at(key));
            _driverInstallations.erase(key);
        }
    }

    std::string NotificationAgentService::GetDriverKey(const std::string& type, const std::string& name)
    {
        return type + ":" + name;
    }

    bool NotificationAgentService::TryShowNotification(const std::string& content, uint32_t& outNotificationId) const
    {
        outNotificationId = 0;
        try
        {
            auto msg = Message::CreateMethodCall(
                "org.freedesktop.Notifications",
                ObjectPath("/org/freedesktop/Notifications"),
                InterfaceName("org.freedesktop.Notifications"),
                "Notify"
            );

            msg.SetArgs(
                std::string("System"),
                static_cast<uint32_t>(0),
                std::string(),
                content,
                content,
                std::vector<std::string>(),
                std::map<std::string, DBusVariant>(),
                0);

            const auto [id] = msg.SendWithReply(_conn).GetArgs<uint32_t>();
            outNotificationId = id;
            return true;
        }
        catch (const std::exception& e)
        {
            Log().Err("Failed to show notification: " + std::string(e.what()));
        }

        return false;
    }

    bool NotificationAgentService::TryCloseNotification(const uint32_t notificationId) const
    {
        try
        {
            auto msg = Message::CreateMethodCall(
                "org.freedesktop.Notifications",
                ObjectPath("/org/freedesktop/Notifications"),
                InterfaceName("org.freedesktop.Notifications"),
                "CloseNotification"
            );

            msg.SetArgs(notificationId);
            msg.SendWithReplyIgnore(_conn);
            return true;
        }
        catch (const std::exception& e)
        {
            Log().Err("Failed to close notification: " + std::string(e.what()));
        }

        return false;
    }

}
