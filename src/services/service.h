#pragma once

#include <format>
#include <map>
#include <ranges>
#include <string>
#include <typeindex>
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
        static std::string GetFullInterfaceName(Service* service) { return std::string(DBUS_INTERFACE) + "." + service->GetName(); }

        static void SendErrorReply(DBusConnection* conn, DBusMessage* msg, const std::string& errorName, const std::string& errorMsg)
        {
            DBusMessage* error = dbus_message_new_error(
                msg,
                errorName.c_str(),
                errorMsg.c_str()
            );
            dbus_connection_send(conn, error, nullptr);
            dbus_connection_flush(conn);
            dbus_message_unref(error);
        }

        static void SendSuccessReply(DBusConnection* conn, DBusMessage* msg)
        {
            DBusMessage* reply = dbus_message_new_method_return(msg);
            dbus_connection_send(conn, reply, nullptr);
            dbus_connection_flush(conn);
            dbus_message_unref(reply);
        }

    public:
        explicit Service(ServiceManager* serviceManager, DBusConnection* conn, DBusError* err) : _serviceManager(serviceManager), _conn(conn), _err(err) {}
        virtual ~Service() = default;

        virtual void InitDBus(DBusConnection* connection, DBusError* error)
        {
            _conn = connection;
            _err = error;
            dbus_bus_add_match(_conn, std::format("type='method_call',interface='{}'", GetFullInterfaceName(this)).c_str(), _err);
        }

        virtual bool HandleMethodCall(DBusMessage* message) = 0;
        virtual std::string GetName() = 0;

    protected:
        ServiceManager* _serviceManager;
        DBusConnection* _conn;
        DBusError* _err;
    };

    class ServiceManager
    {
    public:
        void InitDBus(DBusConnection* conn, DBusError* err)
        {
            _conn = conn;
            _err = err;

            for (auto& service : _services)
                service.second->InitDBus(_conn, _err);

            _dbusInitialized = true;
        }

        template <typename T>
        std::enable_if_t<std::is_base_of_v<Service, T>, T*> Register()
        {
            //static_assert(std::is_default_constructible_v<T>, "Service must be default-constructible"); TODO

            const std::type_index key = typeid(T);
            if (_services.contains(key))
                return nullptr;

            T* instance = new T(this, _dbusInitialized ? _conn : nullptr, _dbusInitialized ? _err : nullptr);
            _services.emplace(key, instance);
            _servicesNamed.emplace(Service::GetFullInterfaceName(instance), instance);

            if (_dbusInitialized)
                instance->InitDBus(_conn, _err);

            return instance;
        }

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

        [[nodiscard]] const std::map<std::type_index, Service*>& List() const { return _services; }

        [[nodiscard]] const std::map<std::string, Service*>& ListNamed() const { return _servicesNamed; }

        ~ServiceManager();

    private:
        std::map<std::type_index, Service*> _services;
        std::map<std::string, Service*> _servicesNamed;

        DBusConnection* _conn = nullptr;
        DBusError* _err = nullptr;
        bool _dbusInitialized = false;
    };
}
