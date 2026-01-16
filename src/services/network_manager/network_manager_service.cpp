#include "network_manager_service.h"

#include "network_device.h"
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
                const auto args = msg.GetArgs<std::string, std::string, std::string>();
                const std::string name = std::get<0>(args);
                const std::string oldOwner = std::get<1>(args);
                const std::string newOwner = std::get<2>(args);

                if (name == "org.freedesktop.NetworkManager" && !newOwner.empty())
                {
                    OnNetworkManagerAppeared();
                }
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
        GError* error = nullptr;
        _nmClient = nm_client_new(nullptr, &error);

        if (!_nmClient)
        {
            // NM not ready yet
            return;
        }

        DiscoverDevices();
        SubscribeToNmSignals();
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

    }

    void NetworkManagerService::AddDevice(NMDevice* nmDev)
    {
        const char* iface = nm_device_get_iface(nmDev);
        const NMDeviceType nmType = nm_device_get_device_type(nmDev);

        if (!iface)
            return;

        ObjectPath path = GetBaseObjectPath().Child("Devices").Child(std::string(iface));
        std::unique_ptr<NetworkDevice> device;

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

        device->_id = iface;
        device->_managed = nm_device_get_managed(nmDev);

        if (const char* hw = nm_device_get_hw_address(nmDev))
            device->_hwAddress = hw;

        device->_type = DeviceTypeToString(nmType);
        device->_state = DeviceStateToString(nm_device_get_state(nmDev));

        _devices.emplace(path, std::move(device));
        EmitDeviceAdded(path);
    }

}