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

#pragma once
#include <any>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <dbus/dbus.h>

namespace JappeStudios::JappeOS::JappeOSCore
{
    struct DBusVariant;

#define DBUS_DEFAULT_SAFE_TIMEOUT 5000

    class Object;
    class Interface;
    template<typename>
    inline constexpr bool alwaysFalse = false;

    /**
     * @brief Exception type for D-Bus errors.
     *
     * Wraps D-Bus error codes and messages into a C++ exception.
     */
    class DBusException : public std::runtime_error
    {
    public:
        explicit DBusException(const char* code, const std::string& msg = "")
            : std::runtime_error("DBusException(code='" + std::string(code) + "', msg='" + msg + "')"),
            _code{code}, _msg {msg}
        {}

        [[nodiscard]] std::string GetCode() const { return _code; }
        [[nodiscard]] std::string GetMsg() const { return _msg; }

    private:
        std::string _code;
        std::string _msg;
    };

    /**
     * @brief Simple helper method to append to a DBusMessageIter.
     */
    static void AppendValue(DBusMessageIter& it, const int type, const void* value)
    {
        if (!dbus_message_iter_append_basic(&it, type, value))
        {
            throw DBusException(DBUS_ERROR_FAILED, "append_basic failed");
        }
    }

    /**
     * @brief Represents a D-Bus object path.
     *
     * An ObjectPath is a strongly-typed wrapper around a valid D-Bus object path
     * string (e.g. "/org/example/Network/Devices/dev0").
     *
     * Invariants:
     * - Always begins with '/'
     * - Contains no empty elements
     * - Contains only valid D-Bus identifier characters
     * - Never ends with '/' (except for the root path "/")
     *
     * ObjectPath is a lightweight value type and is cheap to copy and move.
     * All validation is performed during construction.
     */
    class ObjectPath
    {
    public:
        /**
         * @brief Constructs the root object path ("/").
         */
        ObjectPath() : _path("/") {}

        /**
         * @brief Constructs an ObjectPath from a string.
         *
         * @param path A string containing a D-Bus object path.
         *
         * @throws std::invalid_argument if the path is not a valid D-Bus object path.
         */
        explicit ObjectPath(std::string path);

        /**
         * @brief Implicit conversion to string view.
         *
         * Allows ObjectPath to be passed directly to APIs expecting a string-like
         * type (e.g. D-Bus marshaling).
         */
        operator std::string_view() const noexcept { return _path; }

        /**
         * @brief Equality operator for support with containers like std::unordered_map.
         */
        bool operator==(const ObjectPath&) const = default;

        /**
         * @brief Returns the underlying object path string.
         */
        [[nodiscard]] const std::string& ToString() const noexcept { return _path; }

        /**
         * @brief Returns a new ObjectPath with a child element appended.
         *
         * Example:
         * @code
         * ObjectPath base{"/org/example/Devices"};
         * auto dev = base.Child("dev0");
         * // "/org/example/Devices/dev0"
         * @endcode
         *
         * @param element A single path element (not containing '/').
         *
         * @throws std::invalid_argument if the element is not valid.
         */
        [[nodiscard]] ObjectPath Child(std::string_view element) const;

        /**
         * @brief Returns the parent object path.
         *
         * Example:
         * @code
         * ObjectPath p{"/org/example/Devices/dev0"};
         * auto parent = p.Parent();
         * // "/org/example/Devices"
         * @endcode
         *
         * If this ObjectPath represents the root path ("/"), the root path
         * is returned.
         */
        [[nodiscard]] ObjectPath Parent() const;

    private:
        std::string _path;

    private:
        static bool IsValidElement(std::string_view s) noexcept;
        static bool IsValid(const std::string& path) noexcept;
        friend struct ObjectPathHash;
    };

    /**
     * @brief Creates a hash for ObjectPath
     */
    struct ObjectPathHash
    {
        size_t operator()(const ObjectPath& p) const noexcept
        {
            return std::hash<std::string>{}(p._path);
        }
    };

    // ========== SIGNATURE TRAITS (UNIFIED INTERFACE) ==========
#pragma region SignatureTraits

    template<typename T>
    struct DBusSignatureTraits
    {
        static_assert(alwaysFalse<T>, "Unsupported D-Bus type");
    };

    template<>
    struct DBusSignatureTraits<bool>
    {
        static const char* get() { return "b"; }
    };

    template<>
    struct DBusSignatureTraits<int32_t>
    {
        static const char* get() { return "i"; }
    };

    template<>
    struct DBusSignatureTraits<uint32_t>
    {
        static const char* get() { return "u"; }
    };

    template<>
    struct DBusSignatureTraits<long>
    {
        static const char* get() { return "x"; }
    };

    template<>
    struct DBusSignatureTraits<double>
    {
        static const char* get() { return "d"; }
    };

    template<>
    struct DBusSignatureTraits<std::string>
    {
        static const char* get() { return "s"; }
    };

    template<>
    struct DBusSignatureTraits<ObjectPath>
    {
        static const char* get() { return "o"; }
    };

    /*template<>
    struct DBusSignatureTraits<std::any>
    {
        static const char* get() { return "v"; }
    };*/

    template<typename T>
    struct DBusSignatureTraits<std::vector<T>>
    {
        static const char* get()
        {
            static const std::string sig =
                std::string("a") + DBusSignatureTraits<T>::get();
            return sig.c_str();
        }
    };

    template<typename K, typename V>
    struct DBusSignatureTraits<std::map<K, V>>
    {
        static const char* get()
        {
            static const std::string sig =
                std::string("a{") +
                DBusSignatureTraits<K>::get() +
                DBusSignatureTraits<V>::get() +
                "}";

            return sig.c_str();
        }
    };

    template<typename... Ts>
    struct DBusSignatureTraits<std::tuple<Ts...>>
    {
        static const char* get()
        {
            static const std::string sig = []
            {
                std::string s = "(";
                ((s += DBusSignatureTraits<Ts>::get()), ...);
                s += ")";
                return s;
            }();
            return sig.c_str();
        }
    };

    template<>
    struct DBusSignatureTraits<DBusVariant>
    {
        static const char* get() { return "v"; }
    };

    template<typename T>
    const char* DBusGetSignature()
    {
        return DBusSignatureTraits<T>::get();
    }

#pragma endregion

    template<typename T>
    struct DBusSetTraits;

    /**
     * @brief Represents a D-Bus variant. Contains the signature and value.
     */
    struct DBusVariant
    {
        std::string signature;
        std::any value;

        using AppendFn = void(*)(DBusMessageIter&, const std::any&);
        AppendFn append_fn;

        template<typename T>
        static DBusVariant make(T&& v)
        {
            using U = std::decay_t<T>;
            return {
                DBusGetSignature<U>(),
                std::forward<T>(v),
                [](DBusMessageIter& it, const std::any& a)
                {
                    DBusSetTraits<U>::append(it, std::any_cast<const U&>(a));
                }
            };
        }
    };

