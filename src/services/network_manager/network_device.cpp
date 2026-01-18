#include "network_device.h"

#include <utility>

#include "network_device_def.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::NetworkManager
{

    // NetworkDevice

    NetworkDevice::NetworkDevice(NetworkManagerService& source,
                                 Connection& connection,
                                 ObjectPath objectPath) :
                                 NetworkDevice(source, connection, std::move(objectPath), {})
    {}

    NetworkDevice::NetworkDevice(NetworkManagerService& source,
                                 Connection& connection,
                                 ObjectPath objectPath,
                                 const std::string &childIfaceName) :
                                 _conn(connection),
                                 _path(std::move(objectPath)),
                                 _object(_conn, _path),
                                 _interfaceName(childIfaceName.empty() ? source.GetBaseInterface().Child("Device") : source.GetBaseInterface().Child("Device").Child(childIfaceName)),
                                 _iface(_object.CreateInterface(_interfaceName)),
                                 _id              (_conn, _iface, "Id", ""),
                                 _type            (_conn, _iface, "Type", ""),
                                 _state           (_conn, _iface, "State", NETWORK_DEVICE_STATE_DISCONNECTED),
                                 _hwAddress       (_conn, _iface, "HwAddress", ""),
                                 _managed         (_conn, _iface, "Managed", false),
                                 _activeConnection(_conn, _iface, "ActiveConnection", ObjectPath())
    {}

    // NetworkWifiDevice

    NetworkWifiDevice::NetworkWifiDevice(NetworkManagerService& source,
                                         Connection& connection,
                                         ObjectPath objectPath) :
                                         NetworkDevice(source,
                                                       connection,
                                                       std::move(objectPath),
                                                       "WiFi")
    {
        _iface.RegisterMethod("Scan",       [&](const auto &m) { OnScan(m); });
        _iface.RegisterMethod("Connect",    [&](const auto &m) { OnConnect(m); });
        _iface.RegisterMethod("Disconnect", [&](const auto &m) { OnDisconnect(m); });
        _iface.RegisterProperty<std::vector<ObjectPath>>(
            "AccessPoints",
            [this]
            {
                auto ks = std::views::keys(_accessPoints);
                std::vector<ObjectPath> keys{ks.begin(), ks.end()};
                return keys;
            }
        );
    }

    void NetworkWifiDevice::OnScan(const Message& message)
    {

    }

    void NetworkWifiDevice::OnConnect(const Message& message)
    {

    }

    void NetworkWifiDevice::OnDisconnect(const Message& message)
    {

    }

    void NetworkWifiDevice::EmitAccessPointAdded(const ObjectPath& path) const
    {
        auto sig = Message::CreateSignal(
            _path,
            _interfaceName,
            "AccessPointAdded"
        );

        sig.SetArgs(path);
        sig.Send(_conn);
    }

    void NetworkWifiDevice::EmitAccessPointRemoved(const ObjectPath& path) const
    {
        auto sig = Message::CreateSignal(
            _path,
            _interfaceName,
            "AccessPointRemoved"
        );

        sig.SetArgs(path);
        sig.Send(_conn);
    }

}
