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

#include "installer_service.h"

#include <algorithm>
#include <unordered_set>
#include <pwd.h>
#include <random>

namespace JappeStudios::JappeOS::JappeOSCore::Services::Installer
{

    InstallerService::InstallerService(ServiceManager* serviceManager,
                                       Connection* conn) :
                                       Service(serviceManager, conn),
                                       _object(*_conn, GetBaseObjectPath()),
                                       _iface(_object.CreateInterface(GetBaseInterface())),
                                       _inProgress(*_conn, _iface, "InProgress", false),
                                       _installDone(*_conn, _iface, "InstallDone", false),
                                       _currentLocale(*_conn, _iface, "CurrentLocale", "", true, [this](const std::string& val) { OnSetCurrentLocale(val); }),
                                       _currentTimezone(*_conn, _iface, "CurrentTimezone", "", true, [this](const std::string& val) { OnSetCurrentTimezone(val); }),
                                       _currentKeyboardLayout(*_conn, _iface, "CurrentKeyboardLayout", {}, true, [this](const auto& val) { OnSetCurrentKeyboardLayout(val); })
    {
        _iface.RegisterMethod("GetLocaleInfo", [&] (const auto& m) { OnGetLocaleInfo(m); });
        _iface.RegisterMethod("GetStorageInfo", [&] (const auto& m) { OnGetStorageInfo(m); });
        _iface.RegisterMethod("CreateInstallPlan", [&] (const auto& m) { OnCreateInstallPlan(m); });
        _iface.RegisterMethod("BeginInstallation", [&] (const auto& m) { OnBeginInstallation(m); });

        CreateLocales();
        CreateTimezones();
        CreateKeyboardLayouts();
        CreateStorageInfo();
    }

    InstallerService::~InstallerService()
    {

    }

    void InstallerService::OnGetLocaleInfo(const Message& message)
    {
        std::map<std::string, std::tuple<std::string, std::map<std::string, std::string>>> keyboardLayouts;
        for (const auto &[id, name, variants]: _keyboardLayouts | std::views::values)
        {
            keyboardLayouts.emplace(id, std::make_tuple(name, variants));
        }

        auto msg = Message::CreateMethodReturn(message);
        msg.SetArgs(_locales, _timezones, keyboardLayouts);
        msg.Send(*_conn);
    }

    void InstallerService::OnSetCurrentLocale(const std::string& locale)
    {
        if (_suppressPropertyCallbacks)
            return;

        if (!_locales.contains(locale))
        {
            _suppressPropertyCallbacks = true;
            _currentLocale = _locales.begin()->first;
            _suppressPropertyCallbacks = false;
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Invalid locale");
        }
    }

    void InstallerService::OnSetCurrentTimezone(const std::string& timezone)
    {
        if (_suppressPropertyCallbacks)
            return;

        if (!_timezones.contains(timezone))
        {
            _suppressPropertyCallbacks = true;
            _currentTimezone = *_timezones.begin();
            _suppressPropertyCallbacks = false;
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Invalid timezone");
        }
    }

    void InstallerService::OnSetCurrentKeyboardLayout(const std::tuple<std::string, std::string>& keyboardLayout)
    {

    }

    void InstallerService::OnGetStorageInfo(const Message& message)
    {
        std::map<
            std::string,
            std::tuple<
                uint64_t,
                std::vector<std::tuple<std::string, std::string, uint64_t, std::string>>>> storageDevices;
        for (const auto &val: _storageDevices | std::views::values)
        {
            std::vector<std::tuple<std::string, std::string, uint64_t, std::string>> partitions;
            for (const auto& part : val.partitions)
            {
                partitions.push_back(std::make_tuple(part.device, part.filesystem, part.sizeMiB, /*part.mountpoint*/ ""));
            }

            storageDevices.emplace(val.device, std::make_tuple(val.sizeMiB, partitions));
        }

        auto msg = Message::CreateMethodReturn(message);
        msg.SetArgs(storageDevices);
        msg.Send(*_conn);
    }

