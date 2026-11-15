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

#include "audio_device.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Audio
{

    AudioDevice::AudioDevice(AudioService& source,
                             Connection& connection,
                             ObjectPath objectPath) :
                             _source(source),
                             _conn(connection),
                             _path(std::move(objectPath)),
                             _object(_conn, _path),
                             _interfaceName(source.GetBaseInterface().Child("Device")),
                             _iface(_object.CreateInterface(_interfaceName)),
                             _id       (_conn, _iface, "Id", ""),
                             _name     (_conn, _iface, "Name", ""),
                             _type     (_conn, _iface, "Type", ""),
                             _direction(_conn, _iface, "Direction", ""),
                             _volume   (_conn, _iface, "Volume", 1.0, true, [this](const double& val) { OnSetVolume(val); }),
                             _muted    (_conn, _iface, "Muted", false, true, [this](const bool& val) { OnSetMuted(val); }),
                             _available(_conn, _iface, "Available", false)
    {
    }

    void AudioDevice::OnSetVolume(const double volume)
    {
        if (_suppressPropertyCallbacks)
            return;
        _source.SetDeviceVolume(*this, volume);
    }

    void AudioDevice::OnSetMuted(const bool muted)
    {
        if (_suppressPropertyCallbacks)
            return;
        _source.SetDeviceMuted(*this, muted);
    }

}
