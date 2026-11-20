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

#include "notification_server.h"

#include <ranges>

#include "daemon.h"

namespace JappeStudios::JappeOS::NotificationManager
{

    const char* NotificationServer::SERVER_NAME = "JappeOS Notification Server";
    const char* NotificationServer::SERVER_VENDOR = "JappeOS";
    const char* NotificationServer::SERVER_VERSION = "1.0.0";
    const char* NotificationServer::SERVER_SPEC_VERSION = "1.3";

    const ObjectPath NotificationServer::BASE_OBJECT = ObjectPath("/org/freedesktop/Notifications");
    const InterfaceName NotificationServer::BASE_INTERFACE = InterfaceName("org.freedesktop.Notifications");
    const std::vector<std::string> NotificationServer::CAPABILITIES = {"body", "persistence"};
    const uint32_t NotificationServer::MAX_COUNT = 30;

    NotificationServer::NotificationServer() :
        _conn(DaemonCore::Daemon::GetConnection()),
        _object(_conn, BASE_OBJECT),
        _iface(_object.CreateInterface(BASE_INTERFACE))
    {
        _iface.RegisterMethod("GetCapabilities",      [&] (const auto& m) { OnGetCapabilities(m); });
        _iface.RegisterMethod("GetServerInformation", [&] (const auto& m) { OnGetServerInformation(m); });
        _iface.RegisterMethod("Notify",               [&] (const auto& m) { OnNotify(m); });
        _iface.RegisterMethod("CloseNotification",    [&] (const auto& m) { OnCloseNotification(m); });
    }

    NotificationServer::~NotificationServer()
    {
        for (const auto& it : _notificationExpirationTasks)
        {
            if (it == 0) continue;
            g_source_remove(it);
        }
    }

    void NotificationServer::InvokeAction(const uint32_t notificationId, size_t actionId) const
    {
        const auto& it = _notificationsByUid.find(notificationId);
        if (it == _notificationsByUid.end())
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Notification not found, id=" + std::to_string(notificationId));
        InvokeAction(*it->second, actionId);
    }

    void NotificationServer::InvokeAction(const Notification& notification, const size_t actionId) const
    {
        const auto action = notification.actions.at(actionId);
        auto sig = Message::CreateSignal(
            _object.GetPath(),
            _iface.GetName(),
            "ActionInvoked"
        );
        sig.SetArgs(notification.id, action.key);
        sig.Send(_conn);
    }

    bool NotificationServer::NotificationExists(const uint32_t notificationId)
    {
        std::map<uint32_t, std::shared_ptr<const Notification>>::iterator it;
        std::map<std::string, std::map<uint32_t, std::shared_ptr<const Notification>>>::iterator itSender;
        return NotificationExistsWithIterators(notificationId, it, itSender);
    }

    bool NotificationServer::DismissNotification(const uint32_t notificationId)
    {
        const auto& it = _notificationsByUid.find(notificationId);
        if (it == _notificationsByUid.end())
            throw DBusException(DBUS_ERROR_INVALID_ARGS, "Notification not found, id=" + std::to_string(notificationId));
        return DismissNotification(*it->second);
    }

    bool NotificationServer::DismissNotification(const Notification& notification)
    {
        return RemoveNotification(notification.id, NotificationRemoveReason::Dismissed);
    }

    void NotificationServer::ClearNotifications()
    {
        std::vector<uint32_t> notificationIds;
        for (const auto& key: _notificationsByUid | std::views::keys)
            notificationIds.push_back(key);

        for (const auto& it : notificationIds)
        {
            try
            { RemoveNotification(it, NotificationRemoveReason::Dismissed); }
            catch (const std::exception& ex)
            { Log().Err("ClearNotifications: Failed to dismiss notification: " + std::string(ex.what())); }
        }
    }

    std::shared_ptr<const Notification> NotificationServer::GetNotification(const uint32_t notificationId) const
    {
        const auto& it = _notificationsByUid.find(notificationId);
        if (it == _notificationsByUid.end())
            return nullptr;

        return it->second;
    }

