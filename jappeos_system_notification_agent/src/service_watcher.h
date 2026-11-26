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
#include <functional>

#include "logging/logger.h"

namespace JappeStudios::JappeOS::SystemNotificationAgent
{
    /**
     * Owns a service's lifecycle on the daemon's dispatch thread. The callbacks
     * must outlive this object. Failed initialization is retried by the GLib loop.
     */
    class ServiceWatcher
    {
    public:
        ServiceWatcher(const DaemonCore::DBus::Proxy& proxy,
                       std::function<void()> reset,
                       std::function<void()> refresh);
        ~ServiceWatcher();
        ServiceWatcher(const ServiceWatcher&) = delete;
        ServiceWatcher& operator=(const ServiceWatcher&) = delete;

    private:
        void Refresh();
        void CancelRetry();

        std::function<void()> _reset;
        std::function<void()> _refresh;
        std::unique_ptr<DaemonCore::DBus::SignalSubscription> _subscription;
        unsigned int _retrySource = 0;
        unsigned int _retryDelaySeconds = 2;

    private:
        CREATE_LOGGER("ServiceWatcher")
    };
}
