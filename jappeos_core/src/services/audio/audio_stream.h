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
#include "audio_service.h"

#include <limits>

namespace JappeStudios::JappeOS::JappeOSCore::Services::Audio
{
    class AudioStream
    {
        friend class AudioService;

    public:
        AudioStream(AudioService& source,
                    Connection& connection,
                    ObjectPath objectPath);

        [[nodiscard]] std::string GetId() const              { return _id.Get(); }
        [[nodiscard]] std::string GetName() const            { return _name.Get(); }
        [[nodiscard]] std::string GetApplicationName() const { return _applicationName.Get(); }
        [[nodiscard]] std::string GetDirection() const       { return _direction.Get(); }
        [[nodiscard]] ObjectPath GetDevice() const           { return _device.Get(); }
        [[nodiscard]] double GetVolume() const               { return _volume.Get(); }
        [[nodiscard]] bool GetMuted() const                  { return _muted.Get(); }

    private:
        void OnSetDevice(const ObjectPath& devicePath);
        void OnSetVolume(double volume);
        void OnSetMuted(bool muted);

        AudioService& _source;
        Connection&   _conn;
        ObjectPath    _path;
        Object        _object;
        InterfaceName _interfaceName;
        Interface&    _iface;

        Prop<std::string> _id;
        Prop<std::string> _name;
        Prop<std::string> _applicationName;
        Prop<std::string> _direction;
        Prop<ObjectPath>  _device;
        Prop<double>      _volume;
        Prop<bool>        _muted;

        uint32_t _pwNodeId = std::numeric_limits<uint32_t>::max();
        std::string _pwNodeName;
        bool _suppressPropertyCallbacks = false;
    };
}
