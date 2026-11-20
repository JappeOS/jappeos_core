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
#include <glib.h>

#include "dbus/dbus_wrapper.h"
#include "logging/logger.h"
#include "utils/event.h"
#include "utils/scope_guard.h"

using namespace JappeStudios::JappeOS::DaemonCore::DBus;
using namespace JappeStudios::JappeOS::DaemonCore::Utils;

namespace JappeStudios::JappeOS::NotificationManager
{
    struct NotificationAction
    {
        const std::string key;
        const std::string name;
    };

    struct Notification
    {
        const uint32_t id;
        const std::string sender;
        const bool archived = false;

        const std::string appName;
        const std::string appIcon;
        const std::string summary;
        const std::string body;
        const std::vector<NotificationAction> actions;
        const int expireTimeout;
    };

    enum class NotificationRemoveReason : uint32_t
    {
        /**
         * @brief The notification expired.
         */
        Expired = 1,

        /**
         * @brief The notification was dismissed by the user.
         */
        Dismissed = 2,

        /**
         * @brief The notification was closed by a call to CloseNotification.
         */
        CloseNotificationCalled = 3,

        /**
         * @brief Maximum notification count was reached.
         */
        MaxCountReached = 4,
    };

    /**
     * @brief A notification server that owns the org.freedesktop.Notifications interface.
     */
    class NotificationServer
    {
    public:
        /**
         * @brief Gets called synchronously when a notification was added.
         */
        Event<uint32_t> OnNotificationAdded;

        /**
         * @brief Gets called synchronously when a notification was updated.
         */
        Event<uint32_t> OnNotificationUpdated;

        /**
         * @brief Gets called synchronously when a notification was removed.
         * @note The notification object gets freed after this event has been handled, synchronously.
         *       Do not use the freed notification object asynchronously after this event is fired.
         */
        Event<uint32_t> OnNotificationRemoved;

        explicit NotificationServer();
        ~NotificationServer();

        /**
         * @brief Invokes an action on the specified notification.
         * @param notificationId The ID of the notification to act on
         * @param actionId The index of the action of the notification to act on
         */
        void InvokeAction(uint32_t notificationId, size_t actionId) const;

        /**
         * @brief Invokes an action on the specified notification.
         * @param notification The notification to act on
         * @param actionId The index of the action of the notification to act on
         */
        void InvokeAction(const Notification& notification, size_t actionId) const;

        /**
         * @brief Checks whether a notification exists based on its ID.
         * @param notificationId The ID of the notification to check for
         * @return Whether a notification was found
         */
        bool NotificationExists(uint32_t notificationId);

        /**
         * @brief Removes a notification with the reason 'Dismissed'.
         *
         * This method runs synchronously, and the notification is destroyed after this call.
         * Do not access the notification object or a OnNotificationRemoved-removed notification
         * object after calling this method.
         *
         * @param notificationId The ID of the notification to remove
         * @return Whether the notification was dismissed or not
         */
        bool DismissNotification(uint32_t notificationId);

        /**
         * @brief Removes a notification with the reason 'Dismissed'.
         *
         * @warning This method runs synchronously, and the notification is destroyed after this call.
         *          Do not access the notification object or a OnNotificationRemoved-removed notification
         *          object or its members after calling this method.
         *
         * @param notification The notification to remove
         * @return Whether the notification was dismissed or not
         */
        bool DismissNotification(const Notification& notification);

        /**
         * @brief Removes all notifications with the reason 'Dismissed'.
         * @warning This method runs synchronously. Do not access the notification object or a
         *          OnNotificationRemoved-removed notification object or its members after calling this method.
         */
        void ClearNotifications();

        /**
         * @brief Gets a notification by its ID.
         * @param notificationId The ID of the notification to return
         * @return The notification by its ID, or `nullptr` of not found
         */
        [[nodiscard]] std::shared_ptr<const Notification> GetNotification(uint32_t notificationId) const;

        /**
         * @return References to all notifications on the server
         */
        [[nodiscard]] std::vector<std::shared_ptr<const Notification>> GetNotifications() const;

        /**
         * @param sender The sender name of the notifications
         * @return References to all notifications on the server whose sender equals `sender`
         */
        [[nodiscard]] std::vector<std::shared_ptr<const Notification>> GetNotificationsBySender(const std::string& sender) const;

    public:
        static const char* SERVER_NAME;
        static const char* SERVER_VENDOR;
        static const char* SERVER_VERSION;
        static const char* SERVER_SPEC_VERSION;

    private:
        Connection& _conn;
        Object      _object;
        Interface&  _iface;

        uint32_t _currentUid = 1;
        std::map<std::string, std::map<uint32_t, std::shared_ptr<const Notification>>> _notificationsBySender;
        std::map<uint32_t, std::shared_ptr<const Notification>> _notificationsByUid;
        std::vector<guint> _notificationExpirationTasks;

        void OnGetCapabilities(const Message& message) const;
        void OnGetServerInformation(const Message& message) const;
        void OnNotify(const Message& message);
        void OnCloseNotification(const Message& message);

        void AddNotification(Notification notification);
        bool NotificationExistsWithIterators(
            uint32_t notificationId,
            std::map<uint32_t, std::shared_ptr<const Notification>>::iterator& it,
            std::map<std::string, std::map<uint32_t, std::shared_ptr<const Notification>>>::iterator& itSender);
        bool RemoveNotification(uint32_t notificationId, NotificationRemoveReason reason);
        void ArchiveNotification(const Notification& notification); // <-- TODO: Unused

    private:
        static const ObjectPath BASE_OBJECT;
        static const InterfaceName BASE_INTERFACE;
        static const std::vector<std::string> CAPABILITIES;
        static const uint32_t MAX_COUNT;

        CREATE_LOGGER("NotificationServer")
    };
}
