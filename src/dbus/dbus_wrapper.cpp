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

#include "dbus_wrapper.h"

#include <format>

namespace JappeStudios::JappeOS::JappeOSCore
{

    // ObjectPath

    ObjectPath::ObjectPath(std::string path)
    {
        if (!IsValid(path))
            throw std::invalid_argument("Invalid D-Bus ObjectPath");

        _path = std::move(path);
    }

    ObjectPath ObjectPath::Child(const std::string_view element) const
    {
        if (!IsValidElement(element))
            throw std::invalid_argument("Invalid ObjectPath element: " + std::string(element));

        if (_path == "/")
            return ObjectPath{"/" + std::string(element)};

        return ObjectPath{_path + "/" + std::string(element)};
    }

    ObjectPath ObjectPath::Parent() const
    {
        if (_path == "/")
            return *this;

        const auto pos = _path.find_last_of('/');
        if (pos == 0)
            return ObjectPath{"/"};

        return ObjectPath{_path.substr(0, pos)};
    }

    bool ObjectPath::IsValidElement(const std::string_view s) noexcept
    {
        if (s.empty()) return false;
        for (const char c : s)
        {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
            {
                return false;
            }
        }
        return true;
    }

    bool ObjectPath::IsValid(const std::string& path) noexcept
    {
        if (path.empty() || path[0] != '/') return false;
        if (path.size() > 1 && path.back() == '/') return false;
        if (path == "/") return true;

        std::size_t start = 1;
        while (start < path.size())
        {
            auto end = path.find('/', start);
            if (end == std::string::npos) end = path.size();
            if (!IsValidElement(std::string_view(path).substr(start, end - start)))
                return false;
            start = end + 1;
        }
        return true;
    }

    // InterfaceName

    InterfaceName::InterfaceName(std::string name)
    {
        if (!IsValid(name))
            throw std::invalid_argument("Invalid D-Bus InterfaceName: " + name);

        _name = std::move(name);
    }

    InterfaceName InterfaceName::Child(const std::string_view segment) const
    {
        if (!IsValidSegment(segment))
            throw std::invalid_argument("Invalid InterfaceName segment");

        return InterfaceName{_name + "." + std::string(segment)};
    }

    InterfaceName InterfaceName::Parent() const
    {
        const auto pos = _name.find_last_of('.');
        if (pos == std::string::npos)
            return *this;

        return InterfaceName{_name.substr(0, pos)};
    }

    bool InterfaceName::IsValidSegment(const std::string_view s) noexcept
    {
        if (s.empty()) return false;
        if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
            return false;

        for (const char c : s.substr(1))
        {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
                return false;
        }
        return true;
    }

    bool InterfaceName::IsValid(const std::string& name) noexcept
    {
        std::size_t start = 0;
        while (start < name.size())
        {
            auto end = name.find('.', start);
            if (end == std::string::npos) end = name.size();
            if (!IsValidSegment(std::string_view(name).substr(start, end - start)))
                return false;
            start = end + 1;
        }
        return !name.empty();
    }

    // Connection

    Connection::Connection(const DBusBusType busType)
    {
        DBusError err;
        dbus_error_init(&err);
        _conn = dbus_bus_get(busType, &err);

        if (dbus_error_is_set(&err))
        {
            const auto errmsg = "Failed to connect to D-Bus bus: " + std::string(err.message ? err.message : "unknown");
            dbus_error_free(&err);
            if (_conn) dbus_connection_unref(_conn);
            throw DBusException(DBUS_ERROR_NO_MEMORY, errmsg);
        }

        if (!_conn)
            throw DBusException(DBUS_ERROR_NO_MEMORY, "Failed to connect to D-Bus bus.");
    }

    Connection::Connection(DBusConnection* conn)
    {
        if (!conn)
            throw DBusException(DBUS_ERROR_NO_MEMORY, "Failed to connect to D-Bus bus.");

        _conn = conn;
    }

    Connection::~Connection()
    {
        if (_conn)
            dbus_connection_unref(_conn);
    }

    Connection::Connection(Connection&& other) noexcept : _conn(other._conn)
    {
        other._conn = nullptr;
    }

