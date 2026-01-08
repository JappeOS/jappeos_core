#include "dbus_wrapper.h"

namespace JappeStudios::JappeOS::JappeOSCore
{

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

    /// Creates a connection object from a raw `DBusConnection*` object. Hands off ownership of the `DBusConnection*`
    /// to this `Connection` instance.
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
            throw std::logic_error("Object already exists: " + object->GetPath());
    }

    void Connection::Unregister(const std::string& object)
    {
        _objectRegistry.erase(object);
    }

    bool Connection::HandleMessage(const Message& msg)
    {
        const auto& obj = msg.GetPath();
        const auto it = _objectRegistry.find(obj);
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
            const auto errmsg = "Failed to add match: " + std::string(err.message);
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
            &err);

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

    std::string Message::GetInterface() const
    {
        const auto iface = dbus_message_get_interface(_msg);
        return iface ? iface : "";
    }

    std::string Message::GetMember() const
    {
        const auto member = dbus_message_get_member(_msg);
        return member ? member : "";
    }

    std::string Message::GetPath() const
    {
        const auto path = dbus_message_get_path(_msg);
        return path ? path : "";
    }

    Message Message::CreateMethodCall(const std::string& busName, const std::string& path, const std::string& iface, const std::string& method)
    {
        DBusMessage* msg = dbus_message_new_method_call(busName.c_str(), path.c_str(), iface.c_str(), method.c_str());

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

    Message Message::CreateSignal(const std::string& path, const std::string& iface, const std::string& name)
    {
        DBusMessage* msg = dbus_message_new_signal(path.c_str(), iface.c_str(), name.c_str());

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

    Object::Object(Connection& connection, std::string path) : _connection(connection), _path(std::move(path))
    {
        _connection.Register(this);
    }

    Object::~Object()
    {
        _connection.Unregister(GetPath());
    }

    Interface& Object::CreateInterface(const std::string& name)
    {
        auto [it, inserted] = _interfaces.emplace(
            name,
            std::make_unique<Interface>(*this, name)
        );

        if (!inserted)
            throw std::logic_error("Interface already exists: " + name);

        return *it->second;
    }

    bool Object::HandleMessage(const Message& msg)
    {
        const auto& iface = msg.GetInterface();

        if (iface == "org.freedesktop.DBus.Properties")
            return DispatchProperties(msg);

        /*if (iface == "org.freedesktop.DBus.Introspectable")
            return DispatchIntrospection(msg);*/

        const auto it = _interfaces.find(iface);
        if (it == _interfaces.end())
            return false;

        return DispatchExceptionHandler(_connection, msg, [&](const Connection& conn, const Message& msg)
        {
            return it->second->Dispatch(conn, msg);
        });
    }

    std::string Object::GetPath() const { return _path; }

    bool Object::DispatchProperties(const Message& msg)
    {
        std::string targetInterface;
        std::string property;

        if (msg.GetMember() == "Get")
        {
            const auto args = msg.GetArgs<std::string, std::string>();
            targetInterface = std::get<0>(args);
            property        = std::get<1>(args);

            const auto iface = _interfaces.find(targetInterface);
            if (iface == _interfaces.end())
                throw DBusException(DBUS_ERROR_UNKNOWN_INTERFACE, "Unknown interface");

            return DispatchExceptionHandler(_connection, msg, [&](const Connection& conn, const Message& msg)
            {
                iface->second->HandleGetProperty(conn, msg, targetInterface, property);
                return true;
            });
        }

        if (msg.GetMember() == "Set")
        {
            const auto args = msg.GetArgs<std::string, std::string, std::any>();
            targetInterface = std::get<0>(args);
            property        = std::get<1>(args);

            const auto iface = _interfaces.find(targetInterface);
            if (iface == _interfaces.end())
                throw DBusException(DBUS_ERROR_UNKNOWN_INTERFACE, "Unknown interface");

            return DispatchExceptionHandler(_connection, msg, [&](const Connection& conn, const Message& msg)
            {
                iface->second->HandleSetProperty(conn, msg, targetInterface, property);
                return true;
            });
        }

        if (msg.GetMember() == "GetAll")
        {
            const auto args = msg.GetArgs<std::string>();
            targetInterface = std::get<0>(args);

            const auto iface = _interfaces.find(targetInterface);
            if (iface == _interfaces.end())
                throw DBusException(DBUS_ERROR_UNKNOWN_INTERFACE, "Unknown interface");

            return DispatchExceptionHandler(_connection, msg, [&](const Connection& conn, const Message& msg)
            {
                iface->second->HandleGetAllProperties(conn, msg, targetInterface);
                return true;
            });
        }

        return false;
    }

    // Interface

    Interface::Interface(Object&, std::string name) : _name(std::move(name)) {}

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
                                      const std::string& iface,
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
                                      const std::string& iface,
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

    void Interface::HandleGetAllProperties(const Connection& conn, const Message& msg, const std::string& iface)
    {
        if (iface != _name)
            throw DBusException(DBUS_ERROR_UNKNOWN_INTERFACE, "Unknown interface");

        std::map<std::string, std::any> values;

        for (auto& [propName, prop] : _properties)
        {
            auto temp = Message::CreateMethodReturn(msg);
            prop.getter(temp);
            values[propName] = std::get<0>(temp.GetArgs<std::any>());
        }

        auto reply = Message::CreateMethodReturn(msg);
        reply.SetArgs(values);
        reply.Send(conn);
    }

}
