/*
 * jappeos_core, Core system management daemon for JappeOS.
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

#include <condition_variable>
#include <thread>

#include "../service.h"
#include "../../application.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Watchdog
{
    /**
     * @brief A service that watches this core process and ensures it is responding and not "frozen".
     */
    class WatchdogService : public Service
    {
    public:
        explicit WatchdogService(ServiceManager* serviceManager, Connection* conn);
        ~WatchdogService() override;
        [[nodiscard]] std::string GetName() const override { return "WatchdogService"; }

    private:
        bool _running = true;
        bool _watching = false;
        std::chrono::seconds _timeout{};
        std::thread _watcherThread;
        std::mutex _mutex;
        std::condition_variable _cv;
        std::size_t _esub_BeginWatching;
        std::size_t _esub_EndWatching;

        void BeginWatching();
        void EndWatching();
        void OnTimeout() const;
        void ThreadFunc();
    };
}