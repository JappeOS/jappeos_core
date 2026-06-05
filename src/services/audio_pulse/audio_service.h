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
#include <unordered_set>
#include <string_view>
#include <glib.h>
#include "../service.h"
#include "../../logger.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::AudioPulse
{
#define AUDIO_DIRECTION_OUT      "output"
#define AUDIO_DIRECTION_IN       "input"
#define AUDIO_DIRECTION_OUT_BOOL true
#define AUDIO_DIRECTION_IN_BOOL  false

// From: https://freedesktop.org/software/pulseaudio/doxygen/proplist_8h.html#a77c49f81eb8426259c13fc53619b9670
#define AUDIO_DEVICE_TYPE_INTERNAL   "internal"
#define AUDIO_DEVICE_TYPE_SPEAKER    "speaker"
#define AUDIO_DEVICE_TYPE_HANDSET    "handset"
#define AUDIO_DEVICE_TYPE_TV         "tv"
#define AUDIO_DEVICE_TYPE_WEBCAM     "webcam"
#define AUDIO_DEVICE_TYPE_MICROPHONE "microphone"
#define AUDIO_DEVICE_TYPE_HEADSET    "headset"
#define AUDIO_DEVICE_TYPE_HEADPHONE  "headphone"
#define AUDIO_DEVICE_TYPE_HANDSFREE  "handsFree"
#define AUDIO_DEVICE_TYPE_CAR        "car"
#define AUDIO_DEVICE_TYPE_HIFI       "hifi"
#define AUDIO_DEVICE_TYPE_COMPUTER   "computer"
#define AUDIO_DEVICE_TYPE_PORTABLE   "portable"
#define AUDIO_DEVICE_TYPE_UNKNOWN    "unknown"

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
        std::unordered_map<uint32_t, std::unordered_set<ObjectPath, ObjectPathHash>> _outDevicesByPaIndex;
        std::unordered_map<uint32_t, std::unordered_set<ObjectPath, ObjectPathHash>> _inDevicesByPaIndex;
        std::unordered_map<uint32_t, std::string> _outActivePortByPaIndex;
        std::unordered_map<uint32_t, std::string> _inActivePortByPaIndex;
        ObjectPath _activeOutputDevice{};
        ObjectPath _activeInputDevice{};
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
        void RemoveOutputDevice(const ObjectPath& path);
        void RemoveOutputDevicesByIndex(uint32_t index);
        void HandleOutputDeviceChanged(int event, uint32_t index);
        bool HandleActiveOutputDeviceChanged(const ObjectPath& path);
        void HandleActiveOutputPortChanged(uint32_t sinkIndex, const std::string& portName);
        void QueryOutputDevice(uint32_t index);

        void AddOrUpdateInputDevice(const pa_source_info* info);
        void RemoveInputDevice(const ObjectPath& path);
        void RemoveInputDevicesByIndex(uint32_t index);
        void HandleInputDeviceChanged(int event, uint32_t index);
        bool HandleActiveInputDeviceChanged(const ObjectPath& path);
        void QueryInputDevice(uint32_t index);

        void AddOrUpdateStream(const pa_sink_input_info* info);
        void RemoveStream(uint32_t index);
        void HandleStreamChanged(int event, uint32_t index);
        void QueryStream(uint32_t index);

        void MoveStreamToDevice(uint32_t streamIndex, uint32_t sinkIndex) const;
        void ActivateInputPortAndMakeDefault(uint32_t sourceIndex, std::string sourceName, const std::string& portName);
        void SetDefaultSource(const std::string& sourceName) const;
        void SetSourcePort(uint32_t sourceIndex, const std::string& portName) const;
        void ActivateOutputPortAndMakeDefault(uint32_t sinkIndex, std::string sinkName, const std::string& portName);
        void SetDefaultSink(const std::string& sinkName) const;
        void SetSinkPort(uint32_t sinkIndex, const std::string& portName) const;

        ObjectPath GetObjectPathForDevice(uint32_t deviceId, bool direction, const std::string& portName);
        ObjectPath GetObjectPathForStream(uint32_t deviceId);

    private:
        static JappeOSCore::Logger& Log()
        {
            static JappeOSCore::Logger instance{"AudioService"};
            return instance;
        }

        static void PortIterator(uint32_t portCount, const std::function<void(std::optional<uint32_t> i)>& iterator);
        static std::string PaFormFactorToDeviceType(const std::string& formFactor);
        static std::string GetDeviceDisplayName(const char* paDeviceDescription, const char* paPortDescription = nullptr);
        static std::string EncodeForObjectPath(std::string_view value);
    };
}
