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

#pragma once
#include "../service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::PowerManager
{
    class PowerDevice;

    class PowerManagerService : public Service
    {
    public:
        explicit PowerManagerService(ServiceManager* serviceManager, Connection* conn);
        ~PowerManagerService() override;
        [[nodiscard]] std::string GetName() const override { return "PowerManagerService"; }

        [[nodiscard]] std::vector<ObjectPath> ListDevices() const;

    private:
        Object _object;
        Interface& _iface;
        bool _uPower = false;
        std::unordered_map<ObjectPath, std::unique_ptr<PowerDevice>, ObjectPathHash> _devices;

        std::unique_ptr<SignalSubscription> _subNameOwnerChanged;

        std::unique_ptr<SignalSubscription> _subDeviceAdded;
        std::unique_ptr<SignalSubscription> _subDeviceRemoved;

        void SharedPolicy(const Message& message) const;

        void OnShutdown(const Message& message) const;
        void OnReboot(const Message& message) const;
        void OnSuspend(const Message& message) const;
        void OnListBatteryDevices(const Message& message) const;
        void EmitBatteryDeviceAdded(const ObjectPath& path);
        void EmitBatteryDeviceRemoved(const ObjectPath& path);

        void OnNameOwnerChanged(const Message& msg);
        void OnPowerManagerAppeared();
        void OnPowerManagerDisappeared();
        void DiscoverDevices();
        void SubscribeToPowerManagerSignals();
        void UnsubscribeFromPowerManagerSignals();

        void AddDevice(const ObjectPath& upowerPath);
        void RemoveDevice(const ObjectPath& upowerPath);
    };
}
