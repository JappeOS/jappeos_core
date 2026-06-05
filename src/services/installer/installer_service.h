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
#include <regex>
#include <set>
#include <string>
#include "../service.h"
#include "../../logger.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Installer
{
#define STORAGE_MOUNTPOINT_BOOT "/boot"
#define STORAGE_MOUNTPOINT_ROOT "/"

#define STORAGE_FILESYSTEM_FAT32   "FAT32"
#define STORAGE_FILESYSTEM_EXT4    "EXT4"
#define STORAGE_FILESYSTEM_BTRFS   "BTRFS"
#define STORAGE_FILESYSTEM_XFS     "XFS"
#define STORAGE_FILESYSTEM_UNKNOWN "?"

    class InstallerService : public Service
    {
        struct InstallData;
        struct KeyboardLayoutData;
        struct StorageDeviceData;
        struct StoragePartitionData;

    public:
        explicit InstallerService(ServiceManager* serviceManager, Connection* conn);
        ~InstallerService() override;
        [[nodiscard]] std::string GetName() const override { return "InstallerService"; }

    private:
        Object     _object;
        Interface& _iface;

        std::map<std::string, std::string> _locales{};
        std::set<std::string> _timezones{};
        std::map<std::string, KeyboardLayoutData> _keyboardLayouts{};
        std::map<std::string, StorageDeviceData> _storageDevices{};
        uint32_t _installPlanId = 0;
        std::unique_ptr<InstallData> _installData;

        Prop<bool> _inProgress;
        Prop<bool> _installDone;
        Prop<std::string> _currentLocale;
        Prop<std::string> _currentTimezone;
        Prop<std::tuple<std::string, std::string>> _currentKeyboardLayout;

        bool _suppressPropertyCallbacks = false;

        void OnGetLocaleInfo(const Message& message);
        void OnSetCurrentLocale(const std::string& locale);
        void OnSetCurrentTimezone(const std::string& timezone);
        void OnSetCurrentKeyboardLayout(const std::tuple<std::string, std::string>& keyboardLayout);
        void OnGetStorageInfo(const Message& message);
        void OnCreateInstallPlan(const Message& message);
        void OnBeginInstallation(const Message& message);

        void CreateLocales();
        void CreateTimezones();
        void CreateKeyboardLayouts();
        void CreateStorageInfo();
        void BeginInstallation();

        void ValidateInstallData(const InstallData& data, std::vector<std::string>& outWarnings);
        void ValidateInstallStorageData(const InstallData& data, std::vector<std::string>& outWarnings);
        void ValidateInstallStorageDataManual(const InstallData& data,
                                              const StorageDeviceData& device,
                                              std::vector<std::string>& outWarnings);
        void ValidateInstallStorageDataCustom(const InstallData& data,
                                              const StorageDeviceData& device,
                                              std::vector<std::string>& outWarnings);
        void ValidateInstallStorageData_VerifyMountpoints(const std::string& bootPartition,
                                                          const std::string& rootPartition,
                                                          const std::vector<StoragePartitionData>& partitions,
                                                          std::vector<std::string>& outWarnings);

    private:
        static JappeOSCore::Logger& Log()
        {
            static JappeOSCore::Logger instance{"InstallerService"};
            return instance;
        }

        static bool IsValidHostname(const std::string& hostname);
        static bool IsValidUsername(const std::string& username);
        static bool IsValidStorageFilesystem(const std::string& filesystem, bool allowUnknown = false);
        static bool IsValidStorageMountpoint(const std::string& mountpoint);

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

            bool IsFreeSpace() const { return filesystem.empty(); }
        };

        struct StorageDeviceData
        {
            std::string device;
            uint64_t sizeMiB;
            std::vector<StoragePartitionData> partitions;
        };

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
    };
}
