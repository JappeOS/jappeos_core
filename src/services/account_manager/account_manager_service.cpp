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

#include "account_manager_service.h"
#include "../../utils/dbus_utils.h"
#include "../logger/logger_service.h"
#include "../session_manager/session_manager_service.h"
#include <cerrno>
#include <grp.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#include <shadow.h>

namespace JappeStudios::JappeOS::JappeOSCore::Services::AccountManager
{

    AccountManagerService::AccountManagerService(ServiceManager* serviceManager, Connection* conn) :
                                                       Service(serviceManager, conn),
                                                       _object(*_conn, GetBaseObjectPath()),
                                                       _iface(_object.CreateInterface(GetBaseInterface()))
    {
        _iface.RegisterMethod("CreateInitialUserWithPassword", [&](const auto& m) { OnCreateInitialUserWithPassword(m); });
        _iface.RegisterMethod("ListUsers",                     [&](const auto& m) { OnListUsers(m); });
        _iface.RegisterMethod("GetUserProperty",               [&](const auto& m) { OnGetUserProperty(m); });
    }

    AccountManagerService::~AccountManagerService() = default;

    ObjectPath AccountManagerService::AddUser(const std::string& username,
                                              const std::string& realName,
                                              const bool cache) const
    {
        if (username.empty() || realName.empty())
        {
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Username or realName cannot be empty.");
        }

        auto fwdMsg = Message::CreateMethodCall(
            "org.freedesktop.Accounts",
            ObjectPath("/org/freedesktop/Accounts"),
            InterfaceName("org.freedesktop.Accounts"),
            "CreateUser"
        );

        fwdMsg.SetArgs(username, realName, 1);
        const auto reply = fwdMsg.SendWithReply(*_conn);
        const auto [obj] = reply.GetArgs<ObjectPath>();

        try
        {
            if (cache)
                CacheUser(username);
        }
        catch (...)
        {
            Log().Warn("User `" + username + "` created, but not cached.");
        }

        SetUserGroups(username);
        return obj;
    }

    void AccountManagerService::RemoveUser(const int64_t id, const bool removeFiles) const
    {
        auto fwdMsg = Message::CreateMethodCall(
            "org.freedesktop.Accounts",
            ObjectPath("/org/freedesktop/Accounts"),
            InterfaceName("org.freedesktop.Accounts"),
            "DeleteUser"
        );

        fwdMsg.SetArgs(id, removeFiles);
        fwdMsg.SendWithReplyIgnore(*_conn);
    }

    std::vector<ObjectPath> AccountManagerService::ListUsers() const
    {
        const auto fwdMsg = Message::CreateMethodCall(
            "org.freedesktop.Accounts",
            ObjectPath("/org/freedesktop/Accounts"),
            InterfaceName("org.freedesktop.Accounts"),
            "ListCachedUsers"
        );

        const auto reply = fwdMsg.SendWithReply(*_conn);
        const auto [obj] = reply.GetArgs<std::vector<ObjectPath>>();
        return obj;
    }

    void AccountManagerService::SetUserPassword(const ObjectPath& userObject,
                                                const std::string& cryptedPassword,
                                                const std::string& hint) const
    {
        auto fwdMsg = Message::CreateMethodCall(
            "org.freedesktop.Accounts",
            userObject,
            InterfaceName("org.freedesktop.Accounts.User"),
            "SetPassword"
        );

        fwdMsg.SetArgs(cryptedPassword, hint);
        fwdMsg.SendWithReplyIgnore(*_conn);
    }

    // TODO: Polkit
    void AccountManagerService::SharedPolicy(const Message& message) const
    {
        const auto sender = message.GetSender();
        const auto senderUid = _conn->GetUnixUser(sender);
        const pid_t senderPid = Utils::DBusUtils::GetSenderPID(_rawConn, message.GetRawMessage());

        const auto sessionMgr = _serviceManager->Get<SessionManager::SessionManagerService>();

        if (!sessionMgr->IsManagedUserSession(senderUid))
        {
            throw DBusException(DBUS_ERROR_ACCESS_DENIED, "Unknown user");
        }

        if (!sessionMgr->IsPrivilegedClientProcess(senderPid, true))
        {
            throw DBusException(DBUS_ERROR_ACCESS_DENIED, "Unauthorized process");
        }
    }

