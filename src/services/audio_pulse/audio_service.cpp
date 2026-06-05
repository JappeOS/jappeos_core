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
#include "audio_device.h"
#include "audio_stream.h"
#include "../../utils/dbus_utils.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::AudioPulse
{

    AudioService::AudioService(ServiceManager* serviceManager,
                               Connection* conn) :
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

        InitPulseAudio();
    }

    AudioService::~AudioService()
    {
        CleanupPulseAudio();
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

    void AudioService::SetDeviceVolume(const AudioDevice& dev, const double volume) const
    {
        if (!_context)
            return;

        const bool out = dev.GetDirection() == AUDIO_DIRECTION_OUT;

        pa_cvolume paVolume;
        const auto v = static_cast<pa_volume_t>(volume * PA_VOLUME_NORM);
        pa_cvolume_set(&paVolume, dev._paChannels, v);

        pa_operation* op = out ? pa_context_set_sink_volume_by_index(
            _context,
            dev._paIndex,
            &paVolume,
            nullptr,
            nullptr
        ) : pa_context_set_source_volume_by_index(
            _context,
            dev._paIndex,
            &paVolume,
            nullptr,
            nullptr
        );

        if (op)
            pa_operation_unref(op);
    }

    void AudioService::SetDeviceMuted(const AudioDevice& dev, const bool muted) const
    {
        if (!_context)
            return;

        const bool out = dev.GetDirection() == AUDIO_DIRECTION_OUT;
        pa_operation* op = out ? pa_context_set_sink_mute_by_index(
            _context,
            dev._paIndex,
            muted,
            nullptr,
            nullptr
        ) : pa_context_set_source_mute_by_index(
            _context,
            dev._paIndex,
            muted,
            nullptr,
            nullptr
        );

        if (op)
            pa_operation_unref(op);
    }

    void AudioService::SetStreamDevice(const AudioStream& stream, const ObjectPath& devicePath) const
    {
        if (!_context)
            return;

        const auto it = _devices.find(devicePath);
        if (it == _devices.end())
        {
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Unknown audio device");
        }

        if (it->second->GetDirection() != AUDIO_DIRECTION_OUT)
        {
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Failed to set stream device, target is not an output device");
        }

        pa_operation* op = pa_context_move_sink_input_by_index(
            _context,
            stream._paIndex,
            it->second->_paIndex,
            nullptr,
            nullptr
        );

        if (op)
            pa_operation_unref(op);
    }

    void AudioService::SetStreamVolume(const AudioStream& stream, const double volume) const
    {
        if (!_context)
            return;

        pa_cvolume paVolume;
        const auto v = static_cast<pa_volume_t>(volume * PA_VOLUME_NORM);
        pa_cvolume_set(&paVolume, stream._paChannels, v);

        pa_operation* op = pa_context_set_sink_input_volume(
            _context,
            stream._paIndex,
            &paVolume,
            nullptr,
            nullptr
        );

        if (op)
            pa_operation_unref(op);
    }

    void AudioService::SetStreamMuted(const AudioStream& stream, const bool muted) const
    {
        if (!_context)
            return;

        pa_operation* op = pa_context_set_sink_input_mute(
            _context,
            stream._paIndex,
            muted,
            nullptr,
            nullptr
        );

        if (op)
            pa_operation_unref(op);
    }

    void AudioService::OnListDevices(const Message& message) const
    {
        auto msg = Message::CreateMethodReturn(message);
        const auto list = ListDevices();
        msg.SetArgs(list);
        msg.Send(*_conn);
    }

    void AudioService::OnListStreams(const Message& message) const
    {
        auto msg = Message::CreateMethodReturn(message);
        const auto list = ListStreams();
        msg.SetArgs(list);
        msg.Send(*_conn);
    }

    void AudioService::OnSetActiveInputDevice(const ObjectPath& path)
    {
        if (!_context || _activeInputDevice == path)
            return;

        const auto it = _devices.find(path);
        if (it == _devices.end())
        {
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Unknown audio device");
        }

        if (it->second->GetDirection() != AUDIO_DIRECTION_IN)
        {
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Failed to set stream device, target is not an input device");
        }

        /*pa_operation* op = pa_context_set_default_source(
            _context,
            it->second->_name.Get().c_str(),
            nullptr,
            nullptr
        );
        if (op)
            pa_operation_unref(op);

        SetSourcePort(it->second->_paIndex, it->second->_paPortName);*/

        ActivateInputPortAndMakeDefault(
            it->second->_paIndex,
            it->second->_paDeviceName,
            it->second->_paPortName
        );

        // TODO: We can move streams for input devices too, later, if needed.
    }

    void AudioService::OnSetActiveOutputDevice(const ObjectPath& path)
    {
        if (!_context || _activeOutputDevice == path)
            return;

        const auto it = _devices.find(path);
        if (it == _devices.end())
        {
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Unknown audio device");
        }

        if (it->second->GetDirection() != AUDIO_DIRECTION_OUT)
        {
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Failed to set stream device, target is not an output device");
        }

        /*pa_operation* op = pa_context_set_default_sink(
            _context,
            it->second->_name.Get().c_str(),
            nullptr,
            nullptr
        );
        if (op)
            pa_operation_unref(op);

        SetSinkPort(it->second->_paIndex, it->second->_paPortName);*/

        ActivateOutputPortAndMakeDefault(
            it->second->_paIndex,
            it->second->_paDeviceName,
            it->second->_paPortName
        );

        for (const auto &stream: _streams | std::views::values)
        {
            if (stream->GetDirection() != AUDIO_DIRECTION_OUT)
                continue;

            MoveStreamToDevice(stream->_paIndex, it->second->_paIndex);
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

    void AudioService::InitPulseAudio()
    {
        _pulseReady = false;
        _paLoop = pa_glib_mainloop_new(g_main_context_default());

        if (!_paLoop)
            throw std::runtime_error("Failed to create PulseAudio loop");

        pa_mainloop_api* api = pa_glib_mainloop_get_api(_paLoop);
        _context = pa_context_new(api, "JappeOS-Session");

        if (!_context)
        {
            CleanupPulseAudio();
            throw std::runtime_error("Failed to create PulseAudio context");
        }

        pa_context_set_state_callback(
            _context,
            [](pa_context* ctx, void* userdata)
            {
                static_cast<AudioService*>(userdata)->HandleStateChange(ctx);
            },
            this
        );

        if (pa_context_connect(
                _context,
                nullptr,
                PA_CONTEXT_NOFLAGS,
                nullptr) < 0)
        {
            CleanupPulseAudio();
            throw std::runtime_error("Failed to connect to PulseAudio server");
        }
    }

    void AudioService::CleanupPulseAudio()
    {
        if (_cleaningUpPulse)
            return;
        _cleaningUpPulse = true;
        _pulseReady = false;

        if (_context)
        {
            pa_context_set_subscribe_callback(_context, nullptr, nullptr);
            pa_context_set_state_callback(_context, nullptr, nullptr);
            pa_context_disconnect(_context);
            pa_context_unref(_context);
            _context = nullptr;
        }

        if (_paLoop)
        {
            pa_glib_mainloop_free(_paLoop);
            _paLoop = nullptr;
        }

        _cleaningUpPulse = false;
    }

    void AudioService::HandleStateChange(const pa_context* ctx)
    {
        if (!ctx)
            return;

        switch (pa_context_get_state(ctx))
        {
            case PA_CONTEXT_READY:
                Log().Debug("PulseAudio connected");
                if (!_pulseReady)
                {
                    _pulseReady = true;
                    SyncInitialPulseState();
                }
                break;

            case PA_CONTEXT_FAILED:
                Log().Err("PulseAudio failed");
                CleanupPulseAudio();
                break;

            case PA_CONTEXT_TERMINATED:
                Log().Notice("PulseAudio terminated");
                CleanupPulseAudio();
                break;

            default:
                break;
        }
    }

    void AudioService::SyncInitialPulseState()
    {
        if (!_context)
            return;

        pa_context_set_subscribe_callback(
            _context,
            [](pa_context*, pa_subscription_event_type_t type, uint32_t index, void* userdata)
            {
                static_cast<AudioService*>(userdata)->HandleSubscribeCallback(type, index);
            },
            this
        );

        pa_operation* op = pa_context_subscribe(
            _context,
            static_cast<pa_subscription_mask_t>(
                PA_SUBSCRIPTION_MASK_SINK |
                PA_SUBSCRIPTION_MASK_SOURCE |
                PA_SUBSCRIPTION_MASK_SINK_INPUT |
                PA_SUBSCRIPTION_MASK_SERVER
            ),
            nullptr,
            nullptr
        );
        if (op)
            pa_operation_unref(op);

        op = pa_context_get_sink_info_list(
            _context,
            [](pa_context*, const pa_sink_info* info, int eol, void* userdata)
            {
                if (eol > 0 || !info)
                    return;

                static_cast<AudioService*>(userdata)->AddOrUpdateOutputDevice(info);
            },
            this
        );
        if (op)
            pa_operation_unref(op);

        op = pa_context_get_source_info_list(
            _context,
            [](pa_context*, const pa_source_info* info, int eol, void* userdata)
            {
                if (eol > 0 || !info)
                    return;

                static_cast<AudioService*>(userdata)->AddOrUpdateInputDevice(info);
            },
            this
        );
        if (op)
            pa_operation_unref(op);

        op = pa_context_get_sink_input_info_list(
            _context,
            [](pa_context*, const pa_sink_input_info* info, int eol, void* userdata)
            {
                if (eol > 0 || !info)
                    return;

                static_cast<AudioService*>(userdata)->AddOrUpdateStream(info);
            },
            this
        );
        if (op)
            pa_operation_unref(op);

        QueryServerInfo();
    }

    void AudioService::QueryServerInfo()
    {
        if (!_context)
            return;

        pa_operation* op = pa_context_get_server_info(
            _context,
            [](pa_context*, const pa_server_info* info, void* userdata)
            {
                static_cast<AudioService*>(userdata)->HandleServerInfo(info);
            },
            this
        );

        if (op)
            pa_operation_unref(op);
    }

    void AudioService::HandleServerInfo(const pa_server_info* info)
    {
        // Handle active input device update:

        const auto newSourceName = info->default_source_name
                ? info->default_source_name
                : "";

        if (newSourceName != _defaultSourceName)
        {
            _defaultSourceName = newSourceName;

            const auto it = std::ranges::find_if(
                _devices,
                [&](const auto& pair)
                {
                    const auto& device = pair.second;
                    const auto it = _inActivePortByPaIndex.find(device->_paIndex);
                    if (it == _inActivePortByPaIndex.end())
                        return false;
                    return device &&
                        device->GetName() == newSourceName &&
                        device->GetDirection() == AUDIO_DIRECTION_IN &&
                        device->_paPortName == it->second;
                }
            );

            HandleActiveInputDeviceChanged(it != _devices.end() ? it->first : ObjectPath{});
        }

        // Handle active output device update:

        const auto newSinkName = info->default_sink_name
                ? info->default_sink_name
                : "";

        if (newSinkName != _defaultSinkName)
        {
            _defaultSinkName = newSinkName;

            const auto it = std::ranges::find_if(
                _devices,
                [&](const auto& pair)
                {
                    const auto& device = pair.second;
                    const auto it = _outActivePortByPaIndex.find(device->_paIndex);
                    if (it == _outActivePortByPaIndex.end())
                        return false;
                    return device &&
                        device->GetName() == newSinkName &&
                        device->GetDirection() == AUDIO_DIRECTION_OUT &&
                        device->_paPortName == it->second;
                }
            );

            HandleActiveOutputDeviceChanged(it != _devices.end() ? it->first : ObjectPath{});
        }
    }

    void AudioService::HandleSubscribeCallback(const pa_subscription_event_type_t type, const uint32_t index)
    {
        const auto facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
        const auto event    = type & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

        switch (facility)
        {
            case PA_SUBSCRIPTION_EVENT_SINK:
                HandleOutputDeviceChanged(event, index);
                break;

            case PA_SUBSCRIPTION_EVENT_SOURCE:
                HandleInputDeviceChanged(event, index);
                break;

            case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
                HandleStreamChanged(event, index);
                break;

            case PA_SUBSCRIPTION_EVENT_SERVER:
                QueryServerInfo();
                break;

            default: break;
        }
    }

    void AudioService::AddOrUpdateOutputDevice(const pa_sink_info* info)
    {
        const auto index = info->index;
        const auto sinkName = info->name ? info->name : "";

        auto& vec = _outDevicesByPaIndex[index];
        auto stalePaths = vec;

        const auto activePort = info->active_port ? info->active_port : nullptr;
        const auto activePortName = activePort
                ? (activePort->name ? std::string(activePort->name) : std::string())
                : std::string();

        if (const auto existingPortName = _outActivePortByPaIndex[index]; existingPortName != activePortName)
            HandleActiveOutputPortChanged(index, activePortName);
        _outActivePortByPaIndex[index] = activePortName;

        PortIterator(info->ports ? info->n_ports : 0, [&](const std::optional<uint32_t> port)
        {
            std::string portName;
            const char* portDescription = nullptr;
            if (port != std::nullopt)
            {
                const pa_sink_port_info* paPort = info->ports[port.value()];
                if (!paPort || !paPort->name)
                    return;
                portName = paPort->name;
                portDescription = paPort->description;
            }

            auto path = GetObjectPathForDevice(index, AUDIO_DIRECTION_OUT_BOOL, portName);
            stalePaths.erase(path);
            vec.insert(path);

            const auto it = _devices.find(path);

            AudioDevice* device = nullptr;
            const bool exists = (it != _devices.end());

            if (exists)
            {
                device = it->second.get();
            }
            else
            {
                auto newDevice = std::make_unique<AudioDevice>(*this, *_conn, path);
                device = newDevice.get();
                _devices.emplace(path, std::move(newDevice));
            }

            const char* formFactor = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_FORM_FACTOR);

            device->_suppressPropertyCallbacks = true;

            device->_paIndex = index;
            device->_paChannels = info->channel_map.channels;
            device->_paDeviceName = sinkName;
            device->_paPortName = portName;
            device->_id = AUDIO_DIRECTION_OUT + std::to_string(index) + portName;
            device->_name = GetDeviceDisplayName(info->description, portDescription);
            device->_type = PaFormFactorToDeviceType(formFactor ? formFactor : "");
            device->_direction = AUDIO_DIRECTION_OUT;
            device->_volume = pa_cvolume_avg(&info->volume) / static_cast<float>(PA_VOLUME_NORM);
            device->_muted = info->mute;

            device->_suppressPropertyCallbacks = false;

            if (!exists)
                EmitDeviceAdded(path);

            if (portName == activePortName && _defaultSinkName == sinkName)
                HandleActiveOutputDeviceChanged(path);
        });

        for (const auto& path : stalePaths)
            RemoveOutputDevice(path);
    }

    void AudioService::RemoveOutputDevice(const ObjectPath& path)
    {
        const auto it = _devices.find(path);
        if (it == _devices.end())
            return;

        if (const auto it2 = _outDevicesByPaIndex.find(it->second->_paIndex); it2 != _outDevicesByPaIndex.end())
        {
            it2->second.erase(path);
            if (it2->second.empty())
            {
                _outDevicesByPaIndex.erase(it2);
                _outActivePortByPaIndex.erase(it->second->_paIndex);
            }
        }

        _devices.erase(path);
        EmitDeviceRemoved(path);
    }

    void AudioService::RemoveOutputDevicesByIndex(const uint32_t index)
    {
        const auto it = _outDevicesByPaIndex.find(index);
        if (it == _outDevicesByPaIndex.end())
            return;

        const std::vector<ObjectPath> paths{it->second.begin(), it->second.end()};
        for (const auto& path : paths)
            RemoveOutputDevice(path);
    }

    void AudioService::HandleOutputDeviceChanged(const int event, const uint32_t index)
    {
        switch (event)
        {
            case PA_SUBSCRIPTION_EVENT_NEW:
            case PA_SUBSCRIPTION_EVENT_CHANGE:
                QueryOutputDevice(index);
                break;

            case PA_SUBSCRIPTION_EVENT_REMOVE:
                RemoveOutputDevicesByIndex(index);
                break;

            default: break;
        }
    }

    bool AudioService::HandleActiveOutputDeviceChanged(const ObjectPath& path)
    {
        if (_activeOutputDevice == path)
            return false;

        _activeOutputDevice = path;
        Utils::DBusUtils::EmitPropertyChanged(*_conn, _iface, "ActiveOutputDevice", _activeOutputDevice);
        return true;
    }

    void AudioService::HandleActiveOutputPortChanged(const uint32_t sinkIndex, const std::string& portName)
    {
        for (const auto& val: _streams | std::views::values)
        {
            if (val->_paDeviceIndex != sinkIndex ||
                val->GetDirection() != AUDIO_DIRECTION_OUT)
                continue;
            val->_device = GetObjectPathForDevice(sinkIndex, AUDIO_DIRECTION_OUT_BOOL, portName);
        }
    }

    void AudioService::QueryOutputDevice(const uint32_t index)
    {
        if (!_context)
            return;

        pa_operation* op = pa_context_get_sink_info_by_index(
            _context,
            index,
            [](pa_context*, const pa_sink_info* info, int eol, void* userdata)
            {
                if (eol > 0 || !info)
                    return;

                static_cast<AudioService*>(userdata)->AddOrUpdateOutputDevice(info);
            },
            this
        );

        if (op) pa_operation_unref(op);
    }

    void AudioService::AddOrUpdateInputDevice(const pa_source_info* info)
    {
        const auto index = info->index;
        const auto sourceName = info->name ? info->name : "";

        auto& vec = _inDevicesByPaIndex[index];
        auto stalePaths = vec;

        const auto activePort = info->active_port ? info->active_port : nullptr;
        const auto activePortName = activePort
                ? (activePort->name ? std::string(activePort->name) : std::string())
                : std::string();

        _inActivePortByPaIndex[index] = activePortName;

        PortIterator(info->ports ? info->n_ports : 0, [&](const std::optional<uint32_t> port)
        {
            std::string portName;
            const char* portDescription = nullptr;
            if (port != std::nullopt)
            {
                const pa_source_port_info* paPort = info->ports[port.value()];
                if (!paPort || !paPort->name)
                    return;
                portName = paPort->name;
                portDescription = paPort->description;
            }

            auto path = GetObjectPathForDevice(index, AUDIO_DIRECTION_IN_BOOL, portName);
            stalePaths.erase(path);
            vec.insert(path);

            const auto it = _devices.find(path);

            AudioDevice* device = nullptr;
            const bool exists = (it != _devices.end());

            if (exists)
            {
                device = it->second.get();
            }
            else
            {
                auto newDevice = std::make_unique<AudioDevice>(*this, *_conn, path);
                device = newDevice.get();
                _devices.emplace(path, std::move(newDevice));
            }

            const char* formFactor = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_FORM_FACTOR);

            device->_suppressPropertyCallbacks = true;

            device->_paIndex = index;
            device->_paChannels = info->channel_map.channels;
            device->_paDeviceName = sourceName;
            device->_paPortName = portName;
            device->_id = AUDIO_DIRECTION_IN + std::to_string(index) + portName;
            device->_name = GetDeviceDisplayName(info->description, portDescription);
            device->_type = PaFormFactorToDeviceType(formFactor ? formFactor : "");
            device->_direction = AUDIO_DIRECTION_IN;
            device->_volume = pa_cvolume_avg(&info->volume) / static_cast<float>(PA_VOLUME_NORM);
            device->_muted = info->mute;

            device->_suppressPropertyCallbacks = false;

            if (!exists)
                EmitDeviceAdded(path);

            if (portName == activePortName && _defaultSourceName == sourceName)
                HandleActiveInputDeviceChanged(path);
        });

        for (const auto& path : stalePaths)
            RemoveInputDevice(path);
    }

    void AudioService::RemoveInputDevice(const ObjectPath& path)
    {
        const auto it = _devices.find(path);
        if (it == _devices.end())
            return;

        if (const auto it2 = _inDevicesByPaIndex.find(it->second->_paIndex); it2 != _inDevicesByPaIndex.end())
        {
            it2->second.erase(path);
            if (it2->second.empty())
            {
                _inDevicesByPaIndex.erase(it2);
                _inActivePortByPaIndex.erase(it->second->_paIndex);
            }
        }

        _devices.erase(path);
        EmitDeviceRemoved(path);
    }

    void AudioService::RemoveInputDevicesByIndex(const uint32_t index)
    {
        const auto it = _inDevicesByPaIndex.find(index);
        if (it == _inDevicesByPaIndex.end())
            return;

        const std::vector<ObjectPath> paths{it->second.begin(), it->second.end()};
        for (const auto& path : paths)
            RemoveInputDevice(path);
    }

    void AudioService::HandleInputDeviceChanged(const int event, const uint32_t index)
    {
        switch (event)
        {
            case PA_SUBSCRIPTION_EVENT_NEW:
            case PA_SUBSCRIPTION_EVENT_CHANGE:
                QueryInputDevice(index);
                break;

            case PA_SUBSCRIPTION_EVENT_REMOVE:
                RemoveInputDevicesByIndex(index);
                break;

            default: break;
        }
    }

    bool AudioService::HandleActiveInputDeviceChanged(const ObjectPath& path)
    {
        if (_activeInputDevice == path)
            return false;

        _activeInputDevice = path;
        Utils::DBusUtils::EmitPropertyChanged(*_conn, _iface, "ActiveInputDevice", _activeInputDevice);
        return true;
    }

    void AudioService::QueryInputDevice(const uint32_t index)
    {
        if (!_context)
            return;

        pa_operation* op = pa_context_get_source_info_by_index(
            _context,
            index,
            [](pa_context*, const pa_source_info* info, int eol, void* userdata)
            {
                if (eol > 0 || !info)
                    return;

                static_cast<AudioService*>(userdata)->AddOrUpdateInputDevice(info);
            },
            this
        );

        if (op) pa_operation_unref(op);
    }

    void AudioService::AddOrUpdateStream(const pa_sink_input_info* info)
    {
        const auto index = info->index;
        auto path = GetObjectPathForStream(index);

        const auto it = _streams.find(path);

        AudioStream* stream = nullptr;
        const bool exists = (it != _streams.end());

        if (exists)
        {
            stream = it->second.get();
        }
        else
        {
            auto newStream = std::make_unique<AudioStream>(*this, *_conn, path);
            stream = newStream.get();
            _streams.emplace(path, std::move(newStream));
        }

        const char* appName = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);

        stream->_suppressPropertyCallbacks = true;

        stream->_paIndex = index;
        stream->_paChannels = info->channel_map.channels;
        stream->_paDeviceIndex = info->sink;
        stream->_id = std::to_string(index);
        stream->_name = info->name ? info->name : "";
        stream->_applicationName = appName ? appName : "";
        stream->_direction = AUDIO_DIRECTION_OUT;

        // Update explicitly when the port changes, update will happen automatically when the actual PA device changes.
        if (const auto it2 = _outActivePortByPaIndex.find(info->sink); it2 != _outActivePortByPaIndex.end())
            stream->_device = GetObjectPathForDevice(info->sink, AUDIO_DIRECTION_OUT_BOOL, it2->second);

        stream->_volume = pa_cvolume_avg(&info->volume) / static_cast<float>(PA_VOLUME_NORM);
        stream->_muted = info->mute;

        stream->_suppressPropertyCallbacks = false;

        if (!exists)
            EmitStreamAdded(path);
    }

    void AudioService::RemoveStream(const uint32_t index)
    {
        const auto path = GetObjectPathForStream(index);
        if (_streams.erase(path))
            EmitStreamRemoved(path);
    }

    void AudioService::HandleStreamChanged(const int event, const uint32_t index)
    {
        switch (event)
        {
            case PA_SUBSCRIPTION_EVENT_NEW:
            case PA_SUBSCRIPTION_EVENT_CHANGE:
                QueryStream(index);
                break;

            case PA_SUBSCRIPTION_EVENT_REMOVE:
                RemoveStream(index);
                break;

            default: break;
        }
    }

    void AudioService::QueryStream(const uint32_t index)
    {
        if (!_context)
            return;

        pa_operation* op = pa_context_get_sink_input_info(
            _context,
            index,
            [](pa_context*, const pa_sink_input_info* info, int eol, void* userdata)
            {
                if (eol > 0 || !info)
                    return;

                static_cast<AudioService*>(userdata)->AddOrUpdateStream(info);
            },
            this
        );

        if (op) pa_operation_unref(op);
    }

    void AudioService::MoveStreamToDevice(const uint32_t streamIndex, const uint32_t sinkIndex) const
    {
        if (!_context)
            return;

        pa_operation* op = pa_context_move_sink_input_by_index(
            _context,
            streamIndex,
            sinkIndex,
            nullptr,
            nullptr
        );

        if (op)
            pa_operation_unref(op);
    }

    void AudioService::ActivateInputPortAndMakeDefault(const uint32_t sourceIndex, std::string sourceName, const std::string& portName)
    {
        if (!_context)
            return;

        struct Request
        {
            AudioService* self;
            std::string sourceName;
        };

        auto* request = new Request{
            .self = this,
            .sourceName = std::move(sourceName)
        };

        pa_operation* op = pa_context_set_source_port_by_index(
            _context,
            sourceIndex,
            portName.c_str(),
            [](pa_context*, const int success, void* userdata)
            {
                const std::unique_ptr<Request> req(static_cast<Request*>(userdata));

                if (!success)
                    return;

                req->self->SetDefaultSource(req->sourceName);
            },
            request
        );

        if (op)
            pa_operation_unref(op);
    }

    void AudioService::SetDefaultSource(const std::string& sourceName) const
    {
        if (!_context)
            return;

        pa_operation* op = pa_context_set_default_source(
            _context,
            sourceName.c_str(),
            nullptr,
            nullptr
        );

        if (op)
            pa_operation_unref(op);
    }

    void AudioService::SetSourcePort(const uint32_t sourceIndex, const std::string& portName) const
    {
        if (!_context)
            return;

        pa_operation* op = pa_context_set_source_port_by_index(
            _context,
            sourceIndex,
            portName.c_str(),
            nullptr,
            nullptr
        );

        if (op)
            pa_operation_unref(op);
    }

    void AudioService::ActivateOutputPortAndMakeDefault(const uint32_t sinkIndex,
                                                        std::string sinkName,
                                                        const std::string& portName)
    {
        if (!_context)
            return;

        struct Request
        {
            AudioService* self;
            std::string sinkName;
        };

        auto* request = new Request{
            .self = this,
            .sinkName = std::move(sinkName)
        };

        pa_operation* op = pa_context_set_sink_port_by_index(
            _context,
            sinkIndex,
            portName.c_str(),
            [](pa_context*, const int success, void* userdata)
            {
                const std::unique_ptr<Request> req(static_cast<Request*>(userdata));

                if (!success)
                    return;

                req->self->SetDefaultSink(req->sinkName);
            },
            request
        );

        if (op)
            pa_operation_unref(op);
    }

    void AudioService::SetDefaultSink(const std::string& sinkName) const
    {
        if (!_context)
            return;

        pa_operation* op = pa_context_set_default_sink(
            _context,
            sinkName.c_str(),
            nullptr,
            nullptr
        );

        if (op)
            pa_operation_unref(op);
    }

    void AudioService::SetSinkPort(const uint32_t sinkIndex, const std::string& portName) const
    {
        if (!_context)
            return;

        pa_operation* op = pa_context_set_sink_port_by_index(
            _context,
            sinkIndex,
            portName.c_str(),
            nullptr,
            nullptr
        );

        if (op)
            pa_operation_unref(op);
    }

    ObjectPath AudioService::GetObjectPathForDevice(const uint32_t deviceId,
                                                    const bool direction,
                                                    const std::string& portName)
    {
        return GetBaseObjectPath()
            .Child("Devices")
            .Child(
                std::string(direction ? "out" : "in") +
                "_" +
                std::to_string(deviceId) +
                "_p" +
                EncodeForObjectPath(portName)
            );
    }

    ObjectPath AudioService::GetObjectPathForStream(const uint32_t deviceId)
    {
        return GetBaseObjectPath().Child("Streams").Child(std::to_string(deviceId));
    }

    void AudioService::PortIterator(const uint32_t portCount, const std::function<void(std::optional<uint32_t> i)>& iterator)
    {
        if (!portCount)
        {
            iterator(std::nullopt);
            return;
        }

        for (uint32_t i = 0; i < portCount; i++)
        {
            iterator(i);
        }
    }

    std::string AudioService::PaFormFactorToDeviceType(const std::string& formFactor)
    {
        if (formFactor == "internal")   return AUDIO_DEVICE_TYPE_INTERNAL;
        if (formFactor == "speaker")    return AUDIO_DEVICE_TYPE_SPEAKER;
        if (formFactor == "handset")    return AUDIO_DEVICE_TYPE_HANDSET;
        if (formFactor == "tv")         return AUDIO_DEVICE_TYPE_TV;
        if (formFactor == "webcam")     return AUDIO_DEVICE_TYPE_WEBCAM;
        if (formFactor == "microphone") return AUDIO_DEVICE_TYPE_MICROPHONE;
        if (formFactor == "headset")    return AUDIO_DEVICE_TYPE_HEADSET;
        if (formFactor == "headphone")  return AUDIO_DEVICE_TYPE_HEADPHONE;
        if (formFactor == "hands-free") return AUDIO_DEVICE_TYPE_HANDSFREE;
        if (formFactor == "car")        return AUDIO_DEVICE_TYPE_CAR;
        if (formFactor == "hifi")       return AUDIO_DEVICE_TYPE_HIFI;
        if (formFactor == "computer")   return AUDIO_DEVICE_TYPE_COMPUTER;
        if (formFactor == "portable")   return AUDIO_DEVICE_TYPE_PORTABLE;
        return AUDIO_DEVICE_TYPE_UNKNOWN;
    }

    std::string AudioService::GetDeviceDisplayName(const char* paDeviceDescription, const char* paPortDescription)
    {
        if (!paDeviceDescription && !paPortDescription)
            return "Unknown";

        std::string portName = paPortDescription ? paPortDescription : "Unknown";
        std::string deviceName = paDeviceDescription ? paDeviceDescription : "Unknown";

        if (!paDeviceDescription)
            return portName;

        if (!paPortDescription)
            return deviceName;

        return portName + " – " + deviceName;
    }

    std::string AudioService::EncodeForObjectPath(const std::string_view value)
    {
        constexpr char hex[] = "0123456789ABCDEF";

        std::string out;
        out.reserve(value.size() * 2);
        for (const unsigned char c : value)
        {
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
        return out;
    }

}
