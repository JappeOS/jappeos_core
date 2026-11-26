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

#include "power_manager_agent.h"

namespace JappeStudios::JappeOS::SystemNotificationAgent::Agents
{

    PowerManagerAgent::PowerManagerAgent() :
        NotificationAgent(
            Proxy(
                DaemonCore::Daemon::TryGetConnectionOnBus(DBUS_BUS_SYSTEM),
                "org.freedesktop.UPower",
                ObjectPath("/org/freedesktop/UPower"),
                InterfaceName("org.freedesktop.UPower")
            )
        )
    {
        WatchNotificationService();
        _serviceWatcher = std::make_unique<ServiceWatcher>(
            _proxy,
            [this] { ResetServiceState(); },
            [this] { RefreshServiceState(); }
        );
    }

    PowerManagerAgent::~PowerManagerAgent()
    {
        _serviceWatcher.reset();
        ResetServiceState();
    }

    void PowerManagerAgent::ForgetNotificationIds()
    {
        _currentNotificationId = 0;
    }

    void PowerManagerAgent::RestoreNotifications()
    {
        OnBatteryWarningLevelChanged(_warningLevel);
    }

    void PowerManagerAgent::ResetServiceState()
    {
        _subBatteryWarningLevelChanged.reset();
        _displayDeviceProxy.reset();
        OnBatteryWarningLevelChanged(0);
    }

    void PowerManagerAgent::RefreshServiceState()
    {
        const auto [path] = _proxy.CallMethod<ObjectPath>("GetDisplayDevice");

        _displayDeviceProxy.emplace(
            DaemonCore::Daemon::TryGetConnectionOnBus(DBUS_BUS_SYSTEM),
            _proxy.GetBusName(),
            path,
            _proxy.GetInterface().Child("Device")
        );

        _subBatteryWarningLevelChanged = std::make_unique<SignalSubscription>(
            _displayDeviceProxy->SubscribePropertyChanged<uint32_t>(
                "WarningLevel",
                [this](const uint32_t level) { OnBatteryWarningLevelChanged(level); }));

        OnBatteryWarningLevelChanged(_displayDeviceProxy->GetProperty<uint32_t>("WarningLevel"));
    }

    void PowerManagerAgent::OnBatteryWarningLevelChanged(const uint32_t level)
    {
        _warningLevel = level;
        switch (level)
        {
            case 3:
                TryShowNotification(
                    "Your battery is low. Please connect a charger soon.",
                    _currentNotificationId,
                    _currentNotificationId);
                break;
            case 4:
                TryShowNotification(
                    "Your battery is critically low. Please save your work or connect a charger.",
                    _currentNotificationId,
                    _currentNotificationId);
                break;
            case 5:
                TryShowNotification(
                    "Your battery is critically low, please save your work or connect a charger. The device will shut down shortly.",
                    _currentNotificationId,
                    _currentNotificationId);
                break;
            default:
                [[maybe_unused]] const auto _ = TryCloseNotification(_currentNotificationId);
                _currentNotificationId = 0;
                break;
        }
    }

}