    void AccountManagerService::OnCreateInitialUserWithPassword(const Message& message) const
    {
        SharedPolicy(message);
        const auto [username, realName, cryptedPassword]
                = message.GetArgs<std::string, std::string, std::string>();

        if (empty(username) || empty(realName) || empty(cryptedPassword))
        {
            throw DBusException(
                DBUS_ERROR_INVALID_ARGS,
                "Username, realName and/or crypted password cannot be empty."
            );
        }

        const auto users = ListUsers();
        if (!users.empty())
        {
            throw DBusException(
                DBUS_ERROR_FAILED,
                "Cannot create initial user. There's more than 0 users present."
            );
        }

        const auto user = AddUser(username, realName, true);
        try
        {
            SetUserPassword(user, cryptedPassword, "");
        }
        catch (const std::exception& e)
        {
            Log().Err(std::format("Failed to set password of user `{}`: {}", username, e.what()));
            const auto id = GetUserProperty<uint64_t>(user, "Uid");
            RemoveUser(id, true);
            throw;
        }

        Message::CreateMethodReturn(message).Send(*_conn);
    }

    void AccountManagerService::OnListUsers(const Message& message) const
    {
        SharedPolicy(message);
        const auto users = ListUsers();
        auto reply = Message::CreateMethodReturn(message);
        reply.SetArgs(users);
        reply.Send(*_conn);
    }

    void AccountManagerService::OnGetUserProperty(const Message& message) const
    {
        SharedPolicy(message);
        const auto [userObject, property] = message.GetArgs<ObjectPath, std::string>();

        auto fwdMsg = Message::CreateMethodCall(
            "org.freedesktop.Accounts",
            userObject,
            InterfaceName("org.freedesktop.DBus.Properties"),
            "Get"
        );

        fwdMsg.SetArgs(std::string("org.freedesktop.Accounts.User"), property);
        const auto fwdReply = fwdMsg.SendWithReply(*_conn);
        const auto [v] = fwdReply.GetArgs<DBusVariant>();

        auto reply = Message::CreateMethodReturn(message);
        reply.SetArgs(v);
        reply.Send(*_conn);
    }

    void AccountManagerService::CacheUser(const std::string& username) const
    {
        auto fwdMsg = Message::CreateMethodCall(
            "org.freedesktop.Accounts",
            ObjectPath("/org/freedesktop/Accounts"),
            InterfaceName("org.freedesktop.Accounts"),
            "CacheUser"
        );

        fwdMsg.SetArgs(username);
        fwdMsg.SendWithReplyIgnore(*_conn);
    }

    void AccountManagerService::SetUserGroups(const std::string& username) const
    {
        for (const auto& group : USER_GROUPS)
        {
            try
            {
                const auto result = AddUserToGroup(username, group);
                if (result.alreadyMember)
                    Log().Notice(std::format("User `{}` is already a member of group `{}`.", username, group));
                if (result.skippedNonexistent)
                    Log().Notice(
                        std::format(
                            "User `{}` cannot be added to group `{}` because the group does not exist.",
                            username,
                            group
                        )
                    );
            }
            catch (const std::exception& e)
            {
                Log().Err(std::format(
                    "Failed to add user `{}` to group `{}`: {}",
                    username, group, e.what()
                ));
            }
        }
    }

