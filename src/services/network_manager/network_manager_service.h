#pragma once
#include <NetworkManager.h>
#include "../service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::NetworkManager
{
    class NetworkDevice;

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
        void AddDevice(NMDevice* nmDev);

    private:
        static std::string DeviceTypeToString(NMDeviceType t)
        {
            switch (t)
            {
                case NM_DEVICE_TYPE_WIFI:     return "wifi";
                case NM_DEVICE_TYPE_ETHERNET: return "ethernet";
                default:                      return "unknown";
            }
        }

        static std::string DeviceStateToString(NMDeviceState s)
        {
            switch (s)
            {
                case NM_DEVICE_STATE_ACTIVATED: return "connected";
                case NM_DEVICE_STATE_PREPARE:
                case NM_DEVICE_STATE_CONFIG:
                case NM_DEVICE_STATE_NEED_AUTH:
                    return "connecting";
                default:
                    return "disconnected";
            }
        }
    };
}