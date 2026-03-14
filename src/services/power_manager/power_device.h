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
#include "power_manager_service.h"
#include "../../utils/dbus_utils.h"
#include "../../logger.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::PowerManager
{
#define JOSPM_PROP_ID                "Id"
#define JOSPM_PROP_TYPE              "Type"
#define JOSPM_PROP_POWER_SUPPLY      "PowerSupply"
#define JOSPM_PROP_IS_PRESENT        "IsPresent"
#define JOSPM_PROP_STATE             "State"
#define JOSPM_PROP_CHARGE_PERCENTAGE "ChargePercentage"
#define JOSPM_PROP_TIME_TO_EMPTY     "TimeToEmpty"
#define JOSPM_PROP_TIME_TO_FULL      "TimeToFull"

#define UPOWER_PROP_SERIAL          "Serial"
#define UPOWER_PROP_TYPE            "Type"
#define UPOWER_PROP_POWER_SUPPLY    "PowerSupply"
#define UPOWER_PROP_IS_PRESENT      "IsPresent"
#define UPOWER_PROP_STATE           "State"
#define UPOWER_PROP_PERCENTAGE      "Percentage"
#define UPOWER_PROP_TIME_TO_EMPTY   "TimeToEmpty"
#define UPOWER_PROP_TIME_TO_FULL    "TimeToFull"

    class PowerDevice
    {
        friend class PowerManagerService;

    public:
        PowerDevice(PowerManagerService& source,
                    Connection& connection,
                    ObjectPath objectPath,
                    ObjectPath upowerPath);

    protected:
        Connection& _conn;
        ObjectPath  _path;
        Object      _object;

    private:
        InterfaceName _interfaceName;
        Interface&    _iface;
        Proxy         _proxy;
        std::unique_ptr<SignalSubscription> _subIsPresent;
        std::unique_ptr<SignalSubscription> _subState;
        std::unique_ptr<SignalSubscription> _subChargePercentage;
        std::unique_ptr<SignalSubscription> _subTimeToEmpty;
        std::unique_ptr<SignalSubscription> _subTimeToFull;

        template<typename T>
        void EmitPropertyChanged(const std::string& property, const T& value)
        {
            Log().Debug("EmitPropertyChanged on " + property);
            Utils::DBusUtils::EmitPropertyChanged(_conn, _iface, property, value);
        }

    private:
        static JappeOSCore::Logger& Log()
        {
            static JappeOSCore::Logger instance{"PowerDevice"};
            return instance;
        }

        static std::string DeviceTypeToString(const uint32_t value)
        {
            switch (value)
            {
                case 0:  return "unknown";
                case 1:  return "linePower";
                case 2:  return "battery";
                case 3:  return "ups";
                case 4:  return "monitor";
                case 5:  return "mouse";
                case 6:  return "keyboard";
                case 7:  return "pda";
                case 8:  return "phone";
                case 9:  return "mediaPlayer";
                case 10: return "tablet";
                case 11: return "computer";
                case 12: return "gamingInput";
                case 13: return "pen";
                case 14: return "touchpad";
                case 15: return "modem";
                case 16: return "network";
                case 17: return "headset";
                case 18: return "speakers";
                case 19: return "headphones";
                case 20: return "video";
                case 21: return "otherAudio";
                case 22: return "remoteControl";
                case 23: return "printer";
                case 24: return "scanner";
                case 25: return "camera";
                case 26: return "wearable";
                case 27: return "toy";
                case 28: return "bluetoothGeneric";
                default: return "unknown";
            }
        }

        static std::string DeviceStateToString(const uint32_t value)
        {
            switch (value)
            {
                case 0: return "unknown";
                case 1: return "charging";
                case 2: return "discharging";
                case 3: return "empty";
                case 4: return "fullyCharged";
                case 5: return "pendingCharge";
                case 6: return "pendingDischarge";
                default: return "unknown";
            }
        }
    };
}
