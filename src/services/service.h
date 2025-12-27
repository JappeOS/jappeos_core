#pragma once

#include <format>
#include <map>
#include <ranges>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>
#include <dbus/dbus.h>

#include "../globals.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services
{
    class LoggerService;
    class ServiceManager;
    static auto DBUS_INTERFACE = "org.jappeos.Core";

    class Service
    {
    public:
        /// Gets the full D-Bus interface name of a specific service.
        static std::string GetFullInterfaceName(Service* service) { return std::string(DBUS_INTERFACE) + "." + service->GetName(); }

    public:
        explicit Service(ServiceManager* serviceManager, DBusConnection* conn) : _serviceManager(serviceManager), _conn(conn) {}
        virtual ~Service() = default;

        /// Initializes D-Bus and adds match rule for this service.
        virtual void InitDBus(DBusConnection* connection)
        {
            DBusError err;
            dbus_error_init(&err);

            _conn = connection;

            dbus_bus_add_match(_conn, std::format("type='method_call',interface='{}'", GetFullInterfaceName(this)).c_str(), &err);
            dbus_error_free(&err); // TODO: LOG
        }

        /// Sends an error reply to the connection for a message. Logging before or after calling this method is not required,
        /// since this method logs the `errorName` and `errorMsg` parameters automatically.
        void SendErrorReplyAndLog(DBusConnection* conn, DBusMessage* msg, const std::string& errorName, const std::string& errorMsg);

        /// Sends a success reply to the connection for a message. Logging before or after calling this method is not required,
        /// since this method logs the `message` parameter automatically.
        void SendSuccessReplyAndLog(DBusConnection* conn, DBusMessage* msg, const std::string& message = std::string());

        /// Allows a signal to be received through `HandleMethodCall`, match rules still need to be added separately.
        /// `signalName` is in the following format: "<interface>.<member>".
        void SubscribeToSignal(const std::string& signalName);

        /// Registers a sub-interface for this service. The `name` is just the postfix added to the interface name of this
        /// service.
        void RegisterSubInterface(const std::string& name);

        /// Handles incoming D-Bus messages and signals.
        virtual bool HandleMethodCall(DBusMessage* message, const std::string& subInterface) = 0;

        /// Returns the name of the service.
        virtual std::string GetName() = 0;

    protected:
        ServiceManager* _serviceManager;
        DBusConnection* _conn;
    };

    class ServiceManager
    {
    public:
        /// Initializes D-Bus for all services.
        void InitDBus(DBusConnection* conn)
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

            T* instance = new T(this, _dbusInitialized ? _conn : nullptr);
            _order.push_back(key);
            _services.emplace(key, instance);
            _servicesNamed.emplace(Service::GetFullInterfaceName(instance), instance);

            if (_dbusInitialized)
                instance->InitDBus(_conn);

            return instance;
        }

        /// Returns a pointer to a service by-type.
        template <typename T>
        std::enable_if_t<std::is_base_of_v<Service, T>, T*> Get()
        {
            // Look for a service instance that can be cast to T*
            for (const auto& [typeIdx, service] : _services)
            {
                if (T* casted = dynamic_cast<T*>(service))
                {
                    return casted;
                }
            }
            return nullptr;
        }

        /// Subscribes a service to a signal. `signalName` is in the following format: "<interface>.<member>".
        void SubscribeServiceToSignal(const std::string& signalName, const std::string& serviceName)
        {
            _signalSubscribers[signalName].push_back(serviceName);
        }

        /// Registers a sub-interface for a service. `subInterface` only needs what's appended to
        /// `Service::GetFullInterfaceName(service)`, not the full interface name.
        void RegisterServiceSubInterface(Service* service, const std::string& subInterface)
        {
            _servicesNamed.emplace(Service::GetFullInterfaceName(service) + "." + subInterface, service);
        }

        /// Returns a map of all services.
        [[nodiscard]] const std::map<std::type_index, Service*>& List() const { return _services; }

        /// Returns a map of service names and service objects.
        [[nodiscard]] const std::map<std::string, Service*>& ListNamed() const { return _servicesNamed; }

        /// Returns a map of signal subscribers, with the key being the signal name ("<interface>.<member>") and the
        /// value being a vector of full interface names for the subscribers to that signal name.
        [[nodiscard]] const std::unordered_map<std::string, std::vector<std::string>>& ListSignalSubscribers() const { return _signalSubscribers; }

        ~ServiceManager();

    private:
        std::map<std::type_index, Service*> _services;
        std::vector<std::type_index> _order;
        std::map<std::string, Service*> _servicesNamed; // key: D-Bus interface
        std::unordered_map<std::string, std::vector<std::string>> _signalSubscribers; // key: interface.member (e.g. "org.freedesktop.systemd1.Manager.JobRemoved")

        DBusConnection* _conn = nullptr;
        bool _dbusInitialized = false;
    };
}
