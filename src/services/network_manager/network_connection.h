#pragma once
#include "network_manager_service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::NetworkManager
{
    class NetworkConnection
    {
    public:
        NetworkConnection(NetworkManagerService& source,
                          Connection& connection,
                          ObjectPath objectPath);

        [[nodiscard]] std::string GetId() const { return _id.Get(); }
        [[nodiscard]] std::string GetType() const { return _type.Get(); }
        [[nodiscard]] std::string GetState() const { return _state.Get(); }
        [[nodiscard]] std::string GetIp4Address() const { return _ip4Address.Get(); }
        [[nodiscard]] std::string GetIp6Address() const { return _ip6Address.Get(); }
        [[nodiscard]] int GetSignalStrength() const { return _signalStrength.Get(); }

    protected:
        Connection& _conn;
        ObjectPath _path;
        Object _object;
        InterfaceName _interfaceName;
        Interface& _iface;

    private:
        Prop<std::string> _id;
        Prop<std::string> _type;
        Prop<std::string> _state;
        Prop<std::string> _ip4Address;
        Prop<std::string> _ip6Address;
        Prop<int> _signalStrength;
    };
}