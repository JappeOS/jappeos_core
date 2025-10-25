#include "watchdog_service.h"



namespace JappeStudios::JappeOS::JappeOSCore::Services::Watchdog
{

    WatchdogService::WatchdogService(ServiceManager* serviceManager, DBusConnection* conn, DBusError* err) : Service(serviceManager, conn, err)
    {
        _timeout = std::chrono::seconds(25); // TODO: Dynamic time?
        _watcherThread = std::thread([this]() { this->ThreadFunc(); });
        _esub_BeginWatching = Application::GetInstance()->OnMessageHandlerPre.Subscribe([this](){ BeginWatching(); });
        _esub_EndWatching = Application::GetInstance()->OnMessageHandlerPost.Subscribe([this](){ EndWatching(); });
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

            // Now wait for EndWatching or timeout
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
