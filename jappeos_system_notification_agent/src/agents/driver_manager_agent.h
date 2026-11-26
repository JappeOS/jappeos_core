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
#include "dbus/dbus_wrapper.h"
#include "logging/logger.h"
#include "../notification_agent.h"

using namespace JappeStudios::JappeOS::DaemonCore::DBus;

namespace JappeStudios::JappeOS::SystemNotificationAgent::Agents
{
    class DriverManagerAgent : public NotificationAgent
    {
    public:
        DriverManagerAgent();
        ~DriverManagerAgent() override;

    public:
        AGENT_NAME("DriverManagerAgent")

    protected:
        void ForgetNotificationIds() override;
        void RestoreNotifications() override;

    private:
        using DriverInstallations = std::vector<std::tuple<std::string, std::string>>;

        std::unique_ptr<ServiceWatcher> _serviceWatcher;
        std::unique_ptr<SignalSubscription> _subCurrentInstallsChanged;
        std::unique_ptr<SignalSubscription> _subCurrentInstallsFailureChanged;
        std::unique_ptr<SignalSubscription> _subInstallationWorkInProgress;
        std::unordered_map<std::string, uint32_t> _driverInstallations;
        uint32_t _failureNotificationId = 0;
        bool _wasInstallingDrivers = false;
        DriverInstallations _currentInstalls;
        std::string _failureReason;

        void ResetServiceState();
        void RefreshServiceState();
        void UpdateInstallationNotifications();
        void UpdateFailureNotification();

        void OnCurrentInstallsChanged(const std::vector<std::tuple<std::string, std::string>>& properties);
        void OnCurrentInstallsFailureChanged(const std::string& failureReason);
        void OnInstallationWorkInProgressChanged(bool value);

    private:
        CREATE_LOGGER(CURR_AGENT_NAME)

        static std::string GetDriverKey(const std::string& type, const std::string& name);
    };
}