    std::vector<std::shared_ptr<const Notification>> NotificationServer::GetNotifications() const
    {
        std::vector<std::shared_ptr<const Notification>> notifications;
        for (const auto &val: _notificationsByUid | std::views::values)
            notifications.push_back(val);
        return notifications;
    }

    std::vector<std::shared_ptr<const Notification>> NotificationServer::GetNotificationsBySender(const std::string& sender) const
    {
        std::vector<std::shared_ptr<const Notification>> notifications;
        for (const auto &val: _notificationsBySender.at(sender) | std::views::values)
            notifications.push_back(val);
        return notifications;
    }

    void NotificationServer::OnGetCapabilities(const Message& message) const
    {
        auto msg = Message::CreateMethodReturn(message);
        msg.SetArgs(CAPABILITIES);
        msg.Send(_conn);
    }

    void NotificationServer::OnGetServerInformation(const Message& message) const
    {
        auto msg = Message::CreateMethodReturn(message);
        msg.SetArgs(
            std::string(SERVER_NAME),
            std::string(SERVER_VENDOR),
            std::string(SERVER_VERSION),
            std::string(SERVER_SPEC_VERSION)
        );
        msg.Send(_conn);
    }

    void NotificationServer::OnNotify(const Message& message)
    {
        const auto [
            appName,
            replacesId,
            appIcon,
            summary,
            body,
            actions,
            hints,
            expireTimeout
        ] = message.GetArgs<
            std::string,
            uint32_t,
            std::string,
            std::string,
            std::string,
            std::vector<std::string>,
            std::map<std::string, DBusVariant>,
            int>();

        const auto& sender = message.GetSender();
        uint32_t notificationId = replacesId;
        if (notificationId == 0)
            notificationId = _currentUid++;

        std::vector<NotificationAction> actionsNew{};
        bool isId = true;
        std::string currKey;
        for (const auto& action : actions)
        {
            if (isId)
            {
                currKey = action;
            }
            else
            {
                actionsNew.push_back(NotificationAction{
                    .key = currKey,
                    .name = action,
                });
            }

            isId = !isId;
        }

        AddNotification(Notification{
            .id = notificationId,
            .sender = sender,
            .appName = appName,
            .appIcon = appIcon,
            .summary = summary,
            .body = body,
            .actions = actionsNew,
            .expireTimeout = expireTimeout
        });

        auto ret = Message::CreateMethodReturn(message);
        ret.SetArgs(notificationId);
        ret.Send(_conn);
    }

    void NotificationServer::OnCloseNotification(const Message& message)
    {
        const auto [id] = message.GetArgs<uint32_t>();
        const auto& sender = message.GetSender();

        const auto& it = _notificationsByUid.find(id);
        const std::string foundSender = it != _notificationsByUid.end() ? it->second->sender : "";

        if (sender != foundSender)
            throw DBusException(DBUS_ERROR_FAILED);

        if (!RemoveNotification(id, NotificationRemoveReason::CloseNotificationCalled))
            throw DBusException(DBUS_ERROR_FAILED);

        Message::CreateMethodReturn(message).Send(_conn);
    }

