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

#include "service_watcher.h"

#include <glib.h>
#include <algorithm>

namespace JappeStudios::JappeOS::SystemNotificationAgent
{

    ServiceWatcher::ServiceWatcher(const DaemonCore::DBus::Proxy& proxy,
                                   std::function<void()> reset,
                                   std::function<void()> refresh)
        : _reset(std::move(reset)), _refresh(std::move(refresh))
    {
        _subscription = std::make_unique<DaemonCore::DBus::SignalSubscription>(
            proxy.SubscribeOwnerChanged([this](const auto&, const auto& newOwner)
            {
                CancelRetry();
                _retryDelaySeconds = 2;
                _reset();
                if (!newOwner.empty())
                    Refresh();
            }));
        Refresh();
    }

    ServiceWatcher::~ServiceWatcher()
    {
        CancelRetry();
    }

    void ServiceWatcher::CancelRetry()
    {
        if (_retrySource != 0)
        {
            g_source_remove(_retrySource);
            _retrySource = 0;
        }
    }

    void ServiceWatcher::Refresh()
    {
        try
        {
            _refresh();
            _retryDelaySeconds = 2;
        }
        catch (const std::exception& error)
        {
            _reset();
            if (_retryDelaySeconds == 2)
                Log().Warn("Service initialization failed; retrying: " + std::string(error.what()));

            _retrySource = g_timeout_add_seconds(_retryDelaySeconds, [](void* data) -> gboolean
            {
                auto& watcher = *static_cast<ServiceWatcher*>(data);
                watcher._retrySource = 0;
                watcher.Refresh();
                return G_SOURCE_REMOVE;
            }, this);
            _retryDelaySeconds = std::min(_retryDelaySeconds * 2, 30u);
        }
    }
}
