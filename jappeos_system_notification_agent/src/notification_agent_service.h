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
#include "notification_agent.h"
#include "logging/logger.h"

using namespace JappeStudios::JappeOS::DaemonCore::DBus;

namespace JappeStudios::JappeOS::SystemNotificationAgent
{
    class NotificationAgentService : public DaemonCore::DaemonClass
    {
    public:
        NotificationAgentService();

    private:
        Connection& _conn;
        std::vector<std::unique_ptr<NotificationAgent>> _notificationAgents;

        template<typename T>
        void Register()
        {
            std::unique_ptr<T> instance;
            try
            {
                instance = std::make_unique<T>();
            }
            catch (const std::exception& e)
            {
                Log().Warn("Failed to register notification agent debugName='" + GET_AGENT_NAME(T) + "': " + e.what());
            }
            _notificationAgents.push_back(std::move(instance));
            Log().Debug("Registered notification agent: " + GET_AGENT_NAME(T));
        }

    private:
        CREATE_LOGGER("NotificationAgentService")
    };
}
