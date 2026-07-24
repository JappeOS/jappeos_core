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

#include "driver_manager_service.h"
#include <fstream>
#include <regex>
#include <glib/gmain.h>

#include "../../utils/command_runner.h"
#include "../network_manager/network_access_point.h"

using json = nlohmann::json;

namespace JappeStudios::JappeOS::JappeOSCore::Services::DriverManager
{

    DriverManagerService::DriverManagerService(ServiceManager* serviceManager, Connection* conn) :
                                               Service(serviceManager, conn),
                                               _object(*_conn, GetBaseObjectPath()),
                                               _iface(_object.CreateInterface(GetBaseInterface()))
    {
        _iface.RegisterMethod("ListAvailableDrivers", [&](const auto& m) { OnListAvailableDrivers(m); });
        _iface.RegisterMethod("ListAvailableAdditionalDrivers", [&](const auto& m) { OnListAvailableAdditionalDrivers(m); });

        std::vector<DriverInfo> foundDrivers;
        try
        {
            foundDrivers = ParseDriverInfoFile("/etc/jappeos_core/driver_manager/drivers_list.json");
        }
        catch (const json::exception& e)
        {
            Log().Err("Failed to parse driver list: " + std::string(e.what()));
        }
        catch (const std::exception& e)
        {
            Log().Err("Failed to load driver list: " + std::string(e.what()));
        }

        std::map<std::string, DriverInfo> finalDrivers;
        for (const auto& driver : foundDrivers)
        {
            if (finalDrivers.contains(driver.name))
            {
                Log().Warn("Skipping duplicate driver name: " + driver.name);
                continue;
            }

            finalDrivers.emplace(driver.name, driver);
        }

        _drivers = finalDrivers;

        if (!HardwareFingerprintChanged())
            return;

        Log().Info("Hardware fingerprint changed. Static drivers will be installed.");

        _newConnSub = _serviceManager
                ->Get<NetworkManager::NetworkManagerService>()
                ->OnConnectionAdded.Subscribe([&](const ObjectPath&) { HandleNetworkConnectionAdded(); });
        _newConnSubExists = true;

        TryInstallStaticDriversWithRetry([&](const bool success)
        {
            if (success)
                UnsubscribeNetworkConnectionAdded();
        });
    }

    DriverManagerService::~DriverManagerService()
    {
        UnsubscribeNetworkConnectionAdded();
    }

    void DriverManagerService::OnListAvailableDrivers(const Message& message) const
    {
        auto msg = Message::CreateMethodReturn(message);

        std::vector<std::string> list;
        for (const auto &val: _drivers | std::views::values)
            list.push_back(val.name);

        msg.SetArgs(list);
        msg.Send(*_conn);
    }

    void DriverManagerService::OnListAvailableAdditionalDrivers(const Message& message) const
    {
        auto msg = Message::CreateMethodReturn(message);

        std::vector<std::string> list;
        for (const auto &val: _drivers | std::views::values)
        {
            if (val.type == DriverType::Additional)
                list.push_back(val.name);
        }

        msg.SetArgs(list);
        msg.Send(*_conn);
    }

    void DriverManagerService::EmitAdditionalDriverAvailable(const std::string& name)
    {
        auto sig = Message::CreateSignal(
            GetBaseObjectPath(),
            GetBaseInterface(),
            "AdditionalDriverAvailable"
        );

        sig.SetArgs(name);
        sig.Send(*_conn);
    }

    void DriverManagerService::HandleNetworkConnectionAdded()
    {
        TryInstallStaticDriversWithRetry([&](const bool success)
        {
            if (success)
                UnsubscribeNetworkConnectionAdded();
        });
    }

    void DriverManagerService::UnsubscribeNetworkConnectionAdded()
    {
        if (!_newConnSubExists)
            return;

        _serviceManager
                ->Get<NetworkManager::NetworkManagerService>()
                ->OnConnectionAdded.Unsubscribe(_newConnSub);
        _newConnSubExists = false;
    }

