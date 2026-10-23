#include "DBus.h"

#if JUCE_LINUX || JUCE_BSD
#include <dlfcn.h>
#endif

namespace spinus::core::dbus
{

//==============================================================================
Value Value::string (const juce::String& s)
{
    Value v;
    v.kind = Kind::string;
    v.text = s;
    return v;
}

Value Value::objectPath (const juce::String& s)
{
    auto v = string (s);
    v.kind = Kind::objectPath;
    return v;
}

Value Value::boolean (bool b)
{
    Value v;
    v.kind = Kind::boolean;
    v.number = b ? 1 : 0;
    return v;
}

Value Value::uint32 (juce::uint32 n)
{
    Value v;
    v.kind = Kind::uint32;
    v.number = n;
    return v;
}

Value Value::bytes (const juce::String& s)
{
    auto v = string (s);
    v.kind = Kind::bytes;
    return v;
}

Value Value::array (const juce::String& elementSignature, std::vector<Value> elements)
{
    Value v;
    v.kind = Kind::array;
    v.elementSignature = elementSignature;
    v.children = std::move (elements);
    return v;
}

Value Value::structure (std::vector<Value> fields)
{
    Value v;
    v.kind = Kind::structure;
    v.children = std::move (fields);
    return v;
}

Value Value::variant (Value inner)
{
    Value v;
    v.kind = Kind::variant;
    v.children.push_back (std::move (inner));
    return v;
}

Value Value::dictionary (std::vector<std::pair<juce::String, Value>> entries)
{
    Value v;
    v.kind = Kind::dictionary;
    for (auto& [key, value] : entries)
    {
        v.keys.push_back (key);
        v.children.push_back (std::move (value));
    }
    return v;
}

juce::String Value::signature() const
{
    switch (kind)
    {
    case Kind::string:
        return "s";
    case Kind::objectPath:
        return "o";
    case Kind::boolean:
        return "b";
    case Kind::uint32:
        return "u";
    case Kind::bytes:
        return "ay";
    case Kind::array:
        return "a" + elementSignature;
    case Kind::structure:
    {
        juce::String s ("(");
        for (auto& c : children)
            s << c.signature();
        return s + ")";
    }
    case Kind::variant:
        return "v";
    case Kind::dictionary:
        return "a{sv}";
    }
    return {};
}

juce::String Message::errorText() const
{
    const auto text = arguments.isEmpty() ? juce::String() : arguments[0].toString();
    return errorName + (text.isNotEmpty() ? ": " + text : juce::String());
}

#if JUCE_LINUX || JUCE_BSD

namespace
{
// libdbus's types, as its headers declare them (the library is loaded at run time; no headers needed).
using Bool = juce::uint32;
struct Error
{
    const char* name;
    const char* message;
    unsigned int bits;
    void* padding;
};
// DBusMessageIter has no public fields; 16 pointers is its largest size (on any machine).
struct Iter
{
    void* storage[16] {};
};

/** libdbus's functions Spinus uses, found once; never unloaded. */
struct Library
{
    void* (*connection_open_private) (const char*, Error*) = nullptr;
    Bool (*bus_register) (void*, Error*) = nullptr;
    const char* (*bus_get_unique_name) (void*) = nullptr;
    void (*bus_add_match) (void*, const char*, Error*) = nullptr;
    int (*bus_request_name) (void*, const char*, unsigned int, Error*) = nullptr;
    void* (*connection_send_with_reply_and_block) (void*, void*, int, Error*) = nullptr;
    Bool (*connection_send) (void*, void*, juce::uint32*) = nullptr;
    void (*connection_flush) (void*) = nullptr;
    Bool (*connection_read_write) (void*, int) = nullptr;
    void* (*connection_pop_message) (void*) = nullptr;
    void (*connection_close) (void*) = nullptr;
    void (*connection_unref) (void*) = nullptr;
    void* (*message_new_method_call) (const char*, const char*, const char*, const char*) = nullptr;
    void* (*message_new_method_return) (void*) = nullptr;
    void* (*message_new_error) (void*, const char*, const char*) = nullptr;
    void* (*message_new_signal) (const char*, const char*, const char*) = nullptr;
    void (*message_unref) (void*) = nullptr;
    int (*message_get_type) (void*) = nullptr;
    const char* (*message_get_path) (void*) = nullptr;
    const char* (*message_get_interface) (void*) = nullptr;
    const char* (*message_get_member) (void*) = nullptr;
    const char* (*message_get_sender) (void*) = nullptr;
    const char* (*message_get_error_name) (void*) = nullptr;
    const char* (*message_get_signature) (void*) = nullptr;
    void (*message_set_serial) (void*, juce::uint32) = nullptr;
    Bool (*message_marshal) (void*, char**, int*) = nullptr;
    void* (*message_demarshal) (const char*, int, Error*) = nullptr;
    Bool (*message_iter_init) (void*, Iter*) = nullptr;
    void (*message_iter_init_append) (void*, Iter*) = nullptr;
    Bool (*message_iter_append_basic) (Iter*, int, const void*) = nullptr;
    Bool (*message_iter_open_container) (Iter*, int, const char*, Iter*) = nullptr;
    Bool (*message_iter_close_container) (Iter*, Iter*) = nullptr;
    int (*message_iter_get_arg_type) (Iter*) = nullptr;
    int (*message_iter_get_element_type) (Iter*) = nullptr;
    void (*message_iter_get_basic) (Iter*, void*) = nullptr;
    Bool (*message_iter_next) (Iter*) = nullptr;
    void (*message_iter_recurse) (Iter*, Iter*) = nullptr;
    void (*error_init) (Error*) = nullptr;
    void (*error_free) (Error*) = nullptr;
    Bool (*error_is_set) (const Error*) = nullptr;
    void (*free) (void*) = nullptr;
    Bool (*threads_init_default)() = nullptr;