    Connection& Connection::operator=(Connection&& other) noexcept
    {
        if (this != &other)
        {
            if (_conn) dbus_connection_unref(_conn);
            _conn = other._conn;
            other._conn = nullptr;
        }
        return *this;
    }

    DBusConnection* Connection::GetRawConnection() const { return _conn; }

    void Connection::Register(Object* object)
    {
        auto [it, inserted] = _objectRegistry.emplace(
            object->GetPath(),
            object
        );

        if (!inserted)
            throw std::logic_error("Object already exists: " + object->GetPath().ToString());
    }

    void Connection::Unregister(const ObjectPath& object)
    {
        _objectRegistry.erase(object);
    }

    SignalSubscription Connection::SubscribeSignal(const std::string& busName,
                                                   const ObjectPath& path,
                                                   const InterfaceName& iface,
                                                   const std::string& method,
                                                   const SignalHandler& handler)
    {
        return SignalSubscription(*this, busName, SignalKey{ iface, method, path }, handler);
    }

    bool Connection::HandleMessage(const Message& msg)
    {
        if (msg.GetType() == DBUS_MESSAGE_TYPE_SIGNAL)
            return HandleSignal(msg);

        const auto path = msg.GetPath();
        if (!path.has_value())
            return false;

        const auto it = _objectRegistry.find(path.value());
        if (it == _objectRegistry.end())
            return false;

        return it->second->HandleMessage(msg);
    }

    void Connection::AddMatch(const std::string& rule) const
    {
        DBusError err;
        dbus_error_init(&err);

        dbus_bus_add_match(_conn, rule.c_str(), &err);

        if (dbus_error_is_set(&err))
        {
            const auto errmsg = "Failed to add match: " + std::string(err.message ? err.message : "unknown");
            dbus_error_free(&err);
            throw DBusException(DBUS_ERROR_MATCH_RULE_INVALID, errmsg);
        }
    }

    void Connection::RemoveMatch(const std::string& rule) const
    {
        DBusError err;
        dbus_error_init(&err);

        dbus_bus_remove_match(_conn, rule.c_str(), &err);

        if (dbus_error_is_set(&err))
        {
            const auto errmsg = "Failed to remove match: " + std::string(err.message ? err.message : "unknown");
            dbus_error_free(&err);
            throw DBusException(DBUS_ERROR_MATCH_RULE_INVALID, errmsg);
        }
    }

    void Connection::AcquireName(const std::string& name, const unsigned int flags) const
    {
        DBusError err;
        dbus_error_init(&err);

        const int ret = dbus_bus_request_name(_conn, name.c_str(), flags, &err);

        if (dbus_error_is_set(&err))
        {
            const std::string message = err.message ? err.message : "unknown error";
            dbus_error_free(&err);
            throw DBusException(DBUS_ERROR_FAILED, "D-Bus error: " + message);
        }

        if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
        {
            // ret indicates failure but error might not be set
            throw DBusException(DBUS_ERROR_FAILED,
                "Failed to acquire D-Bus name (return code: " + std::to_string(ret) + ")");
        }
    }

    void Connection::Flush() const
    {
        dbus_connection_flush(_conn);
    }

    int Connection::GetUnixFd() const
    {
        int dbus_fd;
        dbus_connection_get_unix_fd(_conn, &dbus_fd);
        return dbus_fd;
    }

    uid_t Connection::GetUnixUser(const std::string& sender) const
    {
        DBusError error;
        dbus_error_init(&error);
        const uid_t senderUid = dbus_bus_get_unix_user(_conn, sender.c_str(), &error);
        if (dbus_error_is_set(&error))
        {
            const std::string errmsg = std::string("Failed to get UID for sender ") + sender +
                ": " + (error.message ? error.message : "unknown");
            dbus_error_free(&error);
            throw DBusException(DBUS_ERROR_ACCESS_DENIED, errmsg);
        }

        return senderUid;
    }

    bool Connection::ReadWrite(const int timeoutMilliseconds) const
    {
        return dbus_connection_read_write(_conn, timeoutMilliseconds);
    }

