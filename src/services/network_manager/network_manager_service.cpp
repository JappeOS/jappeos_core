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

#include "network_manager_service.h"

#include "network_device.h"
#include "network_connection.h"
#include "../../utils/dbus_utils.h"
#include "../logger/logger_service.h"
#include "../session_manager/session_manager_service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::NetworkManager
{

    NetworkManagerService::NetworkManagerService(ServiceManager* serviceManager,
                                                 Connection* conn) :
                                                 Service(serviceManager, conn),
                                                 _object(*_conn, GetBaseObjectPath()),
                                                 _iface(_object.CreateInterface(GetBaseInterface()))
    {
        _iface.RegisterMethod("ListDevices", [&] (const auto& m) { OnListDevices(m); });

        _subNameOwnerChanged = std::make_unique<SignalSubscription>(_conn->SubscribeSignal(
            "org.freedesktop.DBus",
            ObjectPath("/org/freedesktop/DBus"),
            InterfaceName("org.freedesktop.DBus"),
            "NameOwnerChanged",
            [&](const Message& msg)
            {
                return OnNameOwnerChanged(msg);
            }
        ));

        TryInitNetworkManager();
    }

    NetworkManagerService::~NetworkManagerService() = default;

    std::vector<ObjectPath> NetworkManagerService::ListDevices() const
    {
        auto ks = std::views::keys(_devices);
        std::vector<ObjectPath> keys{ks.begin(), ks.end()};
        return keys;
    }

    // TODO: Polkit
    void NetworkManagerService::SharedPolicy(const Message& message) const
    {
        const auto sender = message.GetSender();
        const auto senderUid = _conn->GetUnixUser(sender);
        const pid_t senderPid = Utils::DBusUtils::GetSenderPID(_rawConn, message.GetRawMessage());

        const auto sessionMgr = _serviceManager->Get<SessionManager::SessionManagerService>();

        if (!sessionMgr->IsManagedUserSession(senderUid))
        {
            throw DBusException(DBUS_ERROR_ACCESS_DENIED, "Unknown user");
        }

        if (!sessionMgr->IsPrivilegedClientProcess(senderPid, true))
        {
            throw DBusException(DBUS_ERROR_ACCESS_DENIED, "Unauthorized process");
        }
    }

    void NetworkManagerService::OnListDevices(const Message& message) const
    {
        SharedPolicy(message);
        auto msg = Message::CreateMethodReturn(message);
        const auto list = ListDevices();
        _serviceManager->Get<Logger::LoggerService>()->Debug("OnListDevices called, returning " + std::to_string(list.size()) + " devices");
        msg.SetArgs(list);
        msg.Send(*_conn);
    }

    void NetworkManagerService::EmitDeviceAdded(const ObjectPath& path)
    {
        auto sig = Message::CreateSignal(
            GetBaseObjectPath(),
            GetBaseInterface(),
            "DeviceAdded"
        );

        _serviceManager->Get<Logger::LoggerService>()->Debug("EmitDeviceAdded called: " + path.ToString());

        sig.SetArgs(path);
        sig.Send(*_conn);
    }

    void NetworkManagerService::EmitDeviceRemoved(const ObjectPath& path)
    {
        auto sig = Message::CreateSignal(
            GetBaseObjectPath(),
            GetBaseInterface(),
            "DeviceRemoved"
        );

        _serviceManager->Get<Logger::LoggerService>()->Debug("EmitDeviceRemoved called: " + path.ToString());

        sig.SetArgs(path);
        sig.Send(*_conn);
    }

    void NetworkManagerService::OnNameOwnerChanged(const Message& msg)
    {
        const auto args = msg.GetArgs<std::string, std::string, std::string>();
        const auto name = std::get<0>(args);
        const auto oldOwner = std::get<1>(args);
        const auto newOwner = std::get<2>(args);

        if (name != "org.freedesktop.NetworkManager")
            return;

        if (!newOwner.empty())
            OnNetworkManagerAppeared();
        else
            OnNetworkManagerDisappeared();
    }

    void NetworkManagerService::TryInitNetworkManager()
    {
        _serviceManager->Get<Logger::LoggerService>()->Debug("TryInitNetworkManager called");
        if (_nmClient)
        {
            g_object_unref(_nmClient);
            _nmClient = nullptr;
        }

        GError* error = nullptr;
        _nmClient = nm_client_new(nullptr, &error);

        if (!_nmClient)
        {
            // NM not ready yet
            _serviceManager->Get<Logger::LoggerService>()->Debug("TryInitNetworkManager NM not ready yet");

            if (error)
                g_error_free(error);
            return;
        }

        DiscoverDevices();
        SubscribeToNmSignals();
        _serviceManager->Get<Logger::LoggerService>()->Debug("TryInitNetworkManager done");
    }

    void NetworkManagerService::OnNetworkManagerAppeared()
    {
        if (_nmClient)
            return;

        TryInitNetworkManager();
    }

    void NetworkManagerService::OnNetworkManagerDisappeared()
    {
        if (!_nmClient)
            return;

        for (const auto &path: _devices | std::views::keys)
        {
            EmitDeviceRemoved(path);
        }

        _connections.clear();
        _devices.clear();

        g_clear_object(&_nmClient);
    }

    void NetworkManagerService::DiscoverDevices()
    {
        const GPtrArray* nmDevices = nm_client_get_devices(_nmClient);

        for (guint i = 0; i < nmDevices->len; ++i)
        {
            const auto nmDev = NM_DEVICE(nmDevices->pdata[i]);
            AddDevice(nmDev);
        }
    }

    void NetworkManagerService::SubscribeToNmSignals()
    {
        g_signal_connect_object(
            _nmClient,
            "device-added",
            G_CALLBACK(+[] (NMClient* client, NMDevice* device, NetworkManagerService* self)
            {
                self->AddDevice(device);
            }),
            this,
            G_CONNECT_DEFAULT
        );

        g_signal_connect_object(
            _nmClient,
            "device-removed",
            G_CALLBACK(+[] (NMClient* client, NMDevice* device, NetworkManagerService* self)
            {
                self->RemoveDevice(device);
            }),
            this,
            G_CONNECT_DEFAULT
        );
    }

    void NetworkManagerService::SubscribeToDeviceSignals(NMDevice* nmDev)
    {
        g_signal_connect_object(
            nmDev,
            "notify::state",
            G_CALLBACK(+[](GObject* obj, GParamSpec*, NetworkManagerService* self)
            {
                self->OnDeviceStateChanged(NM_DEVICE(obj));
            }),
            this,
            G_CONNECT_DEFAULT
        );

        g_signal_connect_object(
            nmDev,
            "notify::active-connection",
            G_CALLBACK(+[](GObject* obj, GParamSpec*, NetworkManagerService* self)
            {
                self->OnDeviceActiveConnectionChanged(NM_DEVICE(obj));
            }),
            this,
            G_CONNECT_DEFAULT
        );

        g_signal_connect_object(
            nmDev,
            "notify::managed",
            G_CALLBACK(+[](GObject* obj, GParamSpec*, NetworkManagerService* self)
            {
                self->OnDeviceManagedChanged(NM_DEVICE(obj));
            }),
            this,
            G_CONNECT_DEFAULT
        );
    }

    void NetworkManagerService::AddDevice(NMDevice* nmDev)
    {
        const char* iface = nm_device_get_iface(nmDev);
        const NMDeviceType nmType = nm_device_get_device_type(nmDev);

        if (!iface)
            return;

        ObjectPath path = GetBaseObjectPath().Child("Devices").Child(EncodeForObjectPath(iface)); // TODO: Interface names can change???
        std::unique_ptr<NetworkDevice> device;

        if (_devices.contains(path))
            return;

        _serviceManager->Get<Logger::LoggerService>()->Debug("Add device: " + path.ToString());

        switch (nmType)
        {
            case NM_DEVICE_TYPE_WIFI:
                device = std::make_unique<NetworkWifiDevice>(*this, *_conn, path);
                break;

            case NM_DEVICE_TYPE_ETHERNET:
                device = std::make_unique<NetworkDevice>(*this, *_conn, path);
                break;

            default:
                // TODO: Ignore unsupported device types for now
                return;
        }

        SubscribeToDeviceSignals(nmDev);

        device->_id = iface;
        device->_managed = nm_device_get_managed(nmDev);

        if (const char* hw = nm_device_get_hw_address(nmDev))
            device->_hwAddress = hw;

        device->_type = DeviceTypeToString(nmType);
        device->_state = DeviceStateToString(nm_device_get_state(nmDev));

        _devices.emplace(path, std::move(device));
        EmitDeviceAdded(path);
    }

    void NetworkManagerService::RemoveDevice(NMDevice* nmDev)
    {
        const char* iface = nm_device_get_iface(nmDev);
        if (!iface)
            return;

        const ObjectPath path = GetBaseObjectPath().Child("Devices").Child(EncodeForObjectPath(iface));

        const auto it = _devices.find(path);
        if (it == _devices.end())
            return;

        EmitDeviceRemoved(path);

        const auto connPath = it->second->GetActiveConnection();
        if (connPath != ObjectPath{})
            _connections.erase(connPath);

        _devices.erase(it);
    }

    void NetworkManagerService::OnDeviceStateChanged(NMDevice* nmDev)
    {
        auto* dev = FindDevice(nmDev);
        if (!dev)
            return;

        const auto newState = DeviceStateToString(nm_device_get_state(nmDev));

        dev->SetState(newState);
    }

    void NetworkManagerService::OnDeviceManagedChanged(NMDevice* nmDev)
    {
        auto* dev = FindDevice(nmDev);
        if (!dev)
            return;

        const bool managed = nm_device_get_managed(nmDev);
        dev->SetManaged(managed);
    }

    void NetworkManagerService::OnDeviceActiveConnectionChanged(NMDevice* nmDev)
    {
        auto* dev = FindDevice(nmDev);
        if (!dev)
            return;

        NMActiveConnection* ac = nm_device_get_active_connection(nmDev);

        if (!ac)
        {
            const auto oldPath = dev->GetActiveConnection();
            if (oldPath != ObjectPath{})
            {
                //EmitConnectionRemoved(oldPath);
                _connections.erase(oldPath);
            }

            dev->SetActiveConnection(ObjectPath{});
            return;
        }

        const char* uuid = nm_active_connection_get_uuid(ac);

        const ObjectPath path = GetBaseObjectPath()
            .Child("Connections")
            .Child(EncodeForObjectPath(uuid));

        EnsureConnectionObject(nmDev, ac, path);
        dev->SetActiveConnection(path);
    }

    NetworkDevice* NetworkManagerService::FindDevice(NMDevice* nmDev)
    {
        const char* iface = nm_device_get_iface(nmDev);
        if (!iface)
            return nullptr;

        const ObjectPath path = GetBaseObjectPath()
            .Child("Devices")
            .Child(EncodeForObjectPath(iface));

        const auto it = _devices.find(path);
        return it != _devices.end() ? it->second.get() : nullptr;
    }

    NMDeviceWifi* NetworkManagerService::FindWifiDeviceForConnection(const NetworkConnection& conn)
    {
        auto* nmDev = nm_client_get_device_by_path(
            _nmClient,
            conn._nmDevicePath.ToString().c_str()
        );

        if (!nmDev || !NM_IS_DEVICE_WIFI(nmDev))
            return nullptr;

        return NM_DEVICE_WIFI(nmDev);
    }

    void NetworkManagerService::EnsureConnectionObject(NMDevice* nmDev, NMActiveConnection* ac, const ObjectPath& path)
    {
        if (_connections.contains(path))
            return;

        auto conn = std::make_unique<NetworkConnection>(*this, *_conn, path);

        conn->_id.Set(nm_active_connection_get_uuid(ac));
        conn->_type.Set(
            nm_active_connection_get_connection_type(ac)
                ? nm_active_connection_get_connection_type(ac)
                : "unknown"
        );
        conn->_state.Set(ActiveConnectionStateToString(nm_active_connection_get_state(ac)));
        conn->_nmActivePath = ObjectPath(nm_object_get_path(NM_OBJECT(ac)));
        conn->_nmDevicePath = ObjectPath(nm_object_get_path(NM_OBJECT(nmDev)));

        UpdateConnectionIpInfo(ac, *conn);
        SubscribeToActiveConnectionSignals(ac);

        if (NM_IS_DEVICE_WIFI(nmDev))
        {
            const auto devWifi = NM_DEVICE_WIFI(nmDev);
            UpdateConnectionSignalStrength(devWifi, *conn);
            SubscribeToWifiSignalStrength(devWifi, *conn);
        }

        _connections.emplace(path, std::move(conn));
    }

    void NetworkManagerService::RemoveConnection(NMActiveConnection* ac)
    {
        const char* uuid = nm_active_connection_get_uuid(ac);
        if (!uuid)
            return;

        const ObjectPath path = GetBaseObjectPath()
            .Child("Connections")
            .Child(EncodeForObjectPath(uuid));

        _connections.erase(path);
    }

    void NetworkManagerService::SubscribeToActiveConnectionSignals(NMActiveConnection* ac)
    {
        g_signal_connect_object(
            ac,
            "notify::state",
            G_CALLBACK(+[](GObject* obj, GParamSpec*, NetworkManagerService* self)
            {
                self->OnActiveConnectionStateChanged(
                    NM_ACTIVE_CONNECTION(obj)
                );
            }),
            this,
            G_CONNECT_DEFAULT
        );

        g_signal_connect_object(
            ac,
            "notify::ip4-config",
            G_CALLBACK(+[](GObject* obj, GParamSpec*, NetworkManagerService* self)
            {
                self->OnActiveConnectionIpChanged(
                    NM_ACTIVE_CONNECTION(obj)
                );
            }),
            this,
            G_CONNECT_DEFAULT
        );

        g_signal_connect_object(
            ac,
            "notify::ip6-config",
            G_CALLBACK(+[](GObject* obj, GParamSpec*, NetworkManagerService* self)
            {
                self->OnActiveConnectionIpChanged(
                    NM_ACTIVE_CONNECTION(obj)
                );
            }),
            this,
            G_CONNECT_DEFAULT
        );
    }

    void NetworkManagerService::SubscribeToWifiSignalStrength(NMDeviceWifi* nmDev, NetworkConnection& conn)
    {
        auto* ap = nm_device_wifi_get_active_access_point(nmDev);
        if (!ap)
        {
            conn._nmActiveApPath = ObjectPath{};
            conn._signalStrength.Set(0);
            return;
        }

        const ObjectPath apPath(
            nm_object_get_path(NM_OBJECT(ap))
        );

        if (conn._nmActiveApPath == apPath)
            return;

        conn._nmActiveApPath = apPath;

        g_signal_connect_object(
            ap,
            "notify::strength",
            G_CALLBACK(+[](GObject* obj, GParamSpec*, NetworkManagerService* self)
            {
                self->OnAccessPointStrengthChanged(
                    NM_ACCESS_POINT(obj)
                );
            }),
            this,
            G_CONNECT_DEFAULT
        );
    }

    NetworkConnection* NetworkManagerService::FindConnection(NMActiveConnection* ac)
    {
        const char* uuid = nm_active_connection_get_uuid(ac);

        if (!uuid)
            return nullptr;

        const ObjectPath path = GetBaseObjectPath()
            .Child("Connections")
            .Child(EncodeForObjectPath(uuid));

        const auto it = _connections.find(path);
        return it != _connections.end()
            ? it->second.get()
            : nullptr;
    }

    void NetworkManagerService::OnActiveConnectionStateChanged(NMActiveConnection* ac)
    {
        auto* conn = FindConnection(ac);
        if (!conn)
            return;

        conn->_state.Set(
            ActiveConnectionStateToString(nm_active_connection_get_state(ac))
        );
    }

    void NetworkManagerService::OnActiveConnectionIpChanged(NMActiveConnection* ac)
    {
        auto* conn = FindConnection(ac);
        if (!conn)
            return;

        UpdateConnectionIpInfo(ac, *conn);
    }

    void NetworkManagerService::OnAccessPointStrengthChanged(NMAccessPoint* ap)
    {
        auto* conn = FindConnectionByAccessPoint(ap);
        if (!conn)
            return;

        auto* dev = FindWifiDeviceForConnection(*conn);
        if (!dev)
            return;

        UpdateConnectionSignalStrength(dev, *conn);
    }

    void NetworkManagerService::UpdateConnectionIpInfo(NMActiveConnection* ac, NetworkConnection& conn)
    {
        // IPv4
        if (auto* ip4 = nm_active_connection_get_ip4_config(ac))
        {
            const GPtrArray* addrs = nm_ip_config_get_addresses(ip4);
            if (addrs && addrs->len > 0)
            {
                auto* addr = static_cast<NMIPAddress*>(addrs->pdata[0]);
                conn._ip4Address.Set(
                    nm_ip_address_get_address(addr)
                );
            }
            else
            {
                conn._ip4Address.Set("");
            }
        }
        else
        {
            conn._ip4Address.Set("");
        }

        // IPv6
        if (auto* ip6 = nm_active_connection_get_ip6_config(ac))
        {
            const GPtrArray* addrs = nm_ip_config_get_addresses(ip6);
            if (addrs && addrs->len > 0)
            {
                auto* addr = static_cast<NMIPAddress*>(addrs->pdata[0]);
                conn._ip6Address.Set(
                    nm_ip_address_get_address(addr)
                );
            }
            else
            {
                conn._ip6Address.Set("");
            }
        }
        else
        {
            conn._ip6Address.Set("");
        }
    }

    void NetworkManagerService::UpdateConnectionSignalStrength(NMDeviceWifi* nmDev, NetworkConnection& conn)
    {
        if (auto* ap = nm_device_wifi_get_active_access_point(nmDev))
        {
            conn._signalStrength.Set(
                nm_access_point_get_strength(ap)
            );
        }
        else
        {
            conn._signalStrength.Set(0);
        }
    }

    NetworkConnection* NetworkManagerService::FindConnectionByAccessPoint(NMAccessPoint* ap)
    {
        const char* apPath = nm_object_get_path(NM_OBJECT(ap));
        if (!apPath)
            return nullptr;

        for (auto& [_, conn] : _connections)
        {
            auto* nmDev = nm_client_get_device_by_path(
                _nmClient,
                conn->_nmDevicePath.ToString().c_str()
            );

            if (!nmDev || !NM_IS_DEVICE_WIFI(nmDev))
                continue;

            auto* wifi = NM_DEVICE_WIFI(nmDev);
            auto* activeAp =
                nm_device_wifi_get_active_access_point(wifi);

            if (!activeAp)
                continue;

            if (strcmp(
                    nm_object_get_path(NM_OBJECT(activeAp)),
                    apPath
                ) == 0)
            {
                return conn.get();
            }
        }

        return nullptr;
    }

}