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
#include <libudev.h>
#include <parted/parted.h>
#include <sys/stat.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>

#include "install_data.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Installer
{
    class InstallStorageDataBuilder final
    {
    public:
        InstallStorageDataBuilder() = delete;

        /**
         * @brief Populate `storageDevices` with all physical block devices visible to udev.
         * @param storageDevices Map to populate
         * @throws std::runtime_error on unrecoverable udev failure.
         */
        static void PopulateStorageDevices(std::map<std::string, StorageDeviceData>& storageDevices);

    private:
        /**
         * @brief Build a StorageDeviceData (with partitions) for one block device path, e.g. "/dev/sda".
         * @param out Populated device
         * @param devPath Device path
         * @return false if libparted cannot open the device
         */
        static bool PopulateDeviceWithParted(StorageDeviceData& out, const std::string& devPath);

        /**
         * @brief Fill in mountpoints for all partitions of a device by reading /proc/mounts.
         * @param device Device to apply mountpoints to
         */
        static void ApplyMountpoints(StorageDeviceData& device);

        /**
         * @return true if any partition of this device is mounted at '/'
         */
        static bool IsRootDevice(const StorageDeviceData& device);

        /**
         * @brief Map a libparted filesystem name to one of our STORAGE_FILESYSTEM_* defines.
         * @param fsType Libparted filesystem type
         * @return A name from STORAGE_FILESYSTEM_* defines
         */
        static std::string MapFilesystem(const PedFileSystemType* fsType);

        /**
         * @brief Safely read an udev attribute.
         * @param dev Udev device
         * @param attr Udev attribute
         * @return Value of the attribute, or "" on failure
         */
        static std::string UdevAttr(udev_device* dev, const char* attr);

        /**
         * @brief Safely read an udev property.
         * @param dev Udev device
         * @param prop Udev property
         * @return Value of the property, or "" on failure
         */
        static std::string UdevProp(udev_device* dev, const char* prop);

        /**
         * @brief Convert a byte count (from libparted) to whole MiB, rounding down.
         * @param bytes Libparted byte count
         * @return Size in MiB, rounded down
         */
        static uint64_t BytesToMiB(long long bytes);
    };
}
