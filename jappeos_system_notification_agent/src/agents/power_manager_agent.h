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
#include "../notification_agent.h"

using namespace JappeStudios::JappeOS::DaemonCore::DBus;

namespace JappeStudios::JappeOS::SystemNotificationAgent::Agents
{
    class PowerManagerAgent : public NotificationAgent
    {
    public:
        PowerManagerAgent();
        ~PowerManagerAgent() override;

    public:
        AGENT_NAME("PowerManagerAgent")

    protected:
        void ForgetNotificationIds() override;
        void RestoreNotifications() override;

    private:
        std::unique_ptr<ServiceWatcher> _serviceWatcher;
        std::optional<Proxy> _displayDeviceProxy;
        std::unique_ptr<SignalSubscription> _subBatteryWarningLevelChanged;
        uint32_t _currentNotificationId = 0;
        uint32_t _warningLevel = 0;

        void ResetServiceState();
        void RefreshServiceState();

        void OnBatteryWarningLevelChanged(uint32_t level);
    };
}