    std::optional<Message> Connection::PopMessage() const
    {
        const auto msg = dbus_connection_pop_message(_conn);
        if (!msg)
        {
            return {};
        }

        return Message{msg};
    }

    bool Connection::HandleSignal(const Message& msg)
    {
        const auto iface = msg.GetInterface();
        if (!iface.has_value())
            return false;

        const SignalKey key
        {
            iface.value(),
            msg.GetMember(),
            msg.GetPath()
        };

        const auto it = _signalHandlers.find(key);
        if (it == _signalHandlers.end())
            return false;

        for (auto& handler : it->second)
        {
            try
            {
                handler(msg);
            }
            catch (const std::exception&) {} // TODO: Proper exception handling
        }

        return true;
    }

    // SignalSubscription

    SignalSubscription::SignalSubscription(Connection& conn,
                                           const std::string& busName,
                                           Connection::SignalKey key,
                                           SignalHandler handler) :
                                           _conn(conn),
                                           _key(std::move(key))
    {
        _match = std::format(
            "type='signal',sender='{}',{}interface='{}',member='{}'",
            busName,
            _key.path.has_value() ? std::format("path='{}',", _key.path.value().ToString()) : "",
            _key.interface.ToString(),
            _key.member
        );

        auto& list = _conn._signalHandlers[_key];
        _index = list.size();
        list.push_back(std::move(handler));

        if (list.size() == 1)
            AddMatchRule();
    }

    SignalSubscription::~SignalSubscription()
    {
        if (!_active) return;

        const auto it = _conn._signalHandlers.find(_key);
        if (it == _conn._signalHandlers.end())
            return;

        auto& list = it->second;
        list[_index] = nullptr;

        if (std::all_of(list.begin(), list.end(),
                        [](auto& f){ return !f; }))
        {
            RemoveMatchRule();
            _conn._signalHandlers.erase(it);
        }
    }

    SignalSubscription::SignalSubscription(SignalSubscription&& other) noexcept
        : _conn(other._conn),
          _key(std::move(other._key)),
          _index(other._index),
          _match(std::move(other._match)),
          _active(other._active)
    {
        other._active = false;
    }

    void SignalSubscription::AddMatchRule() const
    {
        _conn.AddMatch(_match);
    }

    void SignalSubscription::RemoveMatchRule() const
    {
        _conn.RemoveMatch(_match);
    }

    // Message

    Message::Message(DBusMessage* msg)
    {
        if (!msg)
            throw DBusException(DBUS_ERROR_NO_MEMORY, "Null D-Bus message.");

        _msg = msg;
    }

    Message::~Message()
    {
        if (_msg)
            dbus_message_unref(_msg);
    }

    Message::Message(Message&& other) noexcept : _msg(other._msg)
    {
        other._msg = nullptr;
    }

    Message& Message::operator=(Message&& other) noexcept
    {
        if (this != &other)
        {
            if (_msg) dbus_message_unref(_msg);
            _msg = other._msg;
            other._msg = nullptr;
        }
        return *this;
    }

    DBusMessage* Message::GetRawMessage() const { return _msg; }

    Message Message::SendWithReply(const Connection& conn, const int timeoutMilliseconds) const
    {
        DBusError err;
        dbus_error_init(&err);

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(
            conn.GetRawConnection(),
            _msg,
            timeoutMilliseconds < 1 ? DBUS_DEFAULT_SAFE_TIMEOUT : timeoutMilliseconds,
            &err
        );

        if (dbus_error_is_set(&err))
        {
            const auto errmsg = "Send failed: " + std::string(err.message ? err.message : "unknown");
            dbus_error_free(&err);
            if (reply) dbus_message_unref(reply);
            throw DBusException(DBUS_ERROR_FAILED, errmsg);
        }

        if (!reply)
        {
            throw DBusException(DBUS_ERROR_NO_REPLY, "No reply");
        }

        return Message{reply};
    }

    void Message::SendWithReplyIgnore(const Connection& conn, const int timeoutMilliseconds) const
    {
        // We call SendWithReply but discard the result
        // The destructor will clean up the reply message
        [[maybe_unused]] const auto reply = SendWithReply(conn, timeoutMilliseconds);
    }