    bool DriverManagerService::TryInstallStaticDriversWithRetry(std::function<void(bool)> resultCallback)
    {
        if (_tryingToInstallStaticDriversWithRetry)
            return false;

        _tryingToInstallStaticDriversWithRetry = true;
        _tryInstallStaticDriversWithRetryCount = 0;

        struct UserData
        {
            DriverManagerService* instance;
            std::function<void(bool)> resultCallback;
        };

        auto* userdata = new UserData
        {
            .instance = this,
            .resultCallback = std::move(resultCallback),
        };

        g_timeout_add_seconds_full(
            G_PRIORITY_DEFAULT,
            5,
            [](gpointer data) -> gboolean
            {
                const auto* userObj = static_cast<UserData*>(data);
                if (userObj->instance->_tryInstallStaticDriversWithRetryCount >= TRY_INSTALL_STATIC_DRIVERS_RETRY_COUNT)
                {
                    userObj->instance->_tryingToInstallStaticDriversWithRetry = false;
                    Log().Notice("TryInstallStaticDriversWithRetry: Max retry count reached.");
                    userObj->resultCallback(false);
                    return G_SOURCE_REMOVE;
                }

                try
                {
                    CheckPackages();
                }
                catch (std::exception& e)
                {
                    userObj->instance->_tryInstallStaticDriversWithRetryCount++;
                    Log().Notice("TryInstallStaticDriversWithRetry: CheckPackages failed (retrying): " + std::string(e.what()));
                    return G_SOURCE_CONTINUE;
                }

                try
                {
                    userObj->instance->CleanupStaticDrivers();
                    for (const auto &val: userObj->instance->_drivers | std::views::values)
                    {
                        if (val.type != DriverType::Static)
                            continue;
                        InstallDriver(val);
                    }
                }
                catch (std::exception& e)
                {
                    Log().Crit("TryInstallStaticDriversWithRetry: Driver installation failed, even though CheckPackages succeeded, therefore drivers will not be installed: " + std::string(e.what()));
                }

                userObj->instance->_tryingToInstallStaticDriversWithRetry = false;
                Log().Info("TryInstallStaticDriversWithRetry: Done.");
                userObj->resultCallback(true);
                return G_SOURCE_REMOVE;
            },
            userdata,
            [](gpointer data)
            {
                delete static_cast<UserData*>(data);
            }
        );

        return true;
    }

    void DriverManagerService::CleanupStaticDrivers()
    {
        for (const auto &val: _drivers | std::views::values)
        {
            if (val.type != DriverType::Static)
                continue;

            for (const auto& pkg : val.packages)
            {
                if (!TryRemovePackage(pkg))
                {
                    Log().Notice(std::format(
                        "CleanupStaticDrivers: Failed removing package `{}` while cleaning up packages for driver `{}`. Some other package might depend on it.",
                        pkg,
                        val.name
                    ));
                }
            }
        }
    }

