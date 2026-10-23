#pragma once

#include <juce_core/juce_core.h>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

/*  A small D-Bus client (Linux), for the desktop's file dialog (the XDG Desktop Portal).

    libdbus (the reference D-Bus library, in every Linux desktop and in the Flatpak runtime) is loaded at
    run time with dlopen: Spinus starts without it, and then has no D-Bus (its file dialogs are its own).
    Nothing here is for the audio thread: calls block on the bus. A Connection is used by one thread at a
    time. On Windows the functions say there is no D-Bus.
*/
namespace spinus::core::dbus
{

/** True if libdbus could be loaded. */
bool isAvailable();

/** The session bus's address: $DBUS_SESSION_BUS_ADDRESS, else $XDG_RUNTIME_DIR/bus if it is there; empty
    when there is none. Never starts a bus (no autolaunch). */
juce::String sessionBusAddress();

/** A value to send, with its D-Bus type. */
class Value
{
public:
    static Value string (const juce::String&);
    static Value objectPath (const juce::String&);
    static Value boolean (bool);
    static Value uint32 (juce::uint32);
    /** A byte array (ay) holding the text and a terminating zero, as file paths are sent. */
    static Value bytes (const juce::String&);
    /** An array of values of one type; elementSignature is that type ("s", "(us)", ...). */
    static Value array (const juce::String& elementSignature, std::vector<Value> elements);
    static Value structure (std::vector<Value> fields);
    static Value variant (Value inner);
    /** A dictionary of string keys to variants: a{sv}, as D-Bus options are sent. */
    static Value dictionary (std::vector<std::pair<juce::String, Value>> entries);

    juce::String signature() const;

    enum class Kind
    {
        string,
        objectPath,
        boolean,
        uint32,
        bytes,
        array,
        structure,
        variant,
        dictionary
    };
    Kind kind = Kind::string;
    juce::String text;
    juce::uint32 number = 0;
    juce::String elementSignature;
    std::vector<Value> children;
    std::vector<juce::String> keys; // dictionary keys, one for each child
};

/** A message received, its arguments read as juce::var: strings, object paths and signatures as text;
    booleans; numbers; a byte array as text (without its terminating zero); other arrays and structures as
    arrays; a dictionary with string keys as an object; a variant as the value in it. */
struct Message
{
    enum Type
    {
        invalid = 0,
        methodCall = 1,
        methodReturn = 2,
        error = 3,
        signal = 4
    };
    int type = invalid;
    juce::String path, interface, member, sender, errorName, signature;
    juce::Array<juce::var> arguments;

    /** The error's text, for an error message. */
    juce::String errorText() const;

    std::shared_ptr<void> raw; // the libdbus message, for replying
};

/** Builds a method call without sending it and reads it back as it would arrive: for tests of what a
    call carries. Nothing if libdbus is missing. */
std::optional<Message> roundTrip (const juce::String& member, const std::vector<Value>& arguments);

/** A private connection to a bus. */
class Connection
{
public:
    /** Connects and says hello. Nothing (and why in `error`) if that fails. */
    static std::unique_ptr<Connection> open (const juce::String& address, juce::String& error);
    ~Connection();

    /** The connection's name on the bus (":1.42"). */
    juce::String getUniqueName() const;

    /** Calls a method and waits up to timeoutMs for the reply, which is a methodReturn or an error (also
        for a timeout or a lost connection). */
    Message call (const juce::String& destination, const juce::String& path, const juce::String& interface,
                  const juce::String& member, const std::vector<Value>& arguments, int timeoutMs);

    /** Sends a method call without waiting for any reply. */
    bool callNoReply (const juce::String& destination, const juce::String& path, const juce::String& interface,
                      const juce::String& member, const std::vector<Value>& arguments);

    bool addMatch (const juce::String& rule);

    /** Waits up to timeoutMs for a message (signals for the match rules, calls to a name this connection
        owns). */
    std::optional<Message> next (int timeoutMs);

    // For services (tests' fake portal):
    bool requestName (const juce::String& name);
    bool reply (const Message& call, const std::vector<Value>& arguments);
    bool replyError (const Message& call, const juce::String& errorName, const juce::String& text);
    bool emitSignal (const juce::String& path, const juce::String& interface, const juce::String& member,
                     const std::vector<Value>& arguments);

private:
    explicit Connection (void* connection);
    void* connection;
    JUCE_DECLARE_NON_COPYABLE (Connection)
};

} // namespace spinus::core::dbus
