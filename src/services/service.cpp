#include "service.h"
#include "logger/logger_service.h"

using namespace JappeStudios::JappeOS::JappeOSCore::Services;

namespace JappeStudios::JappeOS::JappeOSCore::Services
{
    ServiceManager::~ServiceManager()
    {
        for (auto& [_, service] : _services)
        {
            try
            {
                delete service;
            }
            catch (const std::exception& e)
            {
                NULL_SAFE_CALL(Get<Logger::LoggerService>(), Err(std::string("Cleanup failure: ") + e.what()));
            }
        }

        _services.clear();
        _servicesNamed.clear();
    }
}