/*
 * jappeos_core, Core system management daemon for JappeOS.
 * Copyright (C) 2026  Jappe02
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

#pragma once
#include "network_manager_service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::NetworkManager
{
    class NetworkConnection
    {
        friend class NetworkManagerService;

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

        ObjectPath _nmActivePath;
        ObjectPath _nmDevicePath;
        ObjectPath _nmActiveApPath;
    };
}