    void InstallerService::OnCreateInstallPlan(const Message& message)
    {
        const auto [
            hostname,
            username,
            password,
            timezone,
            locale,
            keyboardLayout,
            disk,
            installProprietary,
            installRecommendedDrivers
        ] = message.GetArgs<
            std::string,
            std::string,
            std::string,
            std::string,
            std::string,
            std::tuple<std::string, std::string>,
            std::tuple< // InstallDiskData
                std::string, // device
                uint32_t,    // mode
                std::vector<std::tuple<std::string, std::string>>,                    // mounts
                std::vector<std::tuple<uint32_t, std::map<std::string, DBusVariant>>> // operations
            >,
            bool,
            bool>();

        std::vector<InstallDiskMountData> diskDataMounts;
        for (const auto& val : std::get<2>(disk))
        {
            diskDataMounts.push_back(InstallDiskMountData{ std::get<0>(val), std::get<1>(val) });
        }

        std::vector<InstallDiskOperationData> diskDataOperations;
        for (const auto& val : std::get<3>(disk))
        {
            const auto type = static_cast<InstallDiskOperationType>(std::get<0>(val));
            const auto& map = std::get<1>(val);
            std::variant<
                InstallDiskOperationCreateData,
                InstallDiskOperationResizeData,
                InstallDiskOperationRemoveData,
                InstallDiskOperationSetMountpointData,
                InstallDiskOperationSetFilesystemData> data;

            switch (type)
            {
                case InstallDiskOperationType::Create:
                {
                    std::string region;
                    if (const auto& it = map.find("region"); it != map.end())
                        region = it->second.get<std::string>();

                    uint64_t sizeMiB = 0;
                    if (const auto& it = map.find("sizeMiB"); it != map.end())
                        sizeMiB = it->second.get<uint64_t>();

                    bool remaining = false;
                    if (const auto& it = map.find("remaining"); it != map.end())
                        remaining = it->second.get<bool>();

                    std::string filesystem;
                    if (const auto& it = map.find("filesystem"); it != map.end())
                        filesystem = it->second.get<std::string>();

                    std::string mountpoint;
                    if (const auto& it = map.find("mountpoint"); it != map.end())
                        mountpoint = it->second.get<std::string>();

                    data = InstallDiskOperationCreateData{
                        region,
                        sizeMiB,
                        remaining,
                        filesystem,
                        mountpoint,
                    };
                    break;
                }
                case InstallDiskOperationType::Resize:
                {
                    std::string partition;
                    if (const auto& it = map.find("partition"); it != map.end())
                        partition = it->second.get<std::string>();

                    uint64_t sizeMiB = 0;
                    if (const auto& it = map.find("sizeMiB"); it != map.end())
                        sizeMiB = it->second.get<uint64_t>();

                    bool remaining = false;
                    if (const auto& it = map.find("remaining"); it != map.end())
                        remaining = it->second.get<bool>();

                    data = InstallDiskOperationResizeData{
                        partition,
                        sizeMiB,
                        remaining,
                    };
                    break;
                }
                case InstallDiskOperationType::Remove:
                {
                    std::string partition;
                    if (const auto& it = map.find("partition"); it != map.end())
                        partition = it->second.get<std::string>();

                    data = InstallDiskOperationRemoveData{
                        partition,
                    };
                    break;
                }
                case InstallDiskOperationType::SetMountpoint:
                {
                    std::string partition;
                    if (const auto& it = map.find("partition"); it != map.end())
                        partition = it->second.get<std::string>();

                    std::string mountpoint;
                    if (const auto& it = map.find("mountpoint"); it != map.end())
                        mountpoint = it->second.get<std::string>();

                    data = InstallDiskOperationSetMountpointData{
                        partition,
                        mountpoint,
                    };
                    break;
                }
                case InstallDiskOperationType::SetFilesystem:
                {
                    std::string partition;
                    if (const auto& it = map.find("partition"); it != map.end())
                        partition = it->second.get<std::string>();

                    std::string filesystem;
                    if (const auto& it = map.find("filesystem"); it != map.end())
                        filesystem = it->second.get<std::string>();

                    data = InstallDiskOperationSetFilesystemData{
                        partition,
                        filesystem,
                    };
                    break;
                }
                default:
                    throw DBusException(DBUS_ERROR_INVALID_ARGS, "Invalid disk operation type");
            }

            diskDataOperations.push_back(InstallDiskOperationData{type, std::move(data)});
        }

        auto diskData = InstallDiskData{
            std::get<0>(disk),
            static_cast<InstallDiskMode>(std::get<1>(disk)),
            diskDataMounts,
            diskDataOperations,
        };

        auto installData = InstallData{
            hostname,
            username,
            password,
            timezone,
            locale,
            keyboardLayout,
            diskData,
            InstallPackagesData{installProprietary, installRecommendedDrivers},
        };

        if (_installPlanId)
            throw DBusException(DBUS_ERROR_FAILED, "Installation plan already exists");

        if (_inProgress.Get())
            throw DBusException(DBUS_ERROR_FAILED, "Installation already in progress");

        std::vector<std::string> warnings;
        ValidateInstallData(installData, warnings);

        static std::random_device rd;
        static std::mt19937 rng(rd());
        static std::uniform_int_distribution<uint32_t> dist;
        _installPlanId = dist(rng);
        _installData = std::make_unique<InstallData>(std::move(installData));

        auto msg = Message::CreateMethodReturn(message);
        msg.SetArgs(_installPlanId, warnings);
        msg.Send(*_conn);
    }

