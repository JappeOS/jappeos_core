#include "network_connection.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::NetworkManager
{

    NetworkConnection::NetworkConnection(NetworkManagerService& source,
                                         Connection& connection,
                                         ObjectPath objectPath) :
                                         _conn(connection),
                                         _path(std::move(objectPath)),
                                         _object(_conn, _path),
                                         _interfaceName(source.GetBaseInterface().Child("Connection")),
                                         _iface(_object.CreateInterface(_interfaceName)),
                                         _id     (_conn, _iface, "Id", ""),
                                         _type (_conn, _iface, "Type", ""),
                                         _state (_conn, _iface, "State", ""),
                                         _ip4Address(_conn, _iface, "Ip4Address", ""),
                                         _ip6Address(_conn, _iface, "Ip6Address", ""),
                                         _signalStrength(_conn, _iface, "SignalStrength", -1)
    {

    }

}