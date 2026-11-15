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
                                         _id            (_conn, _iface, "Id", ""),
                                         _type          (_conn, _iface, "Type", ""),
                                         _state         (_conn, _iface, "State", ""),
                                         _ip4Address    (_conn, _iface, "Ip4Address", ""),
                                         _ip6Address    (_conn, _iface, "Ip6Address", ""),
                                         _signalStrength(_conn, _iface, "SignalStrength", -1)
    {}

}