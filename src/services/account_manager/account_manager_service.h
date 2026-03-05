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

#include "../service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::AccountManager
{
    struct DBusValue
    {
        int dbusType;             // D-Bus type (e.g. DBUS_TYPE_STRING)
        std::string signature;    // Variant signature ("s", "i", etc.)
        std::variant<std::monostate, int32_t, std::uint32_t, bool, double, std::string> value;
    };

    class AccountManagerService : public Service
    {
    public:
        explicit AccountManagerService(ServiceManager* serviceManager, Connection* conn);
        ~AccountManagerService() override;

        bool HandleMethodCallLegacy(DBusMessage* msg) override;
        [[nodiscard]] std::string GetName() const override { return "AccountManagerService"; }

    private:
        void CreateInitialUserWithPasswordDbus(DBusMessage* pmsg,
                                               uid_t senderUid,
                                               pid_t senderPid,
                                               const std::string& username,
                                               const std::string& realName,
                                               const std::string& cryptedPassword);
        void AddUserDbus(DBusMessage* pmsg,
                         uid_t senderUid,
                         pid_t senderPid,
                         const std::string& username,
                         const std::string& realName);
        bool AddUser(const std::string& username,
                     const std::string& realName,
                     std::string& outObjectPath,
                     bool cache = true) const;
        void RemoveUser(DBusMessage* pmsg, uid_t senderUid, pid_t senderPid);
        void ListUsersDbus(DBusMessage* pmsg);
        bool ListUsers(std::vector<std::string>& outObjectPaths) const;
        void GetUserPropertyDbus(DBusMessage* pmsg, const std::string& userObject, const std::string& property);
        bool GetUserProperty(const std::string& userObject, const std::string& property, DBusValue& outValue) const;
        [[nodiscard]] bool CacheUser(const std::string& username) const;
        [[nodiscard]] bool SetUserPassword(const std::string& userObjectPath,
                                           const std::string& cryptedPassword,
                                           const std::string& hint) const;
    };
}
