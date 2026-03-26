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

#include "network_device.h"

#include <utility>

#include "network_device_def.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::NetworkManager
{

    // NetworkDevice

    NetworkDevice::NetworkDevice(NetworkManagerService& source,
                                 Connection& connection,
                                 ObjectPath objectPath) :
                                 _source(source),
                                 _conn(connection),
                                 _path(std::move(objectPath)),
                                 _object(_conn, _path),
                                 _interfaceName(source.GetBaseInterface().Child("Device")),
                                 _iface(_object.CreateInterface(_interfaceName)),
                                 _id              (_conn, _iface, "Id", ""),
                                 _type            (_conn, _iface, "Type", ""),
                                 _state           (_conn, _iface, "State", NETWORK_DEVICE_STATE_DISCONNECTED),
                                 _hwAddress       (_conn, _iface, "HwAddress", ""),
                                 _managed         (_conn, _iface, "Managed", false),
                                 _activeConnection(_conn, _iface, "ActiveConnection", ObjectPath())
    {
        _iface.RegisterMethod("SetEnabled", [&](const auto &m) { OnSetEnabled(m); });
    }

    void NetworkDevice::SetState(const std::string& state)
    {
        if (_state.Get() == state)
            return;

        const auto old = _state;
        _state = state;
    }

    void NetworkDevice::SetManaged(const bool managed)
    {
        if (_managed.Get() == managed)
            return;

        _managed = managed;
    }

    void NetworkDevice::SetActiveConnection(const ObjectPath& path)
    {
        if (_activeConnection.Get() == path)
            return;

        _activeConnection = path;
    }

    void NetworkDevice::OnSetEnabled(const Message& message)
    {
        const auto [enabled] = message.GetArgs<bool>();
        _source.EthernetSetEnabled(*this, enabled, message);
    }

    // NetworkWifiDevice

    NetworkWifiDevice::NetworkWifiDevice(NetworkManagerService& source,
                                         Connection& connection,
                                         ObjectPath objectPath) :
                                         NetworkDevice(source, connection, std::move(objectPath)),
                                         _interfaceName(source.GetBaseInterface().Child("Device").Child("WiFi")),
                                         _iface(_object.CreateInterface(_interfaceName))
    {
        _iface.RegisterMethod("Scan", [&](const auto &m) { OnScan(m); });
        _iface.RegisterMethod("Connect", [&](const auto &m) { OnConnect(m); });
        _iface.RegisterMethod("Disconnect", [&](const auto &m) { OnDisconnect(m); });
        _iface.RegisterProperty<std::vector<ObjectPath>>(
            "AccessPoints",
            [&]
            {
                auto ks = std::views::keys(_accessPoints);
                std::vector<ObjectPath> keys{ks.begin(), ks.end()};
                return keys;
            }
        );
    }

    void NetworkWifiDevice::OnScan(const Message& message)
    {
        _source.WifiScan(*this, message);
    }

    void NetworkWifiDevice::OnConnect(const Message& message)
    {
        const auto [ssid, security, secret]
                = message.GetArgs<std::string, std::string, std::string>();
        _source.WifiConnect(*this, ssid, security, secret, message);
    }

    void NetworkWifiDevice::OnDisconnect(const Message& message)
    {
        _source.WifiDisconnect(*this, message);
    }

    void NetworkWifiDevice::OnSetEnabled(const Message& message)
    {
        const auto [enabled] = message.GetArgs<bool>();
        _source.WifiSetEnabled(*this, enabled, message);
    }

    void NetworkWifiDevice::EmitConnectResult(const uint64_t requestId,
                                              const bool success,
                                              const std::string& reasonCode,
                                              const std::string& reasonMessage) const
    {
        auto sig = Message::CreateSignal(
            _path,
            _interfaceName,
            "ConnectResult"
        );

        sig.SetArgs(requestId, success, reasonCode, reasonMessage);
        sig.Send(_conn);
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