    // ========== SET TRAITS ==========
#pragma region SetTraits

    template<>
    struct DBusSetTraits<bool>
    {
        static constexpr int type = DBUS_TYPE_BOOLEAN;
        static void append(DBusMessageIter& it, const bool v)
        {
            const dbus_bool_t b = v ? TRUE : FALSE;
            AppendValue(it, type, &b);
        }
    };

    template<>
    struct DBusSetTraits<int32_t>
    {
        static constexpr int type = DBUS_TYPE_INT32;
        static void append(DBusMessageIter& it, const int32_t& v)
        {
            AppendValue(it, type, &v);
        }
    };

    template<>
    struct DBusSetTraits<uint32_t>
    {
        static constexpr int type = DBUS_TYPE_UINT32;
        static void append(DBusMessageIter& it, const uint32_t& v)
        {
            AppendValue(it, type, &v);
        }
    };

    template<>
    struct DBusSetTraits<long>
    {
        static constexpr int type = DBUS_TYPE_INT64;
        static void append(DBusMessageIter& it, const long& v)
        {
            AppendValue(it, type, &v);
        }
    };

    template<>
    struct DBusSetTraits<double>
    {
        static constexpr int type = DBUS_TYPE_DOUBLE;
        static void append(DBusMessageIter& it, const double& v)
        {
            AppendValue(it, type, &v);
        }
    };

    template<>
    struct DBusSetTraits<std::string>
    {
        static constexpr int type = DBUS_TYPE_STRING;
        static void append(DBusMessageIter& it, const std::string& v)
        {
            const char* s = v.c_str();
            AppendValue(it, type, &s);
        }
    };

    template<>
    struct DBusSetTraits<ObjectPath>
    {
        static constexpr int type = DBUS_TYPE_OBJECT_PATH;
        static void append(DBusMessageIter& it, const ObjectPath& v)
        {
            const char* s = v.ToString().c_str();
            AppendValue(it, type, &s);
        }
    };

    template<typename T>
    struct DBusSetTraits<std::vector<T>>
    {
        static constexpr int type = DBUS_TYPE_ARRAY;

        static void append(DBusMessageIter& it, const std::vector<T>& vec)
        {
            DBusMessageIter sub;
            const char* sig = DBusGetSignature<T>();

            dbus_message_iter_open_container(&it, type, sig, &sub);

            for (const auto& v : vec)
                DBusSetTraits<T>::append(sub, v);

            dbus_message_iter_close_container(&it, &sub);
        }
    };

    template<typename K, typename V>
    struct DBusSetTraits<std::map<K, V>>
    {
        static constexpr int type = DBUS_TYPE_ARRAY;

        static void append(DBusMessageIter& it, const std::map<K, V>& map)
        {
            DBusMessageIter array;
            const std::string sig = std::string("{")
                            + DBusGetSignature<K>()
                            + DBusGetSignature<V>()
                            + "}";

            dbus_message_iter_open_container(&it, type, sig.c_str(), &array);

            for (const auto& [k, v] : map)
            {
                DBusMessageIter entry;
                dbus_message_iter_open_container(
                    &array, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);

                DBusSetTraits<K>::append(entry, k);
                DBusSetTraits<V>::append(entry, v);

                dbus_message_iter_close_container(&array, &entry);
            }

            dbus_message_iter_close_container(&it, &array);
        }
    };

    /*template<>
    struct DBusSetTraits<std::any>
    {
        static constexpr int type = DBUS_TYPE_VARIANT;

        static void append(DBusMessageIter& it, const std::any& a)
        {
            DBusMessageIter sub;

            if (a.type() == typeid(int32_t))
            {
                dbus_message_iter_open_container(
                    &it, type, "i", &sub);
                DBusSetTraits<int32_t>::append(sub, std::any_cast<int32_t>(a));
            }
            else if (a.type() == typeid(uint32_t))
            {
                dbus_message_iter_open_container(
                    &it, type, "u", &sub);
                DBusSetTraits<uint32_t>::append(sub, std::any_cast<uint32_t>(a));
            }
            else if (a.type() == typeid(bool))
            {
                dbus_message_iter_open_container(
                    &it, type, "b", &sub);
                DBusSetTraits<bool>::append(sub, std::any_cast<bool>(a));
            }
            else if (a.type() == typeid(double))
            {
                dbus_message_iter_open_container(
                    &it, type, "d", &sub);
                DBusSetTraits<double>::append(sub, std::any_cast<double>(a));
            }
            else if (a.type() == typeid(std::string))
            {
                dbus_message_iter_open_container(
                    &it, type, "s", &sub);
                DBusSetTraits<std::string>::append(sub, std::any_cast<std::string>(a));
            }
            else if (a.type() == typeid(ObjectPath))
            {
                dbus_message_iter_open_container(
                    &it, type, "o", &sub);
                DBusSetTraits<ObjectPath>::append(sub, std::any_cast<ObjectPath>(a));
            }
            else
            {
                throw DBusException(DBUS_ERROR_INVALID_ARGS, "Unsupported variant");
            }

            dbus_message_iter_close_container(&it, &sub);
        }
    };*/

    template<typename... Ts>
    struct DBusSetTraits<std::tuple<Ts...>>
    {
        static constexpr int type = DBUS_TYPE_STRUCT;

        static void append(DBusMessageIter& it, const std::tuple<Ts...>& t)
        {
            DBusMessageIter sub;
            dbus_message_iter_open_container(&it, type, nullptr, &sub);

            std::apply([&](const Ts&... elems)
            {
                (DBusSetTraits<Ts>::append(sub, elems), ...);
            }, t);

            dbus_message_iter_close_container(&it, &sub);
        }
    };

    template<>
    struct DBusSetTraits<DBusVariant>
    {
        static constexpr int type = DBUS_TYPE_VARIANT;

        static void append(DBusMessageIter& it, const DBusVariant& v)
        {
            DBusMessageIter sub;

            dbus_message_iter_open_container(
                &it,
                DBUS_TYPE_VARIANT,
                v.signature.c_str(),
                &sub
            );

            v.append_fn(sub, v.value);

            dbus_message_iter_close_container(&it, &sub);
        }

        /*static void append(DBusMessageIter& it, const DBusVariant& v)
        {
            DBusMessageIter sub;

            dbus_message_iter_open_container(
                &it,
                DBUS_TYPE_VARIANT,
                v.signature.c_str(),
                &sub
            );

            appendBySignature(sub, v.signature, v.value);

            dbus_message_iter_close_container(&it, &sub);
        }*/

