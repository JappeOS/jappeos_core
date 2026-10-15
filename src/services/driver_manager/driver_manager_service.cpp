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
#include <glib.h>

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

        if (_drivers.empty())
            Log().Notice("No drivers found in the drivers_list.json file.");

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
        if (_tryingToInstallStaticDriversWithRetry || _drivers.empty())
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
                catch (...)
                {
                    userObj->instance->_tryInstallStaticDriversWithRetryCount++;
                    Log().Notice("TryInstallStaticDriversWithRetry: CheckPackages failed (retrying).");
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

        const std::string lastFingerprint = ReadFileTrimmed(fingerprintPath);
        const std::string currentFingerprint = GetCurrentHardwareFingerprint().value_or("?");

        if (lastFingerprint == currentFingerprint)
            return false;

        const std::filesystem::path dir = std::filesystem::path(fingerprintPath).parent_path();
        if (std::error_code ec; !create_directories(dir, ec) && ec)
            Log().Err("Failed to create directory for fingerprint: " + fingerprintPath);

        if (!WriteFile(fingerprintPath, currentFingerprint))
            Log().Err("Failed to write current fingerprint to: " + fingerprintPath);

        return true;
    }

    // nullopt = "couldn't determine", which is NOT the same as "hardware changed".
    std::optional<std::string> DriverManagerService::GetCurrentHardwareFingerprint()
    {
        // Every display-class PCI device, whether or not a driver is bound.
        const auto collectGpus = []() -> std::vector<std::string>
        {
            std::vector<std::string> out;
            std::error_code ec;

            for (std::filesystem::directory_iterator it("/sys/bus/pci/devices", ec), end;
                 !ec && it != end; it.increment(ec))
            {
                const auto& p = it->path();

                // class is e.g. "0x030000"; base class 0x03 = display controller
                // (covers VGA 0x0300, 3D 0x0302, other 0x0380)
                std::string cls = ReadFileTrimmed(p / "class");
                if (cls.compare(0, 4, "0x03") != 0)
                    continue;

                std::string vendor = ReadFileTrimmed(p / "vendor");
                std::string device = ReadFileTrimmed(p / "device");
                if (vendor.empty() || device.empty())
                    continue;

                out.push_back("gpu=" + vendor + ":" + device);
            }

            // Sort the content, not the names. Duplicates are kept on purpose:
            // two identical GPUs is a different config than one.
            std::sort(out.begin(), out.end());
            return out;
        };

        // Distinct CPU identities across all cores, so core count doesn't matter
        // and mixed setups (big.LITTLE, odd multi-socket) are covered.
        const auto collectCpus = []() -> std::set<std::string>
        {
            static const char* kKeys[] = {
                // x86
                "vendor_id", "cpu family", "model", "stepping",
                // ARM
                "CPU implementer", "CPU architecture", "CPU variant",
                "CPU part", "CPU revision",
            };

            std::set<std::string> out;
            std::map<std::string, std::string> block;
            std::ifstream f("/proc/cpuinfo");
            std::string line;

            auto flush = [&]
            {
                std::string s;
                for (const char* k : kKeys)
                    if (auto it = block.find(k); it != block.end())
                        s += std::string(k) + "=" + it->second + ",";
                if (!s.empty()) out.insert(s);
                block.clear();
            };

            while (std::getline(f, line))
            {
                if (TrimString(line).empty())
                {
                    flush();
                    continue;
                }
                auto colon = line.find(':');
                if (colon == std::string::npos)
                    continue;
                block[TrimString(line.substr(0, colon))] = TrimString(line.substr(colon + 1));
            }
            flush();
            return out;
        };

        const auto cpus = collectCpus();
        if (cpus.empty())
            return std::nullopt;

        std::ostringstream fp;
        fp << "v1;";                       // bump if you change the format
        for (const auto& g : collectGpus()) fp << g << ';';
        for (const auto& c : cpus)          fp << "cpu{" << c << "};";
        return fp.str();
    }

    void DriverManagerService::InstallPackage(const std::string& package)
    {
        Utils::CommandRunner::RunOrThrow({
            "pacman",
            "--noconfirm",
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
                "--noconfirm",
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
            "--noconfirm",
            "-Sy",
            "--dbonly",
        });
    }

    std::string DriverManagerService::ReadFileTrimmed(const std::filesystem::path& path)
    {
        std::ifstream file(path);
        if (!file)
            return {};

        std::string value;
        std::getline(file, value);

        // Remove whitespace/newlines
        return TrimString(value);
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

    std::string DriverManagerService::TrimString(const std::string& s)
    {
        const auto b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return {};
        const auto e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

}