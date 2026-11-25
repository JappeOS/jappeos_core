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
#include "logging/logger.h"

using namespace JappeStudios::JappeOS::DaemonCore::DBus;

namespace JappeStudios::JappeOS::SystemNotificationAgent
{
    class NotificationAgentService : public DaemonCore::DaemonClass
    {
    public:
        explicit NotificationAgentService();
        ~NotificationAgentService();

    private:
        using DriverInstallations = std::vector<std::tuple<std::string, std::string>>;

        Connection& _conn;
        Proxy _driverManagerProxy;
        std::unique_ptr<SignalSubscription> _subDriverCurrentInstallsChanged;
        std::unique_ptr<SignalSubscription> _subDriverCurrentInstallsFailureChanged;
        std::unordered_map<std::string, uint32_t> _driverInstallations;
        uint32_t _failureNotificationId = 0;
        bool _wasInstallingDrivers = false;

        void OnDriverCurrentInstallsChanged(const std::vector<std::tuple<std::string, std::string>>& properties);
        void OnDriverCurrentInstallsFailureChanged(const std::string& failureReason);
        static std::string GetDriverKey(const std::string& type, const std::string& name);

        bool TryShowNotification(const std::string& content, uint32_t& outNotificationId) const;
        bool TryShowNotification(const std::string& content, uint32_t replacesId, uint32_t& outNotificationId) const;
        bool TryCloseNotification(uint32_t notificationId) const;

    private:
        CREATE_LOGGER("NotificationAgentService")
    };
}
