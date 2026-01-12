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
