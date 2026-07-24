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

#include "network_manager_service.h"

#include <NetworkManager.h>

#include <cctype>
#include <cstring>
#include <unordered_set>

#include "network_device.h"
#include "network_connection.h"
#include "../../utils/dbus_utils.h"
#include "../logger/logger_service.h"
#include "../session_manager/session_manager_service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::NetworkManager
{
    namespace
    {
        std::string SsidToString(GBytes* ssid)
        {
            if (!ssid)
                return "";

            gsize len = 0;
            const auto* bytes =
                static_cast<const guint8*>(g_bytes_get_data(ssid, &len));

            if (!bytes || len == 0)
                return "";

            char* utf8 = nm_utils_ssid_to_utf8(bytes, len);
            if (!utf8)
                return "";

            std::string out = utf8;
            g_free(utf8);
            return out;
        }

        std::string AccessPointSecurityToString(NMAccessPoint* ap)
        {
            const auto flags = nm_access_point_get_flags(ap);
            const auto wpa = nm_access_point_get_wpa_flags(ap);
            const auto rsn = nm_access_point_get_rsn_flags(ap);

            if (!(flags & NM_802_11_AP_FLAGS_PRIVACY))
                return "open";

            if (rsn & (NM_802_11_AP_SEC_KEY_MGMT_SAE | NM_802_11_AP_SEC_KEY_MGMT_EAP_SUITE_B_192))
                return "wpa3";

            if (rsn & (NM_802_11_AP_SEC_KEY_MGMT_OWE | NM_802_11_AP_SEC_KEY_MGMT_OWE_TM))
                return "owe";

            if (rsn & (NM_802_11_AP_SEC_KEY_MGMT_PSK | NM_802_11_AP_SEC_KEY_MGMT_802_1X))
                return "wpa2";

            if (wpa & (NM_802_11_AP_SEC_KEY_MGMT_PSK | NM_802_11_AP_SEC_KEY_MGMT_802_1X))
                return "wpa1";

            return "wep";
        }

        std::string NormalizeSecurity(std::string security)
        {
            for (auto& c : security)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            return security;
        }

        bool ConfigureWifiSecurity(NMConnection* connection,
                                   const std::string& security,
                                   const std::string& secret,
                                   std::string& errorMessage)
        {
            if (security.empty() || security == "open")
                return true;

            auto* settingWirelessSecurity = NM_SETTING_WIRELESS_SECURITY(nm_setting_wireless_security_new());

            if (security == "wpa1" || security == "wpa2")
            {
                if (secret.empty())
                {
                    errorMessage = "Secret is required for WPA/WPA2 networks";
                    g_object_unref(settingWirelessSecurity);
                    return false;
                }

                g_object_set(
                    settingWirelessSecurity,
                    NM_SETTING_WIRELESS_SECURITY_KEY_MGMT,
                    "wpa-psk",
                    NM_SETTING_WIRELESS_SECURITY_PSK,
                    secret.c_str(),
                    nullptr
                );
            }
            else if (security == "wpa3")
            {
                if (secret.empty())
                {
                    errorMessage = "Secret is required for WPA3 networks";
                    g_object_unref(settingWirelessSecurity);
                    return false;
                }

                g_object_set(
                    settingWirelessSecurity,
                    NM_SETTING_WIRELESS_SECURITY_KEY_MGMT,
                    "sae",
                    NM_SETTING_WIRELESS_SECURITY_PSK,
                    secret.c_str(),
                    nullptr
                );
            }
            else if (security == "wep")
            {
                if (secret.empty())
                {
                    errorMessage = "Secret is required for WEP networks";
                    g_object_unref(settingWirelessSecurity);
                    return false;
                }

                g_object_set(
                    settingWirelessSecurity,
                    NM_SETTING_WIRELESS_SECURITY_KEY_MGMT,
                    "none",
                    NM_SETTING_WIRELESS_SECURITY_WEP_KEY0,
                    secret.c_str(),
                    nullptr
                );
            }
            else if (security == "owe")
            {
                g_object_set(
                    settingWirelessSecurity,
                    NM_SETTING_WIRELESS_SECURITY_KEY_MGMT,
                    "owe",
                    nullptr
                );
            }
            else
            {
                errorMessage = "Unsupported Wi-Fi security type: " + security;
                g_object_unref(settingWirelessSecurity);
                return false;
            }

            nm_connection_add_setting(
                connection,
                NM_SETTING(settingWirelessSecurity)
            );
            return true;
        }

        std::string ActiveConnectionStateReasonToString(const unsigned int reason)
        {
            switch (reason)
            {
                case NM_ACTIVE_CONNECTION_STATE_REASON_NONE:                  return "none";
                case NM_ACTIVE_CONNECTION_STATE_REASON_USER_DISCONNECTED:     return "userDisconnected";
                case NM_ACTIVE_CONNECTION_STATE_REASON_DEVICE_DISCONNECTED:   return "deviceDisconnected";
                case NM_ACTIVE_CONNECTION_STATE_REASON_SERVICE_STOPPED:       return "serviceStopped";
                case NM_ACTIVE_CONNECTION_STATE_REASON_IP_CONFIG_INVALID:     return "ipConfigInvalid";
                case NM_ACTIVE_CONNECTION_STATE_REASON_CONNECT_TIMEOUT:       return "connectTimeout";
                case NM_ACTIVE_CONNECTION_STATE_REASON_SERVICE_START_TIMEOUT: return "serviceStartTimeout";
                case NM_ACTIVE_CONNECTION_STATE_REASON_SERVICE_START_FAILED:  return "serviceStartFailed";
                case NM_ACTIVE_CONNECTION_STATE_REASON_NO_SECRETS:            return "noSecrets";
                case NM_ACTIVE_CONNECTION_STATE_REASON_LOGIN_FAILED:          return "loginFailed";
                case NM_ACTIVE_CONNECTION_STATE_REASON_CONNECTION_REMOVED:    return "connectionRemoved";
                case NM_ACTIVE_CONNECTION_STATE_REASON_DEPENDENCY_FAILED:     return "dependencyFailed";
                case NM_ACTIVE_CONNECTION_STATE_REASON_DEVICE_REALIZE_FAILED: return "deviceRealizeFailed";
                case NM_ACTIVE_CONNECTION_STATE_REASON_DEVICE_REMOVED:        return "deviceRemoved";
                case NM_ACTIVE_CONNECTION_STATE_REASON_UNKNOWN:
                default:                                                      return "unknown";
            }
        }

        struct WifiConnectAsyncContext
        {
            NetworkManagerService* self;
            uint64_t requestId;
            ObjectPath devicePath;
            std::string ssid;
        };
    }

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

    NetworkManagerService::~NetworkManagerService()
    {
        OnNetworkManagerDisappeared();
    }

    std::vector<ObjectPath> NetworkManagerService::ListDevices() const
    {
        auto ks = std::views::keys(_devices);
        std::vector<ObjectPath> keys{ks.begin(), ks.end()};
        return keys;
    }

    std::vector<ObjectPath> NetworkManagerService::ListConnections() const
    {
        auto ks = std::views::keys(_connections);
        std::vector<ObjectPath> keys{ks.begin(), ks.end()};
        return keys;
    }

    void NetworkManagerService::WifiScan(NetworkWifiDevice& dev, const Message& message)
    {
        SharedPolicy(message);

        if (!_nmClient)
            throw DBusException(DBUS_ERROR_FAILED, "NetworkManager is not available");

        auto* nmDev = nm_client_get_device_by_iface(_nmClient, dev.GetId().c_str());
        if (!nmDev)
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Unknown device");

        if (!NM_IS_DEVICE_WIFI(nmDev))
            throw DBusException(DBUS_ERROR_FAILED, "Device is not a Wi-Fi device");

        auto* wifi = NM_DEVICE_WIFI(nmDev);

        GError* error = nullptr;
        if (!nm_device_wifi_request_scan(wifi, nullptr, &error))
        {
            const std::string msg =
                error && error->message
                    ? error->message
                    : "Wi-Fi scan failed";

            if (error)
                g_error_free(error);

            throw DBusException(DBUS_ERROR_FAILED, msg);
        }

        if (error)
            g_error_free(error);

        SyncWifiAccessPoints(wifi, dev);

        auto reply = Message::CreateMethodReturn(message);
        reply.Send(*_conn);
    }

    void NetworkManagerService::WifiConnect(NetworkWifiDevice& dev,
                                            const std::string& ssid,
                                            const std::string& security,
                                            const std::string& secret,
                                            const Message& message)
    {
        SharedPolicy(message);

        if (!_nmClient)
            throw DBusException(DBUS_ERROR_FAILED, "NetworkManager is not available");

        if (ssid.empty())
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "SSID cannot be empty");

        auto* nmDev = nm_client_get_device_by_iface(_nmClient, dev.GetId().c_str());
        if (!nmDev)
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Unknown device");

        if (!NM_IS_DEVICE_WIFI(nmDev))
            throw DBusException(DBUS_ERROR_FAILED, "Device is not a Wi-Fi device");

        auto* wifi = NM_DEVICE_WIFI(nmDev);
        const uint64_t requestId = _nextWifiConnectRequestId++;

        const std::string requestedSecurity = NormalizeSecurity(security);
        NMAccessPoint* selectedAp = nullptr;

        if (const GPtrArray* aps = nm_device_wifi_get_access_points(wifi))
        {
            for (guint i = 0; i < aps->len; ++i)
            {
                auto* ap = NM_ACCESS_POINT(aps->pdata[i]);
                if (!ap)
                    continue;

                if (SsidToString(nm_access_point_get_ssid(ap)) != ssid)
                    continue;

                if (requestedSecurity.empty())
                {
                    selectedAp = ap;
                    break;
                }

                const auto apSecurity =
                    NormalizeSecurity(AccessPointSecurityToString(ap));
                if (apSecurity == requestedSecurity)
                {
                    selectedAp = ap;
                    break;
                }
            }
        }

        if (!selectedAp)
        {
            const std::string details = requestedSecurity.empty()
                ? ""
                : " with security `" + requestedSecurity + "`";
            throw DBusException(
                DBUS_ERROR_INVALID_ARGS,
                "No visible access point found for SSID `" + ssid + "`" + details
            );
        }

        const char* apPathRaw = nm_object_get_path(NM_OBJECT(selectedAp));
        if (!apPathRaw)
            throw DBusException(DBUS_ERROR_FAILED, "Selected access point has no valid object path");
        const std::string apPath = apPathRaw;

        if (const auto* available = nm_device_get_available_connections(nmDev))
        {
            for (guint i = 0; i < available->len; ++i)
            {
                auto* candidate = static_cast<NMConnection*>(
                    g_ptr_array_index(const_cast<GPtrArray*>(available), i)
                );
                if (!candidate)
                    continue;

                auto* wireless = nm_connection_get_setting_wireless(candidate);
                if (!wireless)
                    continue;

                if (SsidToString(nm_setting_wireless_get_ssid(wireless)) != ssid)
                    continue;

                nm_client_activate_connection_async(
                    _nmClient,
                    candidate,
                    nmDev,
                    apPath.c_str(),
                    nullptr,
                    +[](GObject* sourceObject, GAsyncResult* result, gpointer userData)
                    {
                        auto ctx = std::unique_ptr<WifiConnectAsyncContext>(
                            static_cast<WifiConnectAsyncContext*>(userData)
                        );

                        GError* error = nullptr;
                        auto* ac = nm_client_activate_connection_finish(
                            NM_CLIENT(sourceObject),
                            result,
                            &error
                        );

                        if (!ac)
                        {
                            const std::string errorMessage =
                                error && error->message
                                    ? error->message
                                    : "Activation request was rejected";

                            if (error)
                                g_error_free(error);

                            ctx->self->EmitWifiConnectResult(
                                ctx->devicePath,
                                ctx->requestId,
                                false,
                                "activationStartFailed",
                                errorMessage
                            );
                            return;
                        }

                        ctx->self->TrackPendingWifiConnectRequest(
                            ac,
                            ctx->requestId,
                            ctx->devicePath,
                            ctx->ssid
                        );
                    },
                    new WifiConnectAsyncContext{
                        this,
                        requestId,
                        dev._path,
                        ssid
                    }
                );

                auto reply = Message::CreateMethodReturn(message);
                reply.SetArgs(requestId);
                reply.Send(*_conn);
                return;
            }
        }

        const std::string effectiveSecurity = requestedSecurity.empty()
            ? NormalizeSecurity(AccessPointSecurityToString(selectedAp))
            : requestedSecurity;

        auto* connection = NM_CONNECTION(nm_simple_connection_new());

        auto* settingConnection = NM_SETTING_CONNECTION(nm_setting_connection_new());
        const std::string connectionId = "Wi-Fi " + ssid;
        g_object_set(
            settingConnection,
            NM_SETTING_CONNECTION_ID,
            connectionId.c_str(),
            NM_SETTING_CONNECTION_TYPE,
            NM_SETTING_WIRELESS_SETTING_NAME,
            NM_SETTING_CONNECTION_AUTOCONNECT,
            TRUE,
            nullptr
        );
        nm_connection_add_setting(connection, NM_SETTING(settingConnection));

        auto* settingWireless = NM_SETTING_WIRELESS(nm_setting_wireless_new());
        GBytes* ssidBytes = g_bytes_new(ssid.data(), ssid.size());
        g_object_set(
            settingWireless,
            NM_SETTING_WIRELESS_SSID,
            ssidBytes,
            NM_SETTING_WIRELESS_MODE,
            "infrastructure",
            nullptr
        );
        g_bytes_unref(ssidBytes);

        std::string securityError;
        if (!ConfigureWifiSecurity(
            connection,
            effectiveSecurity,
            secret,
            securityError
        ))
        {
            g_object_unref(settingWireless);
            g_object_unref(connection);
            throw DBusException(DBUS_ERROR_INVALID_ARGS, securityError);
        }
        nm_connection_add_setting(connection, NM_SETTING(settingWireless));

        nm_client_add_and_activate_connection_async(
            _nmClient,
            connection,
            nmDev,
            apPath.c_str(),
            nullptr,
            +[](GObject* sourceObject, GAsyncResult* result, gpointer userData)
            {
                auto ctx = std::unique_ptr<WifiConnectAsyncContext>(
                    static_cast<WifiConnectAsyncContext*>(userData)
                );

                GError* error = nullptr;
                auto* ac = nm_client_add_and_activate_connection_finish(
                    NM_CLIENT(sourceObject),
                    result,
                    &error
                );

                if (!ac)
                {
                    const std::string errorMessage =
                        error && error->message
                            ? error->message
                            : "Add-and-activate request was rejected";

                    if (error)
                        g_error_free(error);

                    ctx->self->EmitWifiConnectResult(
                        ctx->devicePath,
                        ctx->requestId,
                        false,
                        "activationStartFailed",
                        errorMessage
                    );
                    return;
                }

                ctx->self->TrackPendingWifiConnectRequest(
                    ac,
                    ctx->requestId,
                    ctx->devicePath,
                    ctx->ssid
                );
            },
            new WifiConnectAsyncContext{
                this,
                requestId,
                dev._path,
                ssid
            }
        );

        g_object_unref(connection);

        auto reply = Message::CreateMethodReturn(message);
        reply.SetArgs(requestId);
        reply.Send(*_conn);
    }

    void NetworkManagerService::WifiDisconnect(NetworkWifiDevice& dev, const Message& message)
    {
        SharedPolicy(message);

        if (!_nmClient)
            throw DBusException(DBUS_ERROR_FAILED, "NetworkManager is not available");

        auto* nmDev = nm_client_get_device_by_iface(_nmClient, dev.GetId().c_str());
        if (!nmDev)
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Unknown device");

        if (!NM_IS_DEVICE_WIFI(nmDev))
            throw DBusException(DBUS_ERROR_FAILED, "Device is not a Wi-Fi device");

        if (!nm_device_get_active_connection(nmDev))
        {
            auto reply = Message::CreateMethodReturn(message);
            reply.Send(*_conn);
            return;
        }

        GError* error = nullptr;
        if (!nm_device_disconnect(nmDev, nullptr, &error))
        {
            const std::string msg =
                error && error->message
                    ? error->message
                    : "Failed to disconnect Wi-Fi device";

            if (error)
                g_error_free(error);

            throw DBusException(DBUS_ERROR_FAILED, msg);
        }

        if (error)
            g_error_free(error);

        auto reply = Message::CreateMethodReturn(message);
        reply.Send(*_conn);
    }

    void NetworkManagerService::EthernetSetEnabled(NetworkDevice& dev, const bool enabled, const Message& message)
    {
        SharedPolicy(message);

        if (!_nmClient)
            throw DBusException(DBUS_ERROR_FAILED, "NetworkManager is not available");

        _serviceManager->Get<Logger::LoggerService>()->Debug(
            "EthernetSetEnabled called: id=" + dev.GetId() +
            " enabled=" + std::string(enabled ? "true" : "false")
        );

        auto* nmDev = nm_client_get_device_by_iface(_nmClient, dev.GetId().c_str());
        if (!nmDev)
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Unknown device");

        if (!NM_IS_DEVICE_ETHERNET(nmDev))
            throw DBusException(DBUS_ERROR_FAILED, "Device is not an Ethernet device");

        if (enabled)
        {
            if (nm_device_get_active_connection(nmDev))
            {
                auto reply = Message::CreateMethodReturn(message);
                reply.Send(*_conn);
                return;
            }

            const auto* avail = nm_device_get_available_connections(nmDev);
            NMConnection* best = nullptr;
            NMConnection* bestAuto = nullptr;
            NMConnection* bestIface = nullptr;

            const char* iface = nm_device_get_iface(nmDev);

            if (avail)
            {
                for (guint i = 0; i < avail->len; ++i)
                {
                    auto* c = static_cast<NMConnection*>(g_ptr_array_index(const_cast<GPtrArray*>(avail), i));
                    if (!c)
                        continue;

                    if (!best)
                        best = c;

                    auto* s = nm_connection_get_setting_connection(c);
                    const char* cIface = s ? nm_setting_connection_get_interface_name(s) : nullptr;
                    const bool ifaceMatches = iface && cIface && std::strcmp(iface, cIface) == 0;
                    const bool ifaceOk = ifaceMatches || !cIface || cIface[0] == '\0';
                    const bool autoconnect = s && nm_setting_connection_get_autoconnect(s);

                    if (ifaceMatches && autoconnect)
                        bestAuto = c;
                    else if (!bestAuto && ifaceOk && autoconnect)
                        bestAuto = c;

                    if (!bestIface && ifaceMatches)
                        bestIface = c;
                }
            }

            const auto* conn = bestAuto ? bestAuto : (bestIface ? bestIface : best);
            if (!conn)
                throw DBusException(DBUS_ERROR_FAILED, "No available connection for Ethernet device");

            nm_client_activate_connection_async(
                _nmClient,
                const_cast<NMConnection*>(conn),
                nmDev,
                nullptr,
                nullptr,
                nullptr,
                nullptr
            );
        }
        else
        {
            GError* error = nullptr;
            if (!nm_device_disconnect(nmDev, nullptr, &error))
            {
                const std::string msg =
                    error && error->message
                        ? error->message
                        : "Failed to disconnect Ethernet device";

                if (error)
                    g_error_free(error);

                throw DBusException(DBUS_ERROR_FAILED, msg);
            }

            if (error)
                g_error_free(error);
        }

        auto reply = Message::CreateMethodReturn(message);
        reply.Send(*_conn);
    }

    void NetworkManagerService::WifiSetEnabled(NetworkWifiDevice& dev, const bool enabled, const Message& message)
    {
        SharedPolicy(message);

        if (!_nmClient)
            throw DBusException(DBUS_ERROR_FAILED, "NetworkManager is not available");

        _serviceManager->Get<Logger::LoggerService>()->Debug(
            "WifiSetEnabled called: id=" + dev.GetId() +
            " enabled=" + std::string(enabled ? "true" : "false")
        );

        nm_client_wireless_set_enabled(_nmClient, enabled);

        auto reply = Message::CreateMethodReturn(message);
        reply.Send(*_conn);
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
        _serviceManager->Get<Logger::LoggerService>()->Debug(
            "OnListDevices called, returning " + std::to_string(list.size()) + " devices"
        );
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

        FailAllPendingWifiConnectRequests(
            "network_manager_unavailable",
            "NetworkManager disappeared during connection attempt"
        );

        for (const auto &path: _connections | std::views::keys)
        {
            try
            {
                OnConnectionRemoved(path);
            }
            catch (const std::exception& e)
            {
                _serviceManager->Get<Logger::LoggerService>()->Debug(
                    "OnConnectionRemoved event handler failed with an error: " + std::string(e.what())
                );
            }
        }

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
        if (!nmDevices)
            return;

        for (guint i = 0; i < nmDevices->len; ++i)
        {
            if (!nmDevices->pdata[i])
            {
                _serviceManager->Get<Logger::LoggerService>()->Debug(
                    "DiscoverDevices: null device entry at index " + std::to_string(i)
                );
                continue;
            }

            const auto nmDev = NM_DEVICE(nmDevices->pdata[i]);
            if (!G_IS_OBJECT(nmDev))
            {
                _serviceManager->Get<Logger::LoggerService>()->Debug(
                    "DiscoverDevices: non-GObject device entry at index " + std::to_string(i)
                );
                continue;
            }

            AddDevice(nmDev);
        }
    }

    void NetworkManagerService::SubscribeToNmSignals()
    {
        if (!G_IS_OBJECT(_nmClient))
        {
            _serviceManager->Get<Logger::LoggerService>()->Debug(
                "SubscribeToNmSignals: _nmClient is not a GObject"
            );
            return;
        }

        g_signal_connect(
            _nmClient,
            "device-added",
            G_CALLBACK(+[] (NMClient* client, NMDevice* device, NetworkManagerService* self)
            {
                self->AddDevice(device);
            }),
            this
        );

        g_signal_connect(
            _nmClient,
            "device-removed",
            G_CALLBACK(+[] (NMClient* client, NMDevice* device, NetworkManagerService* self)
            {
                self->RemoveDevice(device);
            }),
            this
        );
    }

    void NetworkManagerService::SubscribeToDeviceSignals(NMDevice* nmDev)
    {
        if (!G_IS_OBJECT(nmDev))
        {
            _serviceManager->Get<Logger::LoggerService>()->Debug(
                "SubscribeToDeviceSignals: nmDev is not a GObject"
            );
            return;
        }

        g_signal_connect(
            nmDev,
            "notify::state",
            G_CALLBACK(+[](GObject* obj, GParamSpec*, NetworkManagerService* self)
            {
                self->OnDeviceStateChanged(NM_DEVICE(obj));
            }),
            this
        );

        g_signal_connect(
            nmDev,
            "notify::active-connection",
            G_CALLBACK(+[](GObject* obj, GParamSpec*, NetworkManagerService* self)
            {
                self->OnDeviceActiveConnectionChanged(NM_DEVICE(obj));
            }),
            this
        );

        g_signal_connect(
            nmDev,
            "notify::managed",
            G_CALLBACK(+[](GObject* obj, GParamSpec*, NetworkManagerService* self)
            {
                self->OnDeviceManagedChanged(NM_DEVICE(obj));
            }),
            this
        );

        if (NM_IS_DEVICE_WIFI(nmDev))
        {
            auto* wifi = NM_DEVICE_WIFI(nmDev);

            g_signal_connect(
                wifi,
                "notify::access-points",
                G_CALLBACK(+[](GObject* obj, GParamSpec*, NetworkManagerService* self)
                {
                    self->OnWifiDeviceAccessPointsChanged(
                        NM_DEVICE_WIFI(obj)
                    );
                }),
                this
            );

            g_signal_connect(
                wifi,
                "notify::active-access-point",
                G_CALLBACK(+[](GObject* obj, GParamSpec*, NetworkManagerService* self)
                {
                    self->OnWifiDeviceActiveAccessPointChanged(
                        NM_DEVICE_WIFI(obj)
                    );
                }),
                this
            );
        }
    }

    void NetworkManagerService::AddDevice(NMDevice* nmDev)
    {
        if (!G_IS_OBJECT(nmDev))
        {
            _serviceManager->Get<Logger::LoggerService>()->Debug(
                "AddDevice: nmDev is not a GObject"
            );
            return;
        }

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

        _serviceManager->Get<Logger::LoggerService>()->Debug(
            "AddDevice init: id=" + device->_id.Get() +
            " type=" + device->_type.Get() +
            " state=" + device->_state.Get()
        );

        _devices.emplace(path, std::move(device));
        EmitDeviceAdded(path);

        if (nmType == NM_DEVICE_TYPE_WIFI)
        {
            auto* dev = static_cast<NetworkWifiDevice*>(_devices.at(path).get());
            SyncWifiAccessPoints(
                NM_DEVICE_WIFI(nmDev),
                *dev
            );
        }
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
        FailPendingWifiConnectRequestsForDevice(
            path,
            "device_removed",
            "Device disappeared during connection attempt"
        );

        const auto connPath = it->second->GetActiveConnection();
        if (connPath != ObjectPath{})
        {
            _connections.erase(connPath);
            try
            {
                OnConnectionRemoved(path);
            }
            catch (const std::exception& e)
            {
                _serviceManager->Get<Logger::LoggerService>()->Debug(
                    "OnConnectionRemoved event handler failed with an error: " + std::string(e.what())
                );
            }
        }

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
                _connections.erase(oldPath);
                try
                {
                    OnConnectionRemoved(oldPath);
                }
                catch (const std::exception& e)
                {
                    _serviceManager->Get<Logger::LoggerService>()->Debug(
                        "OnConnectionRemoved event handler failed with an error: " + std::string(e.what())
                    );
                }
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

    NMDeviceWifi* NetworkManagerService::FindWifiDeviceForConnection(const NetworkConnection& conn) const
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
        try
        {
            OnConnectionAdded(path);
        }
        catch (const std::exception& e)
        {
            _serviceManager->Get<Logger::LoggerService>()->Debug(
                "OnConnectionAdded event handler failed with an error: " + std::string(e.what())
            );
        }
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
        try
        {
            OnConnectionRemoved(path);
        }
        catch (const std::exception& e)
        {
            _serviceManager->Get<Logger::LoggerService>()->Debug(
                "OnConnectionRemoved event handler failed with an error: " + std::string(e.what())
            );
        }
    }

    void NetworkManagerService::SubscribeToActiveConnectionSignals(NMActiveConnection* ac)
    {
        if (!G_IS_OBJECT(ac))
        {
            _serviceManager->Get<Logger::LoggerService>()->Debug(
                "SubscribeToActiveConnectionSignals: ac is not a GObject"
            );
            return;
        }

        g_signal_connect(
            ac,
            "notify::state",
            G_CALLBACK(+[](GObject* obj, GParamSpec*, NetworkManagerService* self)
            {
                self->OnActiveConnectionStateChanged(
                    NM_ACTIVE_CONNECTION(obj)
                );
            }),
            this
        );

        g_signal_connect(
            ac,
            "notify::ip4-config",
            G_CALLBACK(+[](GObject* obj, GParamSpec*, NetworkManagerService* self)
            {
                self->OnActiveConnectionIpChanged(
                    NM_ACTIVE_CONNECTION(obj)
                );
            }),
            this
        );

        g_signal_connect(
            ac,
            "notify::ip6-config",
            G_CALLBACK(+[](GObject* obj, GParamSpec*, NetworkManagerService* self)
            {
                self->OnActiveConnectionIpChanged(
                    NM_ACTIVE_CONNECTION(obj)
                );
            }),
            this
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

        if (!G_IS_OBJECT(ap))
        {
            _serviceManager->Get<Logger::LoggerService>()->Debug(
                "SubscribeToWifiSignalStrength: ap is not a GObject"
            );
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

        g_signal_connect(
            ap,
            "notify::strength",
            G_CALLBACK(+[](GObject* obj, GParamSpec*, NetworkManagerService* self)
            {
                self->OnAccessPointStrengthChanged(
                    NM_ACCESS_POINT(obj)
                );
            }),
            this
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

    void NetworkManagerService::TrackPendingWifiConnectRequest(NMActiveConnection* ac,
                                                               const uint64_t requestId,
                                                               const ObjectPath& devicePath,
                                                               const std::string& ssid)
    {
        const char* activePath = nm_object_get_path(NM_OBJECT(ac));
        if (!activePath || activePath[0] == '\0')
        {
            EmitWifiConnectResult(
                devicePath,
                requestId,
                false,
                "activationStartFailed",
                "Active connection path is missing"
            );
            return;
        }

        _pendingWifiConnectRequests[activePath] = PendingWifiConnectRequest{
            requestId,
            devicePath,
            ssid
        };

        SubscribeToActiveConnectionSignals(ac);
        ResolvePendingWifiConnectRequest(ac);
    }

    void NetworkManagerService::ResolvePendingWifiConnectRequest(NMActiveConnection* ac)
    {
        const char* activePath = nm_object_get_path(NM_OBJECT(ac));
        if (!activePath || activePath[0] == '\0')
            return;

        const auto it = _pendingWifiConnectRequests.find(activePath);
        if (it == _pendingWifiConnectRequests.end())
            return;

        const auto state = nm_active_connection_get_state(ac);

        if (state == NM_ACTIVE_CONNECTION_STATE_ACTIVATED)
        {
            EmitWifiConnectResult(
                it->second.devicePath,
                it->second.requestId,
                true,
                "activated",
                ""
            );
            _pendingWifiConnectRequests.erase(it);
            return;
        }

        if (state == NM_ACTIVE_CONNECTION_STATE_DEACTIVATED || state == NM_ACTIVE_CONNECTION_STATE_UNKNOWN)
        {
            const auto reasonCode = ActiveConnectionStateReasonToString(
                nm_active_connection_get_state_reason(ac)
            );

            EmitWifiConnectResult(
                it->second.devicePath,
                it->second.requestId,
                false,
                reasonCode,
                "Connection activation failed"
            );
            _pendingWifiConnectRequests.erase(it);
        }
    }

    void NetworkManagerService::FailPendingWifiConnectRequestsForDevice(const ObjectPath& devicePath,
                                                                        const std::string& reasonCode,
                                                                        const std::string& reasonMessage)
    {
        for (auto it = _pendingWifiConnectRequests.begin(); it != _pendingWifiConnectRequests.end();)
        {
            if (it->second.devicePath != devicePath)
            {
                ++it;
                continue;
            }

            EmitWifiConnectResult(
                it->second.devicePath,
                it->second.requestId,
                false,
                reasonCode,
                reasonMessage
            );

            it = _pendingWifiConnectRequests.erase(it);
        }
    }

    void NetworkManagerService::FailAllPendingWifiConnectRequests(const std::string& reasonCode,
                                                                  const std::string& reasonMessage)
    {
        for (const auto& [_, request] : _pendingWifiConnectRequests)
        {
            EmitWifiConnectResult(
                request.devicePath,
                request.requestId,
                false,
                reasonCode,
                reasonMessage
            );
        }

        _pendingWifiConnectRequests.clear();
    }

    void NetworkManagerService::EmitWifiConnectResult(const ObjectPath& devicePath,
                                                      const uint64_t requestId,
                                                      const bool success,
                                                      const std::string& reasonCode,
                                                      const std::string& reasonMessage)
    {
        const auto it = _devices.find(devicePath);
        if (it == _devices.end())
            return;

        auto* wifiDev = dynamic_cast<NetworkWifiDevice*>(it->second.get());
        if (!wifiDev)
            return;

        wifiDev->EmitConnectResult(
            requestId,
            success,
            reasonCode,
            reasonMessage
        );
    }

    void NetworkManagerService::OnActiveConnectionStateChanged(NMActiveConnection* ac)
    {
        ResolvePendingWifiConnectRequest(ac);

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

    void NetworkManagerService::SyncWifiAccessPoints(NMDeviceWifi* nmDev, NetworkWifiDevice& dev)
    {
        const GPtrArray* aps = nm_device_wifi_get_access_points(nmDev);

        NMAccessPoint* activeAp = nm_device_wifi_get_active_access_point(nmDev);
        const char* activeApPath =
            activeAp ? nm_object_get_path(NM_OBJECT(activeAp)) : nullptr;

        std::unordered_set<ObjectPath, ObjectPathHash> newPaths;
        if (aps)
            newPaths.reserve(aps->len);

        if (aps)
        {
            for (guint i = 0; i < aps->len; ++i)
            {
                auto* ap = NM_ACCESS_POINT(aps->pdata[i]);
                const char* nmApPath = nm_object_get_path(NM_OBJECT(ap));

                if (!nmApPath)
                    continue;

                const ObjectPath apPath = dev._path
                    .Child("AccessPoints")
                    .Child(EncodeForObjectPath(nmApPath));

                newPaths.insert(apPath);

                const bool existed = dev._accessPoints.contains(apPath);
                if (!existed)
                {
                    dev._accessPoints.emplace(
                        apPath,
                        std::make_unique<NetworkAccessPoint>(*this, *_conn, apPath)
                    );
                }

                auto& apObj = *dev._accessPoints.at(apPath);

                apObj._ssid.Set(
                    SsidToString(nm_access_point_get_ssid(ap))
                );
                apObj._strength.Set(
                    static_cast<int>(nm_access_point_get_strength(ap))
                );
                apObj._security.Set(
                    AccessPointSecurityToString(ap)
                );
                apObj._frequency.Set(
                    static_cast<int>(nm_access_point_get_frequency(ap))
                );
                apObj._connected.Set(
                    activeApPath && strcmp(activeApPath, nmApPath) == 0
                );

                if (!existed)
                    dev.EmitAccessPointAdded(apPath);
            }
        }

        for (auto it = dev._accessPoints.begin(); it != dev._accessPoints.end();)
        {
            if (newPaths.contains(it->first))
            {
                ++it;
                continue;
            }

            const auto removedPath = it->first;
            it = dev._accessPoints.erase(it);
            dev.EmitAccessPointRemoved(removedPath);
        }
    }

    void NetworkManagerService::OnWifiDeviceAccessPointsChanged(NMDeviceWifi* nmDev)
    {
        auto* dev = FindDevice(NM_DEVICE(nmDev));
        if (!dev)
            return;

        auto* wifiDev = static_cast<NetworkWifiDevice*>(dev);
        SyncWifiAccessPoints(nmDev, *wifiDev);
    }

    void NetworkManagerService::OnWifiDeviceActiveAccessPointChanged(NMDeviceWifi* nmDev)
    {
        auto* dev = FindDevice(NM_DEVICE(nmDev));
        if (!dev)
            return;

        auto* wifiDev = static_cast<NetworkWifiDevice*>(dev);
        SyncWifiAccessPoints(nmDev, *wifiDev);
    }

    std::string NetworkManagerService::DeviceTypeToString(const unsigned int t)
    {
        switch (t)
        {
            case NM_DEVICE_TYPE_WIFI:     return NETWORK_DEVICE_TYPE_WIFI;
            case NM_DEVICE_TYPE_ETHERNET: return NETWORK_DEVICE_TYPE_ETHERNET;
            default:                      return NETWORK_DEVICE_TYPE_UNKNOWN;
        }
    }

    std::string NetworkManagerService::DeviceStateToString(const unsigned int s)
    {
        switch (s)
        {
            case NM_DEVICE_STATE_ACTIVATED:    return NETWORK_DEVICE_STATE_CONNECTED;
            case NM_DEVICE_STATE_PREPARE:
            case NM_DEVICE_STATE_CONFIG:
            case NM_DEVICE_STATE_NEED_AUTH:    return NETWORK_DEVICE_STATE_CONNECTING;
            case NM_DEVICE_STATE_DISCONNECTED: return NETWORK_DEVICE_STATE_DISCONNECTED;
            case NM_DEVICE_STATE_UNAVAILABLE:
            default:                           return NETWORK_DEVICE_STATE_UNAVAILABLE;
        }
    }

    std::string NetworkManagerService::ActiveConnectionStateToString(const unsigned int s)
    {
        switch (s)
        {
            case NM_ACTIVE_CONNECTION_STATE_ACTIVATED:    return NETWORK_CONNECTION_STATE_ACTIVATED;
            case NM_ACTIVE_CONNECTION_STATE_ACTIVATING:   return NETWORK_CONNECTION_STATE_ACTIVATING;
            case NM_ACTIVE_CONNECTION_STATE_DEACTIVATING: return NETWORK_CONNECTION_STATE_DEACTIVATING;
            default:                                      return NETWORK_CONNECTION_STATE_UNKNOWN;
        }
    }

}
