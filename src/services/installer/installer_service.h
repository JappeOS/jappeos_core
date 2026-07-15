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

#include "install_controller.h"
#include "install_data.h"
#include "../service.h"
#include "../../logger.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Installer
{
    class InstallerService : public Service
    {
    public:
        explicit InstallerService(ServiceManager* serviceManager, Connection* conn);
        ~InstallerService() override;
        [[nodiscard]] std::string GetName() const override { return "InstallerService"; }

    public:
        const char* JOS_LIVE_USER_NAME = "liveuser";
        const char* JOS_LIVE_USER_REAL_NAME = "Live User";

    private:
        Object     _object;
        Interface& _iface;

        std::map<std::string, std::string>        _locales{};
        std::set<std::string>                     _timezones{};
        std::map<std::string, KeyboardLayoutData> _keyboardLayouts{};
        std::map<std::string, StorageDeviceData>  _storageDevices{};
        uint32_t                                  _installPlanId = 0;
        std::unique_ptr<InstallData>              _installData;
        std::unique_ptr<InstallController>        _installController;

        Prop<std::string>                                  _state;
        Prop<std::string>                                  _errorMessage;
        Prop<std::tuple<std::string, double, std::string>> _progress;
        Prop<std::string>                                  _currentLocale;
        Prop<std::string>                                  _currentTimezone;
        Prop<std::tuple<std::string, std::string>>         _currentKeyboardLayout;

        bool _suppressPropertyCallbacks = false;

        void OnGetLocaleInfo(const Message& message);
        void OnSetCurrentLocale(const std::string& locale);
        void OnSetCurrentTimezone(const std::string& timezone);
        void OnSetCurrentKeyboardLayout(const std::tuple<std::string, std::string>& keyboardLayout);
        void OnGetStorageInfo(const Message& message);
        void OnCreateInstallPlan(const Message& message);
        void OnCancelInstallPlan(const Message& message);
        void OnBeginInstallation(const Message& message);
        void OnVerifyUsername(const Message& message) const;
        void OnVerifyHostname(const Message& message) const;

        void HandleInitialBootup(bool& isLive) const;
        void CreateLocales();
        void CreateTimezones();
        void CreateKeyboardLayouts();
        void CreateStorageInfo();
        void BeginInstallation() const;

        void HandleInstallControllerStateChange(InstallState state, const std::string& errorMessage);
        void HandleInstallControllerProgressChange(const InstallProgress& progress);

        void ValidateInstallData(const InstallData& data, std::vector<std::string>& outWarnings);
        void ValidateInstallStorageData(const InstallData& data, std::vector<std::string>& outWarnings);
        void ValidateInstallStorageDataManual(const InstallData& data,
                                              const StorageDeviceData& device,
                                              std::vector<std::string>& outWarnings);
        void ValidateInstallStorageDataCustom(const InstallData& data,
                                              const StorageDeviceData& device,
                                              std::vector<std::string>& outWarnings);
        void ValidateInstallStorageData_VerifyMountpoints(const StoragePartitionData& bootPartition,
                                                          const StoragePartitionData& rootPartition,
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

        static std::string InstallerStateToString(InstallState state);
    };
}