    void Message::Send(const Connection& conn) const
    {
        if (const auto result = dbus_connection_send(conn.GetRawConnection(), _msg, nullptr); !result)
        {
            throw DBusException(DBUS_ERROR_FAILED, "Send failed");
        }
    }

    int Message::GetType() const
    {
        return dbus_message_get_type(_msg);
    }

    std::string Message::GetSender() const
    {
        const auto sender = dbus_message_get_sender(_msg);
        return sender ? sender : "";
    }

    std::optional<InterfaceName> Message::GetInterface() const
    {
        const auto iface = dbus_message_get_interface(_msg);
        return iface ? std::make_optional(InterfaceName(iface)) : std::nullopt;
    }

    std::string Message::GetMember() const
    {
        const auto member = dbus_message_get_member(_msg);
        return member ? member : "";
    }

    std::optional<ObjectPath> Message::GetPath() const
    {
        const auto path = dbus_message_get_path(_msg);
        return path ? std::make_optional(ObjectPath(path)) : std::nullopt;
    }

    Message Message::CreateMethodCall(const std::string& busName,
                                      const ObjectPath& path,
                                      const InterfaceName& iface,
                                      const std::string& method)
    {
        DBusMessage* msg = dbus_message_new_method_call(
            busName.c_str(),
            path.ToString().c_str(),
            iface.ToString().c_str(),
            method.c_str()
        );

        if (!msg)
            throw DBusException(DBUS_ERROR_NO_MEMORY, "Failed to allocate D-Bus message.");

        return Message{msg};
    }

    Message Message::CreateMethodReturn(const Message& methodCall)
    {
        DBusMessage* msg = dbus_message_new_method_return(methodCall.GetRawMessage());

        if (!msg)
            throw DBusException(DBUS_ERROR_NO_MEMORY, "Failed to allocate D-Bus message.");

        return Message{msg};
    }

    Message Message::CreateSignal(const ObjectPath& path, const InterfaceName& iface, const std::string& name)
    {
        DBusMessage* msg = dbus_message_new_signal(path.ToString().c_str(), iface.ToString().c_str(), name.c_str());

        if (!msg)
            throw DBusException(DBUS_ERROR_NO_MEMORY, "Failed to allocate D-Bus message.");

        return Message{msg};
    }

    Message Message::CreateError(const Message& replyTo, const std::string& errorName, const std::string& errorMessage)
    {
        DBusMessage* msg = dbus_message_new_error(replyTo.GetRawMessage(), errorName.c_str(), errorMessage.c_str());

        if (!msg)
            throw DBusException(DBUS_ERROR_NO_MEMORY, "Failed to allocate D-Bus message.");

        return Message{msg};
    }

    // Object

    Object::Object(Connection& connection, ObjectPath path) : _connection(connection), _path(std::move(path))
    {
        _connection.Register(this);
    }

    Object::~Object()
    {
        _connection.Unregister(GetPath());
    }

    Interface& Object::CreateInterface(const InterfaceName& name)
    {
        auto [it, inserted] = _interfaces.emplace(
            name,
            std::make_unique<Interface>(*this, name)
        );

        if (!inserted)
            throw std::logic_error("Interface already exists: " + name.ToString());

        return *it->second;
    }

    bool Object::HandleMessage(const Message& msg)
    {
        const auto& iface = msg.GetInterface();
        if (!iface.has_value())
            return false;

        if (iface.value().ToString() == "org.freedesktop.DBus.Properties")
        {
            return DispatchExceptionHandler(_connection, msg, [&]
            {
                return DispatchProperties(msg);
            });
        }

        /*if (iface == "org.freedesktop.DBus.Introspectable")
            return DispatchIntrospection(msg);*/

        const auto it = _interfaces.find(iface.value());
        if (it == _interfaces.end())
            return false;

        return DispatchExceptionHandler(_connection, msg, [&]
        {
            return it->second->Dispatch(_connection, msg);
        });
    }

    ObjectPath Object::GetPath() const { return _path; }

