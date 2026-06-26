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

#include "partition_step.h"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <libudev.h>
#include <parted/parted.h>

#include "../installer_def.h"
#include "../../../utils/scope_guard.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Installer::Steps
{
    namespace
    {
        using JappeStudios::JappeOS::JappeOSCore::Utils::ScopeGuard;

        PedExceptionOption PartedExceptionHandler(PedException* ex)
        {
            if (!ex)
                return PED_EXCEPTION_CANCEL;

            if ((ex->type == PED_EXCEPTION_INFORMATION ||
                 ex->type == PED_EXCEPTION_WARNING) &&
                (ex->options & PED_EXCEPTION_OK))
            {
                return PED_EXCEPTION_OK;
            }

            if (ex->options & PED_EXCEPTION_CANCEL)
                return PED_EXCEPTION_CANCEL;

            if (ex->options & PED_EXCEPTION_NO)
                return PED_EXCEPTION_NO;

            return PED_EXCEPTION_UNHANDLED;
        }

        [[noreturn]] void ThrowPartedError(const std::string& message)
        {
            throw std::runtime_error("Partitioning failed: " + message);
        }

        PedSector SectorsForMiB(const PedDevice& device, const uint64_t mib)
        {
            const uint64_t bytes = mib * 1024ULL * 1024ULL;
            return static_cast<PedSector>((bytes + device.sector_size - 1) / device.sector_size);
        }

        std::string MapFilesystem(const PedFileSystemType* fsType)
        {
            if (!fsType || !fsType->name)
                return STORAGE_FILESYSTEM_UNKNOWN;

            const std::string name = fsType->name;

            if (name == "fat32" || name == "fat16")  return STORAGE_FILESYSTEM_FAT32;
            if (name == "ext4"  || name == "ext3"
                                || name == "ext2")   return STORAGE_FILESYSTEM_EXT4;
            if (name == "btrfs")                     return STORAGE_FILESYSTEM_BTRFS;
            if (name == "xfs")                       return STORAGE_FILESYSTEM_XFS;

            return STORAGE_FILESYSTEM_UNKNOWN;
        }

        std::string GetUdevDevtype(udev_device* device)
        {
            const char* devType = udev_device_get_property_value(device, "DEVTYPE");
            return devType ? devType : "";
        }

        udev_device* GetUdevBlockDevice(udev* udevCtx, const std::string& devicePath)
        {
            struct stat st{};
            if (stat(devicePath.c_str(), &st) != 0)
                ThrowPartedError("Cannot stat block device '" + devicePath + "': " + std::strerror(errno));

            if (!S_ISBLK(st.st_mode))
                ThrowPartedError("Path is not a block device: " + devicePath);

            udev_device* udevDev = udev_device_new_from_devnum(udevCtx, 'b', st.st_rdev);
            if (!udevDev)
                ThrowPartedError("Block device is not known to udev: " + devicePath);

            return udevDev;
        }

        void ValidateUdevDisk(const std::string& devicePath)
        {
            udev* udevCtx = udev_new();
            if (!udevCtx)
                ThrowPartedError("Failed to create udev context");
            ScopeGuard udevGuard([&] { udev_unref(udevCtx); });

            udev_device* udevDev = GetUdevBlockDevice(udevCtx, devicePath);
            ScopeGuard deviceGuard([&] { udev_device_unref(udevDev); });

            if (GetUdevDevtype(udevDev) != "disk")
                ThrowPartedError("Selected device is not a disk: " + devicePath);
        }

        void ValidateUdevPartition(const std::string& diskPath, const std::string& partitionPath)
        {
            udev* udevCtx = udev_new();
            if (!udevCtx)
                ThrowPartedError("Failed to create udev context");
            ScopeGuard udevGuard([&] { udev_unref(udevCtx); });

            udev_device* udevPart = GetUdevBlockDevice(udevCtx, partitionPath);
            ScopeGuard partGuard([&] { udev_device_unref(udevPart); });

            if (GetUdevDevtype(udevPart) != "partition")
                ThrowPartedError("Selected mount target is not a partition: " + partitionPath);

            udev_device* parent = udev_device_get_parent_with_subsystem_devtype(udevPart, "block", "disk");
            const char* parentDevnode = parent ? udev_device_get_devnode(parent) : nullptr;
            if (!parentDevnode || diskPath != parentDevnode)
            {
                ThrowPartedError(
                    "Selected partition '" + partitionPath +
                    "' does not belong to selected disk '" + diskPath + "'"
                );
            }
        }

        void AddPartition(PedDisk* disk,
                          PedPartition* partition,
                          const PedConstraint* constraint,
                          const std::string& description)
        {
            if (!partition)
                ThrowPartedError("Failed to create " + description + " partition");

            if (!ped_disk_add_partition(disk, partition, constraint))
            {
                ped_partition_destroy(partition);
                ThrowPartedError("Failed to add " + description + " partition");
            }
        }

        std::string GetPartitionPath(PedPartition* partition, const std::string& description)
        {
            char* rawPath = ped_partition_get_path(partition);
            if (!rawPath)
                ThrowPartedError("Failed to resolve " + description + " partition path");

            std::string path(rawPath);
            std::free(rawPath);
            return path;
        }

        std::string ProbePartitionFilesystem(const std::string& diskPath, const std::string& partitionPath)
        {
            PedDevice* device = ped_device_get(diskPath.c_str());
            if (!device)
                ThrowPartedError("Unable to open selected device: " + diskPath);
            ScopeGuard deviceGuard([&] { ped_device_destroy(device); });

            PedDisk* disk = ped_disk_new(device);
            if (!disk)
                ThrowPartedError("Failed to read partition table from selected device: " + diskPath);
            ScopeGuard diskGuard([&] { ped_disk_destroy(disk); });

            for (PedPartition* part = ped_disk_next_partition(disk, nullptr);
                 part != nullptr;
                 part = ped_disk_next_partition(disk, part))
            {
                if (part->type & PED_PARTITION_FREESPACE) continue;
                if (part->type & PED_PARTITION_EXTENDED) continue;
                if (part->type & PED_PARTITION_METADATA) continue;

                const std::string path = GetPartitionPath(part, "manual");
                if (path == partitionPath)
                    return MapFilesystem(ped_file_system_probe(&part->geom));
            }

            ThrowPartedError("Selected partition was not found in partition table: " + partitionPath);
        }

        void AddTarget(InstallContext& context,
                       std::string device,
                       std::string mountpoint,
                       std::string filesystem,
                       const bool format)
        {
            if (mountpoint == STORAGE_MOUNTPOINT_BOOT)
                context.bootPartition = device;
            else if (mountpoint == STORAGE_MOUNTPOINT_ROOT)
                context.rootPartition = device;

            context.partitionTargets.push_back(InstallPartitionTargetData{
                std::move(device),
                std::move(mountpoint),
                std::move(filesystem),
                format,
            });
        }
    }

    void PartitionStep::Execute(InstallContext& context)
    {
        context.bootPartition.clear();
        context.rootPartition.clear();
        context.partitionTargets.clear();

        const auto& diskData = context.data.disk;
        ValidateUdevDisk(diskData.device);

        switch (diskData.mode)
        {
            case InstallDiskMode::Erase:
                ExecuteErase(context, diskData);
                break;
            case InstallDiskMode::Manual:
                ExecuteManual(context, diskData);
                break;
            case InstallDiskMode::Custom:
                ExecuteCustom(context, diskData);
                break;
            default:
                throw std::runtime_error("Unknown disk mode");
        }
    }

    void PartitionStep::ExecuteErase(InstallContext& context, const InstallDiskData& diskData) const
    {
        PedExceptionHandler* previousHandler = ped_exception_get_handler();
        ped_exception_set_handler(PartedExceptionHandler);
        ScopeGuard exceptionHandlerGuard([&] { ped_exception_set_handler(previousHandler); });

        PedDevice* device = ped_device_get(diskData.device.c_str());
        if (!device)
            ThrowPartedError("Unable to open selected device: " + diskData.device);
        ScopeGuard deviceGuard([&] { ped_device_destroy(device); });

        if (ped_device_is_busy(device))
            ThrowPartedError("Selected device is busy: " + diskData.device);

        if (!ped_device_open(device))
            ThrowPartedError("Failed to open selected device for partitioning: " + diskData.device);
        ScopeGuard deviceCloseGuard([&] { ped_device_close(device); });

        const PedDiskType* gptType = ped_disk_type_get("gpt");
        if (!gptType)
            ThrowPartedError("libparted does not support GPT disk labels");

        PedDisk* disk = ped_disk_new_fresh(device, gptType);
        if (!disk)
            ThrowPartedError("Failed to create GPT partition table");
        ScopeGuard diskGuard([&] { ped_disk_destroy(disk); });

        PedConstraint* constraint = ped_constraint_any(device);
        if (!constraint)
            ThrowPartedError("Failed to create partition constraint");
        ScopeGuard constraintGuard([&] { ped_constraint_destroy(constraint); });

        const PedFileSystemType* fat32Type = ped_file_system_type_get(STORAGE_FILESYSTEM_FAT32);
        if (!fat32Type)
            ThrowPartedError("libparted does not support FAT32 partition metadata");

        const PedFileSystemType* btrfsType = ped_file_system_type_get(STORAGE_FILESYSTEM_BTRFS);
        if (!btrfsType)
            ThrowPartedError("libparted does not support Btrfs partition metadata");

        const PedSector oneMiB = SectorsForMiB(*device, 1);
        const PedSector bootStart = oneMiB;
        const PedSector bootLength = SectorsForMiB(*device, STORAGE_PART_BOOT_MIN_RECOMMENDED_SIZE_MIB);
        const PedSector bootEnd = bootStart + bootLength - 1;
        const PedSector rootStart = bootEnd + 1;
        const PedSector rootEnd = device->length - oneMiB - 1;

        if (rootStart >= rootEnd)
            ThrowPartedError("Selected device is too small for boot and system partitions");

        PedPartition* bootPartition = ped_partition_new(
            disk,
            PED_PARTITION_NORMAL,
            fat32Type,
            bootStart,
            bootEnd
        );
        AddPartition(disk, bootPartition, constraint, "boot");
        ped_partition_set_name(bootPartition, "JappeOS Boot");
        if (ped_partition_is_flag_available(bootPartition, PED_PARTITION_ESP))
            ped_partition_set_flag(bootPartition, PED_PARTITION_ESP, 1);
        else if (ped_partition_is_flag_available(bootPartition, PED_PARTITION_BOOT))
            ped_partition_set_flag(bootPartition, PED_PARTITION_BOOT, 1);
        const std::string bootPartitionPath = GetPartitionPath(bootPartition, "boot");

        PedPartition* rootPartition = ped_partition_new(
            disk,
            PED_PARTITION_NORMAL,
            btrfsType,
            rootStart,
            rootEnd
        );
        AddPartition(disk, rootPartition, constraint, "system");
        ped_partition_set_name(rootPartition, "JappeOS System");
        const std::string rootPartitionPath = GetPartitionPath(rootPartition, "system");

        if (!ped_disk_commit(disk))
            ThrowPartedError("Failed to commit partition table to disk");

        if (!ped_device_sync(device))
            ThrowPartedError("Failed to sync selected device after partitioning");

        AddTarget(context, bootPartitionPath, STORAGE_MOUNTPOINT_BOOT, STORAGE_FILESYSTEM_FAT32, true);
        AddTarget(context, rootPartitionPath, STORAGE_MOUNTPOINT_ROOT, STORAGE_FILESYSTEM_BTRFS, true);
    }

    void PartitionStep::ExecuteManual(InstallContext& context, const InstallDiskData& diskData) const
    {
        std::map<std::string, std::string> mountsByMountpoint;
        for (const auto& mount : diskData.mounts)
        {
            ValidateUdevPartition(diskData.device, mount.partition);
            mountsByMountpoint.emplace(mount.mountpoint, mount.partition);
        }

        const auto boot = mountsByMountpoint.find(STORAGE_MOUNTPOINT_BOOT);
        if (boot == mountsByMountpoint.end())
            ThrowPartedError("Manual mode did not specify a boot partition");

        const auto root = mountsByMountpoint.find(STORAGE_MOUNTPOINT_ROOT);
        if (root == mountsByMountpoint.end())
            ThrowPartedError("Manual mode did not specify a root partition");

        AddTarget(
            context,
            boot->second,
            STORAGE_MOUNTPOINT_BOOT,
            ProbePartitionFilesystem(diskData.device, boot->second),
            false
        );

        AddTarget(
            context,
            root->second,
            STORAGE_MOUNTPOINT_ROOT,
            ProbePartitionFilesystem(diskData.device, root->second),
            false
        );
    }

    void PartitionStep::ExecuteCustom(InstallContext&, const InstallDiskData&) const
    {
        throw std::runtime_error("Custom partitioning is not implemented yet");
    }

}