    /*private:
        static void appendBySignature(
            DBusMessageIter& it,
            const std::string& sig,
            const std::any& val)
        {
            if (sig == "i")
                DBusSetTraits<int32_t>::append(it, std::any_cast<int32_t>(val));
            else if (sig == "u")
                DBusSetTraits<uint32_t>::append(it, std::any_cast<uint32_t>(val));
            else if (sig == "b")
                DBusSetTraits<bool>::append(it, std::any_cast<bool>(val));
            else if (sig == "d")
                DBusSetTraits<double>::append(it, std::any_cast<double>(val));
            else if (sig == "s")
                DBusSetTraits<std::string>::append(it, std::any_cast<std::string>(val));
            else if (sig == "o")
                DBusSetTraits<ObjectPath>::append(it, std::any_cast<ObjectPath>(val));
            else if (sig.starts_with("a"))
            {
                throw DBusException(DBUS_ERROR_INVALID_ARGS,
                    "Container variants must be deserialized with explicit type");
            }
            else
            {
                throw DBusException(DBUS_ERROR_INVALID_ARGS,
                    "Unsupported variant signature");
            }
        }*/
    };

#pragma endregion

    // ========== GET TRAITS ==========
#pragma region GetTraits

    template<typename T>
    struct DBusGetTraits;

    template<>
    struct DBusGetTraits<bool>
    {
        static constexpr int type = DBUS_TYPE_BOOLEAN;
        static bool get(DBusMessageIter& it)
        {
            dbus_bool_t b;
            dbus_message_iter_get_basic(&it, &b);
            dbus_message_iter_next(&it);
            return b != FALSE;
        }
    };

    template<>
    struct DBusGetTraits<int32_t>
    {
        static constexpr int type = DBUS_TYPE_INT32;
        static int32_t get(DBusMessageIter& it)
        {
            int32_t v;
            dbus_message_iter_get_basic(&it, &v);
            dbus_message_iter_next(&it);
            return v;
        }
    };

    template<>
    struct DBusGetTraits<uint32_t>
    {
        static constexpr int type = DBUS_TYPE_UINT32;
        static uint32_t get(DBusMessageIter& it)
        {
            uint32_t v;
            dbus_message_iter_get_basic(&it, &v);
            dbus_message_iter_next(&it);
            return v;
        }
    };

    template<>
    struct DBusGetTraits<long>
    {
        static constexpr int type = DBUS_TYPE_INT64;
        static long get(DBusMessageIter& it)
        {
            long v;
            dbus_message_iter_get_basic(&it, &v);
            dbus_message_iter_next(&it);
            return v;
        }
    };

    template<>
    struct DBusGetTraits<double>
    {
        static constexpr int type = DBUS_TYPE_DOUBLE;
        static double get(DBusMessageIter& it)
        {
            double v;
            dbus_message_iter_get_basic(&it, &v);
            dbus_message_iter_next(&it);
            return v;
        }
    };

    template<>
    struct DBusGetTraits<std::string>
    {
        static constexpr int type = DBUS_TYPE_STRING;
        static std::string get(DBusMessageIter& it)
        {
            const char* s;
            dbus_message_iter_get_basic(&it, &s);
            dbus_message_iter_next(&it);
            return s ? s : "";
        }
    };

    template<>
    struct DBusGetTraits<ObjectPath>
    {
        static constexpr int type = DBUS_TYPE_OBJECT_PATH;
        static ObjectPath get(DBusMessageIter& it)
        {
            const char* o;
            dbus_message_iter_get_basic(&it, &o);
            dbus_message_iter_next(&it);
            return o ? ObjectPath(o) : ObjectPath(); // TODO: Maybe throw instead of using default ObjectPath ctor
        }
    };

    template<typename T>
    struct DBusGetTraits<std::vector<T>>
    {
        static constexpr int type = DBUS_TYPE_ARRAY;

        static std::vector<T> get(DBusMessageIter& it)
        {
            DBusMessageIter sub;
            std::vector<T> out;

            dbus_message_iter_recurse(&it, &sub);
            while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID)
            {
                out.push_back(DBusGetTraits<T>::get(sub));
            }

            dbus_message_iter_next(&it);
            return out;
        }
    };

    template<typename K, typename V>
    struct DBusGetTraits<std::map<K, V>>
    {
        static constexpr int type = DBUS_TYPE_ARRAY;

        static std::map<K, V> get(DBusMessageIter& it)
        {
            // Validate container type
            if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY)
            {
                throw DBusException(
                    DBUS_ERROR_INVALID_ARGS,
                    "Expected D-Bus array for dict");
            }

            DBusMessageIter array;
            dbus_message_iter_recurse(&it, &array);

            std::map<K, V> result;

            while (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_INVALID)
            {

                // Each element must be a dict entry
                if (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_DICT_ENTRY)
                {
                    throw DBusException(
                        DBUS_ERROR_INVALID_ARGS,
                        "Expected dict entry");
                }

                DBusMessageIter entry;
                dbus_message_iter_recurse(&array, &entry);

                // Key - get() will advance the iterator
                K key = DBusGetTraits<K>::get(entry);

                // Value - iterator has been advanced by the key get()
                V value = DBusGetTraits<V>::get(entry);

                result.emplace(std::move(key), std::move(value));

                // Move to next dict entry
                dbus_message_iter_next(&array);
            }

            // Advance outer iterator past the array
            dbus_message_iter_next(&it);

            return result;
        }
    };

    /*template<>
    struct DBusGetTraits<std::any>
    {
        static constexpr int type = DBUS_TYPE_VARIANT;

        static std::any get(DBusMessageIter& it)
        {
            DBusMessageIter sub;
            dbus_message_iter_recurse(&it, &sub);

            const int t = dbus_message_iter_get_arg_type(&sub);
            std::any out;

            if (t == DBUS_TYPE_INT32)
                out = DBusGetTraits<int32_t>::get(sub);
            else if (t == DBUS_TYPE_UINT32)
                out = DBusGetTraits<uint32_t>::get(sub);
            else if (t == DBUS_TYPE_BOOLEAN)
                out = DBusGetTraits<bool>::get(sub);
            else if (t == DBUS_TYPE_DOUBLE)
                out = DBusGetTraits<double>::get(sub);
            else if (t == DBUS_TYPE_STRING)
                out = DBusGetTraits<std::string>::get(sub);
            else if (t == DBUS_TYPE_OBJECT_PATH)
                out = DBusGetTraits<ObjectPath>::get(sub);
            else
                throw DBusException(DBUS_ERROR_INVALID_ARGS, "Unsupported variant");

            dbus_message_iter_next(&it);
            return out;
        }
    };*/

    template<typename... Ts>
    struct DBusGetTraits<std::tuple<Ts...>>
    {
        static constexpr int type = DBUS_TYPE_STRUCT;

        static std::tuple<Ts...> get(DBusMessageIter& it)
        {
            DBusMessageIter sub;
            dbus_message_iter_recurse(&it, &sub);

            // Get elements one at a time in order
            auto result = getTupleElements<Ts...>(sub);

            dbus_message_iter_next(&it);
            return result;
        }

    private:
        template<typename... Args>
        static std::tuple<Args...> getTupleElements(DBusMessageIter& it)
        {
            return std::tuple<Args...>{DBusGetTraits<Args>::get(it)...};
        }
    };

    template<>
    struct DBusGetTraits<DBusVariant>
    {
        static constexpr int type = DBUS_TYPE_VARIANT;

        static DBusVariant get(DBusMessageIter& it)
        {
            DBusMessageIter sub;
            dbus_message_iter_recurse(&it, &sub);

            char* sig = dbus_message_iter_get_signature(&sub);

            DBusVariant out;
            out.signature = sig;

            dbus_free(sig);

            switch (dbus_message_iter_get_arg_type(&sub))
            {
                case DBUS_TYPE_INT32:
                    out.value = DBusGetTraits<int32_t>::get(sub);
                    break;
                case DBUS_TYPE_UINT32:
                    out.value = DBusGetTraits<uint32_t>::get(sub);
                    break;
                case DBUS_TYPE_BOOLEAN:
                    out.value = DBusGetTraits<bool>::get(sub);
                    break;
                case DBUS_TYPE_DOUBLE:
                    out.value = DBusGetTraits<double>::get(sub);
                    break;
                case DBUS_TYPE_STRING:
                    out.value = DBusGetTraits<std::string>::get(sub);
                    break;
                case DBUS_TYPE_OBJECT_PATH:
                    out.value = DBusGetTraits<ObjectPath>::get(sub);
                    break;
                default:
                    throw DBusException(DBUS_ERROR_INVALID_ARGS,
                        "Unsupported variant payload");
            }

            dbus_message_iter_next(&it);
            return out;
        }
    };

