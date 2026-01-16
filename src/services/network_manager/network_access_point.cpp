#include "network_access_point.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::NetworkManager
{

    NetworkAccessPoint::NetworkAccessPoint(NetworkManagerService& source,
                                           Connection& connection,
                                           ObjectPath objectPath) :
                                           _conn(connection),
                                           _path(std::move(objectPath)),
                                           _object(_conn, _path),
                                           _interfaceName(source.GetBaseInterface().Child("AccessPoint")),
                                           _iface(_object.CreateInterface(_interfaceName)),
                                           _ssid     (_conn, _iface, "Ssid", ""),
                                           _strength (_conn, _iface, "Strength", 0),
                                           _security (_conn, _iface, "Security", ""),
                                           _frequency(_conn, _iface, "Frequency", 0),
                                           _connected(_conn, _iface, "Connected", false)
    {

    }

}
