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

#include "install_storage_data_builder.h"

#include <cctype>
#include <cstdlib>
#include <fstream>

#include "installer_def.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Installer
{

    void InstallStorageDataBuilder::PopulateStorageDevices(std::map<std::string, StorageDeviceData>& storageDevices)
    {
        storageDevices.clear();

        // --- udev context & enumerator setup -------------------------------------
        udev* udevCtx = udev_new();
        if (!udevCtx)
            throw std::runtime_error("Failed to create udev context");

        udev_enumerate* enumerator = udev_enumerate_new(udevCtx);
        if (!enumerator)
        {
            udev_unref(udevCtx);
            throw std::runtime_error("Failed to create udev enumerator");
        }

        udev_enumerate_add_match_subsystem(enumerator, "block");
        udev_enumerate_add_match_property(enumerator, "DEVTYPE", "disk");
        udev_enumerate_scan_devices(enumerator);

        udev_list_entry* entry;
        udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerator))
        {
            const char* syspath = udev_list_entry_get_name(entry);
            if (!syspath)
                continue;

            udev_device* dev = udev_device_new_from_syspath(udevCtx, syspath);
            if (!dev)
                continue;

            // --- Basic sanity checks ---------------------------------------------
            const char* devnode = udev_device_get_devnode(dev);
            if (!devnode)
            {
                udev_device_unref(dev);
                continue;
            }

            // Skip loop devices, device-mapper, RAM disks, etc.
            std::string devType = UdevProp(dev, "DEVTYPE");
            std::string subsys  = udev_device_get_subsystem(dev)
                                ? udev_device_get_subsystem(dev) : "";
            if (devType != "disk" || subsys != "block")
            {
                udev_device_unref(dev);
                continue;
            }

            // Filter by kernel name: keep only sd*, nvme*, mmcblk*, vd*
            const char* sysname = udev_device_get_sysname(dev);
            if (!sysname)
            {
                udev_device_unref(dev);
                continue;
            }

            std::string kname = sysname;
            const bool isPhysical =
                (kname.rfind("sd",     0) == 0) ||
                (kname.rfind("nvme",   0) == 0) ||
                (kname.rfind("mmcblk", 0) == 0) ||
                (kname.rfind("vd",     0) == 0);
            if (!isPhysical)
            {
                udev_device_unref(dev);
                continue;
            }

            // --- Fill StorageDeviceData from udev --------------------------------
            StorageDeviceData data;
            data.device      = devnode;
            data.model       = UdevAttr(dev, "device/model");
            data.isRemovable = (UdevAttr(dev, "removable") == "1");
            data.sizeMiB     = 0;  // set by libparted below

            // Trim trailing whitespace that some firmware leaves in model strings.
            while (!data.model.empty() && std::isspace(static_cast<unsigned char>(data.model.back())))
                data.model.pop_back();

            udev_device_unref(dev);

            // --- Fill sizes & partitions from libparted --------------------------
            if (!PopulateDeviceWithParted(data, data.device))
                continue;

            ApplyMountpoints(data);

            if (IsRootDevice(data))
                continue;

            storageDevices.emplace(data.device, std::move(data));
        }

        udev_enumerate_unref(enumerator);
        udev_unref(udevCtx);
    }

    bool InstallStorageDataBuilder::PopulateDeviceWithParted(StorageDeviceData& out, const std::string& devPath)
    {
        PedDevice* pedDev = ped_device_get(devPath.c_str());
        if (!pedDev)
            return false;

        // Disk size: sector_size * length -> bytes -> MiB
        out.sizeMiB = BytesToMiB(static_cast<long long>(pedDev->sector_size) * pedDev->length);

        PedDisk* pedDisk = ped_disk_new(pedDev);
        if (!pedDisk)
        {
            // No partition table: treat the whole disk as one free-space region.
            if (out.sizeMiB > 0)
            {
                StoragePartitionData wholeDisk;
                wholeDisk.device     = FreeSpaceRegionId(devPath, 1);
                wholeDisk.filesystem = "";
                wholeDisk.sizeMiB    = out.sizeMiB;
                wholeDisk.mountpoint = "";
                out.partitions.push_back(std::move(wholeDisk));
            }
            ped_device_destroy(pedDev);
            return true;
        }

        size_t freeSpaceIndex = 0;
        for (PedPartition* part = ped_disk_next_partition(pedDisk, nullptr);
             part != nullptr;
             part = ped_disk_next_partition(pedDisk, part))
        {
            // Skip extended container partitions and metadata; keep normal + free.
            if (part->type & PED_PARTITION_EXTENDED) continue;
            if (part->type & PED_PARTITION_METADATA) continue;

            StoragePartitionData pd;
            pd.sizeMiB    = BytesToMiB(
                static_cast<long long>(pedDev->sector_size) * part->geom.length);

            if (part->type & PED_PARTITION_FREESPACE)
            {
                // Free space
                pd.device     = FreeSpaceRegionId(devPath, ++freeSpaceIndex);
                pd.filesystem = "";
                if (pd.sizeMiB == 0)
                    continue;
            }
            else
            {
                // Real partition
                char* partPath = ped_partition_get_path(part);
                pd.device = partPath ? partPath : "";
                free(partPath);

                pd.filesystem = MapFilesystem(ped_file_system_probe(&part->geom));
            }

            pd.mountpoint = "";   // filled separately if needed

            out.partitions.push_back(std::move(pd));
        }

        ped_disk_destroy(pedDisk);
        ped_device_destroy(pedDev);
        return true;
    }

    void InstallStorageDataBuilder::ApplyMountpoints(StorageDeviceData& device)
    {
        std::ifstream mounts("/proc/mounts");
        if (!mounts.is_open())
            return;

        std::map<std::string, std::string> mountMap;
        std::string devPath, mountpoint, rest;
        while (mounts >> devPath >> mountpoint)
        {
            std::getline(mounts, rest);
            mountMap[devPath] = mountpoint;
        }

        for (auto& partition : device.partitions)
        {
            if (auto it = mountMap.find(partition.device); it != mountMap.end())
                partition.mountpoint = it->second;
        }
    }

    bool InstallStorageDataBuilder::IsRootDevice(const StorageDeviceData& device)
    {
        for (const auto& partition : device.partitions)
            if (partition.mountpoint == "/")
                return true;
        return false;
    }

    std::string InstallStorageDataBuilder::MapFilesystem(const PedFileSystemType* fsType)
    {
        if (!fsType || !fsType->name)
            return STORAGE_FILESYSTEM_UNKNOWN;

        const std::string name = fsType->name;

        // libparted names are lower-case
        if (name == "fat32" || name == "fat16")  return STORAGE_FILESYSTEM_FAT32;
        if (name == "ext4"  || name == "ext3"
                            || name == "ext2")   return STORAGE_FILESYSTEM_EXT4;
        if (name == "btrfs")                     return STORAGE_FILESYSTEM_BTRFS;
        if (name == "xfs")                       return STORAGE_FILESYSTEM_XFS;

        return STORAGE_FILESYSTEM_UNKNOWN;
    }

    std::string InstallStorageDataBuilder::UdevAttr(udev_device* dev, const char* attr)
    {
        const char* v = udev_device_get_sysattr_value(dev, attr);
        return v ? v : "";
    }

    std::string InstallStorageDataBuilder::UdevProp(udev_device* dev, const char* prop)
    {
        const char* v = udev_device_get_property_value(dev, prop);
        return v ? v : "";
    }

    std::string InstallStorageDataBuilder::FreeSpaceRegionId(const std::string& devPath, const size_t index)
    {
        return devPath + "#free-" + std::to_string(index);
    }

    uint64_t InstallStorageDataBuilder::BytesToMiB(const long long bytes)
    {
        return static_cast<uint64_t>(bytes) / (1024ULL * 1024ULL);
    }

}
