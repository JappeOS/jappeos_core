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

#include "stdout_logger.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Logger
{

    StdoutLogger::StdoutLogger(ServiceManager* serviceManager, Connection* conn) : LoggerService(serviceManager, conn)
    {
        _loggerThread = std::thread(&StdoutLogger::Process, this);
    }

    StdoutLogger::~StdoutLogger()
    {
        _done = true;
        _cv.notify_one(); // Wake logger thread to exit
        if (_loggerThread.joinable())
            _loggerThread.join(); // Wait for thread to finish
    }

    void StdoutLogger::Emerg(const std::string &str) { Log(SD_EMERG, str); }

    void StdoutLogger::Alert(const std::string &str) { Log(SD_ALERT, str); }

    void StdoutLogger::Crit(const std::string &str) { Log(SD_CRIT, str); }

    void StdoutLogger::Err(const std::string &str) { Log(SD_ERR, str); }

    void StdoutLogger::Warn(const std::string &str) { Log(SD_WARNING, str); }

    void StdoutLogger::Notice(const std::string &str) { Log(SD_NOTICE, str); }

    void StdoutLogger::Info(const std::string &str) { Log(SD_INFO, str); }

    void StdoutLogger::Debug(const std::string &str) { Log(SD_DEBUG, str); }

    void StdoutLogger::Log(const std::string& prefix, const std::string& str)
    {
        {
            std::lock_guard<std::mutex> lock(_queueMutex);
            _logQueue.push(std::move(prefix + str));
        }
        _cv.notify_one(); // Wake logger thread
    }

    void StdoutLogger::Process()
    {
        std::unique_lock<std::mutex> lock(_queueMutex);
        while (!_done || !_logQueue.empty())
        {
            _cv.wait(lock, [this]() { return _done || !_logQueue.empty(); });

            while (!_logQueue.empty())
            {
                std::string msg = _logQueue.front();
                _logQueue.pop();
                lock.unlock();  // Unlock while writing to avoid blocking other threads
                std::cout << msg << std::endl;
                lock.lock();
            }
        }

        std::cout.flush();
    }

}
