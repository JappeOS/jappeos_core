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

#include "audio_stream.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Audio
{

    AudioStream::AudioStream(AudioService& source,
                             Connection& connection,
                             ObjectPath objectPath) :
                             _source(source),
                             _conn(connection),
                             _path(std::move(objectPath)),
                             _object(_conn, _path),
                             _interfaceName(source.GetBaseInterface().Child("Stream")),
                             _iface(_object.CreateInterface(_interfaceName)),
                             _id             (_conn, _iface, "Id", ""),
                             _name           (_conn, _iface, "Name", ""),
                             _applicationName(_conn, _iface, "ApplicationName", ""),
                             _direction      (_conn, _iface, "Direction", ""),
                             _device         (_conn, _iface, "Device", ObjectPath{}, true, [this](const ObjectPath& val) { OnSetDevice(val); }),
                             _volume         (_conn, _iface, "Volume", 1.0, true, [this](const double& val) { OnSetVolume(val); }),
                             _muted          (_conn, _iface, "Muted", false, true, [this](const bool& val) { OnSetMuted(val); })
    {
    }

    void AudioStream::OnSetDevice(const ObjectPath& devicePath)
    {
        if (_suppressPropertyCallbacks)
            return;
        _source.SetStreamDevice(*this, devicePath);
    }

    void AudioStream::OnSetVolume(const double volume)
    {
        if (_suppressPropertyCallbacks)
            return;
        _source.SetStreamVolume(*this, volume);
    }

    void AudioStream::OnSetMuted(const bool muted)
    {
        if (_suppressPropertyCallbacks)
            return;
        _source.SetStreamMuted(*this, muted);
    }

}
