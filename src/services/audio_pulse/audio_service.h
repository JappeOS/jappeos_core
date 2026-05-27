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

#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>
#include <glib.h>
#include "../service.h"
#include "../../logger.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::AudioPulse
{
#define AUDIO_DIRECTION_OUT      "output"
#define AUDIO_DIRECTION_IN       "input"
#define AUDIO_DIRECTION_OUT_BOOL true
#define AUDIO_DIRECTION_IN_BOOL  false

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

        void SetDeviceVolume(const AudioDevice& dev, double volume) const;
        void SetDeviceMuted(const AudioDevice& dev, bool muted) const;
        void SetStreamDevice(const AudioStream& stream, const ObjectPath& devicePath) const;
        void SetStreamVolume(const AudioStream& stream, double volume) const;
        void SetStreamMuted(const AudioStream& stream, bool muted) const;

    private:
        Object     _object;
        Interface& _iface;

        std::unordered_map<ObjectPath, std::unique_ptr<AudioDevice>, ObjectPathHash> _devices;
        std::unordered_map<ObjectPath, std::unique_ptr<AudioStream>, ObjectPathHash> _streams;
        ObjectPath _activeInputDevice{};
        ObjectPath _activeOutputDevice{};
        std::string _defaultSinkName;
        std::string _defaultSourceName;

        pa_glib_mainloop* _paLoop = nullptr;
        pa_context* _context = nullptr;
        bool _pulseReady = false;
        bool _cleaningUpPulse = false;

        void OnListDevices(const Message& message) const;
        void OnListStreams(const Message& message) const;
        void OnSetActiveInputDevice(const ObjectPath& path);
        void OnSetActiveOutputDevice(const ObjectPath& path);
        void EmitDeviceAdded(const ObjectPath& path);
        void EmitDeviceRemoved(const ObjectPath& path);
        void EmitStreamAdded(const ObjectPath& path);
        void EmitStreamRemoved(const ObjectPath& path);

        void InitPulseAudio();
        void CleanupPulseAudio();
        void HandleStateChange(const pa_context* ctx);
        void HandleSubscribeCallback(pa_subscription_event_type_t type, uint32_t index);
        void SyncInitialPulseState();
        void QueryServerInfo();
        void HandleServerInfo(const pa_server_info* info);

        void AddOrUpdateOutputDevice(const pa_sink_info* info);
        void RemoveOutputDevice(uint32_t index);
        void HandleOutputDeviceChanged(int event, uint32_t index);
        void QueryOutputDevice(uint32_t index);
        void AddOrUpdateInputDevice(const pa_source_info* info);
        void RemoveInputDevice(uint32_t index);
        void HandleInputDeviceChanged(int event, uint32_t index);
        void QueryInputDevice(uint32_t index);

        void AddOrUpdateStream(const pa_sink_input_info* info);
        void RemoveStream(uint32_t index);
        void HandleStreamChanged(int event, uint32_t index);
        void QueryStream(uint32_t index);

        void MoveStreamToDevice(uint32_t streamIndex, uint32_t sinkIndex) const;

        ObjectPath GetObjectPathForDevice(uint32_t deviceId, bool direction);
        ObjectPath GetObjectPathForStream(uint32_t deviceId);

    private:
        static JappeOSCore::Logger& Log()
        {
            static JappeOSCore::Logger instance{"AudioService"};
            return instance;
        }
    };
}