    void InstallerService::OnBeginInstallation(const Message& message)
    {
        const auto [plan] = message.GetArgs<uint32_t>();
        if (plan != _installPlanId || !_installData)
            throw DBusException(DBUS_ERROR_FAILED, "Unknown install plan provided");

        BeginInstallation();
        Message::CreateMethodReturn(message).Send(*_conn);
    }

    void InstallerService::CreateLocales()
    {
        _locales.emplace("U", "Unknown");
        _suppressPropertyCallbacks = true;
        _currentLocale = "U";
        _suppressPropertyCallbacks = false;

        if (_locales.empty())
            throw std::runtime_error("No locales found");
    }

    void InstallerService::CreateTimezones()
    {
        _timezones.emplace("Unknown");
        _suppressPropertyCallbacks = true;
        _currentTimezone = "Unknown";
        _suppressPropertyCallbacks = false;

        if (_timezones.empty())
            throw std::runtime_error("No timezones found");
    }

    void InstallerService::CreateKeyboardLayouts()
    {

    }

    void InstallerService::CreateStorageInfo()
    {

    }

    void InstallerService::BeginInstallation()
    {
        if (_inProgress.Get())
            throw DBusException(DBUS_ERROR_FAILED, "Installation already in progress");

        _installDone = false;
        _inProgress = true;
    }

    void InstallerService::ValidateInstallData(const InstallData& data, std::vector<std::string>& outWarnings)
    {
        if (!IsValidHostname(data.hostname))
            throw DBusException(DBUS_ERROR_FAILED, "Invalid hostname");

        if (!IsValidUsername(data.username))
            throw DBusException(DBUS_ERROR_FAILED, "Invalid username");

        if (!_timezones.contains(data.timezone))
            throw DBusException(DBUS_ERROR_FAILED, "Unknown timezone");

        if (!_locales.contains(data.locale))
            throw DBusException(DBUS_ERROR_FAILED, "Unknown locale");

        if (const auto& it = _keyboardLayouts.find(std::get<0>(data.keyboardLayout)); it != _keyboardLayouts.end())
        {
            if (!it->second.variants.contains(std::get<1>(data.keyboardLayout)))
                throw DBusException(DBUS_ERROR_FAILED, "Unknown keyboard layout variant");
        }
        else
        {
            throw DBusException(DBUS_ERROR_FAILED, "Unknown keyboard layout");
        }

        ValidateInstallStorageData(data, outWarnings);
    }

    void InstallerService::ValidateInstallStorageData(const InstallData& data, std::vector<std::string>& outWarnings)
    {
        if (data.disk.mode != InstallDiskMode::Erase &&
            data.disk.mode != InstallDiskMode::Manual &&
            data.disk.mode != InstallDiskMode::Custom)
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Invalid disk operation type");

        const auto& storageDevice = _storageDevices.find(data.disk.device);
        if (storageDevice == _storageDevices.end())
            throw DBusException(DBUS_ERROR_FAILED, "Unknown storage device");

        if (data.disk.mode == InstallDiskMode::Erase && (!data.disk.mounts.empty() || !data.disk.operations.empty()))
            throw DBusException(DBUS_ERROR_FAILED, "Erase mode cannot specify 'mounts' or 'operations'");

        if (data.disk.mode == InstallDiskMode::Manual)
            ValidateInstallStorageDataManual(data, storageDevice->second, outWarnings);
        else if (data.disk.mode == InstallDiskMode::Custom)
            ValidateInstallStorageDataCustom(data, storageDevice->second, outWarnings);
    }

