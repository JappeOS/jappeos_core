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

#include "audio_service.h"

#include <pipewire/pipewire.h>
#include <pipewire/device.h>
#include <pipewire/extensions/metadata.h>

#include <spa/param/route.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/pod/vararg.h>
#include <spa/utils/dict.h>
#include <spa/utils/defs.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <string_view>
#include <unistd.h>

#include "audio_device.h"
#include "audio_stream.h"
#include "../logger/logger_service.h"
#include "../../utils/dbus_utils.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Audio
{
    struct AudioService::NodeProxyData
    {
        AudioService* self = nullptr;
        uint32_t id = std::numeric_limits<uint32_t>::max();
        pw_node* node = nullptr;
        uint32_t channelCount = 0;
        spa_hook listener{};
    };

    struct AudioService::DeviceProxyData
    {
        struct RouteInfo
        {
            uint32_t index = std::numeric_limits<uint32_t>::max();
            uint32_t direction = SPA_DIRECTION_OUTPUT;
            int32_t cardDevice = -1;
            uint32_t availability = SPA_PARAM_AVAILABILITY_unknown;
            std::string name;
            std::string description;
            bool active = false;
        };

        static uint64_t MakeRouteKey(const uint32_t direction, const uint32_t index)
        {
            return (static_cast<uint64_t>(direction) << 32) | static_cast<uint64_t>(index);
        }

        AudioService* self = nullptr;
        uint32_t id = std::numeric_limits<uint32_t>::max();
        pw_device* device = nullptr;
        bool routeParamsKnown = false;
        bool supportsRouteParams = false;
        std::unordered_map<uint64_t, RouteInfo> routesByKey;
        spa_hook listener{};
    };

    struct AudioService::MetadataProxyData
    {
        AudioService* self = nullptr;
        uint32_t id = std::numeric_limits<uint32_t>::max();
        pw_metadata* metadata = nullptr;
        spa_hook listener{};
    };

    namespace
    {
        constexpr uint32_t INVALID_ID = std::numeric_limits<uint32_t>::max();
        constexpr const char* LEGACY_NODE_TARGET_KEY = "node.target";
        constexpr const char* DEFAULT_AUDIO_SOURCE_KEY = "default.audio.source";
        constexpr const char* DEFAULT_AUDIO_SINK_KEY = "default.audio.sink";
        constexpr const char* DEFAULT_CONFIGURED_AUDIO_SOURCE_KEY = "default.configured.audio.source";
        constexpr const char* DEFAULT_CONFIGURED_AUDIO_SINK_KEY = "default.configured.audio.sink";

        const char* LookupProp(const spa_dict* dict, const char* key)
        {
            return dict ? spa_dict_lookup(dict, key) : nullptr;
        }

        std::optional<int32_t> ParseInt32Prop(const char* raw)
        {
            if (!raw || raw[0] == '\0')
                return std::nullopt;

            char* endPtr = nullptr;
            errno = 0;
            const long value = std::strtol(raw, &endPtr, 10);
            if (errno != 0 || !endPtr || *endPtr != '\0')
                return std::nullopt;
            if (value < static_cast<long>(std::numeric_limits<int32_t>::min()) ||
                value > static_cast<long>(std::numeric_limits<int32_t>::max()))
            {
                return std::nullopt;
            }
            return static_cast<int32_t>(value);
        }

        bool ParsePodInt32OrId(const spa_pod* pod, int32_t& out)
        {
            if (!pod)
                return false;

            uint32_t nValues = 0;
            uint32_t choiceType = SPA_CHOICE_None;
            if (spa_pod* choiceValues = spa_pod_get_values(pod, &nValues, &choiceType);
                choiceValues && nValues > 0)
            {
                pod = choiceValues;
            }

            int32_t i = 0;
            if (spa_pod_get_int(pod, &i) >= 0)
            {
                out = i;
                return true;
            }

            uint32_t id = 0;
            if (spa_pod_get_id(pod, &id) >= 0 && id <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
            {
                out = static_cast<int32_t>(id);
                return true;
            }

            int64_t l = 0;
            if (spa_pod_get_long(pod, &l) >= 0 &&
                l >= static_cast<int64_t>(std::numeric_limits<int32_t>::min()) &&
                l <= static_cast<int64_t>(std::numeric_limits<int32_t>::max()))
            {
                out = static_cast<int32_t>(l);
                return true;
            }

            return false;
        }

        bool ParsePodUint32OrInt(const spa_pod* pod, uint32_t& out)
        {
            if (!pod)
                return false;

            uint32_t nValues = 0;
            uint32_t choiceType = SPA_CHOICE_None;
            if (spa_pod* choiceValues = spa_pod_get_values(pod, &nValues, &choiceType);
                choiceValues && nValues > 0)
            {
                pod = choiceValues;
            }

            uint32_t id = 0;
            if (spa_pod_get_id(pod, &id) >= 0)
            {
                out = id;
                return true;
            }

            int32_t i = 0;
            if (spa_pod_get_int(pod, &i) >= 0 && i >= 0)
            {
                out = static_cast<uint32_t>(i);
                return true;
            }

            int64_t l = 0;
            if (spa_pod_get_long(pod, &l) >= 0 &&
                l >= 0 &&
                l <= static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
            {
                out = static_cast<uint32_t>(l);
                return true;
            }

            return false;
        }

        std::string Trim(std::string s)
        {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.pop_back();
            return s;
        }

        std::optional<std::string> ExtractJsonField(const std::string& json, const std::string& field)
        {
            const auto key = "\"" + field + "\"";
            const auto keyPos = json.find(key);
            if (keyPos == std::string::npos)
                return std::nullopt;

            const auto colonPos = json.find(':', keyPos + key.size());
            if (colonPos == std::string::npos)
                return std::nullopt;

            auto valPos = json.find_first_not_of(" \t\r\n", colonPos + 1);
            if (valPos == std::string::npos)
                return std::nullopt;

            if (json[valPos] != '"')
                return std::nullopt;

            ++valPos;
            std::string value;
            for (; valPos < json.size(); ++valPos)
            {
                const char c = json[valPos];
                if (c == '\\')
                {
                    if (valPos + 1 < json.size())
                    {
                        value.push_back(json[valPos + 1]);
                        ++valPos;
                    }
                    continue;
                }
                if (c == '"')
                    break;
                value.push_back(c);
            }
            return value;
        }

        std::optional<std::string> ExtractJsonNumericField(const std::string& json, const std::string& field)
        {
            const auto key = "\"" + field + "\"";
            const auto keyPos = json.find(key);
            if (keyPos == std::string::npos)
                return std::nullopt;

            const auto colonPos = json.find(':', keyPos + key.size());
            if (colonPos == std::string::npos)
                return std::nullopt;

            auto valPos = json.find_first_not_of(" \t\r\n", colonPos + 1);
            if (valPos == std::string::npos)
                return std::nullopt;

            const bool negative = json[valPos] == '-';
            if (negative)
                ++valPos;
            if (valPos >= json.size() || !std::isdigit(static_cast<unsigned char>(json[valPos])))
                return std::nullopt;

            auto endPos = valPos;
            while (endPos < json.size() && std::isdigit(static_cast<unsigned char>(json[endPos])))
                ++endPos;

            return json.substr(negative ? valPos - 1 : valPos, endPos - (negative ? valPos - 1 : valPos));
        }

        std::string ParseMetadataTarget(const char* value)
        {
            if (!value)
                return "";

            auto raw = Trim(value);
            if (raw.empty())
                return "";

            if (raw.front() == '{')
            {
                if (const auto name = ExtractJsonField(raw, "name"); name.has_value())
                    return *name;
                if (const auto id = ExtractJsonField(raw, "id"); id.has_value())
                    return *id;
                if (const auto id = ExtractJsonNumericField(raw, "id"); id.has_value())
                    return *id;
            }

            if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
                return raw.substr(1, raw.size() - 2);

            return raw;
        }

        std::string EscapeJsonString(std::string_view s)
        {
            std::string out;
            out.reserve(s.size());
            for (const char c : s)
            {
                if (c == '"' || c == '\\')
                    out.push_back('\\');
                out.push_back(c);
            }
            return out;
        }

        double ClampVolume(const double volume)
        {
            return std::clamp(volume, 0.0, 1.0);
        }

        std::string DirectionFromMediaClass(const std::string_view mediaClass)
        {
            if (mediaClass == "Audio/Sink" || mediaClass == "Stream/Output/Audio")
                return "output";
            if (mediaClass == "Audio/Source" || mediaClass == "Stream/Input/Audio")
                return "input";
            return "unknown";
        }

        std::string DeviceTypeFromMediaClass(const std::string_view mediaClass)
        {
            if (mediaClass == "Audio/Sink")
                return "sink";
            if (mediaClass == "Audio/Source")
                return "source";
            return "unknown";
        }

        uint32_t SpaDirectionFromLogicalDirection(const std::string_view direction)
        {
            return direction == "input" ? SPA_DIRECTION_INPUT : SPA_DIRECTION_OUTPUT;
        }

        std::string RouteObjectComponent(const uint32_t routeDeviceId,
                                         const uint32_t directionId,
                                         const uint32_t routeIndex)
        {
            const uint64_t syntheticId =
                (static_cast<uint64_t>(routeDeviceId) << 33) |
                (static_cast<uint64_t>(directionId & 0x1) << 32) |
                static_cast<uint64_t>(routeIndex);
            return std::to_string(syntheticId);
        }

        std::optional<std::string> RouteDeviceNameFromNodeName(const std::string_view nodeName)
        {
            constexpr std::string_view outputPrefix = "alsa_output.";
            constexpr std::string_view inputPrefix = "alsa_input.";
            constexpr std::string_view monitorSuffix = ".monitor";

            std::string_view normalizedName = nodeName;
            if (normalizedName.ends_with(monitorSuffix))
                normalizedName = normalizedName.substr(0, normalizedName.size() - monitorSuffix.size());

            std::size_t begin = std::string_view::npos;
            if (normalizedName.starts_with(outputPrefix))
                begin = outputPrefix.size();
            else if (normalizedName.starts_with(inputPrefix))
                begin = inputPrefix.size();
            if (begin == std::string_view::npos)
                return std::nullopt;

            std::size_t end = normalizedName.find(".HiFi__", begin);
            if (end == std::string_view::npos)
                end = normalizedName.rfind('.');
            if (end == std::string_view::npos || end <= begin)
                end = normalizedName.size();
            if (end <= begin)
                return std::nullopt;

            return std::format("alsa_card.{}", std::string(normalizedName.substr(begin, end - begin)));
        }

        std::optional<uint32_t> FindRouteDeviceIdByNodeName(
            const std::string_view nodeName,
            const std::unordered_map<uint32_t, std::string>& routeDeviceNamesById
        )
        {
            const auto routeDeviceName = RouteDeviceNameFromNodeName(nodeName);
            if (!routeDeviceName.has_value())
                return std::nullopt;

            for (const auto& [deviceId, name] : routeDeviceNamesById)
            {
                if (name == *routeDeviceName)
                    return deviceId;
            }
            return std::nullopt;
        }

        std::optional<uint32_t> FindRouteDeviceIdByDeviceName(
            const std::string_view deviceName,
            const std::unordered_map<uint32_t, std::string>& routeDeviceNamesById
        )
        {
            if (deviceName.empty())
                return std::nullopt;

            for (const auto& [deviceId, name] : routeDeviceNamesById)
            {
                if (name == deviceName)
                    return deviceId;
            }
            return std::nullopt;
        }

        std::string BuildRouteDisplayName(const std::string& routeDescription,
                                          const std::string& routeName,
                                          const std::string& fallbackName,
                                          const std::string& routeDeviceDescription)
        {
            std::string name = !routeDescription.empty()
                ? routeDescription
                : (!routeName.empty()
                    ? routeName
                    : (!fallbackName.empty()
                        ? fallbackName
                        : "Device"));

            if (!routeDeviceDescription.empty() && name.find(routeDeviceDescription) == std::string::npos)
                name += std::format(" - {}", routeDeviceDescription);

            return name;
        }
    }

    AudioService::AudioService(ServiceManager* serviceManager, Connection* conn) :
                               Service(serviceManager, conn),
                               _object(*_conn, GetBaseObjectPath()),
                               _iface(_object.CreateInterface(GetBaseInterface()))
    {
        _iface.RegisterMethod("ListDevices", [&] (const auto& m) { OnListDevices(m); });
        _iface.RegisterMethod("ListStreams", [&] (const auto& m) { OnListStreams(m); });
        _iface.RegisterProperty<ObjectPath>(
            "ActiveInputDevice",
            [this]() { return _activeInputDevice; },
            [this](const ObjectPath& path) { OnSetActiveInputDevice(path); }
        );
        _iface.RegisterProperty<ObjectPath>(
            "ActiveOutputDevice",
            [this]() { return _activeOutputDevice; },
            [this](const ObjectPath& path) { OnSetActiveOutputDevice(path); }
        );
        InitPipeWire();
    }

    AudioService::~AudioService()
    {
        ShutdownPipeWire();
    }

    std::vector<ObjectPath> AudioService::ListDevices() const
    {
        std::vector<const AudioDevice*> orderedDevices;
        orderedDevices.reserve(_devices.size());
        for (const auto& [_, device] : _devices)
            orderedDevices.push_back(device.get());

        const ObjectPath activeOutput = _activeOutputDevice;
        const ObjectPath activeInput = _activeInputDevice;

        const auto directionRank = [](const std::string& direction) -> int
        {
            if (direction == "output")
                return 0;
            if (direction == "input")
                return 1;
            return 2;
        };

        const auto activeRank = [&](const AudioDevice* device) -> int
        {
            if (device->_direction.Get() == "output")
                return device->_path == activeOutput ? 0 : 1;
            if (device->_direction.Get() == "input")
                return device->_path == activeInput ? 0 : 1;
            return 1;
        };

        std::sort(
            orderedDevices.begin(),
            orderedDevices.end(),
            [&](const AudioDevice* lhs, const AudioDevice* rhs)
            {
                const int lhsDirection = directionRank(lhs->_direction.Get());
                const int rhsDirection = directionRank(rhs->_direction.Get());
                if (lhsDirection != rhsDirection)
                    return lhsDirection < rhsDirection;

                const int lhsActive = activeRank(lhs);
                const int rhsActive = activeRank(rhs);
                if (lhsActive != rhsActive)
                    return lhsActive < rhsActive;

                if (lhs->_available.Get() != rhs->_available.Get())
                    return lhs->_available.Get() > rhs->_available.Get();

                if (lhs->_name.Get() != rhs->_name.Get())
                    return lhs->_name.Get() < rhs->_name.Get();

                return lhs->_path.ToString() < rhs->_path.ToString();
            }
        );

        std::vector<ObjectPath> paths;
        paths.reserve(orderedDevices.size());
        for (const auto* device : orderedDevices)
            paths.push_back(device->_path);
        return paths;
    }

    std::vector<ObjectPath> AudioService::ListStreams() const
    {
        auto ks = std::views::keys(_streams);
        std::vector<ObjectPath> keys{ks.begin(), ks.end()};
        return keys;
    }

    void AudioService::SetDeviceVolume(AudioDevice& device, const double volume)
    {
        SetNodeVolumeByPath(device._path, volume);
    }

    void AudioService::SetDeviceMuted(AudioDevice& device, const bool muted)
    {
        SetNodeMutedByPath(device._path, muted);
    }

    void AudioService::SetStreamVolume(AudioStream& stream, const double volume)
    {
        SetNodeVolumeByPath(stream._path, volume);
    }

    void AudioService::SetStreamMuted(AudioStream& stream, const bool muted)
    {
        SetNodeMutedByPath(stream._path, muted);
    }

    void AudioService::SetStreamDevice(AudioStream& stream, const ObjectPath& devicePath)
    {
        if (!_metadataProxy || !_metadataProxy->metadata)
            throw DBusException(DBUS_ERROR_FAILED, "PipeWire metadata is not available");

        std::string target;
        if (devicePath != ObjectPath{})
        {
            const auto it = _devices.find(devicePath);
            if (it == _devices.end())
                throw DBusException(DBUS_ERROR_INVALID_ARGS, "Unknown audio device");

            target = !it->second->_pwNodeName.empty()
                ? it->second->_pwNodeName
                : (!it->second->_pwObjectSerial.empty()
                    ? it->second->_pwObjectSerial
                    : std::to_string(it->second->_pwNodeId));
        }

        pw_thread_loop_lock(_pwThreadLoop);
        const int result = pw_metadata_set_property(
            _metadataProxy->metadata,
            stream._pwNodeId,
            PW_KEY_TARGET_OBJECT,
            target.empty() ? nullptr : "Spa:String",
            target.empty() ? nullptr : target.c_str()
        );
        pw_thread_loop_unlock(_pwThreadLoop);

        if (result < 0)
        {
            throw DBusException(
                DBUS_ERROR_FAILED,
                std::format("Failed to set stream target device: {}", result)
            );
        }

        stream._suppressPropertyCallbacks = true;
        stream._device.Set(devicePath);
        stream._suppressPropertyCallbacks = false;
    }

    void AudioService::SharedPolicy(const Message& message) const
    {
        const auto sender = message.GetSender();
        const auto senderUid = _conn->GetUnixUser(sender);
        if (senderUid != geteuid())
            throw DBusException(DBUS_ERROR_ACCESS_DENIED, "Caller UID does not match session service UID");
    }

    void AudioService::InitPipeWire()
    {
        pw_init(nullptr, nullptr);

        _pwCoreListener = new spa_hook{};
        _pwRegistryListener = new spa_hook{};
        spa_zero(*_pwCoreListener);
        spa_zero(*_pwRegistryListener);

        _pwThreadLoop = pw_thread_loop_new("jappeos_audio", nullptr);
        if (!_pwThreadLoop)
        {
            _serviceManager->Get<Logger::LoggerService>()->Err("AudioService: failed to create PipeWire thread loop");
            return;
        }

        if (pw_thread_loop_start(_pwThreadLoop) < 0)
        {
            _serviceManager->Get<Logger::LoggerService>()->Err("AudioService: failed to start PipeWire thread loop");
            return;
        }

        pw_thread_loop_lock(_pwThreadLoop);

        _pwContext = pw_context_new(pw_thread_loop_get_loop(_pwThreadLoop), nullptr, 0);
        if (!_pwContext)
        {
            pw_thread_loop_unlock(_pwThreadLoop);
            _serviceManager->Get<Logger::LoggerService>()->Err("AudioService: failed to create PipeWire context");
            return;
        }

        _pwCore = pw_context_connect(_pwContext, nullptr, 0);
        if (!_pwCore)
        {
            pw_thread_loop_unlock(_pwThreadLoop);
            _serviceManager->Get<Logger::LoggerService>()->Err("AudioService: failed to connect to PipeWire core");
            return;
        }

        static const pw_core_events coreEvents = {
            .version = PW_VERSION_CORE_EVENTS,
            .info = nullptr,
            .done = nullptr,
            .ping = nullptr,
            .error = [](
                void* data,
                uint32_t id,
                int seq,
                int res,
                const char* message
            )
            {
                auto* self = static_cast<AudioService*>(data);
                const std::string_view msg = message ? std::string_view(message) : std::string_view{};
                const bool benignMissingRouteParams =
                    res == -ENOENT &&
                    (msg.find("enum params id:12") != std::string_view::npos ||
                     msg.find("enum params id:13") != std::string_view::npos);
                if (benignMissingRouteParams)
                    return;
                self->_serviceManager->Get<Logger::LoggerService>()->Warn(
                    "AudioService PipeWire error id=" + std::to_string(id) +
                    " seq=" + std::to_string(seq) +
                    " res=" + std::to_string(res) +
                    " message=" + std::string(msg)
                );
            }
        };
        pw_core_add_listener(_pwCore, _pwCoreListener, &coreEvents, this);

        ConnectRegistry();

        pw_thread_loop_unlock(_pwThreadLoop);
    }

    void AudioService::ShutdownPipeWire()
    {
        if (!_pwThreadLoop)
            return;

        pw_thread_loop_lock(_pwThreadLoop);

        for (auto& [_, nodeData] : _nodeProxies)
        {
            spa_hook_remove(&nodeData->listener);
            if (nodeData->node)
                pw_proxy_destroy(reinterpret_cast<pw_proxy*>(nodeData->node));
        }
        _nodeProxies.clear();

        for (auto& [_, deviceData] : _deviceProxies)
        {
            spa_hook_remove(&deviceData->listener);
            if (deviceData->device)
                pw_proxy_destroy(reinterpret_cast<pw_proxy*>(deviceData->device));
        }
        _deviceProxies.clear();

        if (_metadataProxy)
        {
            spa_hook_remove(&_metadataProxy->listener);
            if (_metadataProxy->metadata)
                pw_proxy_destroy(reinterpret_cast<pw_proxy*>(_metadataProxy->metadata));
            _metadataProxy.reset();
        }

        if (_pwRegistry)
        {
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(_pwRegistry));
            _pwRegistry = nullptr;
        }

        if (_pwCore)
        {
            pw_core_disconnect(_pwCore);
            _pwCore = nullptr;
        }

        if (_pwContext)
        {
            pw_context_destroy(_pwContext);
            _pwContext = nullptr;
        }

        if (_pwCoreListener)
        {
            spa_hook_remove(_pwCoreListener);
            delete _pwCoreListener;
            _pwCoreListener = nullptr;
        }

        if (_pwRegistryListener)
        {
            spa_hook_remove(_pwRegistryListener);
            delete _pwRegistryListener;
            _pwRegistryListener = nullptr;
        }

        pw_thread_loop_unlock(_pwThreadLoop);

        pw_thread_loop_stop(_pwThreadLoop);
        pw_thread_loop_destroy(_pwThreadLoop);
        _pwThreadLoop = nullptr;

        _devices.clear();
        _streams.clear();
        _devicePathsByNodeId.clear();
        _streamPathsByNodeId.clear();
        _routeDeviceNamesById.clear();
        _routeDeviceDescriptionsById.clear();
    }

    void AudioService::ConnectRegistry()
    {
        _pwRegistry = pw_core_get_registry(_pwCore, PW_VERSION_REGISTRY, 0);
        if (!_pwRegistry)
        {
            _serviceManager->Get<Logger::LoggerService>()->Err("AudioService: failed to get PipeWire registry");
            return;
        }

        static const pw_registry_events registryEvents = {
            .version = PW_VERSION_REGISTRY_EVENTS,
            .global = [](
                void* data,
                const uint32_t id,
                const uint32_t permissions,
                const char* type,
                const uint32_t version,
                const spa_dict* props
            )
            {
                (void) permissions;
                auto* self = static_cast<AudioService*>(data);
                self->HandleRegistryGlobal(id, type, version, props);
            },
            .global_remove = [](
                void* data,
                const uint32_t id
            )
            {
                auto* self = static_cast<AudioService*>(data);
                self->HandleRegistryGlobalRemove(id);
            }
        };
        pw_registry_add_listener(_pwRegistry, _pwRegistryListener, &registryEvents, this);
    }

    void AudioService::HandleRegistryGlobal(const uint32_t id,
                                            const char* type,
                                            const uint32_t version,
                                            const spa_dict* props)
    {
        if (!type)
            return;

        if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0)
        {
            const auto* mediaClass = LookupProp(props, PW_KEY_MEDIA_CLASS);
            if (!mediaClass)
                return;

            const std::string mc = mediaClass;
            if (mc == "Audio/Sink" || mc == "Audio/Source")
                AddDeviceNode(id, props);
            else if (mc == "Stream/Output/Audio" || mc == "Stream/Input/Audio")
                AddStreamNode(id, props);
            else
                return;

            BindNodeProxy(id);
            return;
        }

        if (std::strcmp(type, PW_TYPE_INTERFACE_Device) == 0)
        {
            if (const auto* deviceName = LookupProp(props, PW_KEY_DEVICE_NAME);
                deviceName && deviceName[0] != '\0')
            {
                _routeDeviceNamesById[id] = deviceName;

                bool matchedAny = false;
                for (auto& [_, device] : _devices)
                {
                    if (device->_pwDeviceId != INVALID_ID || device->_pwNodeName.empty())
                        continue;

                    if (const auto inferred = FindRouteDeviceIdByNodeName(device->_pwNodeName, _routeDeviceNamesById);
                        inferred.has_value() && *inferred == id)
                    {
                        device->_pwDeviceId = id;
                        matchedAny = true;
                    }
                }

                if (matchedAny)
                    RefreshLogicalDevicesForRouteDevice(id);
            }

            if (const auto* deviceDescription = LookupProp(props, "device.description");
                deviceDescription && deviceDescription[0] != '\0')
            {
                _routeDeviceDescriptionsById[id] = deviceDescription;
            }

            BindDeviceProxy(id);
            return;
        }

        if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0)
        {
            const auto* metadataName = LookupProp(props, PW_KEY_METADATA_NAME);
            if (metadataName && std::strcmp(metadataName, "default") != 0)
                return;

            if (!_metadataProxy)
                BindMetadataProxy(id);
        }

        (void) version;
    }

    void AudioService::HandleRegistryGlobalRemove(const uint32_t id)
    {
        if (_devicePathsByNodeId.contains(id))
            RemoveDeviceNode(id);
        if (_streamPathsByNodeId.contains(id))
            RemoveStreamNode(id);

        if (_metadataProxy && _metadataProxy->id == id)
        {
            spa_hook_remove(&_metadataProxy->listener);
            if (_metadataProxy->metadata)
                pw_proxy_destroy(reinterpret_cast<pw_proxy*>(_metadataProxy->metadata));
            _metadataProxy.reset();
        }

        if (const auto it = _nodeProxies.find(id); it != _nodeProxies.end())
        {
            spa_hook_remove(&it->second->listener);
            if (it->second->node)
                pw_proxy_destroy(reinterpret_cast<pw_proxy*>(it->second->node));
            _nodeProxies.erase(it);
        }

        if (const auto it = _deviceProxies.find(id); it != _deviceProxies.end())
        {
            spa_hook_remove(&it->second->listener);
            if (it->second->device)
                pw_proxy_destroy(reinterpret_cast<pw_proxy*>(it->second->device));
            _deviceProxies.erase(it);
        }

        _routeDeviceNamesById.erase(id);
        _routeDeviceDescriptionsById.erase(id);
    }

    void AudioService::AddDeviceNode(const uint32_t id, const spa_dict* props)
    {
        const std::string mediaClass = LookupProp(props, PW_KEY_MEDIA_CLASS)
            ? LookupProp(props, PW_KEY_MEDIA_CLASS)
            : "";

        const char* nodeName = LookupProp(props, PW_KEY_NODE_NAME);
        const char* deviceName = LookupProp(props, PW_KEY_DEVICE_NAME);
        const char* description = LookupProp(props, PW_KEY_NODE_DESCRIPTION);
        const char* nick = LookupProp(props, PW_KEY_NODE_NICK);
        const char* serial = LookupProp(props, PW_KEY_OBJECT_SERIAL);
        const auto routeDeviceId = ParseInt32Prop(LookupProp(props, PW_KEY_DEVICE_ID));
        const auto cardProfileDevice = ParseInt32Prop(LookupProp(props, "card.profile.device"));
        uint32_t resolvedRouteDeviceId = INVALID_ID;
        if (routeDeviceId.has_value() && *routeDeviceId >= 0)
            resolvedRouteDeviceId = static_cast<uint32_t>(*routeDeviceId);
        else if (deviceName && deviceName[0] != '\0')
        {
            if (const auto inferred = FindRouteDeviceIdByDeviceName(deviceName, _routeDeviceNamesById); inferred.has_value())
                resolvedRouteDeviceId = *inferred;
        }
        else if (nodeName && nodeName[0] != '\0')
        {
            if (const auto inferred = FindRouteDeviceIdByNodeName(nodeName, _routeDeviceNamesById); inferred.has_value())
                resolvedRouteDeviceId = *inferred;
        }

        const ObjectPath fallbackPath = GetBaseObjectPath()
            .Child("Devices")
            .Child(std::to_string(id));

        const std::string fallbackName =
            description && description[0] != '\0'
                ? description
                : (nick && nick[0] != '\0'
                    ? nick
                    : (nodeName && nodeName[0] != '\0'
                        ? nodeName
                        : std::format("Device {}", id)));

        AddLogicalDevice(
            fallbackPath,
            id,
            resolvedRouteDeviceId,
            cardProfileDevice.value_or(-1),
            -1,
            fallbackName,
            DeviceTypeFromMediaClass(mediaClass),
            DirectionFromMediaClass(mediaClass),
            true
        );

        if (const auto it = _devices.find(fallbackPath); it != _devices.end())
        {
            it->second->_pwNodeName = nodeName ? nodeName : "";
            it->second->_pwObjectSerial = serial ? serial : "";
        }

        if (resolvedRouteDeviceId != INVALID_ID)
        {
            BindDeviceProxy(resolvedRouteDeviceId);
            RefreshLogicalDevicesForRouteDevice(resolvedRouteDeviceId);
        }
    }

    void AudioService::AddStreamNode(const uint32_t id, const spa_dict* props)
    {
        const ObjectPath path = GetBaseObjectPath()
            .Child("Streams")
            .Child(std::to_string(id));

        if (_streams.contains(path))
            return;

        auto stream = std::make_unique<AudioStream>(*this, *_conn, path);

        const std::string mediaClass = LookupProp(props, PW_KEY_MEDIA_CLASS)
            ? LookupProp(props, PW_KEY_MEDIA_CLASS)
            : "";
        const char* nodeName = LookupProp(props, PW_KEY_NODE_NAME);
        const char* description = LookupProp(props, PW_KEY_NODE_DESCRIPTION);
        const char* appName = LookupProp(props, PW_KEY_APP_NAME);
        const char* targetObject = LookupProp(props, PW_KEY_TARGET_OBJECT);
        if (!targetObject)
            targetObject = LookupProp(props, LEGACY_NODE_TARGET_KEY);

        stream->_pwNodeId = id;
        stream->_pwNodeName = nodeName ? nodeName : "";
        stream->_id.Set(std::to_string(id));
        stream->_name.Set(
            description && description[0] != '\0'
                ? description
                : (nodeName && nodeName[0] != '\0' ? nodeName : std::format("Stream {}", id))
        );
        stream->_applicationName.Set(appName && appName[0] != '\0' ? appName : "");
        stream->_direction.Set(DirectionFromMediaClass(mediaClass));
        stream->_suppressPropertyCallbacks = true;
        stream->_device.Set(FindDevicePathByTargetObject(targetObject ? targetObject : ""));
        stream->_suppressPropertyCallbacks = false;

        _streamPathsByNodeId[id] = path;
        _streams.emplace(path, std::move(stream));
        EmitStreamAdded(path);
    }

    void AudioService::AddLogicalDevice(const ObjectPath& path,
                                        const uint32_t nodeId,
                                        const uint32_t routeDeviceId,
                                        const int32_t cardProfileDevice,
                                        const int32_t routeIndex,
                                        const std::string& name,
                                        const std::string& type,
                                        const std::string& direction,
                                        const bool available)
    {
        auto it = _devices.find(path);
        const bool created = it == _devices.end();
        if (created)
        {
            auto device = std::make_unique<AudioDevice>(*this, *_conn, path);
            it = _devices.emplace(path, std::move(device)).first;
        }

        auto& device = *it->second;
        const uint32_t previousNodeId = device._pwNodeId;
        device._pwNodeId = nodeId;
        device._pwDeviceId = routeDeviceId;
        device._pwCardProfileDevice = cardProfileDevice;
        device._pwRouteIndex = routeIndex;
        device._pwRouteActive = routeIndex < 0;
        device._pwIsRouteDevice = routeIndex >= 0;

        const auto existingNodePath = GetBaseObjectPath()
            .Child("Devices")
            .Child(std::to_string(nodeId));
        if (const auto existing = _devices.find(existingNodePath); existing != _devices.end() && existing->first != path)
        {
            device._pwNodeName = existing->second->_pwNodeName;
            device._pwObjectSerial = existing->second->_pwObjectSerial;
        }

        device._id.Set(std::string(path.Leaf()));
        device._name.Set(name);
        device._type.Set(type);
        device._direction.Set(direction);
        device._available.Set(available);

        if (!created && previousNodeId != nodeId)
        {
            if (const auto itOldPaths = _devicePathsByNodeId.find(previousNodeId); itOldPaths != _devicePathsByNodeId.end())
            {
                auto& oldPaths = itOldPaths->second;
                std::erase(oldPaths, path);
                if (oldPaths.empty())
                    _devicePathsByNodeId.erase(itOldPaths);
            }
        }

        auto& paths = _devicePathsByNodeId[nodeId];
        if (std::find(paths.begin(), paths.end(), path) == paths.end())
            paths.push_back(path);

        if (created)
            EmitDeviceAdded(path);
    }

    void AudioService::RemoveLogicalDevice(const ObjectPath& path)
    {
        const auto it = _devices.find(path);
        if (it == _devices.end())
            return;

        const uint32_t nodeId = it->second->_pwNodeId;
        _devices.erase(it);
        EmitDeviceRemoved(path);

        RebuildNodeDevicePaths(nodeId);

        const ObjectPath replacementPath = FindDevicePathByNodeId(nodeId);
        for (auto& [_, stream] : _streams)
        {
            if (stream->_device.Get() != path)
                continue;

            stream->_suppressPropertyCallbacks = true;
            stream->_device.Set(replacementPath);
            stream->_suppressPropertyCallbacks = false;
        }

        if (_activeInputDevice == path)
        {
            const auto replacementIt = _devices.find(replacementPath);
            if (replacementIt != _devices.end() && replacementIt->second->_direction.Get() == "input")
                UpdateActiveInputDeviceFromBackend(replacementPath);
            else
                UpdateActiveInputDeviceFromBackend(ObjectPath{});
        }
        if (_activeOutputDevice == path)
        {
            const auto replacementIt = _devices.find(replacementPath);
            if (replacementIt != _devices.end() && replacementIt->second->_direction.Get() == "output")
                UpdateActiveOutputDeviceFromBackend(replacementPath);
            else
                UpdateActiveOutputDeviceFromBackend(ObjectPath{});
        }
    }

    void AudioService::RebuildNodeDevicePaths(const uint32_t nodeId)
    {
        std::vector<ObjectPath> rebuilt;
        for (const auto& [path, device] : _devices)
        {
            if (device->_pwNodeId == nodeId)
                rebuilt.push_back(path);
        }

        if (rebuilt.empty())
            _devicePathsByNodeId.erase(nodeId);
        else
            _devicePathsByNodeId[nodeId] = std::move(rebuilt);
    }

    void AudioService::RefreshActiveDevicesForNode(const uint32_t nodeId)
    {
        // If we have a pending explicit input device on this node,
        // honour it instead of the inferred active route.
        /*if (_pendingInputDevice != ObjectPath{})
        {
            if (const auto it = _devices.find(_pendingInputDevice); it != _devices.end() && it->second->_pwNodeId == nodeId)
            {
                if (it->second->_pwRouteActive)   // PipeWire confirmed the route
                    _pendingInputDevice = ObjectPath{};
                // Either way, don't override _activeInputDevice here
                return;
            }
        }

        // If we have a pending explicit output device on this node,
        // honour it instead of the inferred active route.
        if (_pendingOutputDevice != ObjectPath{})
        {
            if (const auto it = _devices.find(_pendingOutputDevice); it != _devices.end() && it->second->_pwNodeId == nodeId)
            {
                if (it->second->_pwRouteActive)   // PipeWire confirmed the route
                    _pendingOutputDevice = ObjectPath{};
                // Either way, don't override _activeOutputDevice here
                return;
            }
        }*/


        // Confirm and clear any pending explicit input device choice
        // once PipeWire's route state agrees with it.
        if (_pendingInputDevice != ObjectPath{})
        {
            const auto it = _devices.find(_pendingInputDevice);
            if (it != _devices.end() &&
                it->second->_pwNodeId == nodeId &&
                it->second->_pwRouteActive)
            {
                _pendingInputDevice = ObjectPath{};
                // Active device is already set correctly; just suppress
                // route-inference from overriding it during re-enumeration.
                return;
            }
            // Pending device is on this node but route not confirmed yet,
            // suppress route-inference updates to avoid intermediate revert.
            if (const auto it2 = _devices.find(_pendingInputDevice);
                it2 != _devices.end() && it2->second->_pwNodeId == nodeId)
            {
                return;
            }
        }

        // Confirm and clear any pending explicit output device choice
        // once PipeWire's route state agrees with it.
        if (_pendingOutputDevice != ObjectPath{})
        {
            const auto it = _devices.find(_pendingOutputDevice);
            if (it != _devices.end() &&
                it->second->_pwNodeId == nodeId &&
                it->second->_pwRouteActive)
            {
                _pendingOutputDevice = ObjectPath{};
                // Active device is already set correctly; just suppress
                // route-inference from overriding it during re-enumeration.
                return;
            }
            // Pending device is on this node but route not confirmed yet,
            // suppress route-inference updates to avoid intermediate revert.
            if (const auto it2 = _devices.find(_pendingOutputDevice);
                it2 != _devices.end() && it2->second->_pwNodeId == nodeId)
            {
                return;
            }
        }

        const ObjectPath preferredPath = FindDevicePathByNodeId(nodeId);
        const auto preferredIt = _devices.find(preferredPath);

        if (const auto current = _activeInputDevice; current != ObjectPath{})
        {
            if (const auto itCurrent = _devices.find(current); itCurrent != _devices.end() &&
                itCurrent->second->_pwNodeId == nodeId)
            {
                if (preferredIt != _devices.end() && preferredIt->second->_direction.Get() == "input")
                {
                    if (current != preferredPath)
                        UpdateActiveInputDeviceFromBackend(preferredPath);
                }
                else
                    UpdateActiveInputDeviceFromBackend(ObjectPath{});
            }
        }
        else if (preferredIt != _devices.end() &&
                 preferredIt->second->_direction.Get() == "input" &&
                 preferredIt->second->_pwRouteActive)
        {
            UpdateActiveInputDeviceFromBackend(preferredPath);
        }

        if (const auto current = _activeOutputDevice; current != ObjectPath{})
        {
            if (const auto itCurrent = _devices.find(current); itCurrent != _devices.end() &&
                itCurrent->second->_pwNodeId == nodeId)
            {
                if (preferredIt != _devices.end() && preferredIt->second->_direction.Get() == "output")
                {
                    if (current != preferredPath)
                        UpdateActiveOutputDeviceFromBackend(preferredPath);
                }
                else
                    UpdateActiveOutputDeviceFromBackend(ObjectPath{});
            }
        }
        else if (preferredIt != _devices.end() &&
                 preferredIt->second->_direction.Get() == "output" &&
                 preferredIt->second->_pwRouteActive)
        {
            UpdateActiveOutputDeviceFromBackend(preferredPath);
        }
    }

    void AudioService::RemoveDeviceNode(const uint32_t id)
    {
        const auto itPaths = _devicePathsByNodeId.find(id);
        if (itPaths == _devicePathsByNodeId.end())
            return;

        const std::vector<ObjectPath> paths = itPaths->second;
        for (const auto& path : paths)
            RemoveLogicalDevice(path);

        _devicePathsByNodeId.erase(id);
    }

    void AudioService::RemoveStreamNode(const uint32_t id)
    {
        const auto itPath = _streamPathsByNodeId.find(id);
        if (itPath == _streamPathsByNodeId.end())
            return;

        const ObjectPath path = itPath->second;
        _streamPathsByNodeId.erase(itPath);

        if (const auto it = _streams.find(path); it != _streams.end())
        {
            _streams.erase(it);
            EmitStreamRemoved(path);
        }
    }

    void AudioService::BindNodeProxy(const uint32_t id)
    {
        if (_nodeProxies.contains(id))
            return;

        auto* nodeProxy = static_cast<pw_node*>(
            pw_registry_bind(
                _pwRegistry,
                id,
                PW_TYPE_INTERFACE_Node,
                PW_VERSION_NODE,
                0
            )
        );
        if (!nodeProxy)
            return;

        auto data = std::make_unique<NodeProxyData>();
        data->self = this;
        data->id = id;
        data->node = nodeProxy;
        spa_zero(data->listener);
        auto* dataPtr = data.get();
        _nodeProxies.emplace(id, std::move(data));

        static const pw_node_events nodeEvents = {
            .version = PW_VERSION_NODE_EVENTS,
            .info = [](
                void* data,
                const pw_node_info* info
            )
            {
                if (!info)
                    return;
                auto* nd = static_cast<NodeProxyData*>(data);
                nd->self->HandleNodeInfo(nd->id, info->props);
            },
            .param = [](
                void* data,
                int seq,
                uint32_t id,
                uint32_t index,
                uint32_t next,
                const spa_pod* param
            )
            {
                (void) seq;
                (void) index;
                (void) next;
                if (id != SPA_PARAM_Props || !param)
                    return;
                auto* nd = static_cast<NodeProxyData*>(data);
                nd->self->HandleNodePropsParam(nd->id, param);
            }
        };

        pw_node_add_listener(nodeProxy, &dataPtr->listener, &nodeEvents, dataPtr);

        uint32_t params[] = { SPA_PARAM_Props };
        pw_node_subscribe_params(nodeProxy, params, 1);
        pw_node_enum_params(nodeProxy, 0, SPA_PARAM_Props, 0, UINT32_MAX, nullptr);
    }

    void AudioService::BindDeviceProxy(const uint32_t id)
    {
        if (_deviceProxies.contains(id))
            return;

        auto* deviceProxy = static_cast<pw_device*>(
            pw_registry_bind(
                _pwRegistry,
                id,
                PW_TYPE_INTERFACE_Device,
                PW_VERSION_DEVICE,
                0
            )
        );
        if (!deviceProxy)
            return;

        auto data = std::make_unique<DeviceProxyData>();
        data->self = this;
        data->id = id;
        data->device = deviceProxy;
        spa_zero(data->listener);
        auto* dataPtr = data.get();
        _deviceProxies.emplace(id, std::move(data));

        static const pw_device_events deviceEvents = {
            .version = PW_VERSION_DEVICE_EVENTS,
            .info = [](
                void* data,
                const pw_device_info* info
            )
            {
                auto* dd = static_cast<DeviceProxyData*>(data);
                dd->self->HandleDeviceInfo(dd->id, info);
            },
            .param = [](
                void* data,
                int seq,
                uint32_t id,
                uint32_t index,
                uint32_t next,
                const spa_pod* param
            )
            {
                (void) seq;
                (void) index;
                (void) next;
                if ((id != SPA_PARAM_EnumRoute && id != SPA_PARAM_Route) || !param)
                    return;
                auto* dd = static_cast<DeviceProxyData*>(data);
                dd->self->HandleDeviceRouteParam(dd->id, id, param);
            }
        };
        pw_device_add_listener(deviceProxy, &dataPtr->listener, &deviceEvents, dataPtr);

        uint32_t params[] = { SPA_PARAM_EnumRoute, SPA_PARAM_Route };
        pw_device_subscribe_params(deviceProxy, params, 2);
    }

    void AudioService::HandleDeviceInfo(const uint32_t deviceId, const pw_device_info* info)
    {
        if (!info)
            return;
        if ((info->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS) == 0)
            return;

        bool hasRouteParams = false;
        for (uint32_t i = 0; i < info->n_params; ++i)
        {
            const uint32_t paramId = info->params[i].id;
            if (paramId == SPA_PARAM_EnumRoute || paramId == SPA_PARAM_Route)
            {
                hasRouteParams = true;
                break;
            }
        }
        const auto itProxy = _deviceProxies.find(deviceId);
        if (itProxy == _deviceProxies.end() || !itProxy->second || !itProxy->second->device)
            return;

        itProxy->second->routeParamsKnown = true;
        itProxy->second->supportsRouteParams = hasRouteParams;
        if (!hasRouteParams)
            return;

        itProxy->second->routesByKey.clear();
        pw_device_enum_params(itProxy->second->device, 0, SPA_PARAM_EnumRoute, 0, UINT32_MAX, nullptr);
        pw_device_enum_params(itProxy->second->device, 0, SPA_PARAM_Route, 0, UINT32_MAX, nullptr);
    }

    void AudioService::HandleDeviceRouteParam(const uint32_t deviceId, const uint32_t paramId, const spa_pod* param)
    {
        const auto itProxy = _deviceProxies.find(deviceId);
        if (itProxy == _deviceProxies.end() || !itProxy->second)
            return;

        int routeIndex = -1;
        uint32_t routeDirection = SPA_DIRECTION_OUTPUT;
        int routeDevice = -1;
        const char* routeNameRaw = nullptr;
        const char* routeDescriptionRaw = nullptr;
        uint32_t routeAvailability = SPA_PARAM_AVAILABILITY_unknown;
        std::string routeName;
        std::string routeDescription;

        uint32_t routeDeviceValueSize = 0;
        uint32_t routeDeviceValueType = 0;
        uint32_t nRouteDevices = 0;
        void* routeDevicesRaw = nullptr;

        const int parseResult = spa_pod_parse_object(
            param,
            SPA_TYPE_OBJECT_ParamRoute,
            nullptr,
            SPA_PARAM_ROUTE_index, SPA_POD_OPT_Int(&routeIndex),
            SPA_PARAM_ROUTE_direction, SPA_POD_OPT_Id(&routeDirection),
            SPA_PARAM_ROUTE_device, SPA_POD_OPT_Int(&routeDevice),
            SPA_PARAM_ROUTE_name, SPA_POD_OPT_String(&routeNameRaw),
            SPA_PARAM_ROUTE_description, SPA_POD_OPT_String(&routeDescriptionRaw),
            SPA_PARAM_ROUTE_available, SPA_POD_OPT_Id(&routeAvailability),
            SPA_PARAM_ROUTE_devices, SPA_POD_OPT_Array(
                &routeDeviceValueSize,
                &routeDeviceValueType,
                &nRouteDevices,
                &routeDevicesRaw
            )
        );

        if (parseResult >= 0 && routeIndex >= 0)
        {
            if (routeNameRaw && routeNameRaw[0] != '\0')
                routeName = routeNameRaw;
            if (routeDescriptionRaw && routeDescriptionRaw[0] != '\0')
                routeDescription = routeDescriptionRaw;

            if (
                routeDevice < 0 &&
                nRouteDevices > 0 &&
                routeDevicesRaw &&
                routeDeviceValueType == SPA_TYPE_Int &&
                routeDeviceValueSize == sizeof(int32_t)
            )
            {
                routeDevice = static_cast<int32_t*>(routeDevicesRaw)[0];
            }
            else if (
                routeDevice < 0 &&
                nRouteDevices > 0 &&
                routeDevicesRaw &&
                routeDeviceValueType == SPA_TYPE_Id &&
                routeDeviceValueSize == sizeof(uint32_t)
            )
            {
                const uint32_t value = static_cast<uint32_t*>(routeDevicesRaw)[0];
                if (value <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
                    routeDevice = static_cast<int32_t>(value);
            }
        }

        if (parseResult < 0 || routeIndex < 0)
        {
            const auto* indexProp = spa_pod_find_prop(param, nullptr, SPA_PARAM_ROUTE_index);
            if (!indexProp || !ParsePodInt32OrId(&indexProp->value, routeIndex) || routeIndex < 0)
                return;

            if (const auto* directionProp = spa_pod_find_prop(param, nullptr, SPA_PARAM_ROUTE_direction))
            {
                uint32_t parsedDirection = SPA_DIRECTION_OUTPUT;
                if (ParsePodUint32OrInt(&directionProp->value, parsedDirection))
                    routeDirection = parsedDirection;
            }

            if (const auto* deviceProp = spa_pod_find_prop(param, nullptr, SPA_PARAM_ROUTE_device))
            {
                int32_t parsedDevice = -1;
                if (ParsePodInt32OrId(&deviceProp->value, parsedDevice))
                    routeDevice = parsedDevice;
            }

            if (const auto* nameProp = spa_pod_find_prop(param, nullptr, SPA_PARAM_ROUTE_name))
            {
                const char* value = nullptr;
                if (spa_pod_get_string(&nameProp->value, &value) >= 0 && value)
                    routeName = value;
            }

            if (const auto* descProp = spa_pod_find_prop(param, nullptr, SPA_PARAM_ROUTE_description))
            {
                const char* value = nullptr;
                if (spa_pod_get_string(&descProp->value, &value) >= 0 && value)
                    routeDescription = value;
            }

            if (const auto* availableProp = spa_pod_find_prop(param, nullptr, SPA_PARAM_ROUTE_available))
            {
                uint32_t parsedAvailability = SPA_PARAM_AVAILABILITY_unknown;
                if (ParsePodUint32OrInt(&availableProp->value, parsedAvailability))
                    routeAvailability = parsedAvailability;
            }

            if (routeDevice < 0)
            {
                const auto* devicesProp = spa_pod_find_prop(param, nullptr, SPA_PARAM_ROUTE_devices);
                if (devicesProp && spa_pod_is_array(&devicesProp->value))
                {
                    uint32_t nValues = 0;
                    void* values = spa_pod_get_array(&devicesProp->value, &nValues);
                    if (values && nValues > 0)
                    {
                        if (SPA_POD_ARRAY_VALUE_TYPE(&devicesProp->value) == SPA_TYPE_Int &&
                            SPA_POD_ARRAY_VALUE_SIZE(&devicesProp->value) == sizeof(int32_t))
                        {
                            routeDevice = static_cast<int32_t*>(values)[0];
                        }
                        else if (SPA_POD_ARRAY_VALUE_TYPE(&devicesProp->value) == SPA_TYPE_Id &&
                                 SPA_POD_ARRAY_VALUE_SIZE(&devicesProp->value) == sizeof(uint32_t))
                        {
                            const uint32_t value = static_cast<uint32_t*>(values)[0];
                            if (value <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
                                routeDevice = static_cast<int32_t>(value);
                        }
                        else if (SPA_POD_ARRAY_VALUE_TYPE(&devicesProp->value) == SPA_TYPE_Long &&
                                 SPA_POD_ARRAY_VALUE_SIZE(&devicesProp->value) == sizeof(int64_t))
                        {
                            const int64_t value = static_cast<int64_t*>(values)[0];
                            if (value >= static_cast<int64_t>(std::numeric_limits<int32_t>::min()) &&
                                value <= static_cast<int64_t>(std::numeric_limits<int32_t>::max()))
                            {
                                routeDevice = static_cast<int32_t>(value);
                            }
                        }
                    }
                }
            }
        }

        const uint32_t routeIndexId = static_cast<uint32_t>(routeIndex);
        auto& routeInfo = itProxy->second->routesByKey[
            DeviceProxyData::MakeRouteKey(routeDirection, routeIndexId)
        ];
        routeInfo.index = routeIndexId;
        routeInfo.direction = routeDirection;
        if (routeDevice >= 0)
            routeInfo.cardDevice = routeDevice;
        routeInfo.availability = routeAvailability;
        if (!routeName.empty())
            routeInfo.name = routeName;
        if (!routeDescription.empty())
            routeInfo.description = routeDescription;

        if (paramId == SPA_PARAM_Route)
        {
            for (auto& [_, route] : itProxy->second->routesByKey)
            {
                if (route.direction == routeInfo.direction)
                    route.active = false;
            }
            routeInfo.active = true;
        }

        RefreshLogicalDevicesForRouteDevice(deviceId);
    }

    void AudioService::RefreshLogicalDevicesForRouteDevice(const uint32_t routeDeviceId)
    {
        const auto itProxy = _deviceProxies.find(routeDeviceId);
        if (itProxy == _deviceProxies.end() || !itProxy->second)
            return;

        const std::string routeDeviceDescription =
            _routeDeviceDescriptionsById.contains(routeDeviceId)
                ? _routeDeviceDescriptionsById.at(routeDeviceId)
                : "";

        struct NodeSnapshot
        {
            uint32_t nodeId = INVALID_ID;
            std::string nodeName;
            std::string nodeSerial;
            std::string direction;
            std::string type;
            std::string fallbackName;
            int32_t cardProfileDevice = -1;
        };

        std::unordered_map<uint32_t, NodeSnapshot> nodesById;
        for (const auto& [_, device] : _devices)
        {
            if (device->_pwDeviceId != routeDeviceId)
                continue;

            auto& node = nodesById[device->_pwNodeId];
            node.nodeId = device->_pwNodeId;
            if (node.nodeName.empty())
                node.nodeName = device->_pwNodeName;
            if (node.nodeSerial.empty())
                node.nodeSerial = device->_pwObjectSerial;
            if (node.direction.empty())
                node.direction = device->_direction.Get();
            if (node.type.empty())
                node.type = device->_type.Get();
            if (node.fallbackName.empty())
                node.fallbackName = device->_name.Get();
            if (node.cardProfileDevice < 0)
                node.cardProfileDevice = device->_pwCardProfileDevice;
        }
        if (nodesById.empty())
            return;

        const auto collectDirectionNodes = [&](const std::string_view direction)
        {
            std::vector<NodeSnapshot*> nodes;
            for (auto& [_, node] : nodesById)
            {
                if (node.direction == direction)
                    nodes.push_back(&node);
            }
            std::sort(nodes.begin(), nodes.end(), [](const auto* lhs, const auto* rhs) { return lhs->nodeId < rhs->nodeId; });
            return nodes;
        };

        const auto removeStaleRouteDevicesForDirection = [&](const std::string_view direction,
                                                             const std::vector<ObjectPath>& expectedPaths)
        {
            std::vector<ObjectPath> stalePaths;
            for (const auto& [path, device] : _devices)
            {
                if (!device->_pwIsRouteDevice)
                    continue;
                if (device->_pwDeviceId != routeDeviceId)
                    continue;
                if (device->_direction.Get() != direction)
                    continue;
                if (std::find(expectedPaths.begin(), expectedPaths.end(), path) != expectedPaths.end())
                    continue;
                stalePaths.push_back(path);
            }

            for (const auto& path : stalePaths)
                RemoveLogicalDevice(path);
        };

        const auto removeFallbackDevicesForNodes = [&](const std::vector<NodeSnapshot*>& directionNodes)
        {
            std::vector<ObjectPath> fallbackPaths;
            fallbackPaths.reserve(directionNodes.size());
            for (const auto* node : directionNodes)
            {
                const ObjectPath fallbackPath = GetBaseObjectPath()
                    .Child("Devices")
                    .Child(std::to_string(node->nodeId));
                if (_devices.contains(fallbackPath))
                    fallbackPaths.push_back(fallbackPath);
            }

            for (const auto& path : fallbackPaths)
                RemoveLogicalDevice(path);
        };

        const auto refreshDirection = [&](const std::string_view direction)
        {
            auto directionNodes = collectDirectionNodes(direction);
            if (directionNodes.empty())
                return;

            const uint32_t directionId = SpaDirectionFromLogicalDirection(direction);
            std::vector<const DeviceProxyData::RouteInfo*> directionRoutes;
            std::vector<const DeviceProxyData::RouteInfo*> matchingRoutes;

            const auto matchesSelectedNodeDevice = [&](const DeviceProxyData::RouteInfo& route)
            {
                for (const auto* node : directionNodes)
                {
                    if (node->cardProfileDevice >= 0 &&
                        route.cardDevice >= 0 &&
                        node->cardProfileDevice == route.cardDevice)
                    {
                        return true;
                    }
                }
                return false;
            };

            for (auto& [_, route] : itProxy->second->routesByKey)
            {
                if (route.direction != directionId)
                    continue;

                directionRoutes.push_back(&route);
            }

            std::sort(
                directionRoutes.begin(),
                directionRoutes.end(),
                [](const auto* lhs, const auto* rhs) { return lhs->index < rhs->index; }
            );

            if (directionRoutes.empty())
            {
                removeStaleRouteDevicesForDirection(direction, {});
                for (const auto* node : directionNodes)
                {
                    const ObjectPath fallbackPath = GetBaseObjectPath()
                        .Child("Devices")
                        .Child(std::to_string(node->nodeId));

                    AddLogicalDevice(
                        fallbackPath,
                        node->nodeId,
                        routeDeviceId,
                        node->cardProfileDevice,
                        -1,
                        node->fallbackName.empty() ? std::format("Device {}", node->nodeId) : node->fallbackName,
                        node->type.empty() ? "unknown" : node->type,
                        std::string(direction),
                        true
                    );

                    if (const auto it = _devices.find(fallbackPath); it != _devices.end())
                    {
                        it->second->_pwNodeName = node->nodeName;
                        it->second->_pwObjectSerial = node->nodeSerial;
                    }
                }

                for (const auto* node : directionNodes)
                    RefreshActiveDevicesForNode(node->nodeId);
                return;
            }

            const bool hasExplicitActiveRoute = std::any_of(
                directionRoutes.begin(),
                directionRoutes.end(),
                [](const DeviceProxyData::RouteInfo* route)
                {
                    return route->active;
                }
            );

            const DeviceProxyData::RouteInfo* inferredActiveRoute = nullptr;
            if (!hasExplicitActiveRoute)
            {
                for (const auto* route : directionRoutes)
                {
                    if (route->availability == SPA_PARAM_AVAILABILITY_no)
                        continue;
                    if (!matchesSelectedNodeDevice(*route))
                        continue;
                    inferredActiveRoute = route;
                    break;
                }

                if (!inferredActiveRoute)
                {
                    for (const auto* route : directionRoutes)
                    {
                        if (!matchesSelectedNodeDevice(*route))
                            continue;
                        inferredActiveRoute = route;
                        break;
                    }
                }
            }

            for (const auto* route : directionRoutes)
            {
                const bool routeInferredActive =
                    inferredActiveRoute && inferredActiveRoute->index == route->index;
                if (route->availability == SPA_PARAM_AVAILABILITY_no &&
                    !route->active &&
                    !routeInferredActive)
                {
                    continue;
                }
                matchingRoutes.push_back(route);
            }

            std::sort(
                matchingRoutes.begin(),
                matchingRoutes.end(),
                [](const auto* lhs, const auto* rhs) { return lhs->index < rhs->index; }
            );

            if (matchingRoutes.empty())
            {
                removeStaleRouteDevicesForDirection(direction, {});
                removeFallbackDevicesForNodes(directionNodes);
                for (const auto* node : directionNodes)
                    RefreshActiveDevicesForNode(node->nodeId);
                return;
            }

            std::vector<ObjectPath> expectedPaths;
            expectedPaths.reserve(matchingRoutes.size());
            for (const auto* route : matchingRoutes)
            {
                NodeSnapshot* selectedNode = directionNodes.front();
                if (route->cardDevice >= 0)
                {
                    if (const auto itSelected = std::find_if(
                        directionNodes.begin(),
                        directionNodes.end(),
                        [&](const NodeSnapshot* node)
                        {
                            return node->cardProfileDevice >= 0 && node->cardProfileDevice == route->cardDevice;
                        }
                    ); itSelected != directionNodes.end())
                    {
                        selectedNode = *itSelected;
                    }
                }

                const ObjectPath routePath = GetBaseObjectPath()
                    .Child("Devices")
                    .Child(RouteObjectComponent(routeDeviceId, directionId, route->index));
                expectedPaths.push_back(routePath);

                const std::string routeName = BuildRouteDisplayName(
                    route->description,
                    route->name,
                    selectedNode->fallbackName.empty()
                        ? std::format("Device {}", selectedNode->nodeId)
                        : selectedNode->fallbackName,
                    routeDeviceDescription
                );
                const int32_t routeCardDevice = route->cardDevice >= 0
                    ? route->cardDevice
                    : selectedNode->cardProfileDevice;

                AddLogicalDevice(
                    routePath,
                    selectedNode->nodeId,
                    routeDeviceId,
                    routeCardDevice,
                    static_cast<int32_t>(route->index),
                    routeName,
                    selectedNode->type.empty() ? "unknown" : selectedNode->type,
                    std::string(direction),
                    route->availability != SPA_PARAM_AVAILABILITY_no
                );

                if (const auto it = _devices.find(routePath); it != _devices.end())
                {
                    it->second->_pwNodeName = selectedNode->nodeName;
                    it->second->_pwObjectSerial = selectedNode->nodeSerial;
                    const bool routeInferredActive =
                        inferredActiveRoute && inferredActiveRoute->index == route->index;
                    it->second->_pwRouteActive = route->active || routeInferredActive;
                }
            }

            removeStaleRouteDevicesForDirection(direction, expectedPaths);
            removeFallbackDevicesForNodes(directionNodes);

            for (const auto* node : directionNodes)
                RefreshActiveDevicesForNode(node->nodeId);
        };

        refreshDirection("output");
        refreshDirection("input");
    }

    void AudioService::HandleNodeInfo(const uint32_t id, const spa_dict* props)
    {
        const char* description = LookupProp(props, PW_KEY_NODE_DESCRIPTION);
        const char* nick = LookupProp(props, PW_KEY_NODE_NICK);
        const char* nodeName = LookupProp(props, PW_KEY_NODE_NAME);
        const char* deviceName = LookupProp(props, PW_KEY_DEVICE_NAME);
        const char* serial = LookupProp(props, PW_KEY_OBJECT_SERIAL);
        const auto routeDeviceId = ParseInt32Prop(LookupProp(props, PW_KEY_DEVICE_ID));
        const auto cardProfileDevice = ParseInt32Prop(LookupProp(props, "card.profile.device"));
        uint32_t resolvedRouteDeviceId = INVALID_ID;
        if (routeDeviceId.has_value() && *routeDeviceId >= 0)
            resolvedRouteDeviceId = static_cast<uint32_t>(*routeDeviceId);
        else if (deviceName && deviceName[0] != '\0')
        {
            if (const auto inferred = FindRouteDeviceIdByDeviceName(deviceName, _routeDeviceNamesById); inferred.has_value())
                resolvedRouteDeviceId = *inferred;
        }
        else if (nodeName && nodeName[0] != '\0')
        {
            if (const auto inferred = FindRouteDeviceIdByNodeName(nodeName, _routeDeviceNamesById); inferred.has_value())
                resolvedRouteDeviceId = *inferred;
        }

        for (const auto& devPath : FindDevicePathsByNodeId(id))
        {
            const auto it = _devices.find(devPath);
            if (it != _devices.end())
            {
                if (!it->second->_pwIsRouteDevice)
                {
                    it->second->_name.Set(
                        description && description[0] != '\0'
                            ? description
                            : (nick && nick[0] != '\0'
                                ? nick
                                : (nodeName && nodeName[0] != '\0'
                                    ? nodeName
                                    : it->second->_name.Get()))
                    );
                }
                if (nodeName && nodeName[0] != '\0')
                    it->second->_pwNodeName = nodeName;
                if (serial && serial[0] != '\0')
                    it->second->_pwObjectSerial = serial;
                if (resolvedRouteDeviceId != INVALID_ID)
                    it->second->_pwDeviceId = resolvedRouteDeviceId;
                if (cardProfileDevice.has_value() &&
                    (!it->second->_pwIsRouteDevice || it->second->_pwCardProfileDevice < 0))
                {
                    it->second->_pwCardProfileDevice = *cardProfileDevice;
                }
                if (!it->second->_pwIsRouteDevice)
                    it->second->_available.Set(true);
            }
        }

        if (cardProfileDevice.has_value() && *cardProfileDevice >= 0)
        {
            const int32_t selectedCardDevice = *cardProfileDevice;
            std::vector<AudioDevice*> matchingRouteDevices;
            AudioDevice* preferredActiveRoute = nullptr;
            bool touchedAnyRoute = false;
            for (const auto& devPath : FindDevicePathsByNodeId(id))
            {
                const auto it = _devices.find(devPath);
                if (it == _devices.end())
                    continue;
                if (!it->second->_pwIsRouteDevice)
                    continue;
                if (it->second->_pwCardProfileDevice < 0)
                    continue;

                touchedAnyRoute = true;
                if (it->second->_pwCardProfileDevice != selectedCardDevice)
                {
                    it->second->_pwRouteActive = false;
                    continue;
                }

                matchingRouteDevices.push_back(it->second.get());
                if (!preferredActiveRoute && it->second->_pwRouteActive)
                    preferredActiveRoute = it->second.get();
            }

            if (!matchingRouteDevices.empty())
            {
                if (!preferredActiveRoute)
                {
                    if (const auto itPreferred = std::find_if(
                        matchingRouteDevices.begin(),
                        matchingRouteDevices.end(),
                        [](const AudioDevice* routeDevice) { return routeDevice->_available.Get(); }
                    ); itPreferred != matchingRouteDevices.end())
                    {
                        preferredActiveRoute = *itPreferred;
                    }
                    else
                    {
                        preferredActiveRoute = matchingRouteDevices.front();
                    }
                }

                for (auto* routeDevice : matchingRouteDevices)
                    routeDevice->_pwRouteActive = routeDevice == preferredActiveRoute;
            }
            if (touchedAnyRoute)
                RefreshActiveDevicesForNode(id);
        }

        if (resolvedRouteDeviceId != INVALID_ID)
        {
            BindDeviceProxy(resolvedRouteDeviceId);
            RefreshLogicalDevicesForRouteDevice(resolvedRouteDeviceId);
        }

        if (const auto streamPath = FindStreamPathByNodeId(id); streamPath != ObjectPath{})
        {
            const auto it = _streams.find(streamPath);
            if (it != _streams.end())
            {
                const char* description = LookupProp(props, PW_KEY_NODE_DESCRIPTION);
                const char* nodeName = LookupProp(props, PW_KEY_NODE_NAME);
                const char* appName = LookupProp(props, PW_KEY_APP_NAME);
                const char* targetObject = LookupProp(props, PW_KEY_TARGET_OBJECT);
                if (!targetObject)
                    targetObject = LookupProp(props, LEGACY_NODE_TARGET_KEY);

                it->second->_name.Set(
                    description && description[0] != '\0'
                        ? description
                        : (nodeName && nodeName[0] != '\0'
                            ? nodeName
                            : it->second->_name.Get())
                );
                if (nodeName && nodeName[0] != '\0')
                    it->second->_pwNodeName = nodeName;
                if (appName && appName[0] != '\0')
                    it->second->_applicationName.Set(appName);

                it->second->_suppressPropertyCallbacks = true;
                it->second->_device.Set(FindDevicePathByTargetObject(targetObject ? targetObject : ""));
                it->second->_suppressPropertyCallbacks = false;
            }
        }
    }

    void AudioService::HandleNodePropsParam(const uint32_t id, const spa_pod* param)
    {
        const bool hasVolumeProp = spa_pod_find_prop(param, nullptr, SPA_PROP_volume) != nullptr;
        const bool hasChannelVolumesProp = spa_pod_find_prop(param, nullptr, SPA_PROP_channelVolumes) != nullptr;
        const bool hasMuteProp = spa_pod_find_prop(param, nullptr, SPA_PROP_mute) != nullptr;

        float volume = -1.0f;
        bool muted = false;
        uint32_t channelValueSize = 0;
        uint32_t channelValueType = 0;
        uint32_t nChannelVolumes = 0;
        void* channelValuesRaw = nullptr;

        const int parseResult = spa_pod_parse_object(
            param,
            SPA_TYPE_OBJECT_Props,
            nullptr,
            SPA_PROP_volume, SPA_POD_OPT_Float(&volume),
            SPA_PROP_mute, SPA_POD_OPT_Bool(&muted),
            SPA_PROP_channelVolumes, SPA_POD_OPT_Array(
                &channelValueSize,
                &channelValueType,
                &nChannelVolumes,
                &channelValuesRaw
            )
        );
        if (parseResult < 0)
            return;

        if (
            hasChannelVolumesProp &&
            nChannelVolumes > 0 &&
            channelValuesRaw &&
            channelValueType == SPA_TYPE_Float &&
            channelValueSize == sizeof(float)
        )
        {
            if (const auto itNode = _nodeProxies.find(id); itNode != _nodeProxies.end() && itNode->second)
                itNode->second->channelCount = nChannelVolumes;
        }

        const std::optional<bool> parsedMuted = hasMuteProp
            ? std::make_optional(muted)
            : std::nullopt;
        const bool skipVolumeUpdate = parsedMuted.has_value() && *parsedMuted;

        std::optional<double> parsedVolume;
        if (
            !skipVolumeUpdate &&
            hasChannelVolumesProp &&
            nChannelVolumes > 0 &&
            channelValuesRaw &&
            channelValueType == SPA_TYPE_Float &&
            channelValueSize == sizeof(float)
        )
        {
            const auto* channelVolumes =
                static_cast<const float*>(channelValuesRaw);

            double sum = 0.0;
            for (uint32_t i = 0; i < nChannelVolumes; ++i)
                sum += channelVolumes[i];
            parsedVolume = ClampVolume(sum / static_cast<double>(nChannelVolumes));
        }
        else if (!skipVolumeUpdate && hasVolumeProp && volume >= 0.0f)
        {
            parsedVolume = ClampVolume(volume);
        }

        const auto devicePaths = FindDevicePathsByNodeId(id);
        for (const auto& devPath : devicePaths)
        {
            if (const auto it = _devices.find(devPath); it != _devices.end())
            {
                it->second->_suppressPropertyCallbacks = true;
                if (parsedVolume.has_value())
                    it->second->_volume.Set(*parsedVolume);
                if (parsedMuted.has_value())
                    it->second->_muted.Set(*parsedMuted);
                it->second->_suppressPropertyCallbacks = false;
            }
        }

        if (const auto streamPath = FindStreamPathByNodeId(id); streamPath != ObjectPath{})
        {
            if (const auto it = _streams.find(streamPath); it != _streams.end())
            {
                it->second->_suppressPropertyCallbacks = true;
                if (parsedVolume.has_value())
                    it->second->_volume.Set(*parsedVolume);
                if (parsedMuted.has_value())
                    it->second->_muted.Set(*parsedMuted);
                it->second->_suppressPropertyCallbacks = false;
            }
        }
    }

    void AudioService::BindMetadataProxy(const uint32_t id)
    {
        if (_metadataProxy)
            return;

        auto* metadata = static_cast<pw_metadata*>(
            pw_registry_bind(
                _pwRegistry,
                id,
                PW_TYPE_INTERFACE_Metadata,
                PW_VERSION_METADATA,
                0
            )
        );
        if (!metadata)
            return;

        auto md = std::make_unique<MetadataProxyData>();
        md->self = this;
        md->id = id;
        md->metadata = metadata;
        spa_zero(md->listener);

        static const pw_metadata_events metadataEvents = {
            .version = PW_VERSION_METADATA_EVENTS,
            .property = [](
                void* data,
                const uint32_t subject,
                const char* key,
                const char* type,
                const char* value
            ) -> int
            {
                auto* md = static_cast<MetadataProxyData*>(data);
                md->self->HandleMetadataProperty(subject, key, type, value);
                return 0;
            }
        };
        pw_metadata_add_listener(metadata, &md->listener, &metadataEvents, md.get());

        _metadataProxy = std::move(md);
    }

    void AudioService::HandleMetadataProperty(const uint32_t subject,
                                              const char* key,
                                              const char* type,
                                              const char* value)
    {
        (void) type;
        if (!key)
            return;

        const std::string keyStr = key;
        const std::string target = ParseMetadataTarget(value);

        if (subject == PW_ID_CORE)
        {
            if (keyStr == DEFAULT_AUDIO_SOURCE_KEY ||
                keyStr == DEFAULT_CONFIGURED_AUDIO_SOURCE_KEY)
            {
                const ObjectPath resolved = FindDevicePathByTargetObject(target);

                if (_pendingInputDevice != ObjectPath{})
                {
                    const auto pendingIt = _devices.find(_pendingInputDevice);
                    const auto resolvedIt = _devices.find(resolved);
                    const bool differentNode =
                        pendingIt != _devices.end() &&
                        resolvedIt != _devices.end() &&
                        pendingIt->second->_pwNodeId != resolvedIt->second->_pwNodeId;

                    if (differentNode)
                    {
                        // External source switched to a different physical device entirely.
                        _pendingInputDevice = ObjectPath{};
                        UpdateActiveInputDeviceFromBackend(resolved);
                    }
                    // else: same-node echo-back with stale route state -> ignore.
                }
                else
                {
                    UpdateActiveInputDeviceFromBackend(resolved);
                }
            }
            else if (keyStr == DEFAULT_AUDIO_SINK_KEY ||
                     keyStr == DEFAULT_CONFIGURED_AUDIO_SINK_KEY)
            {
                const ObjectPath resolved = FindDevicePathByTargetObject(target);

                if (_pendingOutputDevice != ObjectPath{})
                {
                    const auto pendingIt = _devices.find(_pendingOutputDevice);
                    const auto resolvedIt = _devices.find(resolved);
                    const bool differentNode =
                        pendingIt != _devices.end() &&
                        resolvedIt != _devices.end() &&
                        pendingIt->second->_pwNodeId != resolvedIt->second->_pwNodeId;

                    if (differentNode)
                    {
                        // External source switched to a different physical device entirely.
                        _pendingOutputDevice = ObjectPath{};
                        UpdateActiveOutputDeviceFromBackend(resolved);
                    }
                    // else: same-node echo-back with stale route state -> ignore.
                }
                else
                {
                    UpdateActiveOutputDeviceFromBackend(resolved);
                }
            }
            return;
        }

        if (keyStr == PW_KEY_TARGET_OBJECT || keyStr == LEGACY_NODE_TARGET_KEY)
        {
            if (const auto streamPath = FindStreamPathByNodeId(subject); streamPath != ObjectPath{})
            {
                if (const auto it = _streams.find(streamPath); it != _streams.end())
                {
                    it->second->_suppressPropertyCallbacks = true;
                    it->second->_device.Set(FindDevicePathByTargetObject(target));
                    it->second->_suppressPropertyCallbacks = false;
                }
            }
        }
    }

    void AudioService::SetNodeVolumeByPath(const ObjectPath& path, const double volume)
    {
        uint32_t nodeId = INVALID_ID;
        if (const auto it = _devices.find(path); it != _devices.end())
            nodeId = it->second->_pwNodeId;
        else if (const auto it = _streams.find(path); it != _streams.end())
            nodeId = it->second->_pwNodeId;

        if (nodeId == INVALID_ID)
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Unknown audio object path");

        const auto itNode = _nodeProxies.find(nodeId);
        if (itNode == _nodeProxies.end() || !itNode->second->node)
            throw DBusException(DBUS_ERROR_FAILED, "PipeWire node is not available");

        const float vol = static_cast<float>(ClampVolume(volume));
        const uint32_t channelCount = itNode->second->channelCount;
        std::vector<float> channelVolumes;
        if (channelCount > 0)
            channelVolumes.assign(channelCount, vol);

        uint8_t buffer[256];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod* param = nullptr;
        if (!channelVolumes.empty())
        {
            param = reinterpret_cast<spa_pod*>(spa_pod_builder_add_object(
                &builder,
                SPA_TYPE_OBJECT_Props,
                SPA_PARAM_Props,
                SPA_PROP_volume, SPA_POD_Float(vol),
                SPA_PROP_channelVolumes, SPA_POD_Array(
                    sizeof(float),
                    SPA_TYPE_Float,
                    channelVolumes.size(),
                    channelVolumes.data()
                )
            ));
        }
        else
        {
            param = reinterpret_cast<spa_pod*>(spa_pod_builder_add_object(
                &builder,
                SPA_TYPE_OBJECT_Props,
                SPA_PARAM_Props,
                SPA_PROP_volume, SPA_POD_Float(vol)
            ));
        }

        pw_thread_loop_lock(_pwThreadLoop);
        const int result = pw_node_set_param(itNode->second->node, SPA_PARAM_Props, 0, param);
        if (result >= 0)
            pw_node_enum_params(itNode->second->node, 0, SPA_PARAM_Props, 0, 1, nullptr);
        pw_thread_loop_unlock(_pwThreadLoop);

        if (result < 0)
            throw DBusException(DBUS_ERROR_FAILED, std::format("Failed to set volume: {}", result));
    }

    void AudioService::SetNodeMutedByPath(const ObjectPath& path, const bool muted)
    {
        uint32_t nodeId = INVALID_ID;
        if (const auto it = _devices.find(path); it != _devices.end())
        {
            nodeId = it->second->_pwNodeId;
        }
        else if (const auto it = _streams.find(path); it != _streams.end())
        {
            nodeId = it->second->_pwNodeId;
        }

        if (nodeId == INVALID_ID)
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Unknown audio object path");

        const auto itNode = _nodeProxies.find(nodeId);
        if (itNode == _nodeProxies.end() || !itNode->second->node)
            throw DBusException(DBUS_ERROR_FAILED, "PipeWire node is not available");

        uint8_t buffer[256];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod* param = reinterpret_cast<spa_pod*>(spa_pod_builder_add_object(
            &builder,
            SPA_TYPE_OBJECT_Props,
            SPA_PARAM_Props,
            SPA_PROP_mute, SPA_POD_Bool(muted)
        ));

        pw_thread_loop_lock(_pwThreadLoop);
        const int result = pw_node_set_param(itNode->second->node, SPA_PARAM_Props, 0, param);
        if (result >= 0)
            pw_node_enum_params(itNode->second->node, 0, SPA_PARAM_Props, 0, 1, nullptr);
        pw_thread_loop_unlock(_pwThreadLoop);

        if (result < 0)
            throw DBusException(DBUS_ERROR_FAILED, std::format("Failed to set mute: {}", result));
    }

    void AudioService::SetRouteByDevicePath(const ObjectPath& path)
    {
        const auto itDevice = _devices.find(path);
        if (itDevice == _devices.end())
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Unknown audio device");

        const auto& device = *itDevice->second;
        if (!device._pwIsRouteDevice || device._pwRouteIndex < 0 || device._pwDeviceId == INVALID_ID)
            return;

        const auto itProxy = _deviceProxies.find(device._pwDeviceId);
        if (itProxy == _deviceProxies.end() || !itProxy->second || !itProxy->second->device)
            throw DBusException(DBUS_ERROR_FAILED, "PipeWire route device is not available");
        if (itProxy->second->routeParamsKnown && !itProxy->second->supportsRouteParams)
            return;

        uint32_t directionId = SpaDirectionFromLogicalDirection(device._direction.Get());
        int32_t cardDevice = -1;

        if (const auto itRoute = itProxy->second->routesByKey.find(
            DeviceProxyData::MakeRouteKey(directionId, static_cast<uint32_t>(device._pwRouteIndex))
        ); itRoute != itProxy->second->routesByKey.end())
        {
            directionId = itRoute->second.direction;
            cardDevice = itRoute->second.cardDevice;
        }

        uint8_t buffer[256];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod* param = nullptr;

        if (cardDevice >= 0)
        {
            param = reinterpret_cast<spa_pod*>(spa_pod_builder_add_object(
                &builder,
                SPA_TYPE_OBJECT_ParamRoute,
                SPA_PARAM_Route,
                SPA_PARAM_ROUTE_index, SPA_POD_Int(device._pwRouteIndex),
                SPA_PARAM_ROUTE_direction, SPA_POD_Id(directionId),
                SPA_PARAM_ROUTE_device, SPA_POD_Int(cardDevice),
                SPA_PARAM_ROUTE_save, SPA_POD_Bool(true)
            ));
        }
        else
        {
            param = reinterpret_cast<spa_pod*>(spa_pod_builder_add_object(
                &builder,
                SPA_TYPE_OBJECT_ParamRoute,
                SPA_PARAM_Route,
                SPA_PARAM_ROUTE_index, SPA_POD_Int(device._pwRouteIndex),
                SPA_PARAM_ROUTE_direction, SPA_POD_Id(directionId),
                SPA_PARAM_ROUTE_save, SPA_POD_Bool(true)
            ));
        }

        pw_thread_loop_lock(_pwThreadLoop);
        const int result = pw_device_set_param(itProxy->second->device, SPA_PARAM_Route, 0, param);
        if (result >= 0)
            pw_device_enum_params(itProxy->second->device, 0, SPA_PARAM_Route, 0, UINT32_MAX, nullptr);
        pw_thread_loop_unlock(_pwThreadLoop);

        if (result < 0)
        {
            throw DBusException(
                DBUS_ERROR_FAILED,
                std::format("Failed to set route for device {} route {}: {}", device._pwDeviceId, device._pwRouteIndex, result)
            );
        }
    }

    void AudioService::SetDefaultDevice(const bool inputDirection, const ObjectPath& path)
    {
        if (!_metadataProxy || !_metadataProxy->metadata)
            throw DBusException(DBUS_ERROR_FAILED, "PipeWire metadata is not available");

        const char* key = inputDirection
            ? DEFAULT_AUDIO_SOURCE_KEY
            : DEFAULT_AUDIO_SINK_KEY;
        const char* configuredKey = inputDirection
            ? DEFAULT_CONFIGURED_AUDIO_SOURCE_KEY
            : DEFAULT_CONFIGURED_AUDIO_SINK_KEY;

        if (path == ObjectPath{})
        {
            pw_thread_loop_lock(_pwThreadLoop);
            const int configuredResult = pw_metadata_set_property(
                _metadataProxy->metadata,
                PW_ID_CORE,
                configuredKey,
                nullptr,
                nullptr
            );
            int result = 0;
            if (configuredResult < 0)
            {
                result = pw_metadata_set_property(
                    _metadataProxy->metadata,
                    PW_ID_CORE,
                    key,
                    nullptr,
                    nullptr
                );
            }
            pw_thread_loop_unlock(_pwThreadLoop);

            if (configuredResult < 0 && result < 0)
            {
                throw DBusException(
                    DBUS_ERROR_FAILED,
                    std::format("Failed to clear default device (configured={}, runtime={})", configuredResult, result)
                );
            }
            return;
        }

        const auto it = _devices.find(path);
        if (it == _devices.end())
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Unknown audio device");
        if (it->second->_direction.Get() != (inputDirection ? "input" : "output"))
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Audio device direction mismatch");

        const std::string target = !it->second->_pwNodeName.empty()
            ? it->second->_pwNodeName
            : (!it->second->_pwObjectSerial.empty() ? it->second->_pwObjectSerial : std::to_string(it->second->_pwNodeId));

        const std::string json = std::format(
            "{{\"name\":\"{}\",\"id\":{}}}",
            EscapeJsonString(target),
            it->second->_pwNodeId
        );

        pw_thread_loop_lock(_pwThreadLoop);
        const int configuredResult = pw_metadata_set_property(
            _metadataProxy->metadata,
            PW_ID_CORE,
            configuredKey,
            "Spa:String:JSON",
            json.c_str()
        );
        const int result = pw_metadata_set_property(
            _metadataProxy->metadata,
            PW_ID_CORE,
            key,
            "Spa:String:JSON",
            json.c_str()
        );
        pw_thread_loop_unlock(_pwThreadLoop);

        if (configuredResult < 0 && result < 0)
        {
            throw DBusException(
                DBUS_ERROR_FAILED,
                std::format(
                    "Failed to set default device {} (configured={}, runtime={})",
                    target,
                    configuredResult,
                    result
                )
            );
        }

        // Authoritatively update _activeOutputDevice (or Input) immediately.
        // The metadata echo-back in HandleMetadataProperty cannot distinguish
        // between route devices sharing the same PipeWire node, so it always
        // resolves to whichever route was previously active (stale _pwRouteActive).
        // We do this here as the ground truth for the user's explicit choice;
        // subsequent PipeWire events (route change, WirePlumber override) will
        // update it further if needed.
        if (inputDirection)
        {
            _pendingInputDevice = path;
            UpdateActiveInputDeviceFromBackend(path);
        }
        else
        {
            _pendingOutputDevice = path;
            UpdateActiveOutputDeviceFromBackend(path);
        }

        try
        {
            SetRouteByDevicePath(path);
        }
        catch (const DBusException&)
        {
            // Route change failed: clear the pending guard since no
            // confirmation event will ever arrive to clear it naturally.
            if (inputDirection)
                _pendingInputDevice = ObjectPath{};
            else
                _pendingOutputDevice = ObjectPath{};
            throw;
        }

        // Move existing streams explicitly so device switching is effective even
        // when WirePlumber's automatic stream move policy is disabled.
        const std::string streamTarget = !it->second->_pwNodeName.empty()
            ? it->second->_pwNodeName
            : (!it->second->_pwObjectSerial.empty()
                ? it->second->_pwObjectSerial
                : std::to_string(it->second->_pwNodeId));

        const std::string wantedDirection = inputDirection ? "input" : "output";
        int firstStreamMoveError = 0;

        pw_thread_loop_lock(_pwThreadLoop);
        for (const auto& [_, stream] : _streams)
        {
            if (stream->_direction.Get() != wantedDirection)
                continue;

            const int moveResult = pw_metadata_set_property(
                _metadataProxy->metadata,
                stream->_pwNodeId,
                PW_KEY_TARGET_OBJECT,
                streamTarget.empty() ? nullptr : "Spa:String",
                streamTarget.empty() ? nullptr : streamTarget.c_str()
            );
            if (moveResult < 0 && firstStreamMoveError == 0)
                firstStreamMoveError = moveResult;
        }
        pw_thread_loop_unlock(_pwThreadLoop);

        if (firstStreamMoveError < 0)
        {
            _serviceManager->Get<Logger::LoggerService>()->Err(
                std::format("AudioService: failed to move one or more streams to new default device: {}", firstStreamMoveError)
            );
        }
    }

    ObjectPath AudioService::FindDevicePathByNodeId(const uint32_t id) const
    {
        const auto paths = FindDevicePathsByNodeId(id);
        if (paths.empty())
            return ObjectPath{};

        ObjectPath activePath;
        ObjectPath availablePath;
        for (const auto& path : paths)
        {
            const auto it = _devices.find(path);
            if (it == _devices.end())
                continue;

            if (it->second->_pwRouteActive && it->second->_available.Get())
                return path;

            if (it->second->_pwRouteActive && activePath == ObjectPath{})
                activePath = path;
            if (it->second->_available.Get() && availablePath == ObjectPath{})
                availablePath = path;
        }

        if (activePath != ObjectPath{})
            return activePath;
        if (availablePath != ObjectPath{})
            return availablePath;

        return paths.front();
    }

    std::vector<ObjectPath> AudioService::FindDevicePathsByNodeId(const uint32_t id) const
    {
        const auto it = _devicePathsByNodeId.find(id);
        if (it == _devicePathsByNodeId.end())
            return {};
        return it->second;
    }

    ObjectPath AudioService::FindStreamPathByNodeId(const uint32_t id) const
    {
        const auto it = _streamPathsByNodeId.find(id);
        if (it == _streamPathsByNodeId.end())
            return ObjectPath{};
        return it->second;
    }

    ObjectPath AudioService::FindDevicePathByTargetObject(const std::string& targetObject) const
    {
        if (targetObject.empty())
            return ObjectPath{};

        ObjectPath fallbackPath;
        uint32_t fallbackNodeId = INVALID_ID;
        int fallbackRank = std::numeric_limits<int>::max();
        for (const auto& [path, dev] : _devices)
        {
            if (targetObject == dev->_pwNodeName ||
                targetObject == dev->_pwObjectSerial ||
                targetObject == std::to_string(dev->_pwNodeId) ||
                targetObject == dev->_id.Get())
            {
                int rank = 3;
                if (dev->_pwRouteActive && dev->_available.Get())
                    rank = 0;
                else if (dev->_pwRouteActive)
                    rank = 1;
                else if (dev->_available.Get())
                    rank = 2;

                if (fallbackPath == ObjectPath{} ||
                    rank < fallbackRank ||
                    (rank == fallbackRank && path.ToString() < fallbackPath.ToString()))
                {
                    fallbackPath = path;
                    fallbackNodeId = dev->_pwNodeId;
                    fallbackRank = rank;
                }
            }
        }

        if (fallbackPath == ObjectPath{})
            return ObjectPath{};

        if (fallbackNodeId != INVALID_ID)
        {
            const auto preferredPath = FindDevicePathByNodeId(fallbackNodeId);
            if (preferredPath != ObjectPath{})
                return preferredPath;
        }

        return fallbackPath;
    }

    void AudioService::UpdateActiveInputDeviceFromBackend(const ObjectPath& path)
    {
        if (_activeInputDevice == path)
            return;
        _activeInputDevice = path;
        Utils::DBusUtils::EmitPropertyChanged(*_conn, _iface, "ActiveInputDevice", path);
    }

    void AudioService::UpdateActiveOutputDeviceFromBackend(const ObjectPath& path)
    {
        if (_activeOutputDevice == path)
            return;
        _activeOutputDevice = path;
        Utils::DBusUtils::EmitPropertyChanged(*_conn, _iface, "ActiveOutputDevice", path);
    }

    void AudioService::OnListDevices(const Message& message) const
    {
        SharedPolicy(message);
        auto reply = Message::CreateMethodReturn(message);
        reply.SetArgs(ListDevices());
        reply.Send(*_conn);
    }

    void AudioService::OnListStreams(const Message& message) const
    {
        SharedPolicy(message);
        auto reply = Message::CreateMethodReturn(message);
        reply.SetArgs(ListStreams());
        reply.Send(*_conn);
    }

    void AudioService::OnSetActiveInputDevice(const ObjectPath& path)
    {
        SetRouteByDevicePath(path);
        SetDefaultDevice(true, path);
    }

    void AudioService::OnSetActiveOutputDevice(const ObjectPath& path)
    {
        SetRouteByDevicePath(path);
        SetDefaultDevice(false, path);

        for (auto& [_, stream] : _streams)
        {
            if (stream->_direction.Get() == "output")
                SetStreamDevice(*stream, path);
        }
    }

    void AudioService::EmitDeviceAdded(const ObjectPath& path)
    {
        auto sig = Message::CreateSignal(
            GetBaseObjectPath(),
            GetBaseInterface(),
            "DeviceAdded"
        );
        sig.SetArgs(path);
        sig.Send(*_conn);
    }

    void AudioService::EmitDeviceRemoved(const ObjectPath& path)
    {
        auto sig = Message::CreateSignal(
            GetBaseObjectPath(),
            GetBaseInterface(),
            "DeviceRemoved"
        );
        sig.SetArgs(path);
        sig.Send(*_conn);
    }

    void AudioService::EmitStreamAdded(const ObjectPath& path)
    {
        auto sig = Message::CreateSignal(
            GetBaseObjectPath(),
            GetBaseInterface(),
            "StreamAdded"
        );
        sig.SetArgs(path);
        sig.Send(*_conn);
    }

    void AudioService::EmitStreamRemoved(const ObjectPath& path)
    {
        auto sig = Message::CreateSignal(
            GetBaseObjectPath(),
            GetBaseInterface(),
            "StreamRemoved"
        );
        sig.SetArgs(path);
        sig.Send(*_conn);
    }

    std::string AudioService::EncodeForObjectPath(const std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (const unsigned char c : s)
        {
            if (std::isalnum(c) || c == '_')
                out += static_cast<char>(c);
            else
            {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "_%02X", c);
                out += buf;
            }
        }
        return out;
    }
}
