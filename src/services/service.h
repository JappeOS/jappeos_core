#pragma once

#include <format>
#include <functional>
#include <map>
#include <ranges>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>
#include <dbus/dbus.h>

#include "../globals.h"
#include "../dbus/dbus_wrapper.h"

namespace JappeStudios::JappeOS::JappeOSCore {
    class Message;
    class Connection;
}

// TODO: Move method implementations to source file
namespace JappeStudios::JappeOS::JappeOSCore::Services
{
#define NO_SETTER() throw DBusException(DBUS_ERROR_PROPERTY_READ_ONLY);

    class LoggerService;
    class ServiceManager;
    static auto DBUS_INTERFACE = "org.jappeos.Core";
    static auto DBUS_PATH      = "/org/jappeos/Core";

    class Service
    {
    public:
        explicit Service(ServiceManager* serviceManager, Connection* conn);
        virtual ~Service() = default;

        /// Initializes D-Bus and adds match rule for this service.
        virtual void InitDBus(Connection* connection)
        {
            DBusError err;
            dbus_error_init(&err);

            _conn = connection;

            // TODO: REMOVE FOLLOWING LINE AFTER DEPRECATING THE LEGACY MESSAGE HANDLING SYSTEM.
            dbus_bus_add_match(_conn->GetRawConnection(), std::format("type='method_call',interface='{}'", GetBaseInterface().ToString()).c_str(), &err);
            dbus_error_free(&err);
        }

        #pragma region LegacyMethods

        /// Sends an error reply to the connection for a message. Logging before or after calling this method is not required,
        /// since this method logs the `errorName` and `errorMsg` parameters automatically.
        void SendErrorReplyAndLogLegacy(DBusConnection* conn, DBusMessage* msg, const std::string& errorName, const std::string& errorMsg);

        /// Sends a success reply to the connection for a message. Logging before or after calling this method is not required,
        /// since this method logs the `message` parameter automatically.
        void SendSuccessReplyAndLogLegacy(DBusConnection* conn, DBusMessage* msg, const std::string& message = std::string());

        /// Allows a signal to be received through `HandleMethodCall`, match rules still need to be added separately.
        /// `signalName` is in the following format: "<interface>.<member>".
        void SubscribeToSignalLegacy(const std::string& signalName);

        /// Handles incoming D-Bus messages and signals.
        virtual bool HandleMethodCallLegacy(DBusMessage* message) { return false; }

        #pragma endregion

        /// Returns the name of the service.
        [[nodiscard]] virtual std::string GetName() const = 0;

        [[nodiscard]] InterfaceName GetBaseInterface()
        {
            if (!_baseInterface.has_value())
                _baseInterface = InterfaceName(std::string(DBUS_INTERFACE) + "." + GetName());

            return _baseInterface.value();
        }

        [[nodiscard]] ObjectPath GetBaseObjectPath()
        {
            if (!_baseObjectPath.has_value())
                _baseObjectPath = ObjectPath(std::string(DBUS_PATH) + "/" + GetName());

            return _baseObjectPath.value();
        }

    protected:
        ServiceManager* _serviceManager;
        DBusConnection* _rawConn;
        Connection* _conn;

        std::optional<InterfaceName> _baseInterface = std::nullopt;
        std::optional<ObjectPath> _baseObjectPath = std::nullopt;
    };

    class ServiceManager
    {
    public:
        /// Initializes D-Bus for all services.
        void InitDBus(Connection* conn)
        {
            _conn = conn;

            for (auto& service : _services)
                service.second->InitDBus(_conn);

            _dbusInitialized = true;
        }

        /// Registers a service that lives for the entire lifecycle of this daemon.
        template <typename T>
        std::enable_if_t<std::is_base_of_v<Service, T>, T*> Register()
        {
            //static_assert(std::is_default_constructible_v<T>, "Service must be default-constructible"); TODO

            const std::type_index key = typeid(T);
            if (_services.contains(key))
                return nullptr;

            T* instance;
            try
            {
                instance = new T(this, _dbusInitialized ? _conn : nullptr);
            }
            catch (const std::exception& e)
            {
                LogErr(std::format("Failed to register service at {}: {}", _services.size(), e.what()));
                return nullptr;
            }

            _order.push_back(key);
            _services.emplace(key, instance);
            _interfaceServiceMap.emplace(instance->GetBaseInterface(), instance); // TODO: REMOVE THIS LINE AFTER DEPRECATING THE LEGACY MESSAGE HANDLING SYSTEM.

            if (_dbusInitialized)
                instance->InitDBus(_conn);

            return instance;
        }

        /// Returns a pointer to a service by-type.
        template <typename T>
        std::enable_if_t<std::is_base_of_v<Service, T>, T*> Get()
        {
            // Look for a service instance that can be cast to T*
            for (const auto &service: _services | std::views::values)
            {
                if (T* casted = dynamic_cast<T*>(service))
                {
                    return casted;
                }
            }
            return nullptr;
        }

        /// Subscribes a service to a signal. `signalName` is in the following format: "<interface>.<member>".
        void SubscribeServiceToSignalLegacy(const std::string& signalName, Service* service)
        {
            _signalSubscribers[signalName].push_back(service);
        }

        /// Runs a specific task `runnable` before the `service` is destroyed.
        void RunBeforeCleanup(Service* service, const std::function<void()>& runnable)
        {
            _runBeforeCleanup[service->GetBaseInterface().ToString()].push_back(runnable);
        }

        /// Returns a map of all services.
        [[nodiscard]] const std::map<std::type_index, Service*>& List() const { return _services; }

        /// Returns a map of service names and service objects.
        [[nodiscard]] const std::map<std::string, Service*>& ListNamed() const { return _interfaceServiceMap; }

        /// Returns a map of signal subscribers, with the key being the signal name ("<interface>.<member>") and the
        /// value being a vector of full interface names for the subscribers to that signal name.
        [[nodiscard]] const std::unordered_map<std::string, std::vector<Service*>>& ListSignalSubscribers() const { return _signalSubscribers; }

        ~ServiceManager();

    private:
        std::map<std::type_index, Service*> _services;
        std::vector<std::type_index> _order;
        std::map<std::string, Service*> _interfaceServiceMap; // key: D-Bus interface
        std::unordered_map<std::string, std::vector<Service*>> _signalSubscribers; // key: interface.member (e.g. "org.freedesktop.systemd1.Manager.JobRemoved")
        std::unordered_map<std::string, std::vector<std::function<void()>>> _runBeforeCleanup;

        Connection* _conn = nullptr;
        bool _dbusInitialized = false;

        void LogErr(const std::string& str);
    };
}