    void NotificationServer::AddNotification(Notification notification)
    {
        auto notif = std::make_shared<const Notification>(std::move(notification));
        if (notif->id == 0)
            throw DBusException(DBUS_ERROR_FAILED, "id must be more than 0");

        // Insert notifications to maps
        auto [senderIt, senderInserted] =
            _notificationsBySender[notif->sender].insert_or_assign(notif->id, notif);
        auto [uidIt, uidInserted] =
            _notificationsByUid.insert_or_assign(notif->id, notif);

        // Fire events for added/updated notification
        if (!senderInserted || !uidInserted)
        {
            try
            { OnNotificationUpdated(notif->id); }
            catch (const std::exception& e)
            { Log().Err("OnNotificationUpdated event handler failed: " + std::string(e.what())); }
        }
        else
        {
            try
            { OnNotificationAdded(notif->id); }
            catch (const std::exception& e)
            { Log().Err("OnNotificationAdded event handler failed: " + std::string(e.what())); }
        }

        // Remove old notifications
        if (_notificationsByUid.size() > MAX_COUNT)
        {
            try
            {
                RemoveNotification(
                    _notificationsByUid.begin()->second->id,
                    NotificationRemoveReason::MaxCountReached
                );
            }
            catch (const std::exception& e)
            {
                Log().Err("Failed to remove notification because of MaxCountReached: " + std::string(e.what()));
            }
        }

        // Register timeout task for notification expiration
        guint timeout = 0;
        if (notif->expireTimeout == -1)
        {
            // TODO: Server-preferred notification timeouts
        }
        else
        {
            timeout = notif->expireTimeout;
        }

        if (timeout < 1)
            return;

        struct TimeoutTask { const uint32_t id; NotificationServer* server; };

        const auto timeoutTask = g_timeout_add_full(
            G_PRIORITY_DEFAULT,
            timeout,
            [](gpointer data) -> gboolean {
                const auto* task = static_cast<TimeoutTask*>(data);
                try
                {
                    task->server->RemoveNotification(task->id, NotificationRemoveReason::Expired);
                }
                catch (const std::exception& ex)
                {
                    Log().Notice("Failed to remove notification after expiration for id " + std::to_string(task->id) + ": " + ex.what());
                }
                return G_SOURCE_REMOVE;
            },
            new TimeoutTask(notification.id, this),
            [](gpointer data) {
                delete static_cast<TimeoutTask*>(data);
            }
        );

        _notificationExpirationTasks.push_back(timeoutTask);
    }

    bool NotificationServer::NotificationExistsWithIterators(
            const uint32_t notificationId,
            std::map<uint32_t, std::shared_ptr<const Notification>>::iterator& it,
            std::map<std::string, std::map<uint32_t, std::shared_ptr<const Notification>>>::iterator& itSender)
    {
        it = _notificationsByUid.find(notificationId);
        if (it == _notificationsByUid.end())
            return false;

        itSender = _notificationsBySender.find(it->second->sender);
        if (itSender == _notificationsBySender.end())
            return false;

        const auto& itSenderObj = itSender->second.find(notificationId);
        if (itSenderObj == itSender->second.end())
            return false;

        return true;
    }

    bool NotificationServer::RemoveNotification(const uint32_t notificationId, NotificationRemoveReason reason)
    {
        std::map<uint32_t, std::shared_ptr<const Notification>>::iterator it;
        std::map<std::string, std::map<uint32_t, std::shared_ptr<const Notification>>>::iterator itSender;
        if (!NotificationExistsWithIterators(notificationId, it, itSender))
            return false;

        ScopeGuard guard([&]
        {
            _notificationsByUid.erase(it);
            itSender->second.erase(notificationId);
            if (itSender->second.empty())
                _notificationsBySender.erase(itSender);
        });

        try
        { OnNotificationRemoved(notificationId); }
        catch (const std::exception& e)
        { Log().Err("OnNotificationRemoved event handler failed: " + std::string(e.what())); }

        auto sig = Message::CreateSignal(
            _object.GetPath(),
            _iface.GetName(),
            "NotificationClosed"
        );
        sig.SetArgs(notificationId, static_cast<uint32_t>(reason));
        sig.Send(_conn);

        return true;
    }

    void NotificationServer::ArchiveNotification(const Notification& notification)
    {
        AddNotification(Notification{
            .id = notification.id,
            .sender = notification.sender,
            .archived = true,
            .appName = notification.appName,
            .appIcon = notification.appIcon,
            .summary = notification.summary,
            .body = notification.body,
            .actions = {},
            .expireTimeout = 0
        });
    }

}
