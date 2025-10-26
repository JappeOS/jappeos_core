#pragma once
#include <condition_variable>
#include <queue>
#include <thread>

#include "logger_service.h"
#include "../../application.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Logger
{
#define SD_EMERG   "<0>"  /* system is unusable */
#define SD_ALERT   "<1>"  /* action must be taken immediately */
#define SD_CRIT    "<2>"  /* critical conditions */
#define SD_ERR     "<3>"  /* error conditions */
#define SD_WARNING "<4>"  /* warning conditions */
#define SD_NOTICE  "<5>"  /* normal but significant condition */
#define SD_INFO    "<6>"  /* informational */
#define SD_DEBUG   "<7>"  /* debug-level messages */

    class StdoutLogger : public LoggerService
    {
    public:
        explicit StdoutLogger(ServiceManager* serviceManager, DBusConnection* conn);
        ~StdoutLogger() override;

        void Emerg(const std::string &str) override;
        void Alert(const std::string &str) override;
        void Crit(const std::string &str) override;
        void Err(const std::string &str) override;
        void Warn(const std::string &str) override;
        void Notice(const std::string &str) override;
        void Info(const std::string &str) override;
        void Debug(const std::string &str) override;

    private:
        std::atomic<bool> _done = false;
        std::thread _loggerThread;
        std::queue<std::string> _logQueue;
        std::mutex _queueMutex;
        std::condition_variable _cv;

        void Log(const std::string& prefix, const std::string& str);
        void Process();
    };
}
