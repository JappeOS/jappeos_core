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

#include "system_setup_service.h"

#include <filesystem>

#include "../../utils/os_utils.h"
#include "../locale/locale_service.h"
#include "../account_manager/account_manager_service.h"
#include "../installer/init_boot_file.h"
#include "../installer/installer_service.h"

namespace fs = std::filesystem;

namespace JappeStudios::JappeOS::JappeOSCore::Services::SystemSetup
{

    SystemSetupService::SystemSetupService(ServiceManager* serviceManager,
                                           Connection* conn) :
                                           Service(serviceManager, conn)
    {
        HandleInitialBootup();
    }

    SystemSetupService::~SystemSetupService() = default;

    void SystemSetupService::HandleInitialBootup() const
    {
        if (Utils::OsUtils::IsLiveOrInstallationEnvironment())
        {
            Log().Notice("We are in a Live-environment, initial boot files will not be read.");

            try
            {
                const auto accountService = _serviceManager->Get<AccountManager::AccountManagerService>();

                if (accountService == nullptr)
                    throw std::runtime_error("Account service is not available");

                const auto user = accountService->AddUser(JOS_LIVE_USER_NAME, JOS_LIVE_USER_REAL_NAME);
                accountService->SetUserPasswordMode(user, 2);
                accountService->SetUserLocked(user, false);
            }
            catch (const std::exception& ex)
            {
                Log().Err("Failed to create live-user during initial live environment setup: " + std::string(ex.what()));
            }

            return;
        }

        if (!std::filesystem::exists(STORAGE_SYSTEM_FILE_INIT_BOOT_PATH))
        {
            Log().Info(
                std::string(STORAGE_SYSTEM_FILE_INIT_BOOT_PATH) + " does not exist. Continuing regular boot."
            );
            return;
        }

        Log().Notice(
            std::string(STORAGE_SYSTEM_FILE_INIT_BOOT_PATH)
            + " exists. Finishing initial setup, then continuing regular boot."
        );

        try
        {
            auto reader = Installer::InitBootFile::Reader(STORAGE_SYSTEM_FILE_INIT_BOOT_PATH);
            const auto localeService = _serviceManager->Get<Locale::LocaleService>();
            const auto accountService = _serviceManager->Get<AccountManager::AccountManagerService>();

            if (localeService == nullptr || accountService == nullptr)
                throw std::runtime_error("Locale or account service(s) are not available");

            try
            {
                localeService->SetLocale(reader.ReadLocale());
            }
            catch (const std::exception& ex)
            {
                Log().Err("Failed to set locale during initial setup: " + std::string(ex.what()));
            }

            try
            {
                localeService->SetTimezone(reader.ReadTimezone());
            }
            catch (const std::exception& ex)
            {
                Log().Err("Failed to set timezone during initial setup: " + std::string(ex.what()));
            }

            // TODO: Keyboard layout
            Log().Notice("Skipping keyboard layout during initial setup.");

            try
            {
                auto setHostnameMsg = Message::CreateMethodCall(
                    "org.freedesktop.hostname1",
                    ObjectPath("/org/freedesktop/hostname1"),
                    InterfaceName("org.freedesktop.hostname1"),
                    "SetStaticHostname"
                );

                setHostnameMsg.SetArgs(reader.ReadHostname(), false);
                setHostnameMsg.SendWithReplyIgnore(*_conn);
            }
            catch (const std::exception& ex)
            {
                Log().Err("Failed to set hostname during initial setup: " + std::string(ex.what()));
            }

            try
            {
                // TODO: Allow user to input RealName during installation
                // TODO: Check validity
                const auto user = accountService->AddUser(reader.ReadUsername(), reader.ReadUsername());
                if (!reader.ReadPassword().empty())
                    accountService->SetUserPassword(user, reader.ReadPassword(), "");
            }
            catch (const std::exception& ex)
            {
                Log().Err("Failed to create user during initial setup: " + std::string(ex.what()));
            }

            reader.Close();
        }
        catch (const std::exception& ex)
        {
            Log().Err("Failed to read " + std::string(STORAGE_SYSTEM_FILE_INIT_BOOT_PATH) + ": " + ex.what());
        }

        try
        {
            std::filesystem::remove(STORAGE_SYSTEM_FILE_INIT_BOOT_PATH);
            Log().Info("Removed " + std::string(STORAGE_SYSTEM_FILE_INIT_BOOT_PATH));
        }
        catch (const std::filesystem::filesystem_error& ex)
        {
            Log().Err(
                "Failed to remove " + std::string(STORAGE_SYSTEM_FILE_INIT_BOOT_PATH) + ": " + std::string(ex.what())
            );
        }
    }

}
