/*
 * jappeos_core, Core system management daemon for JappeOS.
 * Copyright (C) 2026  Jappe02
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

#include "watchdog_service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Watchdog
{

    WatchdogService::WatchdogService(ServiceManager* serviceManager, Connection* conn) : Service(serviceManager, conn)
    {
        _timeout = std::chrono::seconds(25); // TODO: Dynamic time?
        _watcherThread = std::thread([this]() { this->ThreadFunc(); });
        _esub_BeginWatching = Application::GetInstance()->OnMessageHandlerPre.Subscribe([this]{ BeginWatching(); });
        _esub_EndWatching = Application::GetInstance()->OnMessageHandlerPost.Subscribe([this]{ EndWatching(); });
    }

    WatchdogService::~WatchdogService()
    {
        Application::GetInstance()->OnMessageHandlerPre.Unsubscribe(_esub_BeginWatching);
        Application::GetInstance()->OnMessageHandlerPost.Unsubscribe(_esub_EndWatching);

        {
            std::lock_guard<std::mutex> lock(_mutex);
            _running = false;
            _cv.notify_all();
        }

        if (_watcherThread.joinable()) _watcherThread.join();
    }

    void WatchdogService::BeginWatching()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _watching = true;
        _cv.notify_all();
    }

    void WatchdogService::EndWatching()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _watching = false;
        _cv.notify_all();
    }

    void WatchdogService::OnTimeout() const
    {
        const std::string message = "(fatal) WatchdogService::OnTimeout";
        _serviceManager->Get<Logger::LoggerService>()->Crit(message);
        Application::GetInstance()->FailFastFatalError(
            "WATCHDOG_TIMEOUT",
            message,
            ""
        );
    }

    void WatchdogService::ThreadFunc()
    {
        std::unique_lock<std::mutex> lock(_mutex);
        while (_running)
        {
            // Wait until BeginWatching or stop
            _cv.wait(lock, [this] { return !_running || _watching; });

            if (!_running) break;

            // Wait for EndWatching or timeout
            if (_cv.wait_for(lock, _timeout, [this] { return !_watching || !_running; }))
            {
                // either EndWatching() was called, or we are shutting down
            }
            else
            {
                // timeout expired while still watching
                lock.unlock(); // unlock before calling callback
                OnTimeout();
                lock.lock();
                _watching = false; // reset to avoid repeated firing
            }
        }
    }

}
