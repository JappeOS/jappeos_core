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

#include "dbus_peer.h"

namespace JappeStudios::JappeOS::JappeOSCore::Utils
{
    namespace
    {
        struct PeerSource
        {
            GSource source;
            GPollFD poll;
            std::shared_ptr<Connection>* connection;
        };

        gboolean Prepare(GSource* source, gint* timeout)
        {
            auto* peer = reinterpret_cast<PeerSource*>(source);
            auto* raw = (*peer->connection)->GetRawConnection();
            const auto status = dbus_connection_get_dispatch_status(raw);
            *timeout = status == DBUS_DISPATCH_NEED_MEMORY ? 10 : -1;
            peer->poll.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
            if (dbus_connection_has_messages_to_send(raw)) peer->poll.events |= G_IO_OUT;
            // Synchronous calls can leave signals queued even after the fd is drained.
            return status == DBUS_DISPATCH_DATA_REMAINS;
        }

        gboolean Check(GSource* source)
        {
            auto* peer = reinterpret_cast<PeerSource*>(source);
            return peer->poll.revents != 0 ||
                dbus_connection_get_dispatch_status((*peer->connection)->GetRawConnection()) == DBUS_DISPATCH_DATA_REMAINS;
        }

        gboolean Dispatch(GSource* source, GSourceFunc, gpointer)
        {
            auto* peer = reinterpret_cast<PeerSource*>(source);
            // A session-ending callback may destroy its DBusPeer and this source.
            const auto connection = *peer->connection;
            const bool connected = connection->ReadWrite(0);
            for (unsigned count = 0; count < 64 && !g_source_is_destroyed(source); ++count)
            {
                auto message = connection->PopMessage();
                if (!message) break;
                connection->HandleMessage(*message);
            }
            return connected ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
        }

        void Finalize(GSource* source)
        {
            delete reinterpret_cast<PeerSource*>(source)->connection;
        }

        GSourceFuncs functions{Prepare, Check, Dispatch, Finalize, nullptr, nullptr};
    }

    DBusPeer::DBusPeer(const std::string& address)
        : _connection(std::make_shared<Connection>(address)),
          _source(g_source_new(&functions, sizeof(PeerSource)))
    {
        auto* peer = reinterpret_cast<PeerSource*>(_source);
        peer->connection = new std::shared_ptr<Connection>(_connection);
        peer->poll.fd = _connection->GetUnixFd();
        peer->poll.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
        g_source_add_poll(_source, &peer->poll);
        g_source_set_name(_source, "jappeos-dbus-peer");
        g_source_attach(_source, nullptr);
    }

    DBusPeer::~DBusPeer()
    {
        g_source_destroy(_source);
        g_source_unref(_source);
    }
}
