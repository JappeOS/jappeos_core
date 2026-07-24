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

#include <string>
#include <nlohmann/json.hpp>
#include "../service.h"
#include "../../logger.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::DriverManager
{
    // TODO: Implement non-static driver handling
    class DriverManagerService : public Service
    {
    private:
        enum class DriverType
        {
            Static,
            Additional,
        };

        NLOHMANN_JSON_SERIALIZE_ENUM(DriverType, {
            {DriverType::Static, "Static"},
            {DriverType::Additional, "Additional"},
        })

        struct PathDriverDeviceFinderContentMatchInfo
        {
            std::string path;
            std::string regex;

            NLOHMANN_DEFINE_TYPE_INTRUSIVE(PathDriverDeviceFinderContentMatchInfo, path, regex)
        };

        struct PathDriverDeviceFinderInfo
        {
            std::string path;
            std::string entryRegex;
            std::vector<PathDriverDeviceFinderContentMatchInfo> contentMatches;

            NLOHMANN_DEFINE_TYPE_INTRUSIVE(PathDriverDeviceFinderInfo, path, entryRegex, contentMatches)
        };

        struct DriverInfo
        {
            std::string name;
            DriverType type;
            std::vector<std::string> packages;
            PathDriverDeviceFinderInfo deviceFinder;

            NLOHMANN_DEFINE_TYPE_INTRUSIVE(DriverInfo, name, type, packages, deviceFinder)
        };

    public:
        explicit DriverManagerService(ServiceManager* serviceManager, Connection* conn);
        ~DriverManagerService() override;
        [[nodiscard]] std::string GetName() const override { return "DriverManagerService"; }

    private:
        Object _object;
        Interface& _iface;

        std::map<std::string, DriverInfo> _drivers;
        bool _newConnSubExists = false;
        std::size_t _newConnSub;

        bool _tryingToInstallStaticDriversWithRetry = false;
        int _tryInstallStaticDriversWithRetryCount = 0;

        void OnListAvailableDrivers(const Message& message) const;
        void OnListAvailableAdditionalDrivers(const Message& message) const;
        void EmitAdditionalDriverAvailable(const std::string& name);

        void HandleNetworkConnectionAdded();
        void UnsubscribeNetworkConnectionAdded();

        bool TryInstallStaticDriversWithRetry(std::function<void(bool)> resultCallback);
        void CleanupStaticDrivers();

    private:
        static constexpr int TRY_INSTALL_STATIC_DRIVERS_RETRY_COUNT = 4;

        static JappeOSCore::Logger& Log()
        {
            static JappeOSCore::Logger instance{"DriverManagerService"};
            return instance;
        }

        static std::vector<DriverInfo> ParseDriverInfoFile(const std::string& filePath);
        static bool InstallDriver(const DriverInfo& driver);
        static bool HasDeviceForDriver(const PathDriverDeviceFinderInfo& finder);

        static bool HardwareFingerprintChanged();
        static std::string GetCurrentHardwareFingerprint();

        static void InstallPackage(const std::string& package);
        static bool TryRemovePackage(const std::string& package);
        static void CheckPackages();

        static std::string ReadFile(const std::filesystem::path& path);
        static bool WriteFile(const std::filesystem::path& path, const std::string& content);
    };
};
