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

#pragma once

#include "daemon.h"
#include "dbus/dbus_wrapper.h"
#include "logging/logger.h"
#include "service_watcher.h"

using namespace JappeStudios::JappeOS::DaemonCore::DBus;

namespace JappeStudios::JappeOS::SystemNotificationAgent
{
#define NOTIFICATION_EXPIRES_DEFAULT (-1)
#define NOTIFICATION_EXPIRES_NEVER (0)
#define NOTIFICATION_REPLACES_NOTHING (0)

#define AGENT_NAME(x) static const std::string GetName() { return (#x); }
#define GET_AGENT_NAME(x) (x::GetName())
#define CURR_AGENT_NAME (GetName())

    class NotificationAgent
    {
    public:
        explicit NotificationAgent(Proxy proxy);
        virtual ~NotificationAgent() = default;

    protected:
        Connection& _conn;
        Proxy _proxy;

        // Call at the end of derived initialization, before watching the backend.
        void WatchNotificationService();
        virtual void ForgetNotificationIds() = 0;
        virtual void RestoreNotifications() = 0;

        bool TryShowNotification(const std::string& summary, uint32_t& outNotificationId) const noexcept;
        bool TryShowNotification(
            const std::string& summary,
            const std::string& icon,
            uint32_t& outNotificationId) const noexcept;
        bool TryShowNotification(
            const std::string& summary,
            uint32_t replacesId,
            uint32_t& outNotificationId) const noexcept;
        bool TryShowNotification(
            const std::string& summary,
            const std::string& icon,
            uint32_t replacesId,
            uint32_t& outNotificationId) const noexcept;
        bool TryShowNotification(
            const std::string& summary,
            const std::string& icon,
            uint32_t replacesId,
            int32_t expireTimeMillis,
            uint32_t& outNotificationId) const noexcept;
        bool TryCloseNotification(uint32_t notificationId) const noexcept;

    private:
        std::string _notificationOwner;
        std::unique_ptr<ServiceWatcher> _notificationWatcher;

    private:
        static const std::string DEFAULT_NOTIFICATION_ICON;
        CREATE_LOGGER("NotificationAgent")
    };
}