#pragma endregion

    class Message;

    /**
     * @brief Represents a D-Bus interface name.
     *
     * An InterfaceName is a strongly-typed wrapper around a valid D-Bus interface
     * name (e.g. "org.example.Network.Device").
     *
     * Invariants:
     * - Dot-separated identifier segments
     * - Each segment starts with a letter or underscore
     * - Contains only alphanumeric characters and underscores
     *
     * InterfaceName is a lightweight value type and performs validation on
     * construction.
     */
    class InterfaceName
    {
    public:
        /**
         * @brief Constructs an InterfaceName from a string.
         *
         * @param name A string containing a D-Bus interface name.
         *
         * @throws std::invalid_argument if the name is not a valid interface name.
         */
        explicit InterfaceName(std::string name);

        /**
         * @brief Implicit conversion to string view.
         *
         * Allows InterfaceName to be passed directly to APIs expecting a
         * string-like type.
         */
        operator std::string_view() const noexcept { return _name; }

        /**
         * @brief Equality operator for support with containers like std::unordered_map.
         */
        bool operator==(const InterfaceName&) const = default;

        /**
         * @brief Returns the underlying interface name string.
         */
        [[nodiscard]] const std::string& ToString() const noexcept { return _name; }

        /**
         * @brief Returns a new InterfaceName with a child segment appended.
         *
         * Example:
         * @code
         * InterfaceName base{"org.example.Network"};
         * auto iface = base.Child("Device");
         * // "org.example.Network.Device"
         * @endcode
         *
         * @param segment A single interface name segment.
         *
         * @throws std::invalid_argument if the segment is not valid.
         */
        [[nodiscard]] InterfaceName Child(std::string_view segment) const;

        /**
         * @brief Returns the parent interface name.
         *
         * Example:
         * @code
         * InterfaceName iface{"org.example.Network.Device"};
         * auto parent = iface.Parent();
         * // "org.example.Network"
         * @endcode
         *
         * If the interface has no parent segment, the current instance
         * is returned.
         */
        [[nodiscard]] InterfaceName Parent() const;

    private:
        std::string _name;

    private:
        static bool IsValidSegment(std::string_view s) noexcept;
        static bool IsValid(const std::string& name) noexcept;
        friend struct InterfaceNameHash;
    };

    /**
     * @brief Creates a hash for InterfaceName
     */
    struct InterfaceNameHash
    {
        size_t operator()(const InterfaceName& p) const noexcept
        {
            return std::hash<std::string>{}(p._name);
        }
    };

    /**
     * @brief Represents a std::function used to handle received signals.
     */
    using SignalHandler = std::function<void(const Message&)>;

    /**
     * @brief Represents a D-Bus connection.
     *
     * Manages the lifecycle of a D-Bus connection and maintains a registry of
     * objects that can handle incoming messages.
     */
    class Connection
    {
    public:
        /**
         * @brief Creates a connection to a D-Bus bus.
         * @param busType The type of bus to connect to (DBUS_BUS_SESSION, DBUS_BUS_SYSTEM, etc.)
         * @throws DBusException if connection fails
         */
        explicit Connection(DBusBusType busType);

        /**
         * @brief Creates a connection object from a raw DBusConnection pointer.
         *
         * Takes ownership of the DBusConnection pointer. The caller must not
         * unref the connection after passing it to this constructor.
         *
         * @param conn Raw D-Bus connection pointer
         * @throws DBusException if conn is nullptr
         */
        explicit Connection(DBusConnection* conn);

        ~Connection();

        Connection(Connection&& other) noexcept;
        Connection& operator=(Connection&& other) noexcept;
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;

        /**
         * @brief Gets the underlying raw D-Bus connection pointer.
         * @return Raw DBusConnection pointer (still owned by this object)
         */
        [[nodiscard]] DBusConnection* GetRawConnection() const;

        /**
         * @brief Registers an object to handle messages for a specific path.
         *
         * The object will be added to the connection's registry and will receive
         * all messages directed to its path.
         *
         * This method is called internally by Object::Object().
         * Users should not call this method directly.
         *
         * @param object Pointer to the object to register (must remain valid)
         * @throws std::logic_error if an object with the same path already exists
         */
        void Register(Object* object);

        /**
         * @brief Unregisters an object by path.
         *
         * This method is called internally by Object::~Object().
         * Users should not call this method directly.
         *
         * @param object The object path to unregister
         */
        void Unregister(const ObjectPath& object);

        /**
         * @brief Subscribes to a signal.
         * @param busName Source bus name
         * @param path Object path
         * @param iface Interface name
         * @param method Method name
         * @param handler Method called when a signal is received
         * @return New method call message
         */
        class SignalSubscription SubscribeSignal(const std::string& busName,
                                                 const ObjectPath& path,
                                                 const InterfaceName& iface,
                                                 const std::string& method,
                                                 const SignalHandler& handler);

        /**
         * @brief Routes a message to the appropriate registered object.
         *
         * Looks up the object by the message's path and delegates handling to it.
         *
         * @param msg The message to handle
         * @return true if a matching object was found and handled the message, false otherwise
         */
        bool HandleMessage(const Message& msg);

        /**
         * @brief Adds a message match rule to the bus.
         *
         * Match rules filter which messages this connection receives from the bus.
         *
         * @param rule D-Bus match rule string (e.g., "type='signal',interface='com.example.Foo'")
         * @throws DBusException if adding the match fails
         */
        void AddMatch(const std::string& rule) const;

        /**
         * @brief Removes a message match rule from the bus.
         *
         * Match rules filter which messages this connection receives from the bus.
         *
         * @param rule D-Bus match rule string (e.g., "type='signal',interface='com.example.Foo'")
         * @throws DBusException if removing the match fails
         */
        void RemoveMatch(const std::string& rule) const;

        /**
         * @brief Requests ownership of a well-known bus name.
         * @param name The bus name to acquire (e.g., "com.example.MyService")
         * @param flags D-Bus name request flags (DBUS_NAME_FLAG_REPLACE_EXISTING, etc.)
         * @throws DBusException if name acquisition fails
         */
        void AcquireName(const std::string& name, unsigned int flags) const;

        /**
         * @brief Flushes any pending messages in the outgoing queue.
         */
        void Flush() const;

        /**
         * @brief Gets the Unix file descriptor for this connection.
         * @return File descriptor that can be used with select/poll
         */
        [[nodiscard]] int GetUnixFd() const;

        /**
         * @brief Gets the Unix UID of a message sender.
         * @param sender The sender's unique bus name (e.g., ":1.42")
         * @return The Unix user ID of the sender
         * @throws DBusException if unable to retrieve UID
         */
        [[nodiscard]] uid_t GetUnixUser(const std::string& sender) const;

        /**
         * @brief Processes read/write operations on the connection.
         *
         * This should be called when the connection's file descriptor is readable
         * or writable. It does not block for longer than the specified timeout.
         *
         * @param timeoutMilliseconds Maximum time to block in milliseconds
         * @return true if more processing may be needed, false if disconnected
         */
        [[nodiscard]] bool ReadWrite(int timeoutMilliseconds) const;

        /**
         * @brief Retrieves the next message from the incoming queue.
         *
         * Does not block. Call ReadWrite() first to process incoming data.
         *
         * @return A message if one is available, otherwise std::nullopt
         */
        [[nodiscard]] std::optional<Message> PopMessage() const;

    private:
        using HandlerList = std::vector<SignalHandler>;

        struct SignalKey
        {
            InterfaceName interface;
            std::string member;
            std::optional<ObjectPath> path;   // optional, can be empty

            bool operator==(const SignalKey& other) const noexcept
            {
                return interface == other.interface &&
                       member    == other.member &&
                       path      == other.path;
            }
        };

        struct SignalKeyHash
        {
            std::size_t operator()(const SignalKey& k) const noexcept
            {
                std::size_t h = std::hash<std::string>{}(k.interface.ToString());
                h ^= std::hash<std::string>{}(k.member) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<std::string>{}(k.path.has_value() ? k.path.value().ToString() : "")
                    + 0x9e3779b9 + (h << 6) + (h >> 2);
                return h;
            }
        };

        friend class SignalSubscription;

    private:
        DBusConnection* _conn;
        std::unordered_map<ObjectPath, Object*, ObjectPathHash> _objectRegistry;
        std::unordered_map<
            SignalKey,
            HandlerList,
            SignalKeyHash
        > _signalHandlers;

        bool HandleSignal(const Message& msg);
    };

    /**
     * @brief Represents a D-Bus signal subscription.
     *
     * Holds a reference to the signal in the Connection. Once this object is destroyed, the signal handler is removed.
     */
    class SignalSubscription
    {
    public:
        /**
         * @brief Creates a signal subscription object for a connection.
         *
         * @param conn Reference to the target connection in which to subscribe the signal in
         * @param busName The bus name or sender of the signal
         * @param key Contains signal object, interface and method info
         * @param handler The target method to call when the signal occurs
         */
        SignalSubscription(Connection& conn,
                           const std::string& busName,
                           Connection::SignalKey key,
                           SignalHandler handler);

        ~SignalSubscription();

        SignalSubscription(SignalSubscription&& other) noexcept;
        SignalSubscription& operator=(SignalSubscription&& other) noexcept = delete;
        SignalSubscription(const SignalSubscription&) = delete;
        SignalSubscription& operator=(const SignalSubscription&) = delete;

    private:
        Connection& _conn;
        Connection::SignalKey _key;
        size_t _index;
        std::string _match;
        bool _active{true};

        void AddMatchRule() const;
        void RemoveMatchRule() const;
    };

    /**
     * @brief Represents a D-Bus message.
     *
     * Wraps a DBusMessage pointer and provides type-safe methods for setting
     * and getting message arguments.
     */
    class Message
    {
    public:
        /**
         * @brief Creates a message object from a raw DBusMessage pointer.
         *
         * Takes ownership of the DBusMessage pointer. The caller must not
         * unref the message after passing it to this constructor.
         *
         * @param msg Raw D-Bus message pointer
         * @throws DBusException if msg is nullptr
         */
        explicit Message(DBusMessage* msg);

        ~Message();

        Message(Message&& other) noexcept;
        Message& operator=(Message&& other) noexcept;
        Message(const Message&) = delete;
        Message& operator=(const Message&) = delete;

        /**
         * @brief Gets the underlying raw D-Bus message pointer.
         * @return Raw DBusMessage pointer (still owned by this object)
         */
        [[nodiscard]] DBusMessage* GetRawMessage() const;

        /**
         * @brief Sets the message arguments.
         *
         * Appends the provided arguments to the message. Supported types include:
         * bool, int32_t, uint32_t, double, std::string, ObjectPath, std::vector<T>,
         * std::map<K,V>, std::tuple<Ts...>, and std::any (variant).
         *
         * @param args Arguments to set
         * @throws DBusException if serialization fails
         */
        template<typename... Args>
        void SetArgs(const Args&... args)
        {
            DBusMessageIter it;
            dbus_message_iter_init_append(_msg, &it);

            (DBusSetTraits<Args>::append(it, args), ...);
        }

        /**
         * @brief Gets the message arguments with type checking.
         *
         * Extracts and deserializes arguments from the message. Throws if
         * the actual argument types don't match the requested types.
         *
         * @tparam Args Expected argument types
         * @return Tuple containing the deserialized arguments
         * @throws DBusException if message has no arguments or types don't match
         */
        template<typename... Args>
        [[nodiscard]] std::tuple<Args...> GetArgs() const
        {
            DBusMessageIter it;
            if (!dbus_message_iter_init(_msg, &it))
            {
                throw DBusException(
                    DBUS_ERROR_INVALID_ARGS,
                    "Message has no arguments");
            }

            return GetArgsImpl<Args...>(it);
        }

        /**
         * @brief Sends a method call and waits for a reply.
         * @param conn Connection to send on
         * @param timeoutMilliseconds Timeout in milliseconds (-1 for default)
         * @return The reply message
         * @throws DBusException if send fails or timeout occurs
         */
        [[nodiscard]] Message SendWithReply(const Connection& conn, int timeoutMilliseconds = -1) const;

        /**
         * @brief Sends a method call and waits for a reply, but discards the reply.
         * @param conn Connection to send on
         * @param timeoutMilliseconds Timeout in milliseconds (-1 for default)
         * @throws DBusException if send fails or timeout occurs
         */
        void SendWithReplyIgnore(const Connection& conn, int timeoutMilliseconds = -1) const;

        /**
         * @brief Sends a message without waiting for a reply.
         *
         * Typically used for signals and method calls that don't expect replies.
         *
         * @param conn Connection to send on
         * @throws DBusException if send fails
         */
        void Send(const Connection& conn) const;

        /**
         * @brief Gets the message type.
         * @return D-Bus message type (DBUS_MESSAGE_TYPE_METHOD_CALL, etc.)
         */
        [[nodiscard]] int GetType() const;

        /**
         * @brief Gets the sender's unique bus name.
         * @return Sender name (e.g., ":1.42") or empty string if not set
         */
        [[nodiscard]] std::string GetSender() const;

        /**
         * @brief Gets the interface name from the message.
         * @return Interface name or nullopt if not set
         */
        [[nodiscard]] std::optional<InterfaceName> GetInterface() const;

        /**
         * @brief Gets the method or signal name from the message.
         * @return Member name or empty string if not set
         */
        [[nodiscard]] std::string GetMember() const;

        /**
         * @brief Gets the object path from the message.
         * @return Object path or nullopt if not set
         */
        [[nodiscard]] std::optional<ObjectPath> GetPath() const;

    public:
        /**
         * @brief Creates a method call message.
         * @param busName Destination bus name
         * @param path Object path
         * @param iface Interface name
         * @param method Method name
         * @return New method call message
         * @throws DBusException if allocation fails
         */
        static Message CreateMethodCall(const std::string& busName,
                                        const ObjectPath& path,
                                        const InterfaceName& iface,
                                        const std::string& method);

        /**
         * @brief Creates a method return message in response to a method call.
         * @param methodCall The method call to reply to
         * @return New method return message
         * @throws DBusException if allocation fails
         */
        static Message CreateMethodReturn(const Message& methodCall);

        /**
         * @brief Creates a signal message.
         * @param path Object path emitting the signal
         * @param iface Interface name
         * @param name Signal name
         * @return New signal message
         * @throws DBusException if allocation fails
         */
        static Message CreateSignal(const ObjectPath& path, const InterfaceName& iface, const std::string& name);

        /**
         * @brief Creates an error reply message.
         * @param replyTo The message to reply to
         * @param errorName D-Bus error name (e.g., DBUS_ERROR_FAILED)
         * @param errorMessage Human-readable error description
         * @return New error message
         * @throws DBusException if allocation fails
         */
        static Message CreateError(const Message& replyTo,
                                   const std::string& errorName,
                                   const std::string& errorMessage);

    private:
        DBusMessage* _msg;

        template<typename... Args>
        std::tuple<Args...> GetArgsImpl(DBusMessageIter& it) const
        {
            return GetArgsImplHelper<Args...>(it, std::index_sequence_for<Args...>{});
        }

        template<typename... Args, std::size_t... Is>
        std::tuple<Args...> GetArgsImplHelper(DBusMessageIter& it, std::index_sequence<Is...>) const
        {
            std::tuple<Args...> result;
            (..., (std::get<Is>(result) = GetOne<Args>(it)));
            return result;
        }

        template<typename T>
        T GetOne(DBusMessageIter& it) const
        {
            using Traits = DBusGetTraits<T>;

            const int actual = dbus_message_iter_get_arg_type(&it);

            // Type validation for simple types
            if constexpr (requires { Traits::type; })
            {
                if (actual != Traits::type)
                {
                    throw DBusException(
                        DBUS_ERROR_INVALID_ARGS,
                        "D-Bus argument type mismatch");
                }
            }

            return Traits::get(it);
        }
    };

    /**
     * @brief Represents a D-Bus object at a specific path.
     *
     * An object can have multiple interfaces, each with their own methods
     * and properties. Automatically handles org.freedesktop.DBus.Properties
     * standard interface.
     */
    class Object
    {
    public:
        /**
         * @brief Creates a D-Bus object and registers it with the connection.
         * @param connection Connection to register with
         * @param path Object path (e.g., "/com/example/MyObject")
         * @throws std::logic_error if an object with this path already exists
         */
        explicit Object(Connection& connection, ObjectPath path);

        /**
         * @brief Destroys the object and unregisters it from the connection.
         */
        ~Object();

        Object(Object&& other) noexcept = delete;
        Object& operator=(Object&& other) noexcept = delete;
        Object(const Object&) = delete;
        Object& operator=(const Object&) = delete;

        /**
         * @brief Creates a new interface on this object.
         * @param name Interface name (e.g., "com.example.MyInterface")
         * @return Reference to the created interface
         * @throws std::logic_error if an interface with this name already exists
         */
        Interface& CreateInterface(const InterfaceName& name);

        /**
         * @brief Routes an incoming message to the appropriate interface.
         *
         * Automatically handles org.freedesktop.DBus.Properties interface for
         * property Get/Set/GetAll operations. For other interfaces, delegates
         * to the registered interface handler. Exceptions thrown by handlers
         * are automatically converted to D-Bus error replies.
         *
         * @param msg Message to handle
         * @return true if message was handled, false if no matching interface found
         */
        bool HandleMessage(const Message& msg);

        /**
         * @brief Gets the object's path.
         * @return The object path
         */
        [[nodiscard]] ObjectPath GetPath() const;

    private:
        std::unordered_map<InterfaceName, std::unique_ptr<Interface>, InterfaceNameHash> _interfaces;
        Connection& _connection;
        const ObjectPath _path;

        bool DispatchProperties(const Message& msg);

    private:
        template <typename Func>
        static bool DispatchExceptionHandler(const Connection& conn, const Message& msg, Func fn)
        {
            try
            {
                return fn();
            }
            catch (DBusException& e)
            {
                const auto reply = Message::CreateError(msg, e.GetCode(), e.GetMsg());
                reply.Send(conn);
            }
            catch (std::exception& e)
            {
                const auto what = e.what();
                const auto reply = Message::CreateError(
                    msg,
                    DBUS_ERROR_FAILED,
                    what ? what : "Unknown error"
                );
                reply.Send(conn);
            }

            return true;
        }
    };

    /**
     * @brief Represents a D-Bus interface with methods and properties.
     *
     * Interfaces are created through Object::CreateInterface() and cannot
     * exist independently. Each interface can have multiple methods and
     * properties that are accessible via D-Bus.
     *
     * Example usage:
     * @code
     * Connection conn(DBUS_BUS_SESSION);
     * Object obj(conn, "/com/example/MyObject");
     * Interface& iface = obj.CreateInterface("com.example.MyInterface");
     *
     * // Register a method
     * iface.RegisterMethod("SayHello", [&](const Message& msg) {
     *     auto [name] = msg.GetArgs<std::string>();
     *     auto reply = Message::CreateMethodReturn(msg);
     *     reply.SetArgs(std::string("Hello, ") + name + "!");
     *     reply.Send(conn);
     * });
     *
     * // Register a property
     * int counter = 0;
     * iface.RegisterProperty<int32_t>(
     *     "Counter",
     *     [&]() { return counter; },
     *     [&](const int32_t& val) { counter = val; }
     * );
     * @endcode
     *
     * @note Method handlers must send their own replies. This design prevents
     *       the risk of sending both an error reply and a success reply.
     * @note Properties are accessed through the standard D-Bus Properties interface,
     *       not as direct method calls.
     * @note This class is not thread-safe. Ensure all operations occur from a
     *       single thread or provide external synchronization.
     * @note The Prop<T> class is recommended for properties, instead of raw lambdas.
     */
    class Interface
    {
    public:
        /**
         * @brief Internal structure representing a D-Bus property.
         *
         * Contains the property metadata and callable getters/setters.
         */
        struct Property
        {
            std::string name;
            std::string signature;
            std::function<void(Message&)> getter;   // returns a reply message
            std::function<void(const Message&)> setter;
        };

    public:
        /**
         * @brief Constructs an interface.
         *
         * This constructor is called internally by Object::CreateInterface().
         * Users should not construct Interface objects directly.
         *
         * @param obj Parent object (unused but required for future extensions)
         * @param name Interface name (e.g., "com.example.MyInterface")
         */
        Interface(const Object& obj, InterfaceName name);

        Interface(Interface&& other) noexcept = delete;
        Interface& operator=(Interface&& other) noexcept = delete;
        Interface(const Interface&) = delete;
        Interface& operator=(const Interface&) = delete;

        /**
         * @brief Registers a method handler.
         *
         * The handler receives the incoming method call message and is responsible
         * for creating and sending the reply message (or error message). The handler
         * will not be called if the sender field is empty.
         *
         * @important Unlike some D-Bus bindings, this wrapper does not automatically
         *            send a reply. Your handler must explicitly create and send a reply
         *            to avoid leaving the caller waiting indefinitely.
         *
         * Example with success reply:
         * @code
         * iface.RegisterMethod("Add", [&](const Message& msg) {
         *     auto [a, b] = msg.GetArgs<int32_t, int32_t>();
         *     int32_t result = a + b;
         *
         *     auto reply = Message::CreateMethodReturn(msg);
         *     reply.SetArgs(result);
         *     reply.Send(conn);
         * });
         * @endcode
         *
         * Example with error handling:
         * @code
         * iface.RegisterMethod("Divide", [&](const Message& msg) {
         *     auto [a, b] = msg.GetArgs<int32_t, int32_t>();
         *
         *     if (b == 0) {
         *         auto error = Message::CreateError(
         *             msg,
         *             DBUS_ERROR_INVALID_ARGS,
         *             "Division by zero"
         *         );
         *         error.Send(conn);
         *         return;
         *     }
         *
         *     auto reply = Message::CreateMethodReturn(msg);
         *     reply.SetArgs(a / b);
         *     reply.Send(conn);
         * });
         * @endcode
         *
         * @tparam F Function type (deduced)
         * @param methodName Name of the method
         * @param handler Function that handles method calls (signature: void(const Message&))
         * @throws std::logic_error if a method with this name already exists
         */
        template<typename F>
        void RegisterMethod(const std::string& methodName, F&& handler)
        {
            auto [it, inserted] = _methods.emplace(
                methodName,
                std::function<void(const Message&)>(std::forward<F>(handler))
            );

            if (!inserted)
                throw std::logic_error("Method already exists: " + methodName);
        }

        /**
         * @brief Registers a D-Bus property with getter and/or setter.
         *
         * Properties can be accessed via the org.freedesktop.DBus.Properties interface
         * using the standard Get, Set, and GetAll methods. At least one of getter or
         * setter should be provided.
         *
         * Supported types: bool, int32_t, uint32_t, double, std::string, ObjectPath
         * std::vector<T>, std::map<K,V>, std::tuple<Ts...>, std::any
         *
         * @important Registered properties don't automatically emit org.freedesktop.DBus.Properties::PropertiesChanged
         *            upon value change. The signal should be emitted even if the change is not a result of a D-Bus call.
         *            See Prop<T> for automating the org.freedesktop.DBus.Properties::PropertiesChanged signal.
         *
         * Example - Read-write property:
         * @code
         * int volume = 50;
         * iface.RegisterProperty<int32_t>(
         *     "Volume",
         *     [&]() { return volume; },             // getter
         *     [&](const int32_t& v) { volume = v; } // setter
         * );
         * @endcode
         *
         * Example - Read-only property:
         * @code
         * iface.RegisterProperty<std::string>(
         *     "Version",
         *     []() { return std::string("1.0.0"); } // getter only, no setter
         * );
         * @endcode
         *
         * Example - Complex type property:
         * @code
         * std::map<std::string, int32_t> scores;
         * iface.RegisterProperty<std::map<std::string, int32_t>>(
         *     "Scores",
         *     [&]() { return scores; },
         *     [&](const std::map<std::string, int32_t>& s) { scores = s; }
         * );
         * @endcode
         *
         * @tparam T Property type (must be a supported D-Bus type)
         * @param name Property name
         * @param getter Function that returns the current value
         * @param setter Function that sets a new value (can be nullptr for read-only)
         * @throws std::logic_error if a property with this name already exists
         */
        template<typename T, typename Getter, typename Setter = std::nullptr_t>
        void RegisterProperty(std::string name, Getter getter, Setter setter = nullptr)
        {
            static_assert(std::is_invocable_r_v<T, Getter>,
                          "Getter must be callable and return T");

            if constexpr (!std::is_same_v<Setter, std::nullptr_t>)
            {
                static_assert(std::is_invocable_r_v<void, Setter, const T&>,
                              "Setter must be callable with (const T&)");
            }

            std::function<T()> getterFn = std::move(getter);
            std::function<void(const T&)> setterFn;

            if constexpr (!std::is_same_v<Setter, std::nullptr_t>)
                setterFn = std::move(setter);

            Property prop;
            prop.name = name;

            // property signature is the inner type
            prop.signature = DBusSignatureTraits<T>::get();

            // GET
            if (getterFn)
            {
                prop.getter = [getterFn = std::move(getterFn)](Message& reply)
                {
                    T value = getterFn();

                    // Properties.Get MUST return a variant
                    const DBusVariant v = DBusVariant::make(std::move(value));

                    reply.SetArgs(v); // TODO: Error
                };
            }

            // SET
            if (setterFn)
            {
                prop.setter = [setterFn = std::move(setterFn), expectedSig = prop.signature](const Message& msg)
                {
                    // Set(interface, property, variant)
                    const auto args =
                        msg.GetArgs<std::string, std::string, DBusVariant>();

                    const DBusVariant& v = std::get<2>(args);

                    // Enforce declared property signature
                    if (v.signature != expectedSig)
                    {
                        throw DBusException(
                            DBUS_ERROR_INVALID_ARGS,
                            "Invalid variant signature for property");
                    }

                    try
                    {
                        setterFn(std::any_cast<const T&>(v.value));
                    }
                    catch (const std::bad_any_cast&)
                    {
                        throw DBusException(
                            DBUS_ERROR_INVALID_ARGS,
                            "Invalid variant value for property");
                    }
                };
            }

            auto [it, inserted] = _properties.emplace(name, std::move(prop));
            if (!inserted)
                throw std::logic_error("Property already exists: " + name);
        }

        /**
         * @brief Dispatches a method call to the registered handler.
         *
         * Internal method called by Object::HandleMessage(). Validates that
         * the message has a sender before dispatching to prevent anonymous
         * method calls.
         *
         * @param conn Connection the message was received on
         * @param msg Method call message
         * @return true if method was found and dispatched, false otherwise
         * @throws DBusException with code DBUS_ERROR_ACCESS_DENIED if sender is empty
         */
        bool Dispatch(const Connection& conn, const Message& msg);

        /**
         * @brief Handles org.freedesktop.DBus.Properties.Get request.
         *
         * Internal method called by Object::DispatchProperties(). Retrieves a
         * single property value and sends the reply.
         *
         * @param conn Connection to send reply on
         * @param msg The Get method call
         * @param iface Interface name
         * @param name Property name
         * @throws DBusException with code DBUS_ERROR_UNKNOWN_INTERFACE if interface doesn't match
         * @throws DBusException with code DBUS_ERROR_UNKNOWN_PROPERTY if property not found or has no getter
         */
        void HandleGetProperty(const Connection& conn,
                               const Message& msg,
                               const InterfaceName& iface,
                               const std::string& name);

        /**
         * @brief Handles org.freedesktop.DBus.Properties.Set request.
         *
         * Internal method called by Object::DispatchProperties(). Updates a
         * property value and sends an acknowledgment reply.
         *
         * @param conn Connection to send reply on
         * @param msg The Set method call (contains the new value as a variant)
         * @param iface Interface name
         * @param name Property name
         * @throws DBusException with code DBUS_ERROR_UNKNOWN_INTERFACE if interface doesn't match
         * @throws DBusException with code DBUS_ERROR_UNKNOWN_PROPERTY if property not found or has no setter
         */
        void HandleSetProperty(const Connection& conn,
                               const Message& msg,
                               const InterfaceName& iface,
                               const std::string& name);

        /**
         * @brief Handles org.freedesktop.DBus.Properties.GetAll request.
         *
         * Internal method called by Object::DispatchProperties(). Returns all
         * properties with getters on this interface as a dictionary mapping
         * property names to variant values.
         *
         * @param conn Connection to send reply on
         * @param msg The GetAll method call
         * @param iface Interface name
         * @return D-Bus message containing a{sv} (dictionary of string to variant) with all readable properties
         * @throws DBusException with code DBUS_ERROR_UNKNOWN_INTERFACE if interface doesn't match
         */
        void HandleGetAllProperties(const Connection& conn, const Message& msg, const InterfaceName& iface);

        /**
         * @brief Gets the interface's name.
         * @return The interface name
         */
        [[nodiscard]] InterfaceName GetName() const;

        /**
         * @brief Gets the interface's name.
         * @return The interface name
         */
        [[nodiscard]] const Object& GetObject() const;

    private:
        InterfaceName _name;                                                           ///< Interface name
        const Object& _object;
        std::unordered_map<std::string, std::function<void(const Message&)>> _methods; ///< Map of method names to handler functions
        std::unordered_map<std::string, Property> _properties;                         ///< Map of property names to Property structures
    };

    /**
     * @brief D-Bus property wrapper with automatic change notification.
     *
     * Prop<T> represents a single D-Bus property belonging to an interface.
     * It stores the property value, exposes it through the
     * org.freedesktop.DBus.Properties interface, and automatically emits
     * the PropertiesChanged signal whenever the value changes.
     *
     * The property may be modified either through D-Bus (via Set) or
     * internally through assignment or Set(). In all cases, change
     * notification is emitted exactly once per actual value change.
     *
     * This class is not thread-safe. All access must be serialized by
     * the owning object.
     *
     * @tparam T C++ type of the property value. The type must be
     *           comparable with operator== and supported by the
     *           D-Bus marshalling layer.
     */
    template<typename T>
    class Prop
    {
    public:
        /**
         * @brief Construct and register a D-Bus property.
         *
         * Registers the property with the given interface and initializes
         * it with the provided value. The property becomes immediately
         * accessible through org.freedesktop.DBus.Properties.
         *
         * @param conn   D-Bus connection used to emit change signals.
         * @param iface  Interface to which this property belongs.
         * @param name   D-Bus property name.
         * @param initial Initial property value.
         */
        Prop(const Connection& conn,
             Interface& iface,
             std::string name,
             T initial) :
             _value(std::move(initial)),
             _name(std::move(name)),
             _conn(conn),
             _iface(iface)
        {
            iface.RegisterProperty<T>(_name, [this] { return Get(); }, [this](const T& v) { Set(v); });
        }

        /**
         * @brief Assign a new value to the property.
         *
         * If the new value differs from the current value, the property
         * is updated and a PropertiesChanged signal is emitted.
         *
         * @param value New property value.
         * @return Reference to this property.
         */
        Prop& operator=(const T& value)
        {
            Set(value);
            return *this;
        }

        /**
         * @brief Retrieve the current property value.
         *
         * @return The current value of the property.
         */
        T Get() const { return _value; }

        /**
         * @brief Set the property value.
         *
         * Updates the property value if it differs from the current value
         * and emits a PropertiesChanged signal. If the value is unchanged,
         * no signal is emitted.
         *
         * @param value New property value.
         * @return true if the value was changed, false otherwise.
         */
        bool Set(const T& value)
        {
            if (value == _value)
                return false;

            _value = value;
            EmitPropertiesChanged();
            return true;
        }

    private:
        T _value;
        std::string _name;
        const Connection& _conn;
        Interface& _iface;

        /**
         * @brief Emit the PropertiesChanged signal for this property.
         *
         * Emits org.freedesktop.DBus.Properties::PropertiesChanged for
         * the owning interface, listing this property as changed.
         *
         * This method must only be called after the internal value has
         * been updated.
         */
        void EmitPropertiesChanged()
        {
            auto sig = Message::CreateSignal(
                _iface.GetObject().GetPath(),
                InterfaceName("org.freedesktop.DBus.Properties"),
                "PropertiesChanged"
            );

            sig.SetArgs(
                _iface.GetName().ToString(),
                std::map<std::string, DBusVariant>{
                    { _name, DBusVariant::make(_value) }
                },
                std::vector<std::string>{}
            );

            sig.Send(_conn);
        }
    };
}