    AccountManagerService::GroupAddResult AccountManagerService::AddUserToGroup(const std::string& username,
                                                                                const std::string& groupname)
    {
        // --- 1. Resolve the user's UID to validate they exist ---
        passwd  pwdBuf{};
        passwd* pwdResult = nullptr;
        std::vector<char> pwStrbuf(sysconf(_SC_GETPW_R_SIZE_MAX) > 0
                                        ? sysconf(_SC_GETPW_R_SIZE_MAX)
                                        : 16384);

        int rc = getpwnam_r(username.c_str(), &pwdBuf,
                            pwStrbuf.data(), pwStrbuf.size(), &pwdResult);
        if (rc != 0)
            throw std::runtime_error(
                std::format("getpwnam_r('{}') failed: {}", username, strerror(rc)));
        if (!pwdResult)
            throw std::runtime_error(
                std::format("User '{}' not found in passwd database", username));

        // --- 2. Look up the group ---
        group  grpBuf{};
        group* grpResult = nullptr;
        std::vector<char> grStrbuf(sysconf(_SC_GETGR_R_SIZE_MAX) > 0
                                        ? sysconf(_SC_GETGR_R_SIZE_MAX)
                                        : 16384);

        rc = getgrnam_r(groupname.c_str(), &grpBuf,
                        grStrbuf.data(), grStrbuf.size(), &grpResult);
        if (rc != 0)
            throw std::runtime_error(std::format(
                "getgrnam_r('{}') failed: {}", groupname, strerror(rc)));

        if (!grpResult)
            // Group simply doesn't exist on this system — caller decides policy.
            return {groupname, /*skipped_nonexistent=*/true, false};

        // --- 3. Check if user is already a member ---
        for (char** mem = grpResult->gr_mem; mem && *mem; ++mem)
        {
            if (username == *mem)
                return {groupname, false, /*already_member=*/true};
        }

        // --- 4. Build the new member list (existing members + new user) ---
        std::vector<const char*> newMembers;
        for (char** mem = grpResult->gr_mem; mem && *mem; ++mem)
            newMembers.push_back(*mem);
        newMembers.push_back(username.c_str());
        newMembers.push_back(nullptr); // null-terminate

        grpResult->gr_mem = const_cast<char**>(newMembers.data());

        // --- 5. Commit via lckpwdf / putgrent / ulckpwdf ---
        if (lckpwdf() != 0)
            throw std::runtime_error("lckpwdf() failed: could not lock password files");

        struct PwLockGuard
        {
            ~PwLockGuard() { ulckpwdf(); }
        } lock_guard;

        FILE* grf = fopen("/etc/group", "r+");
        if (!grf)
            throw std::runtime_error(
                std::format("fopen(/etc/group): {}", strerror(errno)));

        // Write to a temp file in the same directory, then rename atomically.
        const std::string tmpPath = "/etc/group.tmp";
        FILE* tmp = fopen(tmpPath.c_str(), "w");
        if (!tmp)
        {
            const int saved_errno = errno;
            fclose(grf);
            throw std::runtime_error(
                std::format("fopen({}) for write: {}", tmpPath, strerror(saved_errno)));
        }

        group lineBuf{};
        std::vector<char> lineStrbuf(16384);
        group* lineResult = nullptr;
        bool   written = false;

        while (true)
        {
            const int ret = fgetgrent_r(grf, &lineBuf,
                                        lineStrbuf.data(), lineStrbuf.size(),
                                        &lineResult);

            if (ret == 0)
            {
                if (!lineResult)
                    break; // EOF
            }
            else if (ret == ENOENT && !lineResult)
            {
                // Some libcs return ENOENT at EOF for fgetgrent_r.
                break;
            }
            else if (ret == ERANGE)
            {
                // Buffer too small; grow and retry.
                lineStrbuf.resize(lineStrbuf.size() * 2);
                continue;
            }
            else
            {
                fclose(grf);
                fclose(tmp);
                unlink(tmpPath.c_str());
                throw std::runtime_error(
                    std::format("fgetgrent_r(/etc/group) failed: {}", strerror(ret)));
            }

            // Rewrite our target group with the updated member list.
            if (std::string(lineResult->gr_name) == groupname)
            {
                lineResult->gr_mem = const_cast<char**>(newMembers.data());
                written = true;
            }

            if (putgrent(lineResult, tmp) != 0)
            {
                const int saved_errno = errno;
                fclose(grf);
                fclose(tmp);
                unlink(tmpPath.c_str());
                throw std::runtime_error(
                    std::format("putgrent failed: {}", strerror(saved_errno)));
            }
        }

        // If the group exists via NSS but not in /etc/group (e.g. vendor/system groups),
        // create/override an entry in /etc/group with the updated member list.
        if (!written)
        {
            if (!grpResult->gr_passwd)
                grpResult->gr_passwd = const_cast<char*>("x");

            if (putgrent(grpResult, tmp) != 0)
            {
                const int saved_errno = errno;
                fclose(grf);
                fclose(tmp);
                unlink(tmpPath.c_str());
                throw std::runtime_error(
                    std::format("putgrent failed while appending: {}", strerror(saved_errno)));
            }
            written = true;
        }

        fclose(grf);

        if (fflush(tmp) != 0 || fsync(fileno(tmp)) != 0)
        {
            fclose(tmp);
            unlink(tmpPath.c_str());
            throw std::runtime_error("fsync(/etc/group.tmp) failed");
        }
        fclose(tmp);

        // Atomic replace - rename(2) is atomic on Linux for same-filesystem paths.
        if (rename(tmpPath.c_str(), "/etc/group") != 0)
        {
            const int saved_errno = errno;
            unlink(tmpPath.c_str());
            throw std::runtime_error(
                std::format("rename({} -> /etc/group): {}", tmpPath, strerror(saved_errno)));
        }

        return {groupname, false, false};
    }

}