    void InstallerService::ValidateInstallStorageDataManual(const InstallData& data,
                                                            const StorageDeviceData& device,
                                                            std::vector<std::string>& outWarnings)
    {
        const auto& partitions = device.partitions;

        if (!data.disk.operations.empty())
            throw DBusException(DBUS_ERROR_FAILED, "Manual mode cannot specify 'operations'");

        std::string bootPartition;
        if (std::ranges::count_if(data.disk.mounts, [&](const auto& m)
        {
            if (m.mountpoint == STORAGE_MOUNTPOINT_BOOT)
            {
                bootPartition = m.partition;
                return true;
            }
            return false;
        }) != 1)
        {
            throw DBusException(DBUS_ERROR_FAILED, "Expected exactly one boot partition");
        }

        std::string rootPartition;
        if (std::ranges::count_if(data.disk.mounts, [&](const auto& m)
        {
            if (m.mountpoint == STORAGE_MOUNTPOINT_ROOT)
            {
                rootPartition = m.partition;
                return true;
            }
            return false;
        }) != 1)
        {
            throw DBusException(DBUS_ERROR_FAILED, "Expected exactly one root partition");
        }

        ValidateInstallStorageData_VerifyMountpoints(bootPartition, rootPartition, partitions, outWarnings);
    }

