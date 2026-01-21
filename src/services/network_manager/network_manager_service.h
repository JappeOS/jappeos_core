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
#include <NetworkManager.h>

#include "network_device_def.h"
#include "../service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::NetworkManager
{
    class NetworkDevice;
    class NetworkConnection;

    // NOTE: libnm is used only inside NetworkManagerService, never inside device classes.
    class NetworkManagerService : public Service
    {
    public:
        NetworkManagerService(ServiceManager* serviceManager, Connection* conn);
        ~NetworkManagerService() override;
        [[nodiscard]] std::string GetName() const override { return "NetworkManagerService"; }

        [[nodiscard]] std::vector<ObjectPath> ListDevices() const;

    private:
        Object _object;
        Interface& _iface;
        NMClient* _nmClient = nullptr;
        std::unordered_map<ObjectPath, std::unique_ptr<NetworkDevice>, ObjectPathHash> _devices;
        std::unordered_map<ObjectPath, std::unique_ptr<NetworkConnection>, ObjectPathHash> _connections;

        std::unique_ptr<SignalSubscription> _subNameOwnerChanged;

        void SharedPolicy(const Message& message) const;

        void OnListDevices(const Message& message) const;
        void EmitDeviceAdded(const ObjectPath& path);
        void EmitDeviceRemoved(const ObjectPath& path);

        void OnNameOwnerChanged(const Message& msg);
        void OnNetworkManagerAppeared();
        void OnNetworkManagerDisappeared();
        void TryInitNetworkManager();
        void DiscoverDevices();
        void SubscribeToNmSignals();

        void SubscribeToDeviceSignals(NMDevice* nmDev);
        void AddDevice(NMDevice* nmDev);
        void RemoveDevice(NMDevice* nmDev);
        void OnDeviceStateChanged(NMDevice* nmDev);
        void OnDeviceManagedChanged(NMDevice* nmDev);
        void OnDeviceActiveConnectionChanged(NMDevice* nmDev);
        NetworkDevice* FindDevice(NMDevice* nmDev);
        NMDeviceWifi* FindWifiDeviceForConnection(const NetworkConnection& conn);

        void EnsureConnectionObject(NMDevice* nmDev, NMActiveConnection* ac, const ObjectPath& path);
        void RemoveConnection(NMActiveConnection* ac);
        void SubscribeToActiveConnectionSignals(NMActiveConnection* ac);
        void SubscribeToWifiSignalStrength(NMDeviceWifi* nmDev, NetworkConnection& conn);
        NetworkConnection* FindConnection(NMActiveConnection* ac);
        void OnActiveConnectionStateChanged(NMActiveConnection* ac);
        void OnActiveConnectionIpChanged(NMActiveConnection* ac);
        void OnAccessPointStrengthChanged(NMAccessPoint* ap);
        void UpdateConnectionIpInfo(NMActiveConnection* ac, NetworkConnection& conn);
        void UpdateConnectionSignalStrength(NMDeviceWifi* nmDev, NetworkConnection& conn);
        NetworkConnection* FindConnectionByAccessPoint(NMAccessPoint* ap);

    private:
        static std::string DeviceTypeToString(const NMDeviceType t)
        {
            switch (t)
            {
                case NM_DEVICE_TYPE_WIFI:     return "wifi";
                case NM_DEVICE_TYPE_ETHERNET: return "ethernet";
                default:                      return "unknown";
            }
        }

        static std::string DeviceStateToString(const NMDeviceState s)
        {
            switch (s)
            {
                case NM_DEVICE_STATE_ACTIVATED: return NETWORK_DEVICE_STATE_CONNECTED;
                case NM_DEVICE_STATE_PREPARE:
                case NM_DEVICE_STATE_CONFIG:
                case NM_DEVICE_STATE_NEED_AUTH:
                    return NETWORK_DEVICE_STATE_CONNECTING;
                default:
                    return NETWORK_DEVICE_STATE_DISCONNECTED;
            }
        }

        static std::string ActiveConnectionStateToString(const NMActiveConnectionState s)
        {
            switch (s)
            {
                case NM_ACTIVE_CONNECTION_STATE_ACTIVATED:
                    return "activated";
                case NM_ACTIVE_CONNECTION_STATE_ACTIVATING:
                    return "activating";
                case NM_ACTIVE_CONNECTION_STATE_DEACTIVATING:
                    return "deactivating";
                default:
                    return "unknown";
            }
        }

        // TODO: Maybe use something better than this to send NM interface names over D-Bus.
        static std::string EncodeForObjectPath(const std::string_view s)
        {
            std::string out;
            for (unsigned char c : s)
            {
                if (std::isalnum(c) || c == '_')
                    out += c;
                else
                {
                    char buf[4];
                    std::snprintf(buf, sizeof(buf), "_%02X", c);
                    out += buf;
                }
            }
            return out;
        }
    };
}