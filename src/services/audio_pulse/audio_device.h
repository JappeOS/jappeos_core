#pragma once

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

#include <cstdint>
#include <limits>

namespace JappeStudios::JappeOS::JappeOSCore::Services::AudioPulse
{
    class AudioDevice
    {
        friend class AudioService;

    public:
        AudioDevice(AudioService& source,
                    Connection& connection,
                    ObjectPath objectPath);

        [[nodiscard]] std::string GetId() const        { return _id.Get(); }
        [[nodiscard]] std::string GetName() const      { return _name.Get(); }
        [[nodiscard]] std::string GetType() const      { return _type.Get(); }
        [[nodiscard]] std::string GetDirection() const { return _direction.Get(); }
        [[nodiscard]] double GetVolume() const         { return _volume.Get(); }
        [[nodiscard]] bool GetMuted() const            { return _muted.Get(); }
        [[nodiscard]] bool GetAvailable() const        { return _available.Get(); }

    private:
        void OnSetVolume(double volume) const;
        void OnSetMuted(bool muted) const;

        AudioService& _source;
        Connection&   _conn;
        ObjectPath    _path;
        Object        _object;
        InterfaceName _interfaceName;
        Interface&    _iface;

        Prop<std::string> _id;
        Prop<std::string> _name;
        Prop<std::string> _type;
        Prop<std::string> _direction;
        Prop<double>      _volume;
        Prop<bool>        _muted;
        Prop<bool>        _available;

        bool _suppressPropertyCallbacks = false;
        uint32_t _paIndex = std::numeric_limits<uint32_t>::max();
        uint32_t _paChannels = 0;
        std::string _paPortName{};
    };
}
