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
    class NetworkDevice
    {
        friend class NetworkManagerService;

    public:
        NetworkDevice(NetworkManagerService& source,
                      Connection& connection,
                      ObjectPath objectPath);

        [[nodiscard]] std::string GetId() const              { return _id.Get(); }
        [[nodiscard]] std::string GetType() const            { return _type.Get(); }
        [[nodiscard]] std::string GetState() const           { return _state.Get(); }
        [[nodiscard]] std::string GetHwAddress() const       { return _hwAddress.Get(); }
        [[nodiscard]] bool GetManaged() const                { return _managed.Get(); }
        [[nodiscard]] ObjectPath GetActiveConnection() const { return _activeConnection.Get(); }

        void SetState(const std::string& state);
        void SetManaged(bool managed);
        void SetActiveConnection(const ObjectPath& path);

    protected:
        Connection&   _conn;
        ObjectPath    _path;
        Object        _object;

    private:
        InterfaceName     _interfaceName;
        Interface&        _iface;
        Prop<std::string> _id;
        Prop<std::string> _type;
        Prop<std::string> _state;
        Prop<std::string> _hwAddress;
        Prop<bool>        _managed;
        Prop<ObjectPath>  _activeConnection;
    };

    class NetworkWifiDevice : public NetworkDevice
    {
    public:
        NetworkWifiDevice(NetworkManagerService& source,
                          Connection& connection,
                          ObjectPath objectPath);

    private:
        InterfaceName _interfaceName;
        Interface&    _iface;
        std::unordered_map<ObjectPath, std::unique_ptr<NetworkAccessPoint>, ObjectPathHash> _accessPoints;

        void OnScan(const Message& message);
        void OnConnect(const Message& message);
        void OnDisconnect(const Message& message);

        void EmitAccessPointAdded(const ObjectPath& path) const;
        void EmitAccessPointRemoved(const ObjectPath& path) const;
    };
}