    void InstallerService::ValidateInstallStorageDataCustom(const InstallData& data,
                                                            const StorageDeviceData& device,
                                                            std::vector<std::string>& outWarnings)
    {
        if (!data.disk.mounts.empty())
            throw DBusException(DBUS_ERROR_FAILED, "Custom mode cannot specify 'mounts'");

        std::vector<StoragePartitionData> partitions(device.partitions);

        const auto removeAdjacentSpaces = [&]
        {
            for (size_t i = 1; i < partitions.size();)
            {
                if (partitions[i - 1].IsFreeSpace() &&
                    partitions[i].IsFreeSpace())
                {
                    partitions[i - 1].sizeMiB += partitions[i].sizeMiB;
                    partitions.erase(partitions.begin() + i);
                }
                else
                {
                    ++i;
                }
            }
        };

        for (const auto& operation : data.disk.operations)
        {
            switch (operation.type)
            {
                case InstallDiskOperationType::Create:
                {
                    const auto& createOP = std::get<InstallDiskOperationCreateData>(operation.data);
                    auto it = std::ranges::find_if(partitions, [&](const auto& m) { return m.device == createOP.region; });
                    if (it == partitions.end())
                        throw DBusException(DBUS_ERROR_FAILED, "Unknown partition or region: " + createOP.region);

                    auto& part = *it;
                    if (!part.IsFreeSpace())
                        throw DBusException(DBUS_ERROR_FAILED, "Cannot create partition in non-free space: " + createOP.region);

                    if (createOP.remaining && createOP.sizeMiB)
                        throw DBusException(DBUS_ERROR_FAILED, "Cannot specify 'sizeMiB' if 'remaining' is true: " + createOP.region);

                    if (!createOP.remaining && createOP.sizeMiB < 1)
                        throw DBusException(DBUS_ERROR_FAILED, "Partition size too small: " + std::to_string(createOP.sizeMiB));

                    const uint64_t createSizeMiB = createOP.remaining ? part.sizeMiB : createOP.sizeMiB;
                    if (createSizeMiB > part.sizeMiB)
                        throw DBusException(DBUS_ERROR_FAILED, "Partition size too big: " + std::to_string(createSizeMiB));

                    if (!IsValidStorageFilesystem(createOP.filesystem))
                        throw DBusException(DBUS_ERROR_FAILED, "Unknown filesystem: " + createOP.filesystem);

                    if (!IsValidStorageMountpoint(createOP.mountpoint))
                        throw DBusException(DBUS_ERROR_FAILED, "Invalid mountpoint: " + createOP.mountpoint);

                    if (part.sizeMiB == createSizeMiB)
                        it = partitions.erase(it);
                    else
                        part.sizeMiB -= createSizeMiB;

                    const auto newPartition = StoragePartitionData{"", createOP.filesystem, createSizeMiB};
                    partitions.insert(it, newPartition);

                    removeAdjacentSpaces();
                    break;
                }
                case InstallDiskOperationType::Resize:
                {
                    const auto& resizeOP = std::get<InstallDiskOperationResizeData>(operation.data);
                    auto it = std::ranges::find_if(partitions, [&](const auto& m) { return m.device == resizeOP.partition; });
                    if (it == partitions.end())
                        throw DBusException(DBUS_ERROR_FAILED, "Unknown partition: " + resizeOP.partition);

                    auto& part = *it;
                    if (part.IsFreeSpace())
                        throw DBusException(DBUS_ERROR_FAILED, "Cannot resize free space: " + resizeOP.partition);

                    if (resizeOP.remaining && resizeOP.sizeMiB)
                        throw DBusException(DBUS_ERROR_FAILED, "Cannot specify 'sizeMiB' if 'remaining' is true: " + resizeOP.partition);

                    if (!resizeOP.remaining && resizeOP.sizeMiB < 1)
                        throw DBusException(DBUS_ERROR_FAILED, "Partition size too small: " + std::to_string(resizeOP.sizeMiB));

                    auto nextPart = std::next(it);
                    uint64_t freeSpaceAfter = 0;
                    if (nextPart != partitions.end() && nextPart->IsFreeSpace())
                    {
                        freeSpaceAfter = nextPart->sizeMiB;
                    }
                    else
                    {
                        nextPart = partitions.end();
                    }

                    const uint64_t resizeSizeMiB = resizeOP.remaining
                        ? part.sizeMiB + freeSpaceAfter
                        : resizeOP.sizeMiB;

                    const int64_t addedSpace
                        = static_cast<int64_t>(resizeSizeMiB) - static_cast<int64_t>(part.sizeMiB);
                    if (resizeSizeMiB > part.sizeMiB &&
                        addedSpace > freeSpaceAfter)
                        throw DBusException(
                            DBUS_ERROR_FAILED,
                            "Partition size too large: " + std::to_string(resizeSizeMiB)
                        );

                    part.sizeMiB = resizeSizeMiB;
                    if (addedSpace == freeSpaceAfter)
                    {
                        if (nextPart != partitions.end())
                            partitions.erase(nextPart);
                    }
                    else if (nextPart != partitions.end())
                    {
                        const auto growth = static_cast<uint64_t>(addedSpace);
                        nextPart->sizeMiB -= growth;
                    }

                    if (addedSpace < 0)
                    {
                        const auto newPartition = StoragePartitionData{"", "", static_cast<uint64_t>(-addedSpace)};
                        partitions.insert(std::next(it), newPartition);
                    }

                    removeAdjacentSpaces();
                    break;
                }
                case InstallDiskOperationType::Remove:
                {
                    const auto& removeOP = std::get<InstallDiskOperationRemoveData>(operation.data);
                    auto part = std::ranges::find_if(partitions, [&](const auto& m) { return m.device == removeOP.partition; });
                    if (part == partitions.end())
                        throw DBusException(DBUS_ERROR_FAILED, "Unknown partition: " + removeOP.partition);

                    if (part->IsFreeSpace())
                        throw DBusException(DBUS_ERROR_FAILED, "Cannot remove free space: " + removeOP.partition);

                    const auto size = part->sizeMiB;
                    part = partitions.erase(part);
                    partitions.insert(part, StoragePartitionData{"", "", size});

                    removeAdjacentSpaces();
                    break;
                }
                case InstallDiskOperationType::SetMountpoint:
                {
                    const auto& setMountOP = std::get<InstallDiskOperationSetMountpointData>(operation.data);
                    auto part = std::ranges::find_if(partitions, [&](const auto& m) { return m.device == setMountOP.partition; });
                    if (part == partitions.end())
                        throw DBusException(DBUS_ERROR_FAILED, "Unknown partition: " + setMountOP.partition);

                    if (part->IsFreeSpace())
                        throw DBusException(DBUS_ERROR_FAILED, "Cannot set mountpoint of free space: " + setMountOP.partition);

                    if (!IsValidStorageMountpoint(setMountOP.mountpoint))
                        throw DBusException(DBUS_ERROR_FAILED, "Invalid mountpoint: " + setMountOP.mountpoint);

                    part->mountpoint = setMountOP.mountpoint;
                    break;
                }
                case InstallDiskOperationType::SetFilesystem:
                {
                    const auto& setFileSysOP = std::get<InstallDiskOperationSetFilesystemData>(operation.data);
                    auto part = std::ranges::find_if(partitions, [&](const auto& m) { return m.device == setFileSysOP.partition; });
                    if (part == partitions.end())
                        throw DBusException(DBUS_ERROR_FAILED, "Unknown partition: " + setFileSysOP.partition);

                    if (part->IsFreeSpace())
                        throw DBusException(DBUS_ERROR_FAILED, "Cannot set filesystem of free space: " + setFileSysOP.partition);

                    if (!IsValidStorageFilesystem(setFileSysOP.filesystem))
                        throw DBusException(DBUS_ERROR_FAILED, "Invalid filesystem: " + setFileSysOP.filesystem);

                    part->filesystem = setFileSysOP.filesystem;
                    break;
                }
                default:
                    throw DBusException(DBUS_ERROR_FAILED, "Unhandled operation type");
            }
        }

        uint64_t partSizes = 0;
        for (const auto& partition : partitions)
            partSizes += partition.sizeMiB;

        if (partSizes != device.sizeMiB)
            throw DBusException(DBUS_ERROR_FAILED, "BUG: Partition sizes do not add up to the device size");

        std::string bootPartition;
        if (std::ranges::count_if(partitions, [&](const auto& p)
        {
            if (p.mountpoint == STORAGE_MOUNTPOINT_BOOT)
            {
                bootPartition = p.device;
                return true;
            }
            return false;
        }) != 1)
        {
            throw DBusException(DBUS_ERROR_FAILED, "Expected exactly one boot partition");
        }

        std::string rootPartition;
        if (std::ranges::count_if(partitions, [&](const auto& p)
        {
            if (p.mountpoint == STORAGE_MOUNTPOINT_ROOT)
            {
                rootPartition = p.device;
                return true;
            }
            return false;
        }) != 1)
        {
            throw DBusException(DBUS_ERROR_FAILED, "Expected exactly one root partition");
        }

        ValidateInstallStorageData_VerifyMountpoints(bootPartition, rootPartition, partitions, outWarnings);
    }

