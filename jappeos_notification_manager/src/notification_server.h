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
        Event<uint32_t> OnNotificationAdded;
        Event<uint32_t> OnNotificationUpdated;
        Event<uint32_t> OnNotificationRemoved;

        explicit NotificationServer();
        ~NotificationServer();

        void InvokeAction(uint32_t notificationId, size_t actionId) const;
        void InvokeAction(const Notification& notification, size_t actionId) const;
        bool DismissNotification(uint32_t notificationId);
        bool DismissNotification(const Notification& notification);
        void ClearNotifications();
        [[nodiscard]] std::shared_ptr<const Notification> GetNotification(uint32_t notificationId) const;
        [[nodiscard]] std::vector<std::shared_ptr<const Notification>> GetNotifications() const;
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
