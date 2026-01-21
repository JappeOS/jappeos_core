/*
 * jappeos_core, Core system management daemon for JappeOS.
 * Copyright (C) 2026  Jappe02
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

#include "power_manager_service.h"

#include "../../utils/dbus_utils.h"
#include "../logger/logger_service.h"
#include "../session_manager/session_manager_service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::PowerManager
{

    PowerManagerService::PowerManagerService(ServiceManager* serviceManager, Connection* conn) :
        Service(serviceManager, conn),
        _mainObject(*_conn, GetBaseObjectPath())
    {
        auto& iface = _mainObject.CreateInterface(GetBaseInterface());
        iface.RegisterMethod("Shutdown", [&](const auto& m) { OnShutdown(m); });
        iface.RegisterMethod("Reboot",   [&](const auto& m) { OnReboot(m); });
        iface.RegisterMethod("Suspend",  [&](const auto& m) { OnSuspend(m); });
    }

    PowerManagerService::~PowerManagerService() = default;

    // TODO: Polkit
    void PowerManagerService::SharedPolicy(const Message& message) const
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

    void PowerManagerService::OnShutdown(const Message& message) const
    {
        SharedPolicy(message);

        auto fwdMsg = Message::CreateMethodCall(
            "org.freedesktop.login1",
            ObjectPath("/org/freedesktop/login1"),
            InterfaceName("org.freedesktop.login1.Manager"),
            "PowerOff"
        );

        fwdMsg.SetArgs(false);
        fwdMsg.SendWithReplyIgnore(*_conn);
        Message::CreateMethodReturn(message).Send(*_conn);
    }

    void PowerManagerService::OnReboot(const Message& message) const
    {
        SharedPolicy(message);

        auto fwdMsg = Message::CreateMethodCall(
            "org.freedesktop.login1",
            ObjectPath("/org/freedesktop/login1"),
            InterfaceName("org.freedesktop.login1.Manager"),
            "Reboot"
        );

        fwdMsg.SetArgs(false);
        fwdMsg.SendWithReplyIgnore(*_conn);
        Message::CreateMethodReturn(message).Send(*_conn);
    }

    void PowerManagerService::OnSuspend(const Message& message) const
    {
        SharedPolicy(message);

        auto fwdMsg = Message::CreateMethodCall(
            "org.freedesktop.login1",
            ObjectPath("/org/freedesktop/login1"),
            InterfaceName("org.freedesktop.login1.Manager"),
            "Suspend"
        );

        fwdMsg.SetArgs(false);
        fwdMsg.SendWithReplyIgnore(*_conn);
        Message::CreateMethodReturn(message).Send(*_conn);
    }

}
