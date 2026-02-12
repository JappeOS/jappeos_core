/*
 * jappeos_core, Core system management daemon for JappeOS.
 * Copyright (C) 2026  Jappe02
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

#include "power_device.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::PowerManager
{

    PowerDevice::PowerDevice(PowerManagerService& source,
                             Connection& connection,
                             ObjectPath objectPath,
                             ObjectPath upowerPath) :
                             _conn(connection),
                             _path(std::move(objectPath)),
                             _object(_conn, _path),
                             _interfaceName(source.GetBaseInterface().Child("BatteryDevice")),
                             _iface(_object.CreateInterface(_interfaceName)),
                             _proxy(_conn,
                                "org.freedesktop.UPower",
                                std::move(upowerPath),
                                InterfaceName("org.freedesktop.UPower.Device"))/*,
                             _id              (_conn, _iface, "Id", ""),
                             _type            (_conn, _iface, "Type", ""),
                             _isCharging      (_conn, _iface, "IsCharging", false),
                             _chargePercentage(_conn, _iface, "ChargePercentage", 0),
                             _timeToEmpty     (_conn, _iface, "TimeToEmpty", 0),
                             _timeToFull      (_conn, _iface, "TimeToFull", 0)*/
    {
        //_id = _proxy.GetProperty<std::string>("Serial");

        // Id

        _iface.RegisterProperty<std::string>(
            JOSPM_PROP_ID,
            [&] { return _proxy.GetProperty<std::string>(UPOWER_PROP_SERIAL); }
        );

        // Type

        _iface.RegisterProperty<std::string>(
            JOSPM_PROP_TYPE,
            [&] { return DeviceTypeToString(_proxy.GetProperty<uint32_t>(UPOWER_PROP_TYPE)); }
        );

        // PowerSupply

        _iface.RegisterProperty<bool>(
            JOSPM_PROP_POWER_SUPPLY,
            [&] { return _proxy.GetProperty<bool>(UPOWER_PROP_POWER_SUPPLY); }
        );

        // IsPresent

        _iface.RegisterProperty<bool>(
            JOSPM_PROP_IS_PRESENT,
            [&] { return _proxy.GetProperty<bool>(UPOWER_PROP_IS_PRESENT); }
        );

        _subIsPresent = std::make_unique<SignalSubscription>(_proxy.SubscribePropertyChanged<bool>(
            UPOWER_PROP_IS_PRESENT,
            [&](const bool& val) { EmitPropertyChanged(JOSPM_PROP_IS_PRESENT, val); }
        ));

        // State

        _iface.RegisterProperty<std::string>(
            JOSPM_PROP_STATE,
            [&] { return DeviceStateToString(_proxy.GetProperty<uint32_t>(UPOWER_PROP_STATE)); }
        );

        _subState = std::make_unique<SignalSubscription>(_proxy.SubscribePropertyChanged<uint32_t>(
            UPOWER_PROP_STATE,
            [&](const uint32_t& val) { EmitPropertyChanged(JOSPM_PROP_STATE, DeviceStateToString(val)); }
        ));

        // ChargePercentage

        _iface.RegisterProperty<double>(
            JOSPM_PROP_CHARGE_PERCENTAGE,
            [&] { return _proxy.GetProperty<double>(UPOWER_PROP_PERCENTAGE); }
        );

        _subChargePercentage = std::make_unique<SignalSubscription>(_proxy.SubscribePropertyChanged<double>(
            UPOWER_PROP_PERCENTAGE,
            [&](const double& val) { EmitPropertyChanged(JOSPM_PROP_CHARGE_PERCENTAGE, val); }
        ));

        // TimeToEmpty

        _iface.RegisterProperty<int64_t>(
            JOSPM_PROP_TIME_TO_EMPTY,
            [&] { return _proxy.GetProperty<int64_t>(UPOWER_PROP_TIME_TO_EMPTY); }
        );

        _subTimeToEmpty = std::make_unique<SignalSubscription>(_proxy.SubscribePropertyChanged<int64_t>(
            UPOWER_PROP_TIME_TO_EMPTY,
            [&](const int64_t& val) { EmitPropertyChanged(JOSPM_PROP_TIME_TO_EMPTY, val); }
        ));

        // TimeToFull

        _iface.RegisterProperty<int64_t>(
            JOSPM_PROP_TIME_TO_FULL,
            [&] { return _proxy.GetProperty<int64_t>(UPOWER_PROP_TIME_TO_FULL); }
        );

        _subTimeToFull = std::make_unique<SignalSubscription>(_proxy.SubscribePropertyChanged<int64_t>(
            UPOWER_PROP_TIME_TO_FULL,
            [&](const int64_t& val) { EmitPropertyChanged(JOSPM_PROP_TIME_TO_FULL, val); }
        ));
    }

}