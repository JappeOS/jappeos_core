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
#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace JappeStudios::JappeOS::JappeOSCore::Services::Installer
{

    /*
     * ========== RECEIVED INSTALL DATA ==========
     */

    enum class InstallDiskMode
    {
        Erase,
        Manual,
        Custom,
    };

    enum class InstallDiskOperationType
    {
        Create,
        Resize,
        Remove,
        SetMountpoint,
        SetFilesystem,
    };

    struct InstallDiskOperationCreateData
    {
        std::string region;
        uint64_t sizeMiB;
        bool remaining;
        std::string filesystem;
        std::string mountpoint;
    };

    struct InstallDiskOperationResizeData
    {
        std::string partition;
        uint64_t sizeMiB;
        bool remaining;
    };

    struct InstallDiskOperationRemoveData
    {
        std::string partition;
    };

    struct InstallDiskOperationSetMountpointData
    {
        std::string partition;
        std::string mountpoint;
    };

    struct InstallDiskOperationSetFilesystemData
    {
        std::string partition;
        std::string filesystem;
    };

    struct InstallDiskOperationData
    {
        InstallDiskOperationType type;
        std::variant<
            InstallDiskOperationCreateData,
            InstallDiskOperationResizeData,
            InstallDiskOperationRemoveData,
            InstallDiskOperationSetMountpointData,
            InstallDiskOperationSetFilesystemData> data;
    };

    struct InstallDiskMountData
    {
        std::string partition;
        std::string mountpoint;
    };

    struct InstallDiskData
    {
        std::string device;
        InstallDiskMode mode;
        std::vector<InstallDiskMountData> mounts;
        std::vector<InstallDiskOperationData> operations;
    };

    struct InstallPackagesData
    {
        bool installProprietary;
        bool installRecommendedDrivers;
    };

    struct InstallData
    {
        std::string hostname;
        std::string username;
        std::string password;
        std::string timezone;
        std::string locale;
        std::tuple<std::string, std::string> keyboardLayout;
        InstallDiskData disk;
        InstallPackagesData packages;
    };

    /*
     * ========== RUNTIME INSTALL DATA ==========
     */

    struct InstallPartitionTargetData
    {
        std::string device;
        std::string mountpoint;
        std::string filesystem;
        bool format;
    };

    struct InstallContext
    {
        const InstallData& data;

        std::string targetRoot;
        std::string rootSqFsImagePath;
        std::string bootPartition;
        std::string rootPartition;
        std::vector<InstallPartitionTargetData> partitionTargets;
    };

    /*
     * ========== INTERNALLY-CREATED INSTALL DATA ==========
     */

    struct KeyboardLayoutData
    {
        std::string id;
        std::string name;
        std::map<std::string, std::string> variants;
    };

    struct StoragePartitionData
    {
        std::string device;
        std::string filesystem;
        uint64_t sizeMiB;
        std::string mountpoint;

        [[nodiscard]] bool IsFreeSpace() const { return filesystem.empty(); }
    };

    struct StorageDeviceData
    {
        std::string device;
        std::string model;
        bool isRemovable;
        uint64_t sizeMiB;
        std::vector<StoragePartitionData> partitions;
    };
}
