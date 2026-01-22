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

        [[nodiscard]] std::string GetSsid() const     { return _ssid.Get(); }
        [[nodiscard]] int GetStrength() const         { return _strength.Get(); }
        [[nodiscard]] std::string GetSecurity() const { return _security.Get(); }
        [[nodiscard]] int GetFrequency() const        { return _frequency.Get(); }
        [[nodiscard]] bool GetConnected() const       { return _connected.Get(); }

    protected:
        Connection&   _conn;
        ObjectPath    _path;
        Object        _object;
        InterfaceName _interfaceName;
        Interface&    _iface;

    private:
        Prop<std::string> _ssid;
        Prop<int>         _strength;
        Prop<std::string> _security;
        Prop<int>         _frequency;
        Prop<bool>        _connected;
    };
}
