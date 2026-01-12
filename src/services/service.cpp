#include "service.h"
#include "logger/logger_service.h"

using namespace JappeStudios::JappeOS::JappeOSCore::Services;

namespace JappeStudios::JappeOS::JappeOSCore::Services
{

    // Service

    Service::Service(ServiceManager* serviceManager, Connection* conn) : _serviceManager(serviceManager), _rawConn(conn->GetRawConnection()), _conn(conn)
    {

    }

    void Service::SendErrorReplyAndLogLegacy(DBusConnection* conn, DBusMessage* msg, const std::string& errorName, const std::string& errorMsg)
    {
        DBusMessage* error = dbus_message_new_error(
            msg,
            errorName.c_str(),
            errorMsg.c_str()
        );
        dbus_connection_send(conn, error, nullptr);
        dbus_connection_flush(conn);
        dbus_message_unref(error);
        _serviceManager->Get<Logger::LoggerService>()->Notice(
            std::format("SendErrorReply(service='{}', errorName='{}', errorMsg='{}')", GetName(), errorName, errorMsg));
    }

    void Service::SendSuccessReplyAndLogLegacy(DBusConnection* conn, DBusMessage* msg, const std::string& message)
    {
        DBusMessage* reply = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, reply, nullptr);
        dbus_connection_flush(conn);
        dbus_message_unref(reply);
        if (message.empty()) _serviceManager->Get<Logger::LoggerService>()->Debug("SendSuccessReply");
        else                 _serviceManager->Get<Logger::LoggerService>()->Debug(
                             std::format("SendSuccessReply(service='{}', msg='{}')", GetName(), message));
    }

    void Service::SubscribeToSignalLegacy(const std::string& signalName) { _serviceManager->SubscribeServiceToSignalLegacy(signalName, this); }

    // ServiceManager

    ServiceManager::~ServiceManager()
    {
        for (auto it = _order.rbegin(); it != _order.rend(); ++it)
        {
            if (auto found = _services.find(*it); found != _services.end())
            {
                try
                {
                    const auto svcIfaceName = found->second->GetBaseInterface().ToString();
                    if (const auto& it = _runBeforeCleanup.find(svcIfaceName); it != _runBeforeCleanup.end())
                    {
                        for (const auto& fun : it->second)
                        {
                            try { fun(); }
                            catch (const std::exception& e) { NULL_SAFE_CALL(Get<Logger::LoggerService>(), Err(std::string("Pre-cleanup runnable failed for `") + svcIfaceName + "`: " + e.what())); }
                        }
                    }

                    delete found->second;
                }
                catch (const std::exception& e)
                {
                    NULL_SAFE_CALL(Get<Logger::LoggerService>(), Err(std::string("Cleanup failure: ") + e.what()));
                }
                _services.erase(found);
            }
        }

        _order.clear();
        _interfaceServiceMap.clear();
        _signalSubscribers.clear();
    }

    void ServiceManager::LogErr(const std::string& str)
    {
        NULL_SAFE_CALL(Get<Logger::LoggerService>(), Err(str));
    }

}