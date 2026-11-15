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

#include "service.h"
#include "logger/logger_service.h"

using namespace JappeStudios::JappeOS::JappeOSCore::Services;

namespace JappeStudios::JappeOS::JappeOSCore::Services
{

    // Service

    Service::Service(ServiceManager* serviceManager, Connection* conn)
        : _serviceManager(serviceManager), _rawConn(conn->GetRawConnection()), _conn(conn)
    {}

    void Service::SendErrorReplyAndLogLegacy(DBusConnection* conn,
                                             DBusMessage* msg,
                                             const std::string& errorName,
                                             const std::string& errorMsg) const
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
            std::format(
                "SendErrorReply(service='{}', errorName='{}', errorMsg='{}')",
                GetName(),
                errorName,
                errorMsg
            )
        );
    }

    void Service::SendSuccessReplyAndLogLegacy(DBusConnection* conn,
                                               DBusMessage* msg,
                                               const std::string& message) const
    {
        DBusMessage* reply = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, reply, nullptr);
        dbus_connection_flush(conn);
        dbus_message_unref(reply);
        if (message.empty()) _serviceManager->Get<Logger::LoggerService>()->Debug("SendSuccessReply");
        else                 _serviceManager->Get<Logger::LoggerService>()->Debug(
                             std::format("SendSuccessReply(service='{}', msg='{}')", GetName(), message));
    }

    void Service::SubscribeToSignalLegacy(const std::string& signalName)
        { _serviceManager->SubscribeServiceToSignalLegacy(signalName, this); }

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
                            try
                            {
                                fun();
                            }
                            catch (const std::exception& e)
                            {
                                NULL_SAFE_CALL(
                                    Get<Logger::LoggerService>(),
                                    Err(std::string("Pre-cleanup runnable failed for `") + svcIfaceName + "`: "
                                        + e.what())
                                );
                            }
                        }
                    }

                    delete found->second;
                }
                catch (const std::exception& e)
                {
                    NULL_SAFE_CALL(
                        Get<Logger::LoggerService>(),
                        Err(std::string("Cleanup failure: ") + e.what())
                    );
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