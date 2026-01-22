# Handling D-Bus messages using the legacy message handling system

> [!WARNING]
> This doc is about the old message handling system. The new `dbus_wrapper` should be used instead. 

This file shows the right way to handle D-Bus signals and messages in a `jappeos_core` service.

## 1. The `HandleMethodCall` method
This method is where all the method calls and signals directed to this service get handled in.

Here's an example:
```c++
bool HandleMethodCall(DBusMessage* msg)
{
    // Get sender
    const char* sender = dbus_message_get_sender(msg);
    if (!sender)
    {
        SendErrorReplyAndLog(_conn, msg, DBUS_ERROR_ACCESS_DENIED, "No sender");
        return true;
    }
    
    DBusError error;
    dbus_error_init(&error);
    
    uid_t senderUid = dbus_bus_get_unix_user(_conn, sender, &error);
    if (dbus_error_is_set(&error))
    {
        dbus_error_free(&error);
        SendErrorReplyAndLog(_conn, msg, DBUS_ERROR_ACCESS_DENIED, std::string("Failed to get UID for sender ") + sender + ": " + (error.message ? error.message : "unknown"));
        return true;
    }
    
    // Get method
    const char* member = dbus_message_get_member(msg);
    if (!member)
    {
        SendErrorReplyAndLog(_conn, msg, DBUS_ERROR_UNKNOWN_METHOD, "No method specified");
        return true;
    }
    
    // Dispatch based on method name
    if (strcmp(member, "MyFirstMethod") == 0)
    {
        DBusMessageIter args;
        if (!dbus_message_iter_init(msg, &args))
        {
            SendErrorReplyAndLog(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Expected arguments: myFirstString, mySecondString");
            return true;
        }

        if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
        {
            SendErrorReplyAndLog(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Expected string myFirstString");
            return true;
        }
        const char* myFirstString = nullptr;
        dbus_message_iter_get_basic(&args, &myFirstString);

        if (!dbus_message_iter_next(&args) ||
            dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
        {
            SendErrorReplyAndLog(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Expected string mySecondString");
            return true;
        }
        const char* mySecondString = nullptr;
        dbus_message_iter_get_basic(&args, &mySecondString);

        if (!myFirstString || !mySecondString)
        {
            SendErrorReplyAndLog(_conn, msg, DBUS_ERROR_INVALID_ARGS, "Null argument(s)");
            return true;
        }

        HandleMyFirstMethod(msg, senderUid, myFirstString, mySecondString);
        return true;
    }
    else if (strcmp(member, "MySecondMethod") == 0)
    {
        DBusMessageIter iter;
        if (dbus_message_iter_init(msg, &iter))
        {
            SendErrorReplyAndLog(_conn, msg, DBUS_ERROR_INVALID_ARGS, "MySecondMethod expects no arguments");
            return true;
        }

        HandleMySecondMethod(msg, senderUid);
        return true;
    }
    
    // Error on unkown method name
    SendErrorReplyAndLog(_conn, msg, DBUS_ERROR_UNKNOWN_METHOD, std::string("Unknown DBus method called: ") + member);
    return false;
}
```
This method can be split into two parts:
1. Gather basic method call info, optionally authorize.
2. Dispatch the correct method based on name.

Authorization checks can either happen in `HandleMethodCall` directly, so that it affects all methods, or inside the
handler methods (like `HandleMySecondMethod` for example).

`HandleMethodCall` will return true if the first part of the method fails, or if any method call is handled, regardless
of whether it succeeded or not. We return false if it passed all the possible method choices.

## 2. The method calls
The method call checks (e.g `if (strcmp(member, "MyFirstMethod") == 0)`) in `HandleMethodCall` simply check the name of
the D-Bus method, read the input arguments, and call the actual handler method (e.g `HandleMyFirstMethod(...)`).

Inside the method call check if-statements we should check the args and pass them into the actual handler method that is
supposed to handle it. That handler method can then use `SendErrorReplyAndLog` normally in case of an error. The handler
method is also responsible for sending either a success reply using `SendSuccessReplyAndLog`, or a reply with some data,
if everything succeeds.

## 2. Signals
Signals may be handled in the `HandleMethodCall` method before everything else, in an `if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_SIGNAL)` if-statement.
Then we can get the interface and member and make sure that it is the signal we are wanting to receive. At the end of the
if-statement, we can just return `true` and ignore the unknown signals.

## 3. Notes
### Logging
Make sure that you do not accidentally log right before or after a `SendErrorReplyAndLog` or `SendSuccessReplyAndLog`.
Those methods already handle logging internally. Only log if the messages are different, like if the log message contains
some extra information, that is not passed to `SendErrorReplyAndLog` for example.

### Non-handler DBus methods
Methods that call a D-Bus method obviously do not use `SendErrorReplyAndLog` or `SendSuccessReplyAndLog`, since there's
no message to send it to. Instead, we can just log and return a boolean from that method to indicate success/failure.