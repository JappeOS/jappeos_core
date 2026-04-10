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

#pragma once

#include "../service.h"

typedef struct pw_thread_loop pw_thread_loop;
typedef struct pw_context pw_context;
typedef struct pw_core pw_core;
typedef struct pw_registry pw_registry;
typedef struct pw_metadata pw_metadata;
typedef struct spa_hook spa_hook;
typedef struct spa_dict spa_dict;
typedef struct spa_pod spa_pod;

namespace JappeStudios::JappeOS::JappeOSCore::Services::Audio
{
    class AudioDevice;
    class AudioStream;

    class AudioService : public Service
    {
    public:
        explicit AudioService(ServiceManager* serviceManager, Connection* conn);
        ~AudioService() override;
        [[nodiscard]] std::string GetName() const override { return "AudioService"; }

        [[nodiscard]] std::vector<ObjectPath> ListDevices() const;
        [[nodiscard]] std::vector<ObjectPath> ListStreams() const;

        void SetDeviceVolume(AudioDevice& device, double volume);
        void SetDeviceMuted(AudioDevice& device, bool muted);
        void SetStreamVolume(AudioStream& stream, double volume);
        void SetStreamMuted(AudioStream& stream, bool muted);
        void SetStreamDevice(AudioStream& stream, const ObjectPath& devicePath);

    private:
        struct NodeProxyData;
        struct MetadataProxyData;

        Object     _object;
        Interface& _iface;
        Prop<ObjectPath> _activeInputDevice;
        Prop<ObjectPath> _activeOutputDevice;
        bool _suppressActiveCallbacks = false;

        pw_thread_loop* _pwThreadLoop = nullptr;
        pw_context*     _pwContext = nullptr;
        pw_core*        _pwCore = nullptr;
        pw_registry*    _pwRegistry = nullptr;
        spa_hook*       _pwCoreListener = nullptr;
        spa_hook*       _pwRegistryListener = nullptr;

        std::unordered_map<ObjectPath, std::unique_ptr<AudioDevice>, ObjectPathHash> _devices;
        std::unordered_map<ObjectPath, std::unique_ptr<AudioStream>, ObjectPathHash> _streams;
        std::unordered_map<uint32_t, ObjectPath> _devicePathsByNodeId;
        std::unordered_map<uint32_t, ObjectPath> _streamPathsByNodeId;
        std::unordered_map<uint32_t, std::unique_ptr<NodeProxyData>> _nodeProxies;
        std::unique_ptr<MetadataProxyData> _metadataProxy;

        void SharedPolicy(const Message& message) const;

        void InitPipeWire();
        void ShutdownPipeWire();
        void ConnectRegistry();

        void HandleRegistryGlobal(uint32_t id, const char* type, uint32_t version, const spa_dict* props);
        void HandleRegistryGlobalRemove(uint32_t id);

        void AddDeviceNode(uint32_t id, const spa_dict* props);
        void AddStreamNode(uint32_t id, const spa_dict* props);
        void RemoveDeviceNode(uint32_t id);
        void RemoveStreamNode(uint32_t id);
        void BindNodeProxy(uint32_t id);
        void HandleNodeInfo(uint32_t id, const spa_dict* props);
        void HandleNodePropsParam(uint32_t id, const spa_pod* param);

        void BindMetadataProxy(uint32_t id);
        void HandleMetadataProperty(uint32_t subject, const char* key, const char* type, const char* value);

        void SetNodeVolumeByPath(const ObjectPath& path, double volume);
        void SetNodeMutedByPath(const ObjectPath& path, bool muted);
        void SetDefaultDevice(bool inputDirection, const ObjectPath& path);

        [[nodiscard]] ObjectPath FindDevicePathByNodeId(uint32_t id) const;
        [[nodiscard]] ObjectPath FindStreamPathByNodeId(uint32_t id) const;
        [[nodiscard]] ObjectPath FindDevicePathByTargetObject(const std::string& targetObject) const;

        void UpdateActiveInputDeviceFromBackend(const ObjectPath& path);
        void UpdateActiveOutputDeviceFromBackend(const ObjectPath& path);

        void OnListDevices(const Message& message) const;
        void OnListStreams(const Message& message) const;
        void OnSetActiveInputDevice(const ObjectPath& path);
        void OnSetActiveOutputDevice(const ObjectPath& path);
        void EmitDeviceAdded(const ObjectPath& path);
        void EmitDeviceRemoved(const ObjectPath& path);
        void EmitStreamAdded(const ObjectPath& path);
        void EmitStreamRemoved(const ObjectPath& path);

    private:
        static std::string EncodeForObjectPath(std::string_view s);
    };
}
