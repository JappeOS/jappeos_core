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
            DaemonCore::Daemon::TryGetConnectionOnBus(DBUS_BUS_SYSTEM),
            "org.jappeos.Core",
            ObjectPath("/org/jappeos/Core/DriverManagerService"),
            InterfaceName("org.jappeos.Core.DriverManagerService"))
    {
        // Subscribe before reading snapshots so subsequent updates are not lost.
        _subDriverCurrentInstallsChanged = std::make_unique<SignalSubscription>(
            _driverManagerProxy.SubscribePropertyChanged<DriverInstallations>(
                "CurrentInstalls",
                [this](const auto& installs) { OnDriverCurrentInstallsChanged(installs); }));
        _subDriverCurrentInstallsFailureChanged = std::make_unique<SignalSubscription>(
            _driverManagerProxy.SubscribePropertyChanged<std::string>(
                "CurrentInstallsFailure",
                [this](const auto& reason) { OnDriverCurrentInstallsFailureChanged(reason); }));
        _subDriverInstallationWorkInProgress = std::make_unique<SignalSubscription>(
            _driverManagerProxy.SubscribePropertyChanged<bool>(
                "InstallationWorkInProgress",
                [this](const auto& state) { OnDriverInstallationWorkInProgressChanged(state); }));

        OnDriverCurrentInstallsChanged(
            _driverManagerProxy.GetProperty<DriverInstallations>("CurrentInstalls"));
        OnDriverCurrentInstallsFailureChanged(
            _driverManagerProxy.GetProperty<std::string>("CurrentInstallsFailure"));
        OnDriverInstallationWorkInProgressChanged(
            _driverManagerProxy.GetProperty<bool>("InstallationWorkInProgress"));
    }

    NotificationAgentService::~NotificationAgentService()
    {
        OnDriverCurrentInstallsChanged({});
    }

    void NotificationAgentService::OnDriverCurrentInstallsChanged(const std::vector<std::tuple<std::string, std::string>>& properties)
    {
        if (!properties.empty())
            _wasInstallingDrivers = true;

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

    void NotificationAgentService::OnDriverCurrentInstallsFailureChanged(const std::string& failureReason)
    {
        if (failureReason.empty())
        {
            if (_failureNotificationId > 0)
                [[maybe_unused]] const auto _ = TryCloseNotification(_failureNotificationId);
            _failureNotificationId = 0;
            return;
        }

        TryShowNotification(failureReason, _failureNotificationId, _failureNotificationId);
    }

    void NotificationAgentService::OnDriverInstallationWorkInProgressChanged(const bool& value) const
    {
        if (value || !_wasInstallingDrivers)
            return;

        uint32_t outNotificationId;
        TryShowNotification("Driver installation finished.", outNotificationId);
    }

    std::string NotificationAgentService::GetDriverKey(const std::string& type, const std::string& name)
    {
        return type + ":" + name;
    }

    bool NotificationAgentService::TryShowNotification(const std::string& content, uint32_t& outNotificationId) const
    {
        return TryShowNotification(content, 0, outNotificationId);
    }

    bool NotificationAgentService::TryShowNotification(const std::string& content, const uint32_t replacesId, uint32_t& outNotificationId) const
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
                replacesId,
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
