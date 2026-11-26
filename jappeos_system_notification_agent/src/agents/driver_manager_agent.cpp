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

#include "driver_manager_agent.h"

#include <algorithm>
#include <cctype>
#include <ranges>

namespace JappeStudios::JappeOS::SystemNotificationAgent::Agents
{

    DriverManagerAgent::DriverManagerAgent() : NotificationAgent(
        Proxy(
            DaemonCore::Daemon::TryGetConnectionOnBus(DBUS_BUS_SYSTEM),
            "org.jappeos.Core",
            ObjectPath("/org/jappeos/Core/DriverManagerService"),
            InterfaceName("org.jappeos.Core.DriverManagerService")
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

    DriverManagerAgent::~DriverManagerAgent()
    {
        _serviceWatcher.reset();
        ResetServiceState();
    }

    void DriverManagerAgent::ForgetNotificationIds()
    {
        _driverInstallations.clear();
        _failureNotificationId = 0;
    }

    void DriverManagerAgent::RestoreNotifications()
    {
        UpdateInstallationNotifications();
        UpdateFailureNotification();
    }

    void DriverManagerAgent::ResetServiceState()
    {
        _subCurrentInstallsChanged.reset();
        _subCurrentInstallsFailureChanged.reset();
        _subInstallationWorkInProgress.reset();
        OnCurrentInstallsChanged({});
        OnCurrentInstallsFailureChanged({});
        _wasInstallingDrivers = false;
    }

    void DriverManagerAgent::RefreshServiceState()
    {
        _subCurrentInstallsChanged = std::make_unique<SignalSubscription>(
            _proxy.SubscribePropertyChanged<DriverInstallations>(
                "CurrentInstalls",
                [this](const auto& installs) { OnCurrentInstallsChanged(installs); }));

        _subCurrentInstallsFailureChanged = std::make_unique<SignalSubscription>(
            _proxy.SubscribePropertyChanged<std::string>(
                "CurrentInstallsFailure",
                [this](const auto& reason) { OnCurrentInstallsFailureChanged(reason); }));

        _subInstallationWorkInProgress = std::make_unique<SignalSubscription>(
            _proxy.SubscribePropertyChanged<bool>(
                "InstallationWorkInProgress",
                [this](const bool state) { OnInstallationWorkInProgressChanged(state); }));

        const auto properties = _proxy.GetAllProperties();
        const auto installs = properties.at("CurrentInstalls").get<DriverInstallations>();
        const auto failure = properties.at("CurrentInstallsFailure").get<std::string>();
        const auto working = properties.at("InstallationWorkInProgress").get<bool>();
        OnCurrentInstallsChanged(installs);
        OnCurrentInstallsFailureChanged(failure);
        _wasInstallingDrivers = working;
    }

    void DriverManagerAgent::OnCurrentInstallsChanged(const std::vector<std::tuple<std::string, std::string>>& properties)
    {
        _currentInstalls = properties;
        if (!properties.empty())
            _wasInstallingDrivers = true;

        UpdateInstallationNotifications();
    }

    void DriverManagerAgent::UpdateInstallationNotifications()
    {
        std::set<std::string> leftovers;
        for (const auto& key: _driverInstallations | std::views::keys)
            leftovers.insert(key);

        for (const auto& property : _currentInstalls)
        {
            std::string type = std::get<0>(property);
            const auto& name = std::get<1>(property);
            const auto key = GetDriverKey(type, name);

            leftovers.erase(key);
            if (_driverInstallations.contains(key))
                continue;

            std::ranges::transform(type, type.begin(),
                                   [](const unsigned char c) {
                                       return static_cast<char>(std::tolower(c));
                                   });

            if (uint32_t outNotificationId; TryShowNotification("Installing " + type + " driver: " + name, outNotificationId))
            {
                _driverInstallations.emplace(key, outNotificationId);
            }
        }

        for (const auto& key : leftovers)
        {
            TryCloseNotification(_driverInstallations.at(key));
            _driverInstallations.erase(key);
        }
    }

    void DriverManagerAgent::OnCurrentInstallsFailureChanged(const std::string& failureReason)
    {
        _failureReason = failureReason;
        UpdateFailureNotification();
    }

    void DriverManagerAgent::UpdateFailureNotification()
    {
        if (_failureReason.empty())
        {
            if (_failureNotificationId > 0)
                [[maybe_unused]] const auto _ = TryCloseNotification(_failureNotificationId);
            _failureNotificationId = 0;
            return;
        }

        TryShowNotification(_failureReason, _failureNotificationId, _failureNotificationId);
    }

    void DriverManagerAgent::OnInstallationWorkInProgressChanged(const bool value)
    {
        if (value)
        {
            _wasInstallingDrivers = true;
            return;
        }

        if (!_wasInstallingDrivers)
            return;

        _wasInstallingDrivers = false;
        uint32_t outNotificationId = 0;
        TryShowNotification("Driver installation finished, please reboot your device to finish setup.", outNotificationId);
    }

    std::string DriverManagerAgent::GetDriverKey(const std::string& type, const std::string& name)
    {
        return type + ":" + name;
    }

}
