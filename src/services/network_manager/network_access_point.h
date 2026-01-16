#pragma once
#include "network_access_point.h"
#include "network_manager_service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::NetworkManager
{
    class NetworkAccessPoint
    {
    public:
        NetworkAccessPoint(NetworkManagerService& source,
                           Connection& connection,
                           ObjectPath objectPath);

        [[nodiscard]] std::string GetSsid() const { return _ssid.Get(); }
        [[nodiscard]] int GetStrength() const { return _strength.Get(); }
        [[nodiscard]] std::string GetSecurity() const { return _security.Get(); }
        [[nodiscard]] int GetFrequency() const { return _frequency.Get(); }
        [[nodiscard]] bool GetConnected() const { return _connected.Get(); }

    protected:
        Connection& _conn;
        ObjectPath _path;
        Object _object;
        InterfaceName _interfaceName;
        Interface& _iface;

    private:
        Prop<std::string> _ssid;
        Prop<int> _strength;
        Prop<std::string> _security;
        Prop<int> _frequency;
        Prop<bool> _connected;
    };
}
