#include "service.h"
#include "logger/logger_service.h"

using namespace JappeStudios::JappeOS::JappeOSCore::Services;

namespace JappeStudios::JappeOS::JappeOSCore::Services
{
    void Service::SubscribeToSignal(const std::string& signalName) { _serviceManager->SubscribeServiceToSignal(signalName, GetFullInterfaceName(this)); }

    ServiceManager::~ServiceManager()
    {
        for (auto it = _order.rbegin(); it != _order.rend(); ++it)
        {
            if (auto found = _services.find(*it); found != _services.end())
            {
                try
                {
                    delete found->second;
                }
                catch (const std::exception& e)
                {
                    NULL_SAFE_CALL(Get<Logger::LoggerService>(), Err(std::string("Cleanup failure: ") + e.what()));
                }
                _services.erase(found);
            }
        }

        _servicesNamed.clear();
    }
}