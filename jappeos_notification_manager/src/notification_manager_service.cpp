/*
 * jappeos_notification_manager, Notification server for JappeOS.
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

#include "notification_manager_service.h"

namespace JappeStudios::JappeOS::NotificationManager
{

    // NotificationManagerService

    const ObjectPath NotificationManagerService::BASE_OBJECT = ObjectPath("/org/jappeos/NotificationManager1");
    const InterfaceName NotificationManagerService::BASE_INTERFACE = InterfaceName("org.jappeos.NotificationManager1");

    NotificationManagerService::NotificationManagerService() :
        _conn(DaemonCore::Daemon::GetConnection()),
        _object(_conn, BASE_OBJECT),
        _iface(_object.CreateInterface(BASE_INTERFACE))
    {
        _iface.RegisterMethod("GetNotifications",   [&](const Message& m){ OnGetNotifications(m); });
        _iface.RegisterMethod("ClearNotifications", [&](const Message& m){ OnClearNotifications(m); });

        _notificationServer
            .OnNotificationAdded
            .Subscribe([&](const uint32_t n)
            {
                const auto notif = _notificationServer.GetNotification(n);
                NULL_CHECK_VAR(notif, "notif");
                const auto obj = std::make_shared<NotificationObject>(_notificationServer, notif);
                _notifPathToObject.insert_or_assign(obj->GetObjectPath(), obj);
                _notifIdToObject.insert_or_assign(obj->GetNotification()->replacesId, obj);
                _notifIdsOrdered.push_back(obj->GetNotification()->replacesId);
                EmitNotificationAdded(n);
            });

        _notificationServer
            .OnNotificationUpdated
            .Subscribe([&](const uint32_t n)
            {
                const auto notif = _notificationServer.GetNotification(n);
                NULL_CHECK_VAR(notif, "notif");
                const auto obj = std::make_shared<NotificationObject>(_notificationServer, notif);
                _notifPathToObject.insert_or_assign(obj->GetObjectPath(), obj);
                _notifIdToObject.insert_or_assign(obj->GetNotification()->replacesId, obj);
                EmitNotificationUpdated(n);
            });

        _notificationServer
            .OnNotificationRemoved
            .Subscribe([&](const uint32_t n)
            {
                const auto notif = _notificationServer.GetNotification(n);
                NULL_CHECK_VAR(notif, "notif");
                const auto obj = GetNotification(n);
                _notifPathToObject.erase(obj->GetObjectPath());
                _notifIdToObject.erase(obj->GetNotification()->replacesId);
                std::erase(_notifIdsOrdered, obj->GetNotification()->replacesId);
                EmitNotificationRemoved(n);
            });
    }

    void NotificationManagerService::EmitNotificationAdded(const uint32_t notificationId) const
    {
        auto sig = Message::CreateSignal(
            _object.GetPath(),
            _iface.GetName(),
            "NotificationAdded"
        );
        sig.SetArgs(GetNotification(notificationId)->GetObjectPath());
        sig.Send(_conn);
    }

    void NotificationManagerService::EmitNotificationUpdated(const uint32_t notificationId) const
    {
        auto sig = Message::CreateSignal(
            _object.GetPath(),
            _iface.GetName(),
            "NotificationUpdated"
        );
        sig.SetArgs(GetNotification(notificationId)->GetObjectPath());
        sig.Send(_conn);
    }

    void NotificationManagerService::EmitNotificationRemoved(const uint32_t notificationId) const
    {
        auto sig = Message::CreateSignal(
            _object.GetPath(),
            _iface.GetName(),
            "NotificationRemoved"
        );
        sig.SetArgs(GetNotification(notificationId)->GetObjectPath());
        sig.Send(_conn);
    }

    void NotificationManagerService::OnGetNotifications(const Message& message) const
    {
        std::vector<ObjectPath> notificationPaths;
        for (const auto& id : _notifIdsOrdered)
            notificationPaths.push_back(GetNotification(id)->GetObjectPath());

        auto ret = Message::CreateMethodReturn(message);
        ret.SetArgs(notificationPaths);
        ret.Send(_conn);
    }

    void NotificationManagerService::OnClearNotifications(const Message& message)
    {
        _notificationServer.ClearNotifications();
        Message::CreateMethodReturn(message).Send(_conn);
    }

    std::shared_ptr<const Notification> NotificationManagerService::GetNotification(const ObjectPath& objectPath) const
    {
        const auto it = _notifPathToObject.find(objectPath);
        if (it == _notifPathToObject.end())
            throw DBusException(DBUS_ERROR_UNKNOWN_OBJECT, "Could not map notification object path to real object");
        const auto second = it->second;
        NULL_CHECK_VAR(second, "second");
        const auto notif = second->GetNotification();
        NULL_CHECK_VAR(notif, "notif");
        return notif;
    }

    std::shared_ptr<const NotificationObject> NotificationManagerService::GetNotification(const uint32_t id) const
    {
        const auto it = _notifIdToObject.find(id);
        if (it == _notifIdToObject.end())
            throw DBusException(DBUS_ERROR_UNKNOWN_OBJECT, "Could not map notification ID to real object");
        const auto second = it->second;
        NULL_CHECK_VAR(second, "second");
        return second;
    }

    // NotificationObject

    NotificationObject::NotificationObject(
        NotificationServer& notificationServer,
        const std::shared_ptr<const Notification>& source) :
        _conn(DaemonCore::Daemon::GetConnection()),
        _object(
            _conn,
            NotificationManagerService::BASE_OBJECT.Child("notifications").Child(std::to_string(source->replacesId))
        ),
        _iface(_object.CreateInterface(NotificationManagerService::BASE_INTERFACE.Child("Notification"))),
        _notificationServer(notificationServer),
        _notification(source)
    {
        _iface.RegisterProperty<std::string>("AppName", [&] { return _notification->appName; });
        _iface.RegisterProperty<std::string>("AppIcon", [&] { return _notification->appIcon; });
        _iface.RegisterProperty<std::string>("Summary", [&] { return _notification->summary; });
        _iface.RegisterProperty<std::string>("Body",    [&] { return _notification->body; });
        _iface.RegisterProperty<std::vector<std::string>>("ActionKeys", [&] { return GetActionKeys(); });

        _iface.RegisterMethod("InvokeAction",  [&](const Message& m){ OnInvokeAction(m); });
        _iface.RegisterMethod("Dismiss",       [&](const Message& m){ OnDismiss(m); });
        _iface.RegisterMethod("GetActionName", [&](const Message& m){ OnGetActionName(m); });
    }

    void NotificationObject::OnGetActionName(const Message& message) const
    {
        const auto [key] = message.GetArgs<std::string>();

        std::string name;
        for (const auto& action : _notification->actions)
        {
            if (action.key != key) continue;
            name = action.name;
            break;
        }

        auto ret = Message::CreateMethodReturn(message);
        ret.SetArgs(name);
        ret.Send(_conn);
    }

    void NotificationObject::OnInvokeAction(const Message& message) const
    {
        const auto [action] = message.GetArgs<size_t>();
        _notificationServer.InvokeAction(*GetNotification(), action);
        Message::CreateMethodReturn(message).Send(_conn);
    }

    void NotificationObject::OnDismiss(const Message& message) const
    {
        _notificationServer.DismissNotification(*GetNotification());
        Message::CreateMethodReturn(message).Send(_conn);
    }

    std::vector<std::string>& NotificationObject::GetActionKeys()
    {
        if (!_actionIds.empty())
            return _actionIds;

        for (const auto& action : _notification->actions)
            _actionIds.push_back(action.key);

        return _actionIds;
    }

}