    bool Object::DispatchProperties(const Message& msg)
    {
        std::string property;

        if (msg.GetMember() == "Get")
        {
            const auto args = msg.GetArgs<std::string, std::string>();
            const auto targetInterface = InterfaceName(std::get<0>(args));
            property = std::get<1>(args);

            const auto iface = _interfaces.find(targetInterface);
            if (iface == _interfaces.end())
                throw DBusException(DBUS_ERROR_UNKNOWN_INTERFACE, "Unknown interface");

            iface->second->HandleGetProperty(_connection, msg, targetInterface, property);
            return true;
        }

        if (msg.GetMember() == "Set")
        {
            const auto args = msg.GetArgs<std::string, std::string, DBusVariant>();
            const auto targetInterface = InterfaceName(std::get<0>(args));
            property = std::get<1>(args);

            const auto iface = _interfaces.find(targetInterface);
            if (iface == _interfaces.end())
                throw DBusException(DBUS_ERROR_UNKNOWN_INTERFACE, "Unknown interface");

            iface->second->HandleSetProperty(_connection, msg, targetInterface, property);
            return true;
        }

        if (msg.GetMember() == "GetAll")
        {
            const auto args = msg.GetArgs<std::string>();
            const auto targetInterface = InterfaceName(std::get<0>(args));

            const auto iface = _interfaces.find(targetInterface);
            if (iface == _interfaces.end())
                throw DBusException(DBUS_ERROR_UNKNOWN_INTERFACE, "Unknown interface");

            iface->second->HandleGetAllProperties(_connection, msg, targetInterface);
            return true;
        }

        return false;
    }

    // Interface

    Interface::Interface(const Object& obj, InterfaceName name) : _name(std::move(name)), _object(obj) {}

    bool Interface::Dispatch(const Connection& conn, const Message& msg)
    {
        const auto it = _methods.find(msg.GetMember());
        if (it == _methods.end())
        {
            return false;
        }

        if (const auto sender = msg.GetSender(); sender.empty())
        {
            throw DBusException(DBUS_ERROR_ACCESS_DENIED, "No sender");
        }

        it->second(msg);
        return true;
    }

    void Interface::HandleGetProperty(const Connection& conn,
                                      const Message& msg,
                                      const InterfaceName& iface,
                                      const std::string& name)
    {
        if (iface != _name)
            throw DBusException(DBUS_ERROR_UNKNOWN_INTERFACE, "Unknown interface");

        const auto it = _properties.find(name);
        if (it == _properties.end() || !it->second.getter)
            throw DBusException(DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property");

        Message reply = Message::CreateMethodReturn(msg);
        it->second.getter(reply);
        reply.Send(conn);
    }

    void Interface::HandleSetProperty(const Connection& conn,
                                      const Message& msg,
                                      const InterfaceName& iface,
                                      const std::string& name)
    {
        if (iface != _name)
            throw DBusException(DBUS_ERROR_UNKNOWN_INTERFACE, "Unknown interface");

        const auto it = _properties.find(name);
        if (it == _properties.end() || !it->second.setter)
            throw DBusException(DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property");

        it->second.setter(msg);
        Message::CreateMethodReturn(msg).Send(conn);
    }

    void Interface::HandleGetAllProperties(const Connection& conn,
                                           const Message& msg,
                                           const InterfaceName& iface)
    {
        if (iface != _name)
            throw DBusException(
                DBUS_ERROR_UNKNOWN_INTERFACE,
                "Unknown interface"
            );

        std::map<std::string, DBusVariant> values;

        for (auto& [propName, prop] : _properties)
        {
            if (!prop.getter)
                continue;

            auto temp = Message::CreateMethodReturn(msg);
            prop.getter(temp);

            auto args = temp.GetArgs<DBusVariant>();
            values.emplace(propName, std::get<0>(args));
        }

        auto reply = Message::CreateMethodReturn(msg);
        reply.SetArgs(values);
        reply.Send(conn);
    }


    InterfaceName Interface::GetName() const { return _name; }

    const Object& Interface::GetObject() const { return _object; }

}
