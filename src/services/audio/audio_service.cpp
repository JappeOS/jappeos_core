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
#include <pipewire/extensions/metadata.h>

#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/pod/vararg.h>
#include <spa/utils/dict.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <unistd.h>

#include "audio_device.h"
#include "audio_stream.h"
#include "../logger/logger_service.h"

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

        const char* LookupProp(const spa_dict* dict, const char* key)
        {
            return dict ? spa_dict_lookup(dict, key) : nullptr;
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
    }

    AudioService::AudioService(ServiceManager* serviceManager, Connection* conn) :
                               Service(serviceManager, conn),
                               _object(*_conn, GetBaseObjectPath()),
                               _iface(_object.CreateInterface(GetBaseInterface())),
                               _activeInputDevice(
                                   *_conn,
                                   _iface,
                                   "ActiveInputDevice",
                                   ObjectPath{},
                                   true,
                                   [this](const ObjectPath& path) { OnSetActiveInputDevice(path); }
                               ),
                               _activeOutputDevice(
                                   *_conn,
                                   _iface,
                                   "ActiveOutputDevice",
                                   ObjectPath{},
                                   true,
                                   [this](const ObjectPath& path) { OnSetActiveOutputDevice(path); }
                               )
    {
        _iface.RegisterMethod("ListDevices", [&] (const auto& m) { OnListDevices(m); });
        _iface.RegisterMethod("ListStreams", [&] (const auto& m) { OnListStreams(m); });
        InitPipeWire();
    }

    AudioService::~AudioService()
    {
        ShutdownPipeWire();
    }

    std::vector<ObjectPath> AudioService::ListDevices() const
    {
        auto ks = std::views::keys(_devices);
        std::vector<ObjectPath> keys{ks.begin(), ks.end()};
        return keys;
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

            target = !it->second->_pwObjectSerial.empty()
                ? it->second->_pwObjectSerial
                : (!it->second->_pwNodeName.empty()
                    ? it->second->_pwNodeName
                    : it->second->_id.Get());
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
                self->_serviceManager->Get<Logger::LoggerService>()->Warn(
                    "AudioService PipeWire error id=" + std::to_string(id) +
                    " seq=" + std::to_string(seq) +
                    " res=" + std::to_string(res) +
                    " message=" + (message ? message : "")
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
    }

    void AudioService::AddDeviceNode(const uint32_t id, const spa_dict* props)
    {
        const ObjectPath path = GetBaseObjectPath()
            .Child("Devices")
            .Child(std::to_string(id));

        if (_devices.contains(path))
            return;

        auto device = std::make_unique<AudioDevice>(*this, *_conn, path);

        const std::string mediaClass = LookupProp(props, PW_KEY_MEDIA_CLASS)
            ? LookupProp(props, PW_KEY_MEDIA_CLASS)
            : "";

        const char* nodeName = LookupProp(props, PW_KEY_NODE_NAME);
        const char* description = LookupProp(props, PW_KEY_NODE_DESCRIPTION);
        const char* nick = LookupProp(props, PW_KEY_NODE_NICK);
        const char* serial = LookupProp(props, PW_KEY_OBJECT_SERIAL);

        device->_pwNodeId = id;
        device->_pwNodeName = nodeName ? nodeName : "";
        device->_pwObjectSerial = serial ? serial : "";
        device->_id.Set(std::to_string(id));
        device->_name.Set(
            description && description[0] != '\0'
                ? description
                : (nick && nick[0] != '\0'
                    ? nick
                    : (nodeName && nodeName[0] != '\0' ? nodeName : std::format("Device {}", id)))
        );
        device->_type.Set(DeviceTypeFromMediaClass(mediaClass));
        device->_direction.Set(DirectionFromMediaClass(mediaClass));
        device->_available.Set(true);

        _devicePathsByNodeId[id] = path;
        _devices.emplace(path, std::move(device));
        EmitDeviceAdded(path);
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

    void AudioService::RemoveDeviceNode(const uint32_t id)
    {
        const auto itPath = _devicePathsByNodeId.find(id);
        if (itPath == _devicePathsByNodeId.end())
            return;

        const ObjectPath path = itPath->second;
        _devicePathsByNodeId.erase(itPath);

        if (const auto it = _devices.find(path); it != _devices.end())
        {
            _devices.erase(it);
            EmitDeviceRemoved(path);
        }

        if (_activeInputDevice.Get() == path)
            UpdateActiveInputDeviceFromBackend(ObjectPath{});
        if (_activeOutputDevice.Get() == path)
            UpdateActiveOutputDeviceFromBackend(ObjectPath{});
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

        pw_node_add_listener(nodeProxy, &data->listener, &nodeEvents, data.get());

        uint32_t params[] = { SPA_PARAM_Props };
        pw_node_subscribe_params(nodeProxy, params, 1);
        pw_node_enum_params(nodeProxy, 0, SPA_PARAM_Props, 0, UINT32_MAX, nullptr);

        _nodeProxies.emplace(id, std::move(data));
    }

    void AudioService::HandleNodeInfo(const uint32_t id, const spa_dict* props)
    {
        if (const auto devPath = FindDevicePathByNodeId(id); devPath != ObjectPath{})
        {
            const auto it = _devices.find(devPath);
            if (it != _devices.end())
            {
                const char* description = LookupProp(props, PW_KEY_NODE_DESCRIPTION);
                const char* nick = LookupProp(props, PW_KEY_NODE_NICK);
                const char* nodeName = LookupProp(props, PW_KEY_NODE_NAME);
                const char* serial = LookupProp(props, PW_KEY_OBJECT_SERIAL);

                it->second->_name.Set(
                    description && description[0] != '\0'
                        ? description
                        : (nick && nick[0] != '\0'
                            ? nick
                            : (nodeName && nodeName[0] != '\0'
                                ? nodeName
                                : it->second->_name.Get()))
                );
                if (nodeName && nodeName[0] != '\0')
                    it->second->_pwNodeName = nodeName;
                if (serial && serial[0] != '\0')
                    it->second->_pwObjectSerial = serial;
                it->second->_available.Set(true);
            }
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

        if (const auto devPath = FindDevicePathByNodeId(id); devPath != ObjectPath{})
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
            if (keyStr == "default.audio.source")
            {
                UpdateActiveInputDeviceFromBackend(
                    FindDevicePathByTargetObject(target)
                );
            }
            else if (keyStr == "default.audio.sink")
            {
                UpdateActiveOutputDeviceFromBackend(
                    FindDevicePathByTargetObject(target)
                );
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

    void AudioService::SetDefaultDevice(const bool inputDirection, const ObjectPath& path)
    {
        if (!_metadataProxy || !_metadataProxy->metadata)
            throw DBusException(DBUS_ERROR_FAILED, "PipeWire metadata is not available");

        const char* key = inputDirection
            ? "default.audio.source"
            : "default.audio.sink";

        if (path == ObjectPath{})
        {
            pw_thread_loop_lock(_pwThreadLoop);
            const int result = pw_metadata_set_property(
                _metadataProxy->metadata,
                PW_ID_CORE,
                key,
                nullptr,
                nullptr
            );
            pw_thread_loop_unlock(_pwThreadLoop);

            if (result < 0)
                throw DBusException(DBUS_ERROR_FAILED, std::format("Failed to clear default device: {}", result));

            if (inputDirection)
                UpdateActiveInputDeviceFromBackend(ObjectPath{});
            else
                UpdateActiveOutputDeviceFromBackend(ObjectPath{});
            return;
        }

        const auto it = _devices.find(path);
        if (it == _devices.end())
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Unknown audio device");

        const std::string target = !it->second->_pwNodeName.empty()
            ? it->second->_pwNodeName
            : (!it->second->_pwObjectSerial.empty() ? it->second->_pwObjectSerial : it->second->_id.Get());

        const std::string json = std::format(
            "{{\"name\":\"{}\"}}",
            EscapeJsonString(target)
        );

        pw_thread_loop_lock(_pwThreadLoop);
        const int result = pw_metadata_set_property(
            _metadataProxy->metadata,
            PW_ID_CORE,
            key,
            "Spa:String:JSON",
            json.c_str()
        );
        pw_thread_loop_unlock(_pwThreadLoop);

        if (result < 0)
            throw DBusException(DBUS_ERROR_FAILED, std::format("Failed to set default device: {}", result));

        if (inputDirection)
            UpdateActiveInputDeviceFromBackend(path);
        else
            UpdateActiveOutputDeviceFromBackend(path);
    }

    ObjectPath AudioService::FindDevicePathByNodeId(const uint32_t id) const
    {
        const auto it = _devicePathsByNodeId.find(id);
        if (it == _devicePathsByNodeId.end())
            return ObjectPath{};
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

        for (const auto& [path, dev] : _devices)
        {
            if (targetObject == dev->_pwNodeName ||
                targetObject == dev->_pwObjectSerial ||
                targetObject == std::to_string(dev->_pwNodeId) ||
                targetObject == dev->_id.Get())
            {
                return path;
            }
        }

        return ObjectPath{};
    }

    void AudioService::UpdateActiveInputDeviceFromBackend(const ObjectPath& path)
    {
        _suppressActiveCallbacks = true;
        _activeInputDevice.Set(path);
        _suppressActiveCallbacks = false;
    }

    void AudioService::UpdateActiveOutputDeviceFromBackend(const ObjectPath& path)
    {
        _suppressActiveCallbacks = true;
        _activeOutputDevice.Set(path);
        _suppressActiveCallbacks = false;
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
        if (_suppressActiveCallbacks)
            return;
        SetDefaultDevice(true, path);
    }

    void AudioService::OnSetActiveOutputDevice(const ObjectPath& path)
    {
        if (_suppressActiveCallbacks)
            return;
        SetDefaultDevice(false, path);
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
