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

#pragma once
#include "daemon.h"
#include "notification_server.h"
#include "logging/logger.h"

using namespace JappeStudios::JappeOS::DaemonCore::DBus;

namespace JappeStudios::JappeOS::NotificationManager
{
    class NotificationObject;

    /**
     * @brief Creates the notification server and signals to listeners via org.jappeos.NotificationManager1.
     */
    class NotificationManagerService : public DaemonCore::DaemonClass
    {
    public:
        explicit NotificationManagerService();
        //~NotificationManagerService();

    public:
        static const ObjectPath BASE_OBJECT;
        static const InterfaceName BASE_INTERFACE;

    private:
        Connection& _conn;
        Object      _object;
        Interface&  _iface;

        NotificationServer _notificationServer;
        std::unordered_map<ObjectPath, std::shared_ptr<NotificationObject>, ObjectPathHash> _notifPathToObject;
        std::unordered_map<uint32_t, std::shared_ptr<NotificationObject>> _notifIdToObject;
        std::vector<uint32_t> _notifIdsOrdered;

        void EmitNotificationAdded(uint32_t notificationId) const;
        void EmitNotificationUpdated(uint32_t notificationId) const;
        void EmitNotificationRemoved(uint32_t notificationId) const;

        void OnGetNotifications(const Message& message) const;
        void OnClearNotifications(const Message& message);

        /**
         * @param objectPath The object path to the notification object
         * @return A notification based on its object path, NEVER 'nullptr'
         */
        std::shared_ptr<const Notification> GetNotification(const ObjectPath& objectPath) const;

        /**
         * @param id The ID of the notification object
         * @return A notification based on its ID, NEVER 'nullptr'
         */
        std::shared_ptr<const NotificationObject> GetNotification(uint32_t id) const;

    private:
        CREATE_LOGGER("NotificationManagerService")
    };

    /**
     * @brief Represents an individual immutable notification object.
     */
    class NotificationObject
    {
    public:
        explicit NotificationObject(
            NotificationServer& notificationServer,
            const std::shared_ptr<const Notification>& source);
        std::shared_ptr<const Notification> GetNotification() const { return _notification; }
        ObjectPath GetObjectPath() const { return _object.GetPath(); }

    private:
        Connection& _conn;
        Object      _object;
        Interface&  _iface;

        NotificationServer&                 _notificationServer;
        std::shared_ptr<const Notification> _notification;
        std::vector<std::string>            _actionIds;

        void OnGetActionName(const Message& message) const;
        void OnInvokeAction(const Message& message) const;
        void OnDismiss(const Message& message) const;

        std::vector<std::string>& GetActionKeys();
    };

}