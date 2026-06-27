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

#include "../service.h"
#include "../../logger.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::AccountManager
{
    class AccountManagerService : public Service
    {
    public:
        explicit AccountManagerService(ServiceManager* serviceManager, Connection* conn);
        ~AccountManagerService() override;
        [[nodiscard]] std::string GetName() const override { return "AccountManagerService"; }

        ObjectPath AddUser(const std::string& username,
                           const std::string& realName,
                           bool cache = true) const;
        void RemoveUser(int64_t id, bool removeFiles) const;
        std::vector<ObjectPath> ListUsers() const;

        template<typename T>
        T GetUserProperty(const ObjectPath& userObject, const std::string& property) const
        {
            auto fwdMsg = Message::CreateMethodCall(
                "org.freedesktop.Accounts",
                userObject,
                InterfaceName("org.freedesktop.DBus.Properties"),
                "Get"
            );

            fwdMsg.SetArgs(std::string("org.freedesktop.Accounts.User"), property);
            const auto reply = fwdMsg.SendWithReply(*_conn);
            const auto args = reply.GetArgs<DBusVariant>();
            return std::get<0>(args).get<T>();
        }

        void SetUserPassword(const ObjectPath& userObject,
                             const std::string& cryptedPassword,
                             const std::string& hint) const;

    public:
        const std::vector<std::string> USER_GROUPS = {
            "seat",
            "video",
            "render",
            "input",
        };

    private:
        Object _object;
        Interface& _iface;

        // D-Bus interface

        void SharedPolicy(const Message& message) const;

        void OnCreateInitialUserWithPassword(const Message& message) const;
        void OnListUsers(const Message& message) const;
        void OnGetUserProperty(const Message& message) const;

        // Internal methods

        void CacheUser(const std::string& username) const;
        void SetUserGroups(const std::string& username) const;

    private:
        /**
         * @brief Represents the result of attempting to add a user to a single group.
         */
        struct GroupAddResult
        {
            std::string group;
            bool        skippedNonexistent; // Group doesn't exist on this system
            bool        alreadyMember;      // User was already in the group
        };

        static JappeOSCore::Logger& Log()
        {
            static JappeOSCore::Logger instance{"AccountManagerService"};
            return instance;
        }

        /**
         * @brief Adds 'username' to 'groupname' by rewriting /etc/group via the standard
         *        fgetgrent_r / putgrent POSIX API. Must be run as root (or with CAP_SETGID).
         *
         * @return a GroupAddResult describing what happened.
         * @throws std::runtime_error on hard failures (I/O errors, user not found, etc.)
         */
        static GroupAddResult AddUserToGroup(const std::string& username,
                                             const std::string& groupname);
    };
}