    bool loaded = false;

    Library()
    {
        auto* handle = dlopen ("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr)
            return;
        bool all = true;
        auto find = [&] (auto& function, const char* name)
        {
            function = reinterpret_cast<std::remove_reference_t<decltype (function)>> (dlsym (handle, name));
            all = all && function != nullptr;
        };
        find (connection_open_private, "dbus_connection_open_private");
        find (bus_register, "dbus_bus_register");
        find (bus_get_unique_name, "dbus_bus_get_unique_name");
        find (bus_add_match, "dbus_bus_add_match");
        find (bus_request_name, "dbus_bus_request_name");
        find (connection_send_with_reply_and_block, "dbus_connection_send_with_reply_and_block");
        find (connection_send, "dbus_connection_send");
        find (connection_flush, "dbus_connection_flush");
        find (connection_read_write, "dbus_connection_read_write");
        find (connection_pop_message, "dbus_connection_pop_message");
        find (connection_close, "dbus_connection_close");
        find (connection_unref, "dbus_connection_unref");
        find (message_new_method_call, "dbus_message_new_method_call");
        find (message_new_method_return, "dbus_message_new_method_return");
        find (message_new_error, "dbus_message_new_error");
        find (message_new_signal, "dbus_message_new_signal");
        find (message_unref, "dbus_message_unref");
        find (message_get_type, "dbus_message_get_type");
        find (message_get_path, "dbus_message_get_path");
        find (message_get_interface, "dbus_message_get_interface");
        find (message_get_member, "dbus_message_get_member");
        find (message_get_sender, "dbus_message_get_sender");
        find (message_get_error_name, "dbus_message_get_error_name");
        find (message_get_signature, "dbus_message_get_signature");
        find (message_set_serial, "dbus_message_set_serial");
        find (message_marshal, "dbus_message_marshal");
        find (message_demarshal, "dbus_message_demarshal");
        find (message_iter_init, "dbus_message_iter_init");
        find (message_iter_init_append, "dbus_message_iter_init_append");
        find (message_iter_append_basic, "dbus_message_iter_append_basic");
        find (message_iter_open_container, "dbus_message_iter_open_container");
        find (message_iter_close_container, "dbus_message_iter_close_container");
        find (message_iter_get_arg_type, "dbus_message_iter_get_arg_type");
        find (message_iter_get_element_type, "dbus_message_iter_get_element_type");
        find (message_iter_get_basic, "dbus_message_iter_get_basic");
        find (message_iter_next, "dbus_message_iter_next");
        find (message_iter_recurse, "dbus_message_iter_recurse");
        find (error_init, "dbus_error_init");
        find (error_free, "dbus_error_free");
        find (error_is_set, "dbus_error_is_set");
        find (free, "dbus_free");
        find (threads_init_default, "dbus_threads_init_default");
        loaded = all && threads_init_default() != 0;
    }
};

const Library* library()
{
    static const Library lib;
    return lib.loaded ? &lib : nullptr;
}

/** A DBusError, freed on every path out. */
struct ScopedError
{
    explicit ScopedError (const Library& l) : lib (l) { lib.error_init (&error); }
    ~ScopedError() { lib.error_free (&error); }
    bool isSet() const { return lib.error_is_set (&error) != 0; }
    juce::String text() const
    {
        return juce::String::fromUTF8 (error.name != nullptr ? error.name : "") + ": " +
               juce::String::fromUTF8 (error.message != nullptr ? error.message : "");
    }
    const Library& lib;
    Error error {};
};

bool append (const Library& lib, Iter& iter, const Value& v)
{
    switch (v.kind)
    {
    case Value::Kind::string:
    case Value::Kind::objectPath:
    {
        const auto utf8 = v.text.toStdString();
        const char* s = utf8.c_str();
        return lib.message_iter_append_basic (&iter, v.kind == Value::Kind::string ? 's' : 'o',
                                              static_cast<const void*> (&s)) != 0;
    }
    case Value::Kind::boolean:
    case Value::Kind::uint32:
    {
        const juce::uint32 n = v.number;
        return lib.message_iter_append_basic (&iter, v.kind == Value::Kind::boolean ? 'b' : 'u', &n) != 0;
    }
    case Value::Kind::bytes:
    {
        Iter sub;
        if (lib.message_iter_open_container (&iter, 'a', "y", &sub) == 0)
            return false;
        const auto utf8 = v.text.toStdString();
        bool ok = true;
        for (size_t i = 0; i <= utf8.size(); ++i) // with the terminating zero
        {
            const auto byte = (unsigned char)utf8.c_str()[i];
            ok = ok && lib.message_iter_append_basic (&sub, 'y', &byte) != 0;
        }
        return lib.message_iter_close_container (&iter, &sub) != 0 && ok;
    }
    case Value::Kind::array:
    case Value::Kind::structure:
    case Value::Kind::variant:
    {
        Iter sub;
        const auto type = v.kind == Value::Kind::array ? 'a' : v.kind == Value::Kind::structure ? 'r' : 'v';
        const auto signature = v.kind == Value::Kind::array     ? v.elementSignature.toStdString()
                               : v.kind == Value::Kind::variant ? v.children.front().signature().toStdString()
                                                                : std::string();
        if (lib.message_iter_open_container (&iter, type, signature.empty() ? nullptr : signature.c_str(), &sub) == 0)
            return false;
        bool ok = true;
        for (auto& c : v.children)
            ok = ok && append (lib, sub, c);
        return lib.message_iter_close_container (&iter, &sub) != 0 && ok;
    }
    case Value::Kind::dictionary:
    {
        Iter dict;
        if (lib.message_iter_open_container (&iter, 'a', "{sv}", &dict) == 0)
            return false;
        bool ok = true;
        for (size_t i = 0; i < v.children.size(); ++i)
        {
            Iter entry;
            ok = ok && lib.message_iter_open_container (&dict, 'e', nullptr, &entry) != 0;
            ok = ok && append (lib, entry, Value::string (v.keys[i]));
            ok = ok && append (lib, entry, Value::variant (v.children[i]));
            ok = ok && lib.message_iter_close_container (&dict, &entry) != 0;
        }
        return lib.message_iter_close_container (&iter, &dict) != 0 && ok;
    }
    }
    return false;
}

bool appendAll (const Library& lib, void* message, const std::vector<Value>& arguments)
{
    Iter iter;
    lib.message_iter_init_append (message, &iter);
    for (auto& a : arguments)
        if (! append (lib, iter, a))
            return false;
    return true;
}

juce::var read (const Library& lib, Iter& iter);

juce::var readBasic (const Library& lib, Iter& iter, int type)
{
    union
    {
        const char* text;
        juce::uint32 u32;
        juce::int32 i32;
        juce::uint64 u64;
        juce::int64 i64;
        double real;
        unsigned char byte;
        juce::uint16 u16;
        juce::int16 i16;
    } value {};
    lib.message_iter_get_basic (&iter, &value);
    switch (type)
    {
    case 's':
    case 'o':
    case 'g':
        return juce::String::fromUTF8 (value.text);
    case 'b':
        return value.u32 != 0;
    case 'y':
        return (int)value.byte;
    case 'n':
        return (int)value.i16;
    case 'q':
        return (int)value.u16;
    case 'i':
        return (int)value.i32;
    case 'u':
        return (juce::int64)value.u32;
    case 'x':
        return value.i64;
    case 't':
        return (juce::int64)value.u64;
    case 'd':
        return value.real;
    default:
        return {};
    }
}

juce::var read (const Library& lib, Iter& iter)
{
    const auto type = lib.message_iter_get_arg_type (&iter);
    if (type == 'v')
    {
        Iter sub;
        lib.message_iter_recurse (&iter, &sub);
        return read (lib, sub);
    }
    if (type == 'r')
    {
        Iter sub;
        lib.message_iter_recurse (&iter, &sub);
        juce::Array<juce::var> fields;
        while (lib.message_iter_get_arg_type (&sub) != 0)
        {
            fields.add (read (lib, sub));
            lib.message_iter_next (&sub);
        }
        return fields;
    }
    if (type == 'a')
    {
        const auto element = lib.message_iter_get_element_type (&iter);
        Iter sub;
        lib.message_iter_recurse (&iter, &sub);
        if (element == 'y')
        {
            juce::MemoryBlock bytes;
            while (lib.message_iter_get_arg_type (&sub) == 'y')
            {
                const auto byte = (char)(int)readBasic (lib, sub, 'y');
                bytes.append (&byte, 1);
                lib.message_iter_next (&sub);
            }
            auto size = bytes.getSize();
            while (size > 0 && static_cast<const char*> (bytes.getData())[size - 1] == 0)
                --size;
            return juce::String::fromUTF8 (static_cast<const char*> (bytes.getData()), (int)size);
        }
        if (element == 'e')
        {
            auto* object = new juce::DynamicObject();
            juce::var result (object);
            while (lib.message_iter_get_arg_type (&sub) == 'e')
            {
                Iter entry;
                lib.message_iter_recurse (&sub, &entry);
                const auto key = read (lib, entry).toString();
                lib.message_iter_next (&entry);
                object->setProperty (key, read (lib, entry));
                lib.message_iter_next (&sub);
            }
            return result;
        }
        juce::Array<juce::var> elements;
        while (lib.message_iter_get_arg_type (&sub) != 0)
        {
            elements.add (read (lib, sub));
            lib.message_iter_next (&sub);
        }
        return elements;
    }
    return readBasic (lib, iter, type);
}

juce::String text (const char* s) { return s != nullptr ? juce::String::fromUTF8 (s) : juce::String(); }

/** Takes over the reference to a libdbus message. */
Message toMessage (const Library& lib, void* raw)
{
    Message m;
    m.raw = std::shared_ptr<void> (raw, [&lib] (void* p) { lib.message_unref (p); });
    m.type = lib.message_get_type (raw);
    m.path = text (lib.message_get_path (raw));
    m.interface = text (lib.message_get_interface (raw));
    m.member = text (lib.message_get_member (raw));
    m.sender = text (lib.message_get_sender (raw));
    m.errorName = text (lib.message_get_error_name (raw));
    m.signature = text (lib.message_get_signature (raw));
    Iter iter;
    if (lib.message_iter_init (raw, &iter) != 0)
        do
            m.arguments.add (read (lib, iter));
        while (lib.message_iter_next (&iter) != 0);
    return m;
}

Message errorMessage (const juce::String& name, const juce::String& text)
{
    Message m;
    m.type = Message::error;
    m.errorName = name;
    m.arguments.add (text);
    return m;
}

constexpr const char* noLibrary = "libdbus-1.so.3 is not installed";
} // namespace

bool isAvailable() { return library() != nullptr; }

juce::String sessionBusAddress()
{
    if (const auto address = juce::SystemStats::getEnvironmentVariable ("DBUS_SESSION_BUS_ADDRESS", {});
        address.isNotEmpty())
        return address;
    if (const auto runtime = juce::SystemStats::getEnvironmentVariable ("XDG_RUNTIME_DIR", {}); runtime.isNotEmpty())
        if (const auto bus = juce::File (runtime).getChildFile ("bus"); bus.exists())
            return "unix:path=" + bus.getFullPathName();
    return {};
}

std::optional<Message> roundTrip (const juce::String& member, const std::vector<Value>& arguments)
{
    const auto* lib = library();
    if (lib == nullptr)
        return std::nullopt;
    auto* message =
        lib->message_new_method_call ("org.example.Test", "/org/example/Test", "org.example.Test", member.toRawUTF8());
    if (message == nullptr)
        return std::nullopt;
    std::optional<Message> result;
    char* data = nullptr;
    int size = 0;
    lib->message_set_serial (message, 1);
    if (appendAll (*lib, message, arguments) && lib->message_marshal (message, &data, &size) != 0)
    {
        ScopedError error (*lib);
        if (auto* copy = lib->message_demarshal (data, size, &error.error))
            result = toMessage (*lib, copy);
        lib->free (data);
    }
    lib->message_unref (message);
    return result;
}

//==============================================================================
Connection::Connection (void* c) : connection (c) {}

Connection::~Connection()
{
    const auto* lib = library();
    lib->connection_close (connection);
    lib->connection_unref (connection);
}

std::unique_ptr<Connection> Connection::open (const juce::String& address, juce::String& errorText)
{
    const auto* lib = library();
    if (lib == nullptr)
    {
        errorText = noLibrary;
        return nullptr;
    }
    if (address.isEmpty())
    {
        errorText = "no session bus";
        return nullptr;
    }
    ScopedError error (*lib);
    auto* c = lib->connection_open_private (address.toRawUTF8(), &error.error);
    if (c == nullptr)
    {
        errorText = error.text();
        return nullptr;
    }
    std::unique_ptr<Connection> connection (new Connection (c));
    if (lib->bus_register (c, &error.error) == 0)
    {
        errorText = error.text();
        return nullptr;
    }
    return connection;
}

juce::String Connection::getUniqueName() const { return text (library()->bus_get_unique_name (connection)); }

Message Connection::call (const juce::String& destination, const juce::String& path, const juce::String& interface,
                          const juce::String& member, const std::vector<Value>& arguments, int timeoutMs)
{
    const auto& lib = *library();
    auto* message = lib.message_new_method_call (destination.toRawUTF8(), path.toRawUTF8(), interface.toRawUTF8(),
                                                 member.toRawUTF8());
    if (message == nullptr)
        return errorMessage ("org.freedesktop.DBus.Error.NoMemory", "could not make the call");
    if (! appendAll (lib, message, arguments))
    {
        lib.message_unref (message);
        return errorMessage ("org.freedesktop.DBus.Error.InvalidArgs", "could not add the arguments");
    }
    ScopedError error (lib);
    auto* reply = lib.connection_send_with_reply_and_block (connection, message, timeoutMs, &error.error);
    lib.message_unref (message);
    if (reply == nullptr)
        return errorMessage (text (error.error.name), text (error.error.message));
    return toMessage (lib, reply);
}

bool Connection::callNoReply (const juce::String& destination, const juce::String& path, const juce::String& interface,
                              const juce::String& member, const std::vector<Value>& arguments)
{
    const auto& lib = *library();
    auto* message = lib.message_new_method_call (destination.toRawUTF8(), path.toRawUTF8(), interface.toRawUTF8(),
                                                 member.toRawUTF8());
    if (message == nullptr)
        return false;
    const bool sent = appendAll (lib, message, arguments) && lib.connection_send (connection, message, nullptr) != 0;
    lib.message_unref (message);
    lib.connection_flush (connection);
    return sent;
}

bool Connection::addMatch (const juce::String& rule)
{
    const auto& lib = *library();
    ScopedError error (lib);
    lib.bus_add_match (connection, rule.toRawUTF8(), &error.error);
    return ! error.isSet();
}

std::optional<Message> Connection::next (int timeoutMs)
{
    const auto& lib = *library();
    auto* message = lib.connection_pop_message (connection);
    if (message == nullptr)
    {
        lib.connection_read_write (connection, timeoutMs);
        message = lib.connection_pop_message (connection);
    }
    if (message == nullptr)
        return std::nullopt;
    return toMessage (lib, message);
}

bool Connection::requestName (const juce::String& name)
{
    const auto& lib = *library();
    ScopedError error (lib);
    constexpr unsigned int doNotQueue = 4;
    return lib.bus_request_name (connection, name.toRawUTF8(), doNotQueue, &error.error) == 1; // primary owner
}

bool Connection::reply (const Message& call, const std::vector<Value>& arguments)
{
    const auto& lib = *library();
    auto* message = lib.message_new_method_return (call.raw.get());
    if (message == nullptr)
        return false;
    const bool sent = appendAll (lib, message, arguments) && lib.connection_send (connection, message, nullptr) != 0;
    lib.message_unref (message);
    lib.connection_flush (connection);
    return sent;
}

bool Connection::replyError (const Message& call, const juce::String& errorName, const juce::String& errorText)
{
    const auto& lib = *library();
    auto* message = lib.message_new_error (call.raw.get(), errorName.toRawUTF8(), errorText.toRawUTF8());
    if (message == nullptr)
        return false;
    const bool sent = lib.connection_send (connection, message, nullptr) != 0;
    lib.message_unref (message);
    lib.connection_flush (connection);
    return sent;
}

bool Connection::emitSignal (const juce::String& path, const juce::String& interface, const juce::String& member,
                             const std::vector<Value>& arguments)
{
    const auto& lib = *library();
    auto* message = lib.message_new_signal (path.toRawUTF8(), interface.toRawUTF8(), member.toRawUTF8());
    if (message == nullptr)
        return false;
    const bool sent = appendAll (lib, message, arguments) && lib.connection_send (connection, message, nullptr) != 0;
    lib.message_unref (message);
    lib.connection_flush (connection);
    return sent;
}

#else // Windows: no D-Bus.

bool isAvailable() { return false; }
juce::String sessionBusAddress() { return {}; }
std::optional<Message> roundTrip (const juce::String&, const std::vector<Value>&) { return std::nullopt; }
Connection::Connection (void* c) : connection (c) {}
Connection::~Connection() = default;
std::unique_ptr<Connection> Connection::open (const juce::String&, juce::String& error)
{
    error = "no D-Bus on this system";
    return nullptr;
}
juce::String Connection::getUniqueName() const { return {}; }
Message Connection::call (const juce::String&, const juce::String&, const juce::String&, const juce::String&,
                          const std::vector<Value>&, int)
{
    return {};
}
bool Connection::callNoReply (const juce::String&, const juce::String&, const juce::String&, const juce::String&,
                              const std::vector<Value>&)
{
    return false;
}
bool Connection::addMatch (const juce::String&) { return false; }
std::optional<Message> Connection::next (int) { return std::nullopt; }
bool Connection::requestName (const juce::String&) { return false; }
bool Connection::reply (const Message&, const std::vector<Value>&) { return false; }
bool Connection::replyError (const Message&, const juce::String&, const juce::String&) { return false; }
bool Connection::emitSignal (const juce::String&, const juce::String&, const juce::String&, const std::vector<Value>&)
{
    return false;
}

#endif

} // namespace spinus::core::dbus