    void InstallerService::ValidateInstallStorageData_VerifyMountpoints(const std::string& bootPartition,
                                                                        const std::string& rootPartition,
                                                                        const std::vector<StoragePartitionData>& partitions,
                                                                        std::vector<std::string>& outWarnings)
    {
        if (const auto& it = std::ranges::find_if(partitions, [&](const auto& m) { return m.device == bootPartition; });
            it != partitions.end())
        {
            if (it->filesystem != STORAGE_FILESYSTEM_FAT32)
                throw DBusException(DBUS_ERROR_FAILED, "Boot must be a FAT32 partition");

            if (it->sizeMiB < 100)
                throw DBusException(DBUS_ERROR_FAILED, "Boot partition must be at least 100 MiB of size");

            if (it->sizeMiB < 512)
                outWarnings.push_back("Boot partition should be at least 512 MiB of size");
        }
        else
        {
            throw DBusException(DBUS_ERROR_FAILED, "Boot partition does not exist");
        }

        if (const auto& it = std::ranges::find_if(partitions, [&](const auto& m) { return m.device == rootPartition; });
            it != partitions.end())
        {
            if (it->filesystem == STORAGE_FILESYSTEM_FAT32)
                throw DBusException(DBUS_ERROR_FAILED, "Root must not be a FAT32 partition");

            if (it->sizeMiB < 40960)
                throw DBusException(DBUS_ERROR_FAILED, "Root partition must be at least 40 GiB of size");

            if (it->sizeMiB < 40960 * 2)
                outWarnings.push_back("Root partition should be at least 80 GiB of size");
        }
        else
        {
            throw DBusException(DBUS_ERROR_FAILED, "Root partition does not exist");
        }
    }

    bool InstallerService::IsValidHostname(const std::string& hostname)
    {
        if (hostname.empty() || hostname.size() > 63)
            return false;

        static const std::regex pattern(
            R"(^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?$)"
        );

        return std::regex_match(hostname, pattern);
    }

    bool InstallerService::IsValidUsername(const std::string& username)
    {
        static const std::regex pattern(
            R"(^[a-z_][a-z0-9_-]{0,31}$)"
        );

        if (!std::regex_match(username, pattern))
            return false;

        static const std::unordered_set<std::string> reserved = {
            "root",
            "daemon",
            "bin",
            "sys",
            "adm",
            "mail",
            "nobody",
            "systemd-network",
            "systemd-resolve",
            "systemd-timesync",
            "jos-greeter"
        };

        if (reserved.contains(username))
            return false;

        return getpwnam(username.c_str()) != nullptr;
    }

    bool InstallerService::IsValidStorageFilesystem(const std::string& filesystem, const bool allowUnknown)
    {
        return filesystem == STORAGE_FILESYSTEM_FAT32 ||
            filesystem == STORAGE_FILESYSTEM_EXT4 ||
            filesystem == STORAGE_FILESYSTEM_BTRFS ||
            filesystem == STORAGE_FILESYSTEM_XFS ||
            (allowUnknown ? filesystem == STORAGE_FILESYSTEM_UNKNOWN : false);
    }

    bool InstallerService::IsValidStorageMountpoint(const std::string& mountpoint)
    {
        return mountpoint == STORAGE_MOUNTPOINT_BOOT || mountpoint == STORAGE_MOUNTPOINT_ROOT;
    }

}