    std::vector<DriverManagerService::DriverInfo> DriverManagerService::ParseDriverInfoFile(const std::string& filePath)
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            throw std::runtime_error("Failed to open driver info file: " + filePath);
        }

        const json j = json::parse(file, nullptr, true, true);
        return j.get<std::vector<DriverInfo>>();
    }

    bool DriverManagerService::InstallDriver(const DriverInfo& driver)
    {
        if (!HasDeviceForDriver(driver.deviceFinder))
            return false;

        Log().Info("Installing driver: " + driver.name);

        bool anySuccess = false;
        bool allSuccess = true;
        for (const auto& pkg : driver.packages)
        {
            try
            {
                InstallPackage(pkg);
                anySuccess = true;
            }
            catch (const std::exception& e)
            {
                Log().Err(std::format(
                    "Failed to install package `{}` required by driver `{}`: {}",
                    pkg,
                    driver.name,
                    e.what()
                ));
                allSuccess = false;
            }
        }

        if (!anySuccess)
        {
            Log().Err("Failed to install driver: " + driver.name);
            return false;
        }

        if (!allSuccess)
            Log().Notice("Driver was partially installed (missing packages): " + driver.name);
        else
            Log().Notice("Driver was installed successfully: " + driver.name);

        return true;
    }

    bool DriverManagerService::HasDeviceForDriver(const PathDriverDeviceFinderInfo& finder)
    {
        std::regex entryRegex(finder.entryRegex);

        for (const auto& entry : std::filesystem::directory_iterator(finder.path))
        {
            if (!entry.is_directory())
                continue;

            const std::string name = entry.path().filename().string();

            if (!std::regex_search(name, entryRegex))
                continue;

            bool matches = true;

            for (const auto& content : finder.contentMatches)
            {
                std::filesystem::path filePath = entry.path() / content.path;

                std::ifstream file(filePath);
                if (!file)
                {
                    matches = false;
                    break;
                }

                std::string value;
                std::getline(file, value);

                if (!std::regex_search(value, std::regex(content.regex)))
                {
                    matches = false;
                    break;
                }
            }

            if (matches)
                return true;
        }

        return false;
    }

    bool DriverManagerService::HardwareFingerprintChanged()
    {
        const std::string fingerprintPath = "/etc/jappeos_core/driver_manager/hardware.txt";

        const std::string lastFingerprint = ReadFile(fingerprintPath);
        const std::string currentFingerprint = GetCurrentHardwareFingerprint();

        if (lastFingerprint == currentFingerprint)
            return false;

        if (!WriteFile(fingerprintPath, currentFingerprint))
            Log().Err("Failed to write current fingerprint to: " + fingerprintPath);

        return true;
    }

    std::string DriverManagerService::GetCurrentHardwareFingerprint()
    {
        std::ostringstream fingerprint;

        // -----------------
        // GPU(s)
        // -----------------

        std::vector<std::filesystem::path> cards;

        for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm"))
        {
            if (!entry.is_directory())
                continue;

            auto name = entry.path().filename().string();

            // card0, card1...
            if (name.rfind("card", 0) != 0)
                continue;

            // Ignore connectors like card0-HDMI-A-1
            if (name.find('-') != std::string::npos)
                continue;

            cards.push_back(entry.path());
        }

        std::sort(cards.begin(), cards.end());

        for (const auto& card : cards)
        {
            fingerprint
                << ReadFile(card / "device/vendor")
                << ReadFile(card / "device/device");
        }

        // -----------------
        // CPU
        // -----------------

        fingerprint
            << ReadFile("/sys/devices/system/cpu/cpu0/topology/physical_package_id");

        std::ifstream cpuinfo("/proc/cpuinfo");
        std::string line;

        while (std::getline(cpuinfo, line))
        {
            if (line.starts_with("vendor_id"))
            {
                fingerprint << line.substr(line.find(':') + 1);
            }
            else if (line.starts_with("cpu family"))
            {
                fingerprint << line.substr(line.find(':') + 1);
            }
            else if (line.starts_with("model\t"))
            {
                fingerprint << line.substr(line.find(':') + 1);
            }
            else if (line.starts_with("stepping"))
            {
                fingerprint << line.substr(line.find(':') + 1);
                break; // only need first CPU
            }
        }

        return fingerprint.str();
    }

    void DriverManagerService::InstallPackage(const std::string& package)
    {
        Utils::CommandRunner::RunOrThrow({
            "pacman",
            "-Syu",
            package,
        });
    }

    bool DriverManagerService::TryRemovePackage(const std::string& package)
    {
        try
        {
            Utils::CommandRunner::RunOrThrow({
                "pacman",
                "-Rs",
                package,
            });
        }
        catch (...)
        {
            return false;
        }

        return true;
    }

    void DriverManagerService::CheckPackages()
    {
        Utils::CommandRunner::RunOrThrow({
            "pacman",
            "-Sy",
            "--dbonly",
        });
    }

    std::string DriverManagerService::ReadFile(const std::filesystem::path& path)
    {
        std::ifstream file(path);
        if (!file)
            return {};

        std::string value;
        std::getline(file, value);

        // Remove whitespace/newlines
        std::erase_if(value, ::isspace);

        return value;
    }

    bool DriverManagerService::WriteFile(const std::filesystem::path& path, const std::string& content)
    {
        std::ofstream file(path, std::ios::out | std::ios::trunc);
        if (!file)
            return false;

        file << content;
        file.close();

        return file.good();
    }

}