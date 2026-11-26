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

#include "notification_agent.h"

namespace JappeStudios::JappeOS::SystemNotificationAgent
{

    NotificationAgent::NotificationAgent(Proxy proxy)
                : _conn(DaemonCore::Daemon::GetConnection()), _proxy(std::move(proxy)) {}

    void NotificationAgent::WatchNotificationService()
    {
        const Proxy notifications(
            _conn,
            "org.freedesktop.Notifications",
            ObjectPath("/org/freedesktop/Notifications"),
            InterfaceName("org.freedesktop.Notifications")
        );

        _notificationWatcher = std::make_unique<ServiceWatcher>(
            notifications,
            [this]
            {
                _notificationOwner.clear();
                ForgetNotificationIds();
            },
            [this]
            {
                const Proxy bus(
                    _conn,
                    DBUS_SERVICE_DBUS,
                    ObjectPath(DBUS_PATH_DBUS),
                    InterfaceName(DBUS_INTERFACE_DBUS)
                );

                const auto [owner] = bus.CallMethod<std::string>(
                    "GetNameOwner",
                    std::string("org.freedesktop.Notifications")
                );

                // Pin notification IDs to this connection, even if an owner
                // change is queued while a backend callback sends a notification.
                const Proxy server(
                    _conn,
                    owner,
                    ObjectPath("/org/freedesktop/Notifications"),
                    InterfaceName("org.freedesktop.Notifications")
                );

                [[maybe_unused]] const auto _ = server.CallMethod<std::string, std::string, std::string, std::string>("GetServerInformation");
                _notificationOwner = owner;
                RestoreNotifications();
            });
    }

    bool NotificationAgent::TryShowNotification(const std::string& summary, uint32_t& outNotificationId) const noexcept
    {
        return TryShowNotification(summary, 0, outNotificationId);
    }

    bool NotificationAgent::TryShowNotification(
        const std::string& summary,
        const uint32_t replacesId,
        uint32_t& outNotificationId) const noexcept
    {
        return TryShowNotification(
            summary,
            replacesId,
            NOTIFICATION_EXPIRES_NEVER,
            outNotificationId
        );
    }

    bool NotificationAgent::TryShowNotification(
        const std::string& summary,
        const uint32_t replacesId,
        const int32_t expireTimeMillis,
        uint32_t& outNotificationId) const noexcept
    {
        if (_notificationOwner.empty())
            return false;

        try
        {
            auto msg = Message::CreateMethodCall(
                _notificationOwner,
                ObjectPath("/org/freedesktop/Notifications"),
                InterfaceName("org.freedesktop.Notifications"),
                "Notify"
            );

            msg.SetArgs(
                std::string("System"),
                replacesId,
                std::string(),
                summary,
                std::string(),
                std::vector<std::string>(),
                std::map<std::string, DBusVariant>(),
                expireTimeMillis);

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

    bool NotificationAgent::TryCloseNotification(const uint32_t notificationId) const noexcept
    {
        if (notificationId == 0 || _notificationOwner.empty())
            return false;

        try
        {
            auto msg = Message::CreateMethodCall(
                _notificationOwner,
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