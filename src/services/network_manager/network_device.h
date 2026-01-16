#pragma once
#include "network_access_point.h"
#include "network_manager_service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::NetworkManager
{
    class NetworkDevice
    {
        friend class NetworkManagerService;

    public:
        NetworkDevice(NetworkManagerService& source,
                      Connection& connection,
                      ObjectPath objectPath);

        [[nodiscard]] std::string GetId() const { return _id.Get(); }
        [[nodiscard]] std::string GetType() const { return _type.Get(); }
        [[nodiscard]] std::string GetState() const { return _state.Get(); }
        [[nodiscard]] std::string GetHwAddress() const { return _hwAddress.Get(); }
        [[nodiscard]] bool GetManaged() const { return _managed.Get(); }
        [[nodiscard]] std::string GetActiveConnection() const { return _activeConnection.Get(); }

        void SetState(const std::string& state);

        void EmitStateChanged(const std::string& oldState, const std::string& newState);

    protected:
        NetworkDevice(NetworkManagerService& source,
                      Connection& connection,
                      ObjectPath objectPath,
                      const std::string &childIfaceName);

        Connection& _conn;
        ObjectPath _path;
        Object _object;
        InterfaceName _interfaceName;
        Interface& _iface;

    private:
        Prop<std::string> _id;
        Prop<std::string> _type;
        Prop<std::string> _state;
        Prop<std::string> _hwAddress;
        Prop<bool> _managed;
        Prop<std::string> _activeConnection;
    };

    class NetworkWifiDevice : public NetworkDevice
    {
    public:
        NetworkWifiDevice(NetworkManagerService& source,
                          Connection& connection,
                          ObjectPath objectPath);

    private:
        //InterfaceName _interfaceName;
        //Interface& _iface;
        std::unordered_map<ObjectPath, std::unique_ptr<NetworkAccessPoint>, ObjectPathHash> _accessPoints;

        void OnScan(const Message& message);
        void OnConnect(const Message& message);
        void OnDisconnect(const Message& message);

        void EmitAccessPointAdded(const ObjectPath& path) const;
        void EmitAccessPointRemoved(const ObjectPath& path) const;
    };
}
