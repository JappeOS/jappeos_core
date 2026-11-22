/*
 * jappeos_system_notification_agent, System notification agent for JappeOS.
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

#include "notification_agent_service.h"

#include <ranges>
#include <algorithm>

namespace JappeStudios::JappeOS::SystemNotificationAgent
{

    NotificationAgentService::NotificationAgentService() :
        _conn(DaemonCore::Daemon::GetConnection()),
        _driverManagerProxy(
            _conn,
            "org.jappeos.Core",
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

    NotificationAgentService::~NotificationAgentService()
    {
        OnDriverCurrentInstallsChanged({});
    }

    void NotificationAgentService::OnDriverCurrentInstallsChanged(const std::vector<std::tuple<std::string, std::string>>& properties)
    {
        if (!properties.empty())
        {
            _wasInstallingDrivers = true;
        }
        else if (_wasInstallingDrivers)
        {
            _wasInstallingDrivers = false;
            uint32_t outNotificationId;
            TryShowNotification("Driver installation finished.", outNotificationId);
        }

        std::set<std::string> leftovers;
        for (const auto& key: _driverInstallations | std::views::keys)
            leftovers.insert(key);

        for (const auto& property : properties)
        {
            std::string type = std::get<0>(property);
            const auto& name = std::get<1>(property);
            const auto key = GetDriverKey(type, name);

            std::ranges::transform(type, type.begin(),
                                   [](const unsigned char c) {
                                       return static_cast<char>(std::tolower(c));
                                   });

            if (uint32_t outNotificationId; TryShowNotification("Installing " + type + " driver: " + name, outNotificationId))
            {
                _driverInstallations.emplace(key, outNotificationId);
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
