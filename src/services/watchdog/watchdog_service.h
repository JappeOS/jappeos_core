#pragma once

#include <atomic>
#include <condition_variable>
#include <thread>

#include "../service.h"
#include "../../application.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Watchdog
{
    /// A service that watches this core process and ensures the operating system is responding and not "frozen".
    class WatchdogService : public Service
    {
    public:
        explicit WatchdogService(ServiceManager* serviceManager, DBusConnection* conn);
        ~WatchdogService() override;

        bool HandleMethodCall(DBusMessage* msg) override { return false; }
        std::string GetName() override { return "WatchdogService"; }

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