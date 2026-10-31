#include "CommandRegistry.h"

#include "dsp/ParameterText.h"
#include "dsp/SpinusPluginFormat.h"
#include "dsp/threeosc/ThreeOscPresets.h"
#include "engine/ParameterIds.h"
#include "model/Automation.h"
#include "model/Freeze.h"
#include "model/Scales.h"
#include "model/ModelJson.h"
#include "model/StockDevices.h"
#include "model/Validation.h"

namespace spinus::commands
{

//==============================================================================
// Schema building

namespace
{
juce::var schemaObject (std::initializer_list<std::pair<const char*, juce::var>> items)
{
    auto* o = new juce::DynamicObject();
    for (auto& [key, value] : items)
        o->setProperty (key, value);
    return juce::var (o);
}
} // namespace

ParameterSchema& ParameterSchema::custom (const char* name, const char* description, bool isRequired,
                                          const juce::var& schema)
{
    schema.getDynamicObject()->setProperty ("description", description);
    properties->setProperty (name, schema);

    if (isRequired)
        required.add (name);

    return *this;
}

ParameterSchema& ParameterSchema::id (const char* name, const char* description, bool isRequired)
{
    return custom (name, description, isRequired, schemaObject ({ { "type", "integer" }, { "minimum", 1 } }));
}

ParameterSchema& ParameterSchema::number (const char* name, const char* description, bool isRequired, double min,
                                          double max)
{
    return custom (name, description, isRequired,
                   schemaObject ({ { "type", "number" }, { "minimum", min }, { "maximum", max } }));
}

ParameterSchema& ParameterSchema::integer (const char* name, const char* description, bool isRequired, int min, int max)
{
    return custom (name, description, isRequired,
                   schemaObject ({ { "type", "integer" }, { "minimum", min }, { "maximum", max } }));
}

ParameterSchema& ParameterSchema::string (const char* name, const char* description, bool isRequired)
{
    return custom (name, description, isRequired, schemaObject ({ { "type", "string" } }));
}

ParameterSchema& ParameterSchema::boolean (const char* name, const char* description)
{
    return custom (name, description, false, schemaObject ({ { "type", "boolean" } }));
}

juce::var ParameterSchema::build() const
{
    juce::Array<juce::var> requiredList;
    for (auto& r : required)
        requiredList.add (r);

    return schemaObject ({ { "type", "object" },
                           { "properties", juce::var (properties.get()) },
                           { "required", requiredList },
                           { "additionalProperties", false } });
}

//==============================================================================
// Parameter reading

namespace
{

/** Reads command parameters, collecting every problem instead of stopping at the first. */
struct Params
{
    explicit Params (const juce::var& v) : json (v)
    {
        if (! json.isObject())
            errors.add ("parameters must be a JSON object");
    }

    bool has (const char* key) const { return json.hasProperty (key); }

    juce::var get (const char* key, bool required)
    {
        auto v = json.getProperty (key, {});
        if (v.isVoid() && required)
            errors.add (juce::String ("missing '") + key + "'");
        return v;
    }

    template <typename IdType> IdType id (const char* key, bool required = true)
    {
        auto v = get (key, required);
        if (v.isVoid())
            return {};
        if (! (v.isInt() || v.isInt64()) || (int)v <= 0)
        {
            errors.add (juce::String ("'") + key + "' must be a positive integer ID");
            return {};
        }
        return IdType { (uint32_t)(int)v };
    }

    std::optional<double> number (const char* key, double min, double max, bool required = false)
    {
        auto v = get (key, required);
        if (v.isVoid())
            return std::nullopt;
        if (! (v.isDouble() || v.isInt() || v.isInt64()) || (double)v < min || (double)v > max)
        {
            errors.add (juce::String ("'") + key + "' must be a number from " + juce::String (min) + " to " +
                        juce::String (max));
            return std::nullopt;
        }
        return (double)v;
    }

    std::optional<juce::String> string (const char* key, bool required = false)
    {
        auto v = get (key, required);
        if (v.isVoid())
            return std::nullopt;
        if (! v.isString())
        {
            errors.add (juce::String ("'") + key + "' must be a string");
            return std::nullopt;
        }
        return v.toString();
    }

    std::optional<bool> boolean (const char* key)
    {
        auto v = get (key, false);
        if (v.isVoid())
            return std::nullopt;
        if (! v.isBool())
        {
            errors.add (juce::String ("'") + key + "' must be true or false");
            return std::nullopt;
        }
        return (bool)v;
    }

    std::optional<juce::uint32> colour (const char* key)
    {
        auto s = string (key);
        if (! s)
            return std::nullopt;
        if (! s->startsWith ("#") || s->length() != 9 || ! s->substring (1).containsOnly ("0123456789abcdefABCDEF"))
        {
            errors.add (juce::String ("'") + key + "' must be a colour like #ff336699");
            return std::nullopt;
        }
        return (juce::uint32)s->substring (1).getHexValue64();
    }

    /** Rejects keys the schema does not list (catches typos from scripts and LLMs). */
    void allowOnly (std::initializer_list<const char*> keys)
    {
        if (auto* o = json.getDynamicObject())
            for (auto& property : o->getProperties())
                if (std::none_of (keys.begin(), keys.end(), [&] (auto k) { return property.name.toString() == k; }))
                    errors.add ("unknown parameter '" + property.name.toString() + "'");
    }

    juce::var json;
    juce::StringArray errors;
};

//==============================================================================
// Helpers shared by the commands

juce::String quoted (const juce::String& s) { return "'" + s + "'"; }

/** "drums/Amen Break.wav" -> "Amen Break". Project paths use "/" on every system, and Windows also "\\". */
juce::String fileStem (const juce::String& path)
{
    const auto name = path.substring (juce::jmax (path.lastIndexOfChar ('/'), path.lastIndexOfChar ('\\')) + 1);
    return name.containsChar ('.') ? name.upToLastOccurrenceOf (".", false, false) : name;
}

template <typename Item, typename IdType> int indexOf (const std::vector<Item>& items, IdType id)
{
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].id == id)
            return (int)i;
    return -1;
}

template <typename Item, typename IdType>
void collectChanged (const std::vector<Item>& before, const std::vector<Item>& after, std::set<IdType>& changed)
{
    for (auto& a : after)
    {
        auto it = std::find_if (before.begin(), before.end(), [&] (auto& b) { return b.id == a.id; });
        if (it == before.end() || ! (*it == a))
            changed.insert (a.id);
    }

    for (auto& b : before)
        if (indexOf (after, b.id) < 0)
            changed.insert (b.id);
}

juce::var resultWith (std::initializer_list<std::pair<const char*, uint32_t>> ids)
{
    auto* o = new juce::DynamicObject();
    for (auto& [key, value] : ids)
        if (value != 0)
            o->setProperty (key, (int)value);
    return juce::var (o);
}

/** Base for commands built from JSON parameters: keeps the parameters for getParameters(). */
class JsonCommand : public Command
{
public:
    JsonCommand (juce::String commandName, juce::var params)
        : name (std::move (commandName)), parameters (std::move (params))
    {
    }

    juce::String getName() const override { return name; }
    juce::var getParameters() const override { return parameters; }
    ChangeSet getChanges() const override { return changes; }

protected:
    juce::String name;
    juce::var parameters;
    ChangeSet changes;
};

/** For commands whose undo restores whole sections of the project (simple and always exact). */
struct Snapshot
{
    std::vector<Channel> channels;
    std::vector<Pattern> patterns;
    Playlist playlist;
    Mixer mixer;
    Launcher launcher;

    static Snapshot of (const Project& p) { return { p.channels, p.patterns, p.playlist, p.mixer, p.launcher }; }

    void restore (Project& p) const
    {
        p.channels = channels;
        p.patterns = patterns;
        p.playlist = playlist;
        p.mixer = mixer;
        p.launcher = launcher;
    }

    ChangeSet diff (const Project& after) const
    {
        ChangeSet c;
        collectChanged (channels, after.channels, c.channels);
        collectChanged (patterns, after.patterns, c.patterns);
        collectChanged (playlist.lanes, after.playlist.lanes, c.lanes);
        collectChanged (playlist.clips, after.playlist.clips, c.clips);
        collectChanged (mixer.inserts, after.mixer.inserts, c.inserts);

        if (! (mixer.master == after.mixer.master))
            c.inserts.insert (masterInsertId);

        auto ids = [] (auto& items)
        {
            std::vector<uint32_t> r;
            r.reserve (items.size());
            for (auto& i : items)
                r.push_back (i.id.value);
            return r;
        };

        c.channelOrder = ids (channels) != ids (after.channels);
        c.insertOrder = ids (mixer.inserts) != ids (after.mixer.inserts);
        c.laneOrder = ids (playlist.lanes) != ids (after.playlist.lanes);
        c.launcher = ! (launcher == after.launcher);
        c.markers = playlist.markers != after.playlist.markers;
        return c;
    }
};

/** A command that edits the project in perform() and undoes by restoring a snapshot. */
class SnapshotCommand : public JsonCommand
{
public:
    using JsonCommand::JsonCommand;

    juce::Result perform (Project& p) final
    {
        auto before = Snapshot::of (p);
        if (auto r = apply (p); r.failed())
        {
            before.restore (p);
            return r;
        }

        changes = before.diff (p);
        snapshot = std::move (before);
        return juce::Result::ok();
    }

    void undo (Project& p) final { snapshot.restore (p); }

protected:
    /** Makes the change. On failure the project is restored automatically. */
    virtual juce::Result apply (Project&) = 0;

private:
    Snapshot snapshot;
};

// Snapshots copy the channel, pattern, playlist and mixer sections. That is fine for structural edits
// (add/remove/route), which are rare. Frequent edits (notes, knobs) use targeted undo below.

//==============================================================================
// project.*

class SetTempo final : public JsonCommand
{
public:
    SetTempo (juce::var p, double newBpm) : JsonCommand ("project.setTempo", std::move (p)), bpm (newBpm)
    {
        changes.settings = true;
    }

    juce::String getDescription() const override { return "Set tempo to " + juce::String (bpm, 2) + " BPM"; }

    juce::Result perform (Project& p) override
    {
        old = p.settings.tempoBpm;
        p.settings.tempoBpm = bpm;
        return juce::Result::ok();
    }

    void undo (Project& p) override { p.settings.tempoBpm = old; }

private:
    double bpm, old = 0.0;
};

/** A pattern's length changed (its notes or time signature): its Playlist clips that were as long as the
    pattern, from its start, follow it (FL); clips made shorter or longer by hand stay. Remembers the old
    lengths in `old` for undo. */
void fitPatternClips (Project& p, PatternId pattern, Ticks oldLength, Ticks newLength, ChangeSet& changes,
                      std::map<ClipId, Ticks>& old)
{
    if (oldLength == newLength)
        return;
    for (auto& clip : p.playlist.clips)
        if (auto* pc = std::get_if<PatternClip> (&clip.content);
            pc != nullptr && pc->pattern == pattern && pc->offset == 0 && clip.length == oldLength)
        {
            old.emplace (clip.id, clip.length);
            clip.length = newLength;
            changes.clips.insert (clip.id);
        }
}

void restoreClipLengths (Project& p, const std::map<ClipId, Ticks>& old)
{
    for (auto& [id, length] : old)
        if (auto* clip = p.findClip (id))
            clip->length = length;
}

/** The song's time signature: bars in the Playlist, the metronome, and every pattern without its own. */
class SetTimeSignature final : public JsonCommand
{
public:
    SetTimeSignature (juce::var p, TimeSignature s)
        : JsonCommand ("project.setTimeSignature", std::move (p)), signature (s)
    {
        changes.settings = true;
    }

    juce::String getDescription() const override
    {
        return "Set time signature to " + juce::String (signature.numerator) + "/" +
               juce::String (signature.denominator);
    }

    juce::Result perform (Project& p) override
    {
        if (! signature.isValid())
            return juce::Result::fail ("a time signature needs 1-32 beats over 1, 2, 4, 8, 16 or 32");
        old = p.settings.timeSignature;
        p.settings.timeSignature = signature;
        // Patterns that follow the song's time signature: whole bars of the new one.
        oldLengths.clear();
        oldClipLengths.clear();
        for (auto& pattern : p.patterns)
            if (! pattern.timeSignature)
            {
                oldLengths[pattern.id] = pattern.length;
                pattern.length = p.fittedLength (pattern);
                changes.patterns.insert (pattern.id);
                fitPatternClips (p, pattern.id, oldLengths[pattern.id], pattern.length, changes, oldClipLengths);
            }
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        p.settings.timeSignature = old;
        for (auto& [id, length] : oldLengths)
            if (auto* pattern = p.findPattern (id))
                pattern->length = length;
        restoreClipLengths (p, oldClipLengths);
    }

private:
    TimeSignature signature, old;
    std::map<PatternId, Ticks> oldLengths;
    std::map<ClipId, Ticks> oldClipLengths;
};

/** The song's key (used by Spinus Tune, the pattern generator and Record idea), or none. */
class SetKey final : public JsonCommand
{
public:
    SetKey (juce::var p, std::optional<Key> k) : JsonCommand ("project.setKey", std::move (p)), key (std::move (k))
    {
        changes.settings = true;
    }

    juce::String getDescription() const override
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return key ? "Set key to " + juce::String (names[key->tonic]) + " " + key->scale : juce::String ("Clear key");
    }

    juce::Result perform (Project& p) override
    {
        old = p.settings.key;
        p.settings.key = key;
        return juce::Result::ok();
    }

    void undo (Project& p) override { p.settings.key = old; }

private:
    std::optional<Key> key, old;
};

//==============================================================================
// channel.*

class AddChannel final : public SnapshotCommand
{
public:
    AddChannel (juce::var p, juce::String nameToUse, std::optional<Device> device, juce::var insertParam,
                std::optional<int> position, bool asAudioTrack)
        : SnapshotCommand ("channel.add", std::move (p)), channelName (std::move (nameToUse)),
          instrument (std::move (device)), insert (std::move (insertParam)), index (position), audioTrack (asAudioTrack)
    {
    }

    juce::String getDescription() const override
    {
        return (audioTrack ? "Add audio track " : "Add channel ") + quoted (channelName);
    }

    juce::var getResult() const override
    {
        return resultWith (
            { { "channel", channelId.value }, { "insert", newInsertId.value }, { "device", deviceId.value } });
    }

protected:
    juce::Result apply (Project& p) override
    {
        const bool newInsert = insert.isVoid() || insert.toString() == "new";
        auto target = masterInsertId;

        if (! newInsert && insert.toString() != "master")
        {
            target = InsertId { (uint32_t)(int)insert };
            if (! p.findInsert (target))
                return juce::Result::fail ("insert " + insert.toString() + " does not exist");
        }

        if (index && (*index < 0 || *index > (int)p.channels.size()))
            return juce::Result::fail ("index out of range");

        // First perform allocates IDs; redo reuses them.
        if (! channelId)
        {
            channelId = p.allocateId<ChannelId>();
            if (instrument)
                deviceId = p.allocateId<DeviceId>();
            if (newInsert)
                newInsertId = p.allocateId<InsertId>();
        }

        if (newInsert)
        {
            Insert i;
            i.id = newInsertId;
            i.name = channelName;
            p.mixer.inserts.push_back (i);
            target = newInsertId;
        }

        Channel c;
        c.id = channelId;
        c.name = channelName;
        c.targetInsert = target;
        c.audioTrack = audioTrack;

        if (instrument)
        {
            c.instrument = *instrument;
            c.instrument->id = deviceId;
        }

        p.channels.insert (p.channels.begin() + (index ? *index : (int)p.channels.size()), c);
        return juce::Result::ok();
    }

private:
    juce::String channelName;
    std::optional<Device> instrument;
    juce::var insert;
    std::optional<int> index;
    bool audioTrack = false;
    ChannelId channelId;
    DeviceId deviceId;
    InsertId newInsertId;
};

class RemoveChannel final : public SnapshotCommand
{
public:
    RemoveChannel (juce::var p, ChannelId c) : SnapshotCommand ("channel.remove", std::move (p)), channel (c) {}

    juce::String getDescription() const override { return "Remove channel " + quoted (removedName); }

protected:
    juce::Result apply (Project& p) override
    {
        const auto i = indexOf (p.channels, channel);
        if (i < 0)
            return juce::Result::fail ("channel " + juce::String (channel.value) + " does not exist");

        removedName = p.channels[(size_t)i].name;
        std::optional<DeviceId> device;
        if (auto& instrument = p.channels[(size_t)i].instrument)
            device = instrument->id;

        p.channels.erase (p.channels.begin() + i);

        for (auto& pattern : p.patterns)
            pattern.notes.erase (channel);

        for (auto& insert : p.mixer.inserts)
            if (insert.notesFrom == channel)
                insert.notesFrom = {};

        // Automation of the channel or its instrument, and audio clips playing through the channel, would
        // point at nothing.
        std::erase_if (p.playlist.clips,
                       [&] (const Clip& clip)
                       {
                           if (auto* audio = std::get_if<AudioClip> (&clip.content))
                               return audio->channel == channel;
                           auto* a = std::get_if<AutomationClip> (&clip.content);
                           return a != nullptr &&
                                  (a->target.channel == channel || (device && a->target.device == *device));
                       });
        std::erase_if (p.launcher.clips,
                       [&] (const LauncherClip& clip)
                       {
                           auto* audio = std::get_if<AudioClip> (&clip.content);
                           return audio != nullptr && audio->channel == channel;
                       });

        return juce::Result::ok();
    }

private:
    ChannelId channel;
    juce::String removedName;
};

/** FL's "Replace" on a channel: another instrument plays the channel's notes. The channel keeps everything
    else (colour, group, insert, volume, pan, notes, Playlist clips); its name too, unless it was the old
    instrument's name, which becomes the new one's (and so does its insert's, if the insert is the channel's
    own and still has that name). Automation of the old instrument's settings is removed in the same step: the
    new one has other settings, so those clips would move nothing (undo brings them back). The old instrument's
    settings come back with undo (the engine keeps a removed device's state). */
class ReplaceInstrument final : public SnapshotCommand
{
public:
    ReplaceInstrument (juce::var p, ChannelId c, Device device, std::optional<juce::String> nameToUse)
        : SnapshotCommand ("channel.replaceInstrument", std::move (p)), channel (c), instrument (std::move (device)),
          name (std::move (nameToUse))
    {
    }

    juce::String getDescription() const override
    {
        return "Replace instrument of " + quoted (channelName) + " with " + getDeviceDisplayName (instrument);
    }

    juce::var getResult() const override
    {
        return resultWith ({ { "channel", channel.value }, { "device", deviceId.value } });
    }

protected:
    juce::Result apply (Project& p) override
    {
        auto* c = p.findChannel (channel);
        if (c == nullptr)
            return juce::Result::fail ("channel " + juce::String (channel.value) + " does not exist");
        if (c->audioTrack)
            return juce::Result::fail ("an audio track has no instrument");

        if (! deviceId) // first perform allocates the ID; redo reuses it
            deviceId = p.allocateId<DeviceId>();

        const auto old = c->instrument;
        const auto oldName = c->name;
        auto newName = name.value_or (oldName);
        if (! name && old && oldName == getDeviceDisplayName (*old))
            newName = getDeviceDisplayName (instrument);
        channelName = oldName;

        c->instrument = instrument;
        c->instrument->id = deviceId;
        c->frozen = false; // the new instrument plays live; freeze it again when it sounds right

        if (newName != oldName)
        {
            c->name = newName;
            // Its own insert (named with it when it was added) follows; a bus other channels play into keeps
            // its name.
            const auto shared = std::any_of (p.channels.begin(), p.channels.end(), [&] (const Channel& other)
                                             { return other.id != channel && other.targetInsert == c->targetInsert; });
            if (auto* insert = p.findInsert (c->targetInsert);
                insert != nullptr && c->targetInsert != masterInsertId && insert->name == oldName && ! shared)
                insert->name = std::move (newName);
        }

        if (old)
            std::erase_if (p.playlist.clips,
                           [&] (const Clip& clip)
                           {
                               auto* a = std::get_if<AutomationClip> (&clip.content);
                               return a != nullptr && a->target.kind == ParameterTarget::Kind::deviceParameter &&
                                      a->target.device == old->id;
                           });
        return juce::Result::ok();
    }

private:
    ChannelId channel;
    Device instrument;
    std::optional<juce::String> name;
    juce::String channelName;
    DeviceId deviceId;
};

class SetChannel final : public JsonCommand
{
public:
    struct Values
    {
        std::optional<juce::String> name;
        std::optional<juce::uint32> colour;
        std::optional<double> volumeDb, pan, pitchCents;
        std::optional<bool> muted, solo;
        std::optional<juce::String> group;
        std::optional<double> recordInput;
        std::optional<bool> recordStereo, recordArmed;
    };

    SetChannel (juce::var p, ChannelId c, Values v)
        : JsonCommand ("channel.set", std::move (p)), channel (c), values (std::move (v))
    {
        changes.channels.insert (c);
    }

    juce::String getDescription() const override
    {
        if (values.name)
            return "Rename channel to " + quoted (*values.name);
        if (values.group)
            return values.group->isEmpty() ? "Take channel out of its group"
                                           : "Put channel in group " + quoted (*values.group);
        if (values.volumeDb)
            return "Change channel volume";
        if (values.pan)
            return "Change channel pan";
        if (values.muted)
            return *values.muted ? "Mute channel" : "Unmute channel";
        if (values.recordArmed)
            return *values.recordArmed ? "Arm track for recording" : "Disarm track";
        if (values.recordInput || values.recordStereo)
            return "Choose the track's input";
        return "Change channel";
    }

    juce::Result perform (Project& p) override
    {
        auto* c = p.findChannel (channel);
        if (c == nullptr)
            return juce::Result::fail ("channel " + juce::String (channel.value) + " does not exist");

        old = *c;
        if (values.name)
            c->name = *values.name;
        if (values.colour)
            c->colour = *values.colour;
        if (values.volumeDb)
            c->volumeDb = (float)*values.volumeDb;
        if (values.pan)
            c->pan = (float)*values.pan;
        if (values.pitchCents)
            c->pitchCents = (float)*values.pitchCents;
        if (values.muted)
            c->muted = *values.muted;
        if (values.solo)
            c->solo = *values.solo;
        if (values.group)
            c->group = values.group->trim();
        if (values.recordInput)
            c->recordInput = juce::roundToInt (*values.recordInput);
        if (values.recordStereo)
            c->recordStereo = *values.recordStereo;
        if (values.recordArmed)
            c->recordArmed = *values.recordArmed;
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        if (auto* c = p.findChannel (channel))
            *c = old;
    }

private:
    ChannelId channel;
    Values values;
    Channel old;
};

class RouteChannel final : public JsonCommand
{
public:
    RouteChannel (juce::var p, ChannelId c, InsertId i)
        : JsonCommand ("channel.route", std::move (p)), channel (c), insert (i)
    {
        changes.channels.insert (c);
    }

    juce::String getDescription() const override { return "Route channel to insert"; }

    juce::Result perform (Project& p) override
    {
        auto* c = p.findChannel (channel);
        if (c == nullptr)
            return juce::Result::fail ("channel " + juce::String (channel.value) + " does not exist");
        if (! p.findInsert (insert))
            return juce::Result::fail ("insert " + juce::String (insert.value) + " does not exist");

        old = c->targetInsert;
        c->targetInsert = insert;
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        if (auto* c = p.findChannel (channel))
            c->targetInsert = old;
    }

private:
    ChannelId channel;
    InsertId insert, old;
};

/** Freeze or unfreeze a channel or an insert (model/Freeze.h). The audio is rendered by the session. */
template <typename IdType> class SetFrozen final : public JsonCommand
{
public:
    static constexpr bool isChannel = std::is_same_v<IdType, ChannelId>;

    SetFrozen (juce::var p, IdType itemId, bool shouldFreeze)
        : JsonCommand (isChannel ? "channel.freeze" : "insert.freeze", std::move (p)), item (itemId),
          frozen (shouldFreeze)
    {
        if constexpr (isChannel)
            changes.channels.insert (itemId);
        else
            changes.inserts.insert (itemId);
    }

    juce::String getDescription() const override
    {
        return juce::String (frozen ? "Freeze " : "Unfreeze ") + (isChannel ? "channel" : "insert") +
               (name.isNotEmpty() ? " " + quoted (name) : juce::String());
    }

    juce::Result perform (Project& p) override
    {
        auto* target = find (p);
        const auto what = juce::String (isChannel ? "channel " : "insert ") + juce::String (item.value);
        if (target == nullptr)
            return juce::Result::fail (what + " does not exist");

        if constexpr (! isChannel)
            if (frozen)
                if (auto why = whyCannotFreeze (p, item); why.isNotEmpty())
                    return juce::Result::fail (why);

        name = target->name;
        old = target->frozen;
        target->frozen = frozen;
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        if (auto* target = find (p))
            target->frozen = old;
    }

private:
    auto* find (Project& p) const
    {
        if constexpr (isChannel)
            return p.findChannel (item);
        else
            return p.findInsert (item);
    }

    IdType item;
    bool frozen, old = false;
    juce::String name;
};

template <typename IdType> class MoveItem final : public JsonCommand
{
public:
    using Accessor =
        std::function<std::vector<std::conditional_t<std::is_same_v<IdType, ChannelId>, Channel, Insert>>&(Project&)>;

    MoveItem (juce::String commandName, juce::var p, IdType itemId, int newIndex, Accessor a, juce::String what)
        : JsonCommand (std::move (commandName), std::move (p)), item (itemId), index (newIndex), items (std::move (a)),
          noun (std::move (what))
    {
        if constexpr (std::is_same_v<IdType, ChannelId>)
            changes.channelOrder = true;
        else
            changes.insertOrder = true;
    }

    juce::String getDescription() const override { return "Move " + noun; }

    juce::Result perform (Project& p) override
    {
        auto& list = items (p);
        const auto from = indexOf (list, item);
        if (from < 0)
            return juce::Result::fail (noun + " " + juce::String (item.value) + " does not exist");
        if (index < 0 || index >= (int)list.size())
            return juce::Result::fail ("index out of range");

        oldIndex = from;
        moveTo (list, from, index);
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        auto& list = items (p);
        moveTo (list, indexOf (list, item), oldIndex);
    }

private:
    template <typename List> static void moveTo (List& list, int from, int to)
    {
        auto value = list[(size_t)from];
        list.erase (list.begin() + from);
        list.insert (list.begin() + to, value);
    }

    IdType item;
    int index, oldIndex = 0;
    Accessor items;
    juce::String noun;
};

//==============================================================================
// insert.*

class AddInsert final : public SnapshotCommand
{
public:
    AddInsert (juce::var p, juce::String n, std::optional<int> position)
        : SnapshotCommand ("insert.add", std::move (p)), insertName (std::move (n)), index (position)
    {
    }

    juce::String getDescription() const override { return "Add mixer insert"; }
    juce::var getResult() const override { return resultWith ({ { "insert", insert.value } }); }

protected:
    juce::Result apply (Project& p) override
    {
        if (index && (*index < 0 || *index > (int)p.mixer.inserts.size()))
            return juce::Result::fail ("index out of range");

        if (! insert)
            insert = p.allocateId<InsertId>();

        Insert i;
        i.id = insert;
        i.name = insertName.isNotEmpty() ? insertName : "Insert " + juce::String ((int)p.mixer.inserts.size() + 1);
        p.mixer.inserts.insert (p.mixer.inserts.begin() + (index ? *index : (int)p.mixer.inserts.size()), i);
        return juce::Result::ok();
    }

private:
    juce::String insertName;
    std::optional<int> index;
    InsertId insert;
};

/** A new insert that other inserts play into (a bus, e.g. "Drums" for a group's inserts): one undo step. */
class AddBus final : public SnapshotCommand
{
public:
    AddBus (juce::var p, juce::String n, std::vector<InsertId> s)
        : SnapshotCommand ("insert.addBus", std::move (p)), busName (std::move (n)), sources (std::move (s))
    {
    }

    juce::String getDescription() const override { return "Route into a new bus " + quoted (busName); }
    juce::var getResult() const override { return resultWith ({ { "insert", bus.value } }); }

protected:
    juce::Result apply (Project& p) override
    {
        if (sources.empty())
            return juce::Result::fail ("give at least one insert to route into the bus");
        int last = -1;
        std::optional<InsertId> sharedOutput;
        bool sameOutput = true;
        for (auto id : sources)
        {
            if (id == masterInsertId)
                return juce::Result::fail ("the master cannot play into a bus");
            const auto at = std::find_if (p.mixer.inserts.begin(), p.mixer.inserts.end(),
                                          [id] (const Insert& i) { return i.id == id; });
            if (at == p.mixer.inserts.end())
                return juce::Result::fail ("insert " + juce::String (id.value) + " does not exist");
            last = juce::jmax (last, (int)(at - p.mixer.inserts.begin()));
            sameOutput = sameOutput && (! sharedOutput || *sharedOutput == at->output);
            sharedOutput = at->output;
        }

        if (! bus)
            bus = p.allocateId<InsertId>();
        Insert i;
        i.id = bus;
        i.name = busName.isNotEmpty() ? busName : "Bus";
        // Where they all went before (a bus inside a bus stays there), or the master.
        i.output = sameOutput && sharedOutput ? *sharedOutput : masterInsertId;
        p.mixer.inserts.insert (p.mixer.inserts.begin() + last + 1, i);
        for (auto id : sources)
            p.findInsert (id)->output = bus;
        return juce::Result::ok();
    }

private:
    juce::String busName;
    std::vector<InsertId> sources;
    InsertId bus;
};

class RemoveInsert final : public SnapshotCommand
{
public:
    RemoveInsert (juce::var p, InsertId i) : SnapshotCommand ("insert.remove", std::move (p)), insert (i) {}

    juce::String getDescription() const override { return "Remove insert " + quoted (removedName); }

protected:
    juce::Result apply (Project& p) override
    {
        if (insert == masterInsertId)
            return juce::Result::fail ("the master insert cannot be removed");

        const auto i = indexOf (p.mixer.inserts, insert);
        if (i < 0)
            return juce::Result::fail ("insert " + juce::String (insert.value) + " does not exist");

        const auto removed = p.mixer.inserts[(size_t)i];
        removedName = removed.name;
        p.mixer.inserts.erase (p.mixer.inserts.begin() + i);

        // Everything that fed it goes to master instead; sends and sidechains from it disappear (FL behaviour).
        for (auto& c : p.channels)
            if (c.targetInsert == insert)
                c.targetInsert = masterInsertId;

        for (auto* other : allInserts (p))
        {
            if (other->output == insert)
                other->output = masterInsertId;
            std::erase_if (other->sends, [&] (auto& s) { return s.target == insert; });
            std::erase (other->sidechainSources, insert);
        }

        std::set<DeviceId> devices;
        for (auto& slot : removed.effects)
            devices.insert (slot.device.id);

        std::erase_if (p.playlist.clips,
                       [&] (const Clip& clip)
                       {
                           auto* a = std::get_if<AutomationClip> (&clip.content);
                           return a != nullptr && (a->target.insert == insert || devices.contains (a->target.device));
                       });

        return juce::Result::ok();
    }

private:
    static std::vector<Insert*> allInserts (Project& p)
    {
        std::vector<Insert*> r { &p.mixer.master };
        for (auto& i : p.mixer.inserts)
            r.push_back (&i);
        return r;
    }

    InsertId insert;
    juce::String removedName;
};

class SetInsert final : public JsonCommand
{
public:
    struct Values
    {
        std::optional<juce::String> name, icon;
        std::optional<juce::uint32> colour;
        std::optional<double> volumeDb, pan, stereoWidth;
        std::optional<bool> muted, solo;
    };

    SetInsert (juce::var p, InsertId i, Values v)
        : JsonCommand ("insert.set", std::move (p)), insert (i), values (std::move (v))
    {
        changes.inserts.insert (i);
    }

    juce::String getDescription() const override
    {
        if (values.name)
            return "Rename insert to " + quoted (*values.name);
        if (values.volumeDb)
            return "Change insert volume";
        if (values.muted)
            return *values.muted ? "Mute insert" : "Unmute insert";
        return "Change insert";
    }

    juce::Result perform (Project& p) override
    {
        auto* i = p.findInsert (insert);
        if (i == nullptr)
            return juce::Result::fail ("insert " + juce::String (insert.value) + " does not exist");

        old = *i;
        if (values.name)
            i->name = *values.name;
        if (values.icon)
            i->icon = *values.icon;
        if (values.colour)
            i->colour = *values.colour;
        if (values.volumeDb)
            i->volumeDb = (float)*values.volumeDb;
        if (values.pan)
            i->pan = (float)*values.pan;
        if (values.stereoWidth)
            i->stereoWidth = (float)*values.stereoWidth;
        if (values.muted)
            i->muted = *values.muted;
        if (values.solo)
            i->solo = *values.solo;
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        if (auto* i = p.findInsert (insert))
            *i = old;
    }

private:
    InsertId insert;
    Values values;
    Insert old;
};

class SetNotesFrom final : public JsonCommand
{
public:
    SetNotesFrom (juce::var p, InsertId i, ChannelId c)
        : JsonCommand ("insert.setNotesFrom", std::move (p)), insert (i), channel (c)
    {
        changes.inserts.insert (i);
    }

    juce::String getDescription() const override
    {
        return channel ? "Take notes from a channel" : "Stop taking notes from a channel";
    }

    juce::Result perform (Project& p) override
    {
        auto* i = p.findInsert (insert);
        if (i == nullptr || insert == masterInsertId)
            return juce::Result::fail ("insert " + juce::String (insert.value) + " does not exist or is the master");
        if (channel && p.findChannel (channel) == nullptr)
            return juce::Result::fail ("channel " + juce::String (channel.value) + " does not exist");

        old = i->notesFrom;
        i->notesFrom = channel;
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        if (auto* i = p.findInsert (insert))
            i->notesFrom = old;
    }

private:
    InsertId insert;
    ChannelId channel, old;
};

class SetInsertOutput final : public JsonCommand
{
public:
    SetInsertOutput (juce::var p, InsertId i, InsertId t)
        : JsonCommand ("insert.setOutput", std::move (p)), insert (i), target (t)
    {
        changes.inserts.insert (i);
    }

    juce::String getDescription() const override { return "Route insert output"; }

    juce::Result perform (Project& p) override
    {
        auto* i = p.findInsert (insert);
        if (i == nullptr || insert == masterInsertId)
            return juce::Result::fail ("insert " + juce::String (insert.value) + " does not exist or is the master");
        if (! p.findInsert (target))
            return juce::Result::fail ("insert " + juce::String (target.value) + " does not exist");

        // Ignore the current output while checking: it is being replaced.
        old = i->output;
        i->output = {};
        if (wouldCreateRoutingCycle (p, insert, target))
        {
            i->output = old;
            return juce::Result::fail ("that routing would create a feedback loop");
        }

        i->output = target;
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        if (auto* i = p.findInsert (insert))
            i->output = old;
    }

private:
    InsertId insert, target, old;
};

class SetSidechain final : public JsonCommand
{
public:
    SetSidechain (juce::var p, InsertId i, InsertId s)
        : JsonCommand ("insert.setSidechain", std::move (p)), insert (i), source (s)
    {
        changes.inserts.insert (i);
    }

    juce::String getDescription() const override { return source ? "Set sidechain" : "Remove sidechain"; }

    juce::Result perform (Project& p) override
    {
        auto* i = p.findInsert (insert);
        if (i == nullptr)
            return juce::Result::fail ("insert " + juce::String (insert.value) + " does not exist");

        old = i->sidechainSources;
        if (! source)
        {
            i->sidechainSources.clear();
            return juce::Result::ok();
        }

        if (! p.findInsert (source))
            return juce::Result::fail ("insert " + juce::String (source.value) + " does not exist");

        // The insert waits for its source's sound: the source must not depend on the insert.
        i->sidechainSources.clear();
        if (wouldCreateRoutingCycle (p, source, insert))
        {
            i->sidechainSources = old;
            return juce::Result::fail ("that sidechain would create a feedback loop");
        }

        i->sidechainSources = { source };
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        if (auto* i = p.findInsert (insert))
            i->sidechainSources = old;
    }

private:
    InsertId insert, source;
    std::vector<InsertId> old;
};

class SetSend final : public JsonCommand
{
public:
    SetSend (juce::var p, InsertId i, InsertId t, double level)
        : JsonCommand ("insert.setSend", std::move (p)), insert (i), target (t), levelDb ((float)level)
    {
        changes.inserts.insert (i);
    }

    juce::String getDescription() const override { return "Set send level"; }

    juce::Result perform (Project& p) override
    {
        auto* i = p.findInsert (insert);
        if (i == nullptr)
            return juce::Result::fail ("insert " + juce::String (insert.value) + " does not exist");
        if (! p.findInsert (target))
            return juce::Result::fail ("insert " + juce::String (target.value) + " does not exist");

        auto existing = std::find_if (i->sends.begin(), i->sends.end(), [&] (auto& s) { return s.target == target; });
        if (existing == i->sends.end() && wouldCreateRoutingCycle (p, insert, target))
            return juce::Result::fail ("that send would create a feedback loop");

        oldSends = i->sends;
        if (existing != i->sends.end())
            existing->levelDb = levelDb;
        else
            i->sends.push_back ({ target, levelDb });
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        if (auto* i = p.findInsert (insert))
            i->sends = oldSends;
    }

private:
    InsertId insert, target;
    float levelDb;
    std::vector<Send> oldSends;
};

class RemoveSend final : public JsonCommand
{
public:
    RemoveSend (juce::var p, InsertId i, InsertId t)
        : JsonCommand ("insert.removeSend", std::move (p)), insert (i), target (t)
    {
        changes.inserts.insert (i);
    }

    juce::String getDescription() const override { return "Remove send"; }

    juce::Result perform (Project& p) override
    {
        auto* i = p.findInsert (insert);
        if (i == nullptr)
            return juce::Result::fail ("insert " + juce::String (insert.value) + " does not exist");

        oldSends = i->sends;
        if (std::erase_if (i->sends, [&] (auto& s) { return s.target == target; }) == 0)
            return juce::Result::fail ("insert " + juce::String (insert.value) + " has no send to " +
                                       juce::String (target.value));
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        if (auto* i = p.findInsert (insert))
            i->sends = oldSends;
    }

private:
    InsertId insert, target;
    std::vector<Send> oldSends;
};

/** The insert holding the effect with this device, and the slot's position in it. */
std::pair<Insert*, int> findEffect (Project& p, DeviceId device)
{
    auto search = [&] (Insert& i) -> int
    {
        for (size_t s = 0; s < i.effects.size(); ++s)
            if (i.effects[s].device.id == device)
                return (int)s;
        return -1;
    };

    if (auto s = search (p.mixer.master); s >= 0)
        return { &p.mixer.master, s };
    for (auto& i : p.mixer.inserts)
        if (auto s = search (i); s >= 0)
            return { &i, s };
    return { nullptr, -1 };
}

juce::String effectName (const Device& d) { return getDeviceDisplayName (d); }

class AddEffect final : public SnapshotCommand
{
public:
    AddEffect (juce::var p, InsertId i, Device d, std::optional<int> position)
        : SnapshotCommand ("insert.addEffect", std::move (p)), insert (i), device (std::move (d)), index (position)
    {
    }

    juce::String getDescription() const override { return "Add effect " + quoted (effectName (device)); }
    juce::var getResult() const override { return resultWith ({ { "device", deviceId.value } }); }

protected:
    juce::Result apply (Project& p) override
    {
        auto* i = p.findInsert (insert);
        if (i == nullptr)
            return juce::Result::fail ("insert " + juce::String (insert.value) + " does not exist");
        if (index && *index > (int)i->effects.size())
            return juce::Result::fail ("index out of range");

        if (! deviceId)
            deviceId = p.allocateId<DeviceId>();

        EffectSlot slot;
        slot.device = device;
        slot.device.id = deviceId;
        i->effects.insert (i->effects.begin() + (index ? *index : (int)i->effects.size()), slot);
        return juce::Result::ok();
    }

private:
    InsertId insert;
    Device device;
    std::optional<int> index;
    DeviceId deviceId;
};

class RemoveEffect final : public SnapshotCommand
{
public:
    RemoveEffect (juce::var p, DeviceId d) : SnapshotCommand ("insert.removeEffect", std::move (p)), device (d) {}

    juce::String getDescription() const override { return "Remove effect " + quoted (removedName); }

protected:
    juce::Result apply (Project& p) override
    {
        auto [insert, slot] = findEffect (p, device);
        if (insert == nullptr)
            return juce::Result::fail ("no effect with device " + juce::String (device.value));

        removedName = effectName (insert->effects[(size_t)slot].device);
        insert->effects.erase (insert->effects.begin() + slot);

        std::erase_if (p.playlist.clips,
                       [&] (const Clip& clip)
                       {
                           auto* a = std::get_if<AutomationClip> (&clip.content);
                           return a != nullptr && a->target.device == device;
                       });
        return juce::Result::ok();
    }

private:
    DeviceId device;
    juce::String removedName;
};

class MoveEffect final : public SnapshotCommand
{
public:
    MoveEffect (juce::var p, DeviceId d, std::optional<InsertId> to, int position)
        : SnapshotCommand ("insert.moveEffect", std::move (p)), device (d), target (to), index (position)
    {
    }

    juce::String getDescription() const override { return "Move effect " + quoted (movedName); }

protected:
    juce::Result apply (Project& p) override
    {
        auto [from, slot] = findEffect (p, device);
        if (from == nullptr)
            return juce::Result::fail ("no effect with device " + juce::String (device.value));

        auto* to = from;
        if (target)
        {
            to = p.findInsert (*target);
            if (to == nullptr)
                return juce::Result::fail ("insert " + juce::String (target->value) + " does not exist");
        }

        const auto room = (int)to->effects.size() - (to == from ? 1 : 0);
        if (index > room)
            return juce::Result::fail ("index out of range");

        auto moved = from->effects[(size_t)slot];
        movedName = effectName (moved.device);
        from->effects.erase (from->effects.begin() + slot);
        to->effects.insert (to->effects.begin() + index, moved);
        return juce::Result::ok();
    }

private:
    DeviceId device;
    std::optional<InsertId> target;
    int index;
    juce::String movedName;
};

/** device.setParameters: settings of an instrument or effect, by Spinus's parameter ID (the one
    automation uses, engine/ParameterIds.h), as values from 0 to 1 (the automixer's EQ bands, for example), or,
    for a Spinus device, as text in the units it shows ("35%", "220 ms": dsp/ParameterText.h's rule). The text is
    read once, on a copy of the device made for it, into values from 0 to 1: the project stores those. */
class SetParameters : public JsonCommand
{
public:
    SetParameters (juce::var p, DeviceId d, std::map<juce::String, float> v,
                   juce::String commandName = "device.setParameters", std::map<juce::String, juce::String> t = {})
        : JsonCommand (std::move (commandName), std::move (p)), device (d), values (std::move (v)),
          texts (std::move (t))
    {
    }

    juce::String getDescription() const override
    {
        const auto count = (int)(values.size() + texts.size());
        return count == 1 ? "Change a setting of " + quoted (name)
                          : "Change " + juce::String (count) + " settings of " + quoted (name);
    }

    juce::Result perform (Project& p) override
    {
        auto* d = find (p);
        if (d == nullptr)
            return juce::Result::fail ("no device " + juce::String (device.value));
        name = effectName (*d);
        if (! texts.empty())
        {
            if (auto r = readTexts (*d); r.failed())
                return r;
            texts.clear(); // read: a redo uses the values
        }
        old.clear();
        for (auto& [parameter, value] : values)
        {
            if (auto it = d->parameters.find (parameter); it != d->parameters.end())
                old[parameter] = it->second;
            else
                old[parameter] = std::nullopt;
            d->parameters[parameter] = value;
        }
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        if (auto* d = find (p))
            for (auto& [parameter, value] : old)
            {
                if (value)
                    d->parameters[parameter] = *value;
                else
                    d->parameters.erase (parameter);
            }
    }

protected:
    /** The texts as values from 0 to 1, on a new copy of the Spinus device: by its parameter ID or its name. */
    juce::Result readTexts (const Device& d)
    {
        const auto stock = d.kind == Device::Kind::stock && d.stockType.startsWith ("spinus.");
        auto instance =
            stock ? dsp::SpinusPluginFormat::create ("spinus:" + d.stockType.fromFirstOccurrenceOf (".", false, false))
                  : nullptr;
        if (instance == nullptr)
            return juce::Result::fail ("settings as text work for Spinus's own devices only; for " + effectName (d) +
                                       " use 'values' (0 to 1)");
        const engine::ParameterFinder finder (*instance);
        for (auto& [key, text] : texts)
        {
            auto* parameter = finder.find (key);
            for (auto* each : instance->getParameters())
                if (parameter == nullptr && each->getName (200).equalsIgnoreCase (key.trim()))
                    parameter = each;
            if (parameter == nullptr)
                return juce::Result::fail (effectName (d) + " has no setting " + quoted (key));
            const auto value = dsp::param::normalisedForText (*parameter, text);
            if (! value)
            {
                const auto choices = dsp::param::choicesOf (*parameter);
                return juce::Result::fail (
                    "cannot read " + quoted (text) + " for " + quoted (parameter->getName (200)) +
                    (choices.isEmpty() ? " (it shows values like " + quoted (parameter->getCurrentValueAsText()) + ")"
                                       : " (its choices: " + choices.joinIntoString (", ") + ")"));
            }
            values[engine::getParameterId (*parameter)] = *value;
        }
        return juce::Result::ok();
    }

    Device* find (Project& p)
    {
        changes = {};
        if (auto [insert, slot] = findEffect (p, device); insert != nullptr)
        {
            changes.inserts.insert (insert->id);
            return &insert->effects[(size_t)slot].device;
        }
        for (auto& c : p.channels)
            if (c.instrument && c.instrument->id == device)
            {
                changes.channels.insert (c.id);
                return &*c.instrument;
            }
        return nullptr;
    }

    DeviceId device;
    std::map<juce::String, float> values;
    std::map<juce::String, juce::String> texts;
    std::map<juce::String, std::optional<float>> old;
    juce::String name;
};

/** device.loadPreset: one of a Spinus instrument's built-in presets (3x Osc's), as every one of its
    settings through device.setParameters, so it is one undo step. "Default" puts every setting back to that of a
    new one. */
class LoadPreset final : public SetParameters
{
public:
    LoadPreset (juce::var p, DeviceId d, juce::String presetName)
        : SetParameters (std::move (p), d, {}, "device.loadPreset"), preset (std::move (presetName))
    {
    }

    juce::String getDescription() const override { return "Load preset " + quoted (preset) + " on " + quoted (name); }

    juce::Result perform (Project& p) override
    {
        auto* d = find (p);
        if (d == nullptr)
            return juce::Result::fail ("no device " + juce::String (device.value));
        if (d->kind != Device::Kind::stock || d->stockType != "spinus.3x-osc")
            return juce::Result::fail (effectName (*d) + " has no built-in presets (only 3x Osc has)");
        const auto* found = dsp::threeOscPresets::find (preset);
        if (found == nullptr && ! preset.trim().equalsIgnoreCase (dsp::threeOscPresets::defaultName))
        {
            juce::StringArray names { dsp::threeOscPresets::defaultName };
            for (auto& each : dsp::threeOscPresets::all())
                names.add (each.name);
            return juce::Result::fail ("3x Osc has no preset " + quoted (preset) + "; its presets are " +
                                       names.joinIntoString (", "));
        }
        preset = found != nullptr ? found->name : juce::String (dsp::threeOscPresets::defaultName);
        values = dsp::threeOscPresets::values (found);
        return SetParameters::perform (p);
    }

private:
    juce::String preset;
};

class SetEffect final : public JsonCommand
{
public:
    SetEffect (juce::var p, DeviceId d, std::optional<bool> b)
        : JsonCommand ("insert.setEffect", std::move (p)), device (d), bypassed (b)
    {
    }

    juce::String getDescription() const override
    {
        if (bypassed)
            return (*bypassed ? "Bypass " : "Enable ") + quoted (name);
        return "Change effect " + quoted (name);
    }

    juce::Result perform (Project& p) override
    {
        auto [insert, slot] = findEffect (p, device);
        if (insert == nullptr)
            return juce::Result::fail ("no effect with device " + juce::String (device.value));

        auto& d = insert->effects[(size_t)slot].device;
        name = effectName (d);
        old = d.bypassed;
        if (bypassed)
            d.bypassed = *bypassed;

        changes = {};
        changes.inserts.insert (insert->id);
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        if (auto [insert, slot] = findEffect (p, device); insert != nullptr)
            insert->effects[(size_t)slot].device.bypassed = old;
    }

private:
    DeviceId device;
    std::optional<bool> bypassed;
    bool old = false;
    juce::String name;
};

//==============================================================================
// pattern.*

class AddPattern final : public SnapshotCommand
{
public:
    AddPattern (juce::var p, juce::String n, std::optional<Ticks> l)
        : SnapshotCommand ("pattern.add", std::move (p)), patternName (std::move (n)), length (l)
    {
    }

    juce::String getDescription() const override
    {
        return "Add pattern " + quoted (patternName.isNotEmpty() ? patternName : madeName);
    }
    juce::var getResult() const override { return resultWith ({ { "pattern", pattern.value } }); }

protected:
    juce::Result apply (Project& p) override
    {
        if (! pattern)
            pattern = p.allocateId<PatternId>();

        Pattern pat;
        pat.id = pattern;
        pat.name = patternName.isNotEmpty() ? patternName : "Pattern " + juce::String ((int)p.patterns.size() + 1);
        madeName = pat.name;
        if (length)
            pat.length = *length;
        p.patterns.push_back (pat);
        return juce::Result::ok();
    }

private:
    juce::String patternName, madeName;
    std::optional<Ticks> length;
    PatternId pattern;
};

class RemovePattern final : public SnapshotCommand
{
public:
    RemovePattern (juce::var p, PatternId id) : SnapshotCommand ("pattern.remove", std::move (p)), pattern (id) {}

    juce::String getDescription() const override { return "Remove pattern " + quoted (removedName); }

protected:
    juce::Result apply (Project& p) override
    {
        const auto i = indexOf (p.patterns, pattern);
        if (i < 0)
            return juce::Result::fail ("pattern " + juce::String (pattern.value) + " does not exist");

        removedName = p.patterns[(size_t)i].name;
        p.patterns.erase (p.patterns.begin() + i);
        std::erase_if (p.playlist.clips,
                       [&] (const Clip& clip)
                       {
                           auto* pc = std::get_if<PatternClip> (&clip.content);
                           return pc != nullptr && pc->pattern == pattern;
                       });
        std::erase_if (p.launcher.clips,
                       [&] (const LauncherClip& clip)
                       {
                           auto* pc = std::get_if<PatternClip> (&clip.content);
                           return pc != nullptr && pc->pattern == pattern;
                       });
        return juce::Result::ok();
    }

private:
    PatternId pattern;
    juce::String removedName;
};

/** Replaces one channel's notes in one pattern. Every piano-roll and step-sequencer edit is one of
    these, which matches how the Engine Adapter rewrites clips per channel. */
class SetNotes final : public JsonCommand
{
public:
    SetNotes (juce::var p, PatternId pat, ChannelId ch, std::vector<Note> n, juce::String d)
        : JsonCommand ("pattern.setNotes", std::move (p)), pattern (pat), channel (ch), notes (std::move (n)),
          description (std::move (d))
    {
        changes.patterns.insert (pat);
    }

    juce::String getDescription() const override { return description.isNotEmpty() ? description : "Edit notes"; }

    juce::Result perform (Project& p) override
    {
        auto* pat = p.findPattern (pattern);
        if (pat == nullptr)
            return juce::Result::fail ("pattern " + juce::String (pattern.value) + " does not exist");
        if (! p.findChannel (channel))
            return juce::Result::fail ("channel " + juce::String (channel.value) + " does not exist");

        for (auto& n : notes)
        {
            if (n.pitch < 0 || n.pitch > 127 || n.length <= 0 || n.start < 0 || n.velocity < 0.0f || n.velocity > 1.0f)
                return juce::Result::fail ("invalid note: pitch 0-127, length > 0, start >= 0, velocity 0-1");
            if (n.releaseVelocity < 0.0f || n.releaseVelocity > 1.0f || n.pan < -1.0f || n.pan > 1.0f ||
                n.finePitchCents < -100.0f || n.finePitchCents > 100.0f || n.getVolume() < 0.0f || n.getVolume() > 1.0f)
                return juce::Result::fail (
                    "invalid note: release velocity 0-1, pan -1 to 1, fine pitch -100 to 100 cents, volume 0-1");
        }

        auto it = pat->notes.find (channel);
        hadEntry = it != pat->notes.end();
        old = hadEntry ? it->second : std::vector<Note> {};

        if (notes.empty())
            pat->notes.erase (channel);
        else
            pat->notes[channel] = notes;

        // The pattern grows to the end of the bar its notes reach, and shrinks back when they go (FL).
        oldLength = pat->length;
        pat->length = p.fittedLength (*pat);
        oldClipLengths.clear();
        fitPatternClips (p, pattern, oldLength, pat->length, changes, oldClipLengths);
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        if (auto* pat = p.findPattern (pattern))
        {
            if (hadEntry)
                pat->notes[channel] = old;
            else
                pat->notes.erase (channel);
            pat->length = oldLength;
        }
        restoreClipLengths (p, oldClipLengths);
    }

private:
    PatternId pattern;
    ChannelId channel;
    std::vector<Note> notes, old;
    Ticks oldLength = 0;
    std::map<ClipId, Ticks> oldClipLengths;
    bool hadEntry = false;
    juce::String description;
};

/** A pattern's length (FL: patterns grow as notes are placed further on; the Generate dialog's bars). */
class SetPatternLength final : public JsonCommand
{
public:
    SetPatternLength (juce::var p, PatternId pat, Ticks l)
        : JsonCommand ("pattern.setLength", std::move (p)), pattern (pat), length (l)
    {
        changes.patterns.insert (pat);
    }

    juce::String getDescription() const override { return "Change pattern length"; }

    juce::Result perform (Project& p) override
    {
        auto* pat = p.findPattern (pattern);
        if (pat == nullptr)
            return juce::Result::fail ("pattern " + juce::String (pattern.value) + " does not exist");
        old = pat->length;
        pat->length = length;
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        if (auto* pat = p.findPattern (pattern))
            pat->length = old;
    }

private:
    PatternId pattern;
    Ticks length, old = 0;
};

/** A pattern's own time signature, or none to follow the song's. */
class SetPatternTimeSignature final : public JsonCommand
{
public:
    SetPatternTimeSignature (juce::var p, PatternId pat, std::optional<TimeSignature> s)
        : JsonCommand ("pattern.setTimeSignature", std::move (p)), pattern (pat), signature (s)
    {
        changes.patterns.insert (pat);
    }

    juce::String getDescription() const override
    {
        return signature ? "Set pattern time signature to " + juce::String (signature->numerator) + "/" +
                               juce::String (signature->denominator)
                         : juce::String ("Pattern follows the song's time signature");
    }

    juce::Result perform (Project& p) override
    {
        auto* pat = p.findPattern (pattern);
        if (pat == nullptr)
            return juce::Result::fail ("pattern " + juce::String (pattern.value) + " does not exist");
        if (signature && ! signature->isValid())
            return juce::Result::fail ("a time signature needs 1-32 beats over 1, 2, 4, 8, 16 or 32");
        old = pat->timeSignature;
        pat->timeSignature = signature;
        // Whole bars of the new time signature: a 3/4 pattern is 3 beats long.
        oldLength = pat->length;
        pat->length = p.fittedLength (*pat);
        oldClipLengths.clear();
        fitPatternClips (p, pattern, oldLength, pat->length, changes, oldClipLengths);
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        if (auto* pat = p.findPattern (pattern))
        {
            pat->timeSignature = old;
            pat->length = oldLength;
        }
        restoreClipLengths (p, oldClipLengths);
    }

private:
    PatternId pattern;
    std::optional<TimeSignature> signature, old;
    Ticks oldLength = 0;
    std::map<ClipId, Ticks> oldClipLengths;
};

/** Whether a pattern's Channel Rack steps are sixteenth-note triplets. Notes stay where they are. */
/** A name for a copy of a pattern: "Chorus" -> "Chorus 2", "Verse 2" -> "Verse 3", not one in use. */
juce::String copyName (const Project& p, const juce::String& name)
{
    auto base = name.trimEnd();
    int number = 2;
    const auto digits = base.getTrailingIntValue();
    if (digits > 0 && base.endsWith (juce::String (digits)))
    {
        base = base.dropLastCharacters (juce::String (digits).length()).trimEnd();
        number = digits + 1;
    }
    auto used = [&p] (const juce::String& n)
    { return std::any_of (p.patterns.begin(), p.patterns.end(), [&n] (const Pattern& pat) { return pat.name == n; }); };
    while (used (base + " " + juce::String (number)))
        ++number;
    return base + " " + juce::String (number);
}

/** A copy of a pattern, with all its notes, right after it in the list; its new ID. */
PatternId clonePatternIn (Project& p, PatternId source, const juce::String& name, PatternId id)
{
    const auto* original = p.findPattern (source);
    jassert (original != nullptr);
    Pattern copy = *original;
    copy.id = id;
    copy.name = name.isNotEmpty() ? name : copyName (p, original->name);
    const auto at =
        std::find_if (p.patterns.begin(), p.patterns.end(), [source] (auto& pat) { return pat.id == source; });
    p.patterns.insert (at + 1, std::move (copy));
    return id;
}

/** Copies a pattern (pattern.clone). */
class ClonePattern final : public SnapshotCommand
{
public:
    ClonePattern (juce::var p, PatternId pat, juce::String n)
        : SnapshotCommand ("pattern.clone", std::move (p)), source (pat), newName (std::move (n))
    {
    }

    juce::String getDescription() const override { return "Clone pattern " + quoted (madeName); }
    juce::var getResult() const override { return resultWith ({ { "pattern", made.value } }); }

protected:
    juce::Result apply (Project& p) override
    {
        if (p.findPattern (source) == nullptr)
            return juce::Result::fail ("pattern " + juce::String (source.value) + " does not exist");
        if (! made)
            made = p.allocateId<PatternId>();
        clonePatternIn (p, source, newName, made);
        madeName = p.findPattern (made)->name;
        return juce::Result::ok();
    }

private:
    PatternId source, made;
    juce::String newName, madeName;
};

/** A Playlist clip gets a copy of its pattern of its own, to change without changing the others
    (clip.makeUnique, FL's "Make unique"). */
class MakeClipUnique final : public SnapshotCommand
{
public:
    MakeClipUnique (juce::var p, ClipId c) : SnapshotCommand ("clip.makeUnique", std::move (p)), clip (c) {}

    juce::String getDescription() const override { return "Make clip unique (" + madeName + ")"; }
    juce::var getResult() const override { return resultWith ({ { "pattern", made.value } }); }

protected:
    juce::Result apply (Project& p) override
    {
        auto* c = p.findClip (clip);
        auto* content = c != nullptr ? std::get_if<PatternClip> (&c->content) : nullptr;
        if (content == nullptr)
            return juce::Result::fail ("clip " + juce::String (clip.value) + " is not a pattern clip on the playlist");
        if (! made)
            made = p.allocateId<PatternId>();
        const auto source = content->pattern;
        clonePatternIn (p, source, {}, made);
        // The pattern list may have moved in memory: find the clip again.
        std::get<PatternClip> (p.findClip (clip)->content).pattern = made;
        madeName = p.findPattern (made)->name;
        return juce::Result::ok();
    }

private:
    ClipId clip;
    PatternId made;
    juce::String madeName;
};

/** Renames a pattern or changes its colour. */
class SetPattern final : public JsonCommand
{
public:
    SetPattern (juce::var p, PatternId pat, std::optional<juce::String> n, std::optional<juce::uint32> c)
        : JsonCommand ("pattern.set", std::move (p)), pattern (pat), newName (std::move (n)), colour (c)
    {
        changes.patterns.insert (pat);
    }

    juce::String getDescription() const override
    {
        return newName ? "Rename pattern to " + quoted (newName->trim()) : juce::String ("Colour pattern");
    }

    juce::Result perform (Project& p) override
    {
        auto* pat = p.findPattern (pattern);
        if (pat == nullptr)
            return juce::Result::fail ("pattern " + juce::String (pattern.value) + " does not exist");
        if (newName && newName->trim().isEmpty())
            return juce::Result::fail ("a pattern needs a name");
        oldName = pat->name;
        oldColour = pat->colour;
        if (newName)
            pat->name = newName->trim();
        if (colour)
            pat->colour = *colour;
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        if (auto* pat = p.findPattern (pattern))
        {
            pat->name = oldName;
            pat->colour = oldColour;
        }
    }

private:
    PatternId pattern;
    std::optional<juce::String> newName;
    std::optional<juce::uint32> colour;
    juce::String oldName;
    juce::uint32 oldColour = 0;
};

class SetPatternSwing final : public JsonCommand
{
public:
    SetPatternSwing (juce::var p, PatternId pat, float s)
        : JsonCommand ("pattern.setSwing", std::move (p)), pattern (pat), swing (s)
    {
        changes.patterns.insert (pat);
    }

    juce::String getDescription() const override
    {
        return "Pattern swing " + juce::String (juce::roundToInt (swing * 100.0f)) + "%";
    }

    juce::Result perform (Project& p) override
    {
        auto* pat = p.findPattern (pattern);
        if (pat == nullptr)
            return juce::Result::fail ("pattern " + juce::String (pattern.value) + " does not exist");
        old = pat->swing;
        pat->swing = swing;
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        if (auto* pat = p.findPattern (pattern))
            pat->swing = old;
    }

private:
    PatternId pattern;
    float swing, old = 0.0f;
};

class SetPatternTripletSteps final : public JsonCommand
{
public:
    SetPatternTripletSteps (juce::var p, PatternId pat, bool t)
        : JsonCommand ("pattern.setTripletSteps", std::move (p)), pattern (pat), triplets (t)
    {
        changes.patterns.insert (pat);
    }

    juce::String getDescription() const override
    {
        return triplets ? "Pattern steps: triplets" : "Pattern steps: sixteenths";
    }

    juce::Result perform (Project& p) override
    {
        auto* pat = p.findPattern (pattern);
        if (pat == nullptr)
            return juce::Result::fail ("pattern " + juce::String (pattern.value) + " does not exist");
        old = pat->tripletSteps;
        pat->tripletSteps = triplets;
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        if (auto* pat = p.findPattern (pattern))
            pat->tripletSteps = old;
    }

private:
    PatternId pattern;
    bool triplets, old = false;
};

//==============================================================================
// lane.* and clip.*

//==============================================================================
// marker.*

void sortMarkers (Project& p)
{
    std::stable_sort (p.playlist.markers.begin(), p.playlist.markers.end(),
                      [] (const Marker& a, const Marker& b) { return a.at < b.at; });
}

Marker* findMarker (Project& p, MarkerId id)
{
    for (auto& m : p.playlist.markers)
        if (m.id == id)
            return &m;
    return nullptr;
}

class AddMarker final : public SnapshotCommand
{
public:
    AddMarker (juce::var p, Ticks t, juce::String n)
        : SnapshotCommand ("marker.add", std::move (p)), at (t), markerName (std::move (n))
    {
    }

    juce::String getDescription() const override { return "Add marker " + quoted (madeName); }
    juce::var getResult() const override { return resultWith ({ { "marker", marker.value } }); }

protected:
    juce::Result apply (Project& p) override
    {
        if (! marker)
            marker = p.allocateId<MarkerId>();
        madeName = markerName.trim().isNotEmpty() ? markerName.trim()
                                                  : "Marker " + juce::String ((int)p.playlist.markers.size() + 1);
        p.playlist.markers.push_back ({ marker, at, madeName });
        sortMarkers (p);
        return juce::Result::ok();
    }

private:
    Ticks at;
    juce::String markerName, madeName;
    MarkerId marker;
};

class SetMarker final : public SnapshotCommand
{
public:
    SetMarker (juce::var p, MarkerId m, std::optional<juce::String> n, std::optional<Ticks> t)
        : SnapshotCommand ("marker.set", std::move (p)), marker (m), newName (std::move (n)), at (t)
    {
    }

    juce::String getDescription() const override
    {
        return newName ? "Rename marker to " + quoted (newName->trim()) : juce::String ("Move marker");
    }

protected:
    juce::Result apply (Project& p) override
    {
        auto* m = findMarker (p, marker);
        if (m == nullptr)
            return juce::Result::fail ("marker " + juce::String (marker.value) + " does not exist");
        if (newName && newName->trim().isEmpty())
            return juce::Result::fail ("a marker needs a name");
        if (newName)
            m->name = newName->trim();
        if (at)
            m->at = *at;
        sortMarkers (p);
        return juce::Result::ok();
    }

private:
    MarkerId marker;
    std::optional<juce::String> newName;
    std::optional<Ticks> at;
};

class RemoveMarker final : public SnapshotCommand
{
public:
    RemoveMarker (juce::var p, MarkerId m) : SnapshotCommand ("marker.remove", std::move (p)), marker (m) {}

    juce::String getDescription() const override { return "Remove marker"; }

protected:
    juce::Result apply (Project& p) override
    {
        auto& markers = p.playlist.markers;
        const auto before = markers.size();
        std::erase_if (markers, [this] (const Marker& m) { return m.id == marker; });
        return markers.size() < before
                   ? juce::Result::ok()
                   : juce::Result::fail ("marker " + juce::String (marker.value) + " does not exist");
    }

private:
    MarkerId marker;
};

class AddLane final : public SnapshotCommand
{
public:
    AddLane (juce::var p, juce::String n, std::optional<int> position)
        : SnapshotCommand ("lane.add", std::move (p)), laneName (std::move (n)), index (position)
    {
    }

    juce::String getDescription() const override { return "Add playlist track"; }
    juce::var getResult() const override { return resultWith ({ { "lane", lane.value } }); }

protected:
    juce::Result apply (Project& p) override
    {
        auto& lanes = p.playlist.lanes;
        if (index && (*index < 0 || *index > (int)lanes.size()))
            return juce::Result::fail ("index out of range");

        if (! lane)
            lane = p.allocateId<LaneId>();

        Lane l;
        l.id = lane;
        l.name = laneName.isNotEmpty() ? laneName : "Track " + juce::String ((int)lanes.size() + 1);
        lanes.insert (lanes.begin() + (index ? *index : (int)lanes.size()), l);
        return juce::Result::ok();
    }

private:
    juce::String laneName;
    std::optional<int> index;
    LaneId lane;
};

class RemoveLane final : public SnapshotCommand
{
public:
    RemoveLane (juce::var p, LaneId l) : SnapshotCommand ("lane.remove", std::move (p)), lane (l) {}

    juce::String getDescription() const override { return "Remove playlist track"; }

protected:
    juce::Result apply (Project& p) override
    {
        const auto i = indexOf (p.playlist.lanes, lane);
        if (i < 0)
            return juce::Result::fail ("lane " + juce::String (lane.value) + " does not exist");

        // Tracks grouped under it move up to its own group.
        const auto parent = p.playlist.lanes[(size_t)i].group;
        for (auto& other : p.playlist.lanes)
            if (other.group == lane)
                other.group = parent;
        p.playlist.lanes.erase (p.playlist.lanes.begin() + i);
        std::erase_if (p.playlist.clips, [&] (const Clip& c) { return c.lane == lane; });
        return juce::Result::ok();
    }

private:
    LaneId lane;
};

class SetLane final : public JsonCommand
{
public:
    SetLane (juce::var p, LaneId l, std::optional<juce::String> n, std::optional<bool> m,
             std::optional<juce::uint32> c = std::nullopt, std::optional<LaneId> g = std::nullopt,
             std::optional<bool> folded = std::nullopt)
        : JsonCommand ("lane.set", std::move (p)), lane (l), newName (std::move (n)), muted (m), colour (c), group (g),
          collapsed (folded)
    {
        changes.lanes.insert (l);
    }

    juce::String getDescription() const override
    {
        if (group)
            return group->isValid() ? "Group playlist track" : "Ungroup playlist track";
        if (collapsed)
            return *collapsed ? "Fold track group" : "Unfold track group";
        if (newName)
            return "Rename playlist track to " + quoted (*newName);
        if (colour && ! muted)
            return "Colour playlist track";
        return muted.value_or (false) ? "Mute playlist track" : "Unmute playlist track";
    }

    juce::Result perform (Project& p) override
    {
        auto* l = p.findLane (lane);
        if (l == nullptr)
            return juce::Result::fail ("lane " + juce::String (lane.value) + " does not exist");

        if (group && group->isValid())
        {
            if (*group == lane || p.findLane (*group) == nullptr)
                return juce::Result::fail ("group must be another playlist track");
            if (p.isLaneGroupedUnder (*group, lane))
                return juce::Result::fail ("a track cannot be grouped under a track in its own group");
        }

        old = *l;
        if (newName)
            l->name = *newName;
        if (muted)
            l->muted = *muted;
        if (colour)
            l->colour = *colour;
        if (group)
            l->group = *group;
        if (collapsed)
            l->collapsed = *collapsed;
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        if (auto* l = p.findLane (lane))
            *l = old;
    }

private:
    LaneId lane;
    std::optional<juce::String> newName;
    std::optional<bool> muted;
    std::optional<juce::uint32> colour;
    std::optional<LaneId> group;
    std::optional<bool> collapsed;
    Lane old;
};

/** Places a pattern on the playlist. */
class AddPatternClip final : public JsonCommand
{
public:
    AddPatternClip (juce::var p, LaneId l, PatternId pat, Ticks s, std::optional<Ticks> len, Ticks off)
        : JsonCommand ("clip.addPattern", std::move (p)), lane (l), pattern (pat), start (s), length (len), offset (off)
    {
    }

    juce::String getDescription() const override { return "Add pattern to playlist"; }
    juce::var getResult() const override { return resultWith ({ { "clip", clip.value } }); }

    juce::Result perform (Project& p) override
    {
        if (p.findLane (lane) == nullptr)
            return juce::Result::fail ("lane " + juce::String (lane.value) + " does not exist");

        auto* pat = p.findPattern (pattern);
        if (pat == nullptr)
            return juce::Result::fail ("pattern " + juce::String (pattern.value) + " does not exist");

        if (start < 0 || (length && *length <= 0) || offset < 0)
            return juce::Result::fail ("start and offset must be >= 0 and length > 0");

        if (! clip)
            clip = p.allocateId<ClipId>();

        p.playlist.clips.push_back (
            { clip, lane, start, length.value_or (pat->length), false, PatternClip { pattern, offset } });
        changes.clips = { clip };
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        std::erase_if (p.playlist.clips, [this] (const Clip& c) { return c.id == clip; });
    }

private:
    LaneId lane;
    PatternId pattern;
    Ticks start;
    std::optional<Ticks> length;
    Ticks offset;
    ClipId clip;
};

/** An audio clip on the playlist. It plays through a channel (FL's audio clip channel):
    an existing one, or a new one created with its own mixer insert, named after the file. */
class AddAudioClip final : public SnapshotCommand
{
public:
    struct Values
    {
        LaneId lane;
        Ticks start = 0, length = 0, offset = 0, fadeIn = 0, fadeOut = 0;
        float gainDb = 0.0f;
        juce::String file;
        std::optional<ChannelId> channel; // none: a new channel named `name`
        juce::String name;
    };

    AddAudioClip (juce::var p, Values v) : SnapshotCommand ("clip.addAudio", std::move (p)), values (std::move (v)) {}

    juce::String getDescription() const override { return "Add audio clip " + quoted (values.name); }
    juce::var getResult() const override
    {
        return resultWith ({ { "clip", clip.value }, { "channel", channel.value }, { "insert", insert.value } });
    }

protected:
    juce::Result apply (Project& p) override
    {
        if (p.findLane (values.lane) == nullptr)
            return juce::Result::fail ("lane " + juce::String (values.lane.value) + " does not exist");
        if (values.channel && p.findChannel (*values.channel) == nullptr)
            return juce::Result::fail ("channel " + juce::String (values.channel->value) + " does not exist");
        if (values.file.isEmpty())
            return juce::Result::fail ("file must not be empty");

        // First perform allocates IDs; redo reuses them.
        if (! clip)
        {
            clip = p.allocateId<ClipId>();
            if (! values.channel)
            {
                channel = p.allocateId<ChannelId>();
                insert = p.allocateId<InsertId>();
            }
        }

        if (values.channel)
        {
            channel = *values.channel;
        }
        else
        {
            Insert i;
            i.id = insert;
            i.name = values.name;
            p.mixer.inserts.push_back (i);

            Channel c;
            c.id = channel;
            c.name = values.name;
            c.targetInsert = insert;
            c.audioTrack = true;
            p.channels.push_back (c);
        }

        AudioClip a;
        a.file = values.file;
        a.channel = channel;
        a.offset = values.offset;
        a.gainDb = values.gainDb;
        a.fadeIn = values.fadeIn;
        a.fadeOut = values.fadeOut;
        a.stretchMode = "off"; // time-stretch arrives with Rubber Band
        p.playlist.clips.push_back ({ clip, values.lane, values.start, values.length, false, a });
        return juce::Result::ok();
    }

private:
    Values values;
    ClipId clip;
    ChannelId channel;
    InsertId insert;
};

/** Changes an audio clip's file, gain, fades, pitch, speed, stretch mode or direction. */
class SetAudioClip final : public SnapshotCommand
{
public:
    struct Values
    {
        std::optional<juce::String> file;
        std::optional<float> gainDb;
        std::optional<Ticks> fadeIn, fadeOut;
        std::optional<float> pitch;
        std::optional<double> speed;
        std::optional<juce::String> stretch;
        std::optional<bool> reversed;
        bool fit = false; // the clip keeps the same audio: its length grows or shrinks with the speed
    };

    SetAudioClip (juce::var p, ClipId c, Values v)
        : SnapshotCommand ("clip.setAudio", std::move (p)), clip (c), values (std::move (v))
    {
    }

    juce::String getDescription() const override
    {
        if (values.reversed)
            return *values.reversed ? "Reverse clip" : "Play clip forwards";
        if (values.pitch || values.speed || values.stretch)
            return values.speed && ! values.pitch ? "Change clip speed" : "Change clip pitch";
        if (values.file && ! values.gainDb && ! values.fadeIn && ! values.fadeOut)
            return "Change audio file";
        if (values.gainDb && ! values.fadeIn && ! values.fadeOut)
            return "Change clip volume";
        return "Change clip fades";
    }

protected:
    juce::Result apply (Project& p) override
    {
        auto* c = p.findClip (clip);
        auto* a = c != nullptr ? std::get_if<AudioClip> (&c->content) : nullptr;
        if (a == nullptr)
            return juce::Result::fail ("clip " + juce::String (clip.value) + " is not an audio clip");
        const auto speedBefore = getPlaybackSpeed (*a);
        if (values.file)
        {
            if (values.file->isEmpty())
                return juce::Result::fail ("file must not be empty");
            a->file = *values.file;
        }
        if (values.gainDb)
            a->gainDb = *values.gainDb;
        if (values.fadeIn)
            a->fadeIn = *values.fadeIn;
        if (values.fadeOut)
            a->fadeOut = *values.fadeOut;
        if (values.pitch)
            a->pitchSemitones = *values.pitch;
        if (values.speed)
            a->speed = *values.speed;
        if (values.reversed)
            a->reversed = *values.reversed;
        if (values.stretch)
            a->stretchMode = *values.stretch;
        else if ((values.pitch || values.speed) && (a->stretchMode == "off" || a->stretchMode == "tempo-sync"))
            a->stretchMode = "stretch"; // a pitch or speed of its own: Rubber Band, unless resampling was chosen

        if (values.fit)
        {
            // The same audio in the clip: twice as fast, half as long (and its offset and fades with it).
            const auto ratio = speedBefore / getPlaybackSpeed (*a);
            auto scale = [ratio] (Ticks t) { return (Ticks)std::llround ((double)t * ratio); };
            c->length = juce::jmax ((Ticks)1, scale (c->length));
            a->offset = scale (a->offset);
            a->fadeIn = scale (a->fadeIn);
            a->fadeOut = scale (a->fadeOut);
        }
        return juce::Result::ok();
    }

private:
    ClipId clip;
    Values values;
};

//==============================================================================
// Takes and comping: takes of one audio track are its audio clips on lanes grouped
// under the first take's lane; where takes overlap, one plays and the others' parts there are muted.

/** The IDs of the pieces a command cuts clips into: the same ones again when it is redone. */
struct CutIds
{
    std::vector<ClipId> ids;
    size_t used = 0;

    ClipId next (Project& p)
    {
        if (used == ids.size())
            ids.push_back (p.allocateId<ClipId>());
        return ids[used++];
    }
};

/** Cuts the clip at `at` (inside it) into two clips, as clip.split does for audio; returns the second's index. */
size_t cutAudioClip (Project& p, size_t index, Ticks at, CutIds& cuts)
{
    const auto id = cuts.next (p);
    auto& c = p.playlist.clips[index];
    const auto cut = at - c.start;
    auto second = c;
    second.id = id;
    second.start = at;
    second.length = c.length - cut;
    c.length = cut;
    auto& audio = std::get<AudioClip> (second.content);
    audio.offset += cut;
    audio.fadeIn = 0;
    std::get<AudioClip> (c.content).fadeOut = 0;
    p.playlist.clips.insert (p.playlist.clips.begin() + (std::ptrdiff_t)index + 1, std::move (second));
    return index + 1;
}

/** Mutes what the channel's other clips play between `from` and `to`, cutting them where they reach past it;
    `except` (the take that plays there) is left as it is. */
void muteOtherTakes (Project& p, ChannelId channel, Ticks from, Ticks to, ClipId except, CutIds& cuts)
{
    cuts.used = 0;
    for (size_t i = 0; i < p.playlist.clips.size(); ++i)
    {
        auto* audio = std::get_if<AudioClip> (&p.playlist.clips[i].content);
        const auto& c = p.playlist.clips[i];
        if (audio == nullptr || audio->channel != channel || c.id == except || c.start >= to ||
            c.start + c.length <= from)
            continue;
        if (c.start < from) // the part before stays as it was; the rest is looked at next
        {
            i = cutAudioClip (p, i, from, cuts) - 1;
            continue;
        }
        if (c.start + c.length > to) // the part after stays as it was
            cutAudioClip (p, i, to, cuts);
        p.playlist.clips[i].muted = true;
    }
}

/** Records another take over earlier ones: on a new lane under the first take's lane, through the same audio
    track; it plays where it was recorded, and the earlier takes' parts there are muted. */
class AddTake final : public SnapshotCommand
{
public:
    struct Values
    {
        ChannelId channel;
        juce::String file;
        Ticks start = 0, length = 1, offset = 0;
    };

    AddTake (juce::var p, Values v) : SnapshotCommand ("clip.addTake", std::move (p)), values (std::move (v)) {}

    juce::String getDescription() const override { return "Record take " + juce::String (number); }
    juce::var getResult() const override { return resultWith ({ { "clip", clip.value }, { "lane", lane.value } }); }

protected:
    juce::Result apply (Project& p) override
    {
        const auto* channel = p.findChannel (values.channel);
        if (channel == nullptr)
            return juce::Result::fail ("channel " + juce::String (values.channel.value) + " does not exist");
        if (values.file.isEmpty())
            return juce::Result::fail ("file must not be empty");

        // The first take's lane is the group; the new lane goes below the last lane with a take.
        const Clip* first = nullptr;
        int last = -1;
        std::set<LaneId> takeLanes;
        for (const auto& c : p.playlist.clips)
            if (const auto* audio = std::get_if<AudioClip> (&c.content);
                audio != nullptr && audio->channel == values.channel)
            {
                if (first == nullptr || c.id.value < first->id.value)
                    first = &c;
                takeLanes.insert (c.lane);
                for (size_t i = 0; i < p.playlist.lanes.size(); ++i)
                    if (p.playlist.lanes[i].id == c.lane)
                        last = juce::jmax (last, (int)i);
            }
        if (! lane)
            lane = p.allocateId<LaneId>();
        if (! clip)
            clip = p.allocateId<ClipId>();
        number = (int)takeLanes.size() + 1;

        // The first take: a lane of its own, at the end; later ones under it.
        Lane l;
        l.id = lane;
        if (first == nullptr)
        {
            l.name = channel->name;
            p.playlist.lanes.push_back (l);
        }
        else
        {
            const auto* groupLane = p.findLane (first->lane);
            l.name = "Take " + juce::String (number);
            l.colour = groupLane != nullptr ? groupLane->colour : l.colour;
            l.group = first->lane;
            p.playlist.lanes.insert (p.playlist.lanes.begin() + last + 1, l);
        }

        muteOtherTakes (p, values.channel, values.start, values.start + values.length, {}, cuts);

        AudioClip audio;
        audio.file = values.file;
        audio.channel = values.channel;
        audio.offset = values.offset;
        audio.stretchMode = "off";
        p.playlist.clips.push_back ({ clip, lane, values.start, values.length, false, audio });
        return juce::Result::ok();
    }

private:
    Values values;
    LaneId lane;
    ClipId clip;
    CutIds cuts;
    int number = 2;
};

/** Comping: this clip plays, and the same audio track's other clips are muted where it plays. */
class UseTake final : public SnapshotCommand
{
public:
    UseTake (juce::var p, ClipId c) : SnapshotCommand ("clip.useTake", std::move (p)), clip (c) {}

    juce::String getDescription() const override { return "Use take"; }

protected:
    juce::Result apply (Project& p) override
    {
        auto* c = p.findClip (clip);
        const auto* audio = c != nullptr ? std::get_if<AudioClip> (&c->content) : nullptr;
        if (audio == nullptr)
            return juce::Result::fail ("clip " + juce::String (clip.value) + " is not an audio clip");
        c->muted = false;
        const auto channel = audio->channel;
        const auto from = c->start, to = c->start + c->length;
        muteOtherTakes (p, channel, from, to, clip, cuts);
        return juce::Result::ok();
    }

private:
    ClipId clip;
    CutIds cuts;
};

/** Spinus Tune: replaces the notes of an audio clip and their corrections (one undo step per edit). */
class SetTune final : public SnapshotCommand
{
public:
    SetTune (juce::var p, ClipId c, std::vector<TuneNote> n, juce::String d)
        : SnapshotCommand ("clip.setTune", std::move (p)), clip (c), notes (std::move (n)), description (std::move (d))
    {
    }

    juce::String getDescription() const override { return description.isNotEmpty() ? description : "Tune notes"; }

protected:
    juce::Result apply (Project& p) override
    {
        auto* c = p.findClip (clip);
        auto* a = c != nullptr ? std::get_if<AudioClip> (&c->content) : nullptr;
        if (a == nullptr)
            return juce::Result::fail ("clip " + juce::String (clip.value) + " is not an audio clip");

        std::stable_sort (notes.begin(), notes.end(), [] (auto& x, auto& y) { return x.start < y.start; });
        for (size_t i = 1; i < notes.size(); ++i)
            if (notes[i].start < notes[i - 1].end - 1.0e-6)
                return juce::Result::fail ("tune notes must not overlap");
        a->tune = notes;
        return juce::Result::ok();
    }

private:
    ClipId clip;
    std::vector<TuneNote> notes;
    juce::String description;
};

/** Checks automation points and puts them in time order. */
juce::Result checkPoints (std::vector<AutomationPoint>& points)
{
    if (points.empty())
        return juce::Result::fail ("an automation clip needs at least one point");
    for (auto& point : points)
        if (point.time < 0 || point.value < 0.0f || point.value > 1.0f || point.curve < -1.0f || point.curve > 1.0f)
            return juce::Result::fail ("points need time >= 0, value 0..1 and curve -1..1");
    std::stable_sort (points.begin(), points.end(), [] (auto& a, auto& b) { return a.time < b.time; });
    return juce::Result::ok();
}

bool targetExists (const Project& p, const ParameterTarget& t)
{
    using K = ParameterTarget::Kind;
    switch (t.kind)
    {
    case K::channelVolume:
    case K::channelPan:
        return p.findChannel (t.channel) != nullptr;
    case K::insertVolume:
    case K::insertPan:
        return p.findInsert (t.insert) != nullptr;
    case K::deviceParameter:
        if (t.parameterId.isEmpty())
            return false;
        for (auto& c : p.channels)
            if (c.instrument && c.instrument->id == t.device)
                return true;
        for (auto* insert : { &p.mixer.master })
            for (auto& slot : insert->effects)
                if (slot.device.id == t.device)
                    return true;
        for (auto& insert : p.mixer.inserts)
            for (auto& slot : insert.effects)
                if (slot.device.id == t.device)
                    return true;
        return false;
    }
    return false;
}

/** An automation clip: a curve for one parameter, on a lane; or on a new lane at the
    bottom when no lane is given (how "Create automation clip" places it). */
class AddAutomationClip final : public SnapshotCommand
{
public:
    AddAutomationClip (juce::var p, std::optional<LaneId> l, juce::String newLane, Ticks s, Ticks len,
                       ParameterTarget t, std::vector<AutomationPoint> pts)
        : SnapshotCommand ("clip.addAutomation", std::move (p)), lane (l), laneName (std::move (newLane)), start (s),
          length (len), target (std::move (t)), points (std::move (pts))
    {
    }

    juce::String getDescription() const override { return "Add automation clip"; }
    juce::var getResult() const override { return resultWith ({ { "clip", clip.value }, { "lane", usedLane.value } }); }

protected:
    juce::Result apply (Project& p) override
    {
        if (lane && p.findLane (*lane) == nullptr)
            return juce::Result::fail ("lane " + juce::String (lane->value) + " does not exist");
        if (! targetExists (p, target))
            return juce::Result::fail ("the automation target does not exist");
        if (auto r = checkPoints (points); r.failed())
            return r;

        // First perform allocates IDs; redo reuses them.
        if (! clip)
        {
            clip = p.allocateId<ClipId>();
            usedLane = lane ? *lane : p.allocateId<LaneId>();
        }

        if (! lane)
            p.playlist.lanes.push_back ({ usedLane, laneName, 0xff3d4450, false });

        AutomationClip a;
        a.target = target;
        a.points = points;
        p.playlist.clips.push_back ({ clip, usedLane, start, length, false, a });
        return juce::Result::ok();
    }

private:
    std::optional<LaneId> lane;
    juce::String laneName;
    Ticks start, length;
    ParameterTarget target;
    std::vector<AutomationPoint> points;
    ClipId clip;
    LaneId usedLane;
};

/** Replaces an automation clip's points (one point edit, as a whole list). */
class SetAutomationPoints final : public SnapshotCommand
{
public:
    SetAutomationPoints (juce::var p, ClipId c, std::vector<AutomationPoint> pts)
        : SnapshotCommand ("clip.setPoints", std::move (p)), clip (c), points (std::move (pts))
    {
    }

    juce::String getDescription() const override { return "Edit automation"; }

protected:
    juce::Result apply (Project& p) override
    {
        auto* c = p.findClip (clip);
        auto* a = c != nullptr ? std::get_if<AutomationClip> (&c->content) : nullptr;
        if (a == nullptr)
            return juce::Result::fail ("clip " + juce::String (clip.value) + " is not an automation clip");
        if (auto r = checkPoints (points); r.failed())
            return r;
        a->points = points;
        return juce::Result::ok();
    }

private:
    ClipId clip;
    std::vector<AutomationPoint> points;
};

class SetClip final : public JsonCommand
{
public:
    struct Values
    {
        std::optional<LaneId> lane;
        std::optional<Ticks> start, length, offset;
        std::optional<bool> muted;
    };

    SetClip (juce::var p, ClipId c, Values v) : JsonCommand ("clip.set", std::move (p)), clip (c), values (v)
    {
        changes.clips.insert (c);
    }

    juce::String getDescription() const override
    {
        if (values.length && ! values.start)
            return "Resize clip";
        if (values.muted)
            return *values.muted ? "Mute clip" : "Unmute clip";
        return "Move clip";
    }

    juce::Result perform (Project& p) override
    {
        auto* c = p.findClip (clip);
        if (c == nullptr)
            return juce::Result::fail ("clip " + juce::String (clip.value) + " does not exist");
        if (values.lane && p.findLane (*values.lane) == nullptr)
            return juce::Result::fail ("lane " + juce::String (values.lane->value) + " does not exist");
        if ((values.start && *values.start < 0) || (values.length && *values.length <= 0) ||
            (values.offset && *values.offset < 0))
            return juce::Result::fail ("start and offset must be >= 0 and length > 0");

        old = *c;
        if (values.lane)
            c->lane = *values.lane;
        if (values.start)
            c->start = *values.start;
        if (values.length)
            c->length = *values.length;
        if (values.muted)
            c->muted = *values.muted;
        if (values.offset)
        {
            if (auto* pc = std::get_if<PatternClip> (&c->content))
                pc->offset = *values.offset;
            else if (auto* ac = std::get_if<AudioClip> (&c->content))
                ac->offset = *values.offset;
        }
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        if (auto* c = p.findClip (clip))
            *c = old;
    }

private:
    ClipId clip;
    Values values;
    Clip old;
};

class RemoveClip final : public JsonCommand
{
public:
    RemoveClip (juce::var p, ClipId c) : JsonCommand ("clip.remove", std::move (p)), clip (c)
    {
        changes.clips.insert (c);
    }

    juce::String getDescription() const override { return "Delete clip"; }

    juce::Result perform (Project& p) override
    {
        const auto i = indexOf (p.playlist.clips, clip);
        if (i < 0)
            return juce::Result::fail ("clip " + juce::String (clip.value) + " does not exist");

        index = i;
        old = p.playlist.clips[(size_t)i];
        p.playlist.clips.erase (p.playlist.clips.begin() + i);
        return juce::Result::ok();
    }

    void undo (Project& p) override { p.playlist.clips.insert (p.playlist.clips.begin() + index, old); }

private:
    ClipId clip;
    int index = 0;
    Clip old;
};

/** Cuts a clip in two at a song time: the first part keeps the clip's ID, the second is a new clip
    right after it that plays on from there (pattern and audio clips through their offset; automation
    clips get the value at the cut as a point at both sides, so the curve sounds the same). */
class SplitClip final : public JsonCommand
{
public:
    SplitClip (juce::var p, ClipId c, Ticks t) : JsonCommand ("clip.split", std::move (p)), clip (c), at (t)
    {
        changes.clips.insert (c);
    }

    juce::String getDescription() const override { return "Slice clip"; }
    juce::var getResult() const override { return resultWith ({ { "clip", made.value } }); }

    juce::Result perform (Project& p) override
    {
        const auto i = indexOf (p.playlist.clips, clip);
        if (i < 0)
            return juce::Result::fail ("clip " + juce::String (clip.value) + " does not exist");
        auto& c = p.playlist.clips[(size_t)i];
        if (at <= c.start || at >= c.start + c.length)
            return juce::Result::fail ("'at' must be inside the clip (after its start, before its end)");

        if (! made.isValid())
            made = p.allocateId<ClipId>();

        old = c;
        const auto cut = at - c.start;
        auto second = c;
        second.id = made;
        second.start = at;
        second.length = c.length - cut;
        c.length = cut;

        if (auto* pc = std::get_if<PatternClip> (&second.content))
            pc->offset += cut;
        else if (auto* audio = std::get_if<AudioClip> (&second.content))
        {
            audio->offset += cut;
            audio->fadeIn = 0;
            std::get<AudioClip> (c.content).fadeOut = 0;
        }
        else if (auto* automation = std::get_if<AutomationClip> (&second.content))
        {
            auto& first = std::get<AutomationClip> (c.content).points;
            const AutomationPoint middle { 0, model::automationValueAt (first, (double)cut), 0.0f };

            std::vector<AutomationPoint> after { middle };
            for (auto& point : first)
                if (point.time > cut)
                    after.push_back ({ point.time - cut, point.value, point.curve });
            automation->points = std::move (after);

            std::erase_if (first, [cut] (auto& point) { return point.time >= cut; });
            first.push_back ({ cut, middle.value, 0.0f });
        }

        p.playlist.clips.insert (p.playlist.clips.begin() + i + 1, std::move (second));
        changes.clips = { clip, made };
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        std::erase_if (p.playlist.clips, [this] (const Clip& c) { return c.id == made; });
        if (auto* c = p.findClip (clip))
            *c = old;
    }

private:
    ClipId clip, made;
    Ticks at;
    Clip old;
};

/** Places a clip given whole, as the project file writes it (its pattern, audio or automation, length,
    offset, fades and mute): the Playlist's paste and duplicate. */
class AddClip final : public JsonCommand
{
public:
    AddClip (juce::var p, Clip c) : JsonCommand ("clip.add", std::move (p)), clip (std::move (c)) {}

    juce::String getDescription() const override { return "Add clip"; }
    juce::var getResult() const override { return resultWith ({ { "clip", made.value } }); }

    juce::Result perform (Project& p) override
    {
        if (p.findLane (clip.lane) == nullptr)
            return juce::Result::fail ("lane " + juce::String (clip.lane.value) + " does not exist");
        if (clip.start < 0 || clip.length <= 0)
            return juce::Result::fail ("start must be >= 0 and length > 0");

        if (! made.isValid())
            made = p.allocateId<ClipId>();
        auto placed = clip;
        placed.id = made;
        p.playlist.clips.push_back (std::move (placed));
        changes.clips = { made };
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        std::erase_if (p.playlist.clips, [this] (const Clip& c) { return c.id == made; });
    }

private:
    Clip clip;
    ClipId made;
};

//==============================================================================
// launcher.* (the clip launcher)

/** One edit of the clip launcher, made by `edit` in apply(); undo restores the snapshot. The IDs the first
    perform makes are kept in `made` so redo makes the same ones. */
class LauncherEdit final : public SnapshotCommand
{
public:
    struct Made
    {
        LauncherTrackId track;
        SceneId scene;
        ClipId clip;
        ChannelId channel;
        InsertId insert;
    };
    using Edit = std::function<juce::Result (Project&, Made&)>;

    LauncherEdit (const char* commandName, juce::var p, juce::String what, Edit e)
        : SnapshotCommand (commandName, std::move (p)), description (std::move (what)), edit (std::move (e))
    {
    }

    juce::String getDescription() const override { return description; }
    juce::var getResult() const override
    {
        return resultWith ({ { "track", made.track.value },
                             { "scene", made.scene.value },
                             { "clip", made.clip.value },
                             { "channel", made.channel.value },
                             { "insert", made.insert.value } });
    }

protected:
    juce::Result apply (Project& p) override { return edit (p, made); }

private:
    juce::String description;
    Edit edit;
    Made made;
};

juce::Result launcherSlotExists (const Project& p, LauncherTrackId track, SceneId scene)
{
    if (indexOf (p.launcher.tracks, track) < 0)
        return juce::Result::fail ("launcher track " + juce::String (track.value) + " does not exist");
    if (indexOf (p.launcher.scenes, scene) < 0)
        return juce::Result::fail ("scene " + juce::String (scene.value) + " does not exist");
    return juce::Result::ok();
}

/** Puts a clip in its slot, replacing what was there. */
void placeInSlot (Project& p, const LauncherClip& clip)
{
    std::erase_if (p.launcher.clips,
                   [&] (const LauncherClip& c) { return c.track == clip.track && c.scene == clip.scene; });
    p.launcher.clips.push_back (clip);
}

//==============================================================================
// Registration

template <typename Make>
CommandInfo info (const char* name, const char* summary, const ParameterSchema& schema, Make make)
{
    return { name, summary, schema.build(),
             [make] (const juce::var& json, juce::String& error) -> std::unique_ptr<Command>
             {
                 Params params (json);
                 auto command = make (params);
                 if (! params.errors.isEmpty())
                 {
                     error = params.errors.joinIntoString ("; ");
                     return nullptr;
                 }
                 return command;
             } };
}

juce::var noteSchema()
{
    ParameterSchema s;
    s.integer ("start", "Start in ticks (960 per quarter note)", true, 0, std::numeric_limits<int>::max())
        .integer ("length", "Length in ticks", true, 1, std::numeric_limits<int>::max())
        .integer ("pitch", "MIDI note number", true, 0, 127)
        .number ("velocity", "Velocity", false, 0.0, 1.0)
        .number ("releaseVelocity", "Release velocity", false, 0.0, 1.0)
        .number ("pan", "Per-note pan", false, -1.0, 1.0)
        .number ("finePitchCents", "Fine pitch", false, -100.0, 100.0)
        .custom ("expressions", "Per-note modulation values by name; \"volume\" is the per-note volume, 0-1", false,
                 schemaObject ({ { "type", "object" } }));
    return s.build();
}

juce::var deviceSchema()
{
    return juce::JSON::parse (R"json({"type": "object", "properties": {
        "kind": {"enum": ["stock", "plugin"], "description": "\"stock\": Spinus's own or a bundled device, by stockType; \"plugin\": a plugin installed on this computer"},
        "stockType": {"type": "string", "description": "For kind \"stock\": e.g. \"spinus.3x-osc\", \"spinus.drum-machine\", \"spinus.eq\", \"bundled.dragonfly.hall\" (spinus --describe all lists them)"},
        "plugin": {"type": "object", "description": "For kind \"plugin\": the plugin as Spinus's plugin list names it (a saved project shows it)", "properties": {
            "format": {"type": "string", "description": "\"VST3\", \"CLAP\", \"LV2\" or \"VST\""},
            "name": {"type": "string"},
            "manufacturer": {"type": "string"},
            "fileOrIdentifier": {"type": "string", "description": "The plugin's file, or its identifier (LV2's URI)"},
            "uniqueId": {"type": "integer"}}}},
        "required": ["kind"]})json");
}

void addChannelCommands (std::vector<CommandInfo>& list)
{
    list.push_back (info (
        "channel.add", "Add a channel to the channel rack, by default with its own new mixer insert.",
        ParameterSchema()
            .string ("name", "Channel name", true)
            .custom ("instrument", "Instrument device (without id)", false, deviceSchema())
            .custom ("insert", "\"new\" (default), \"master\", or an insert ID", false,
                     schemaObject ({ { "type", juce::Array<juce::var> { "string", "integer" } } }))
            .integer ("index", "Position in the channel rack", false, 0, 100000)
            .boolean ("audioTrack", "An audio track (no instrument; for recording and audio clips) instead of "
                                    "a Channel Rack channel"),
        [] (Params& p) -> std::unique_ptr<Command>
        {
            p.allowOnly ({ "name", "instrument", "insert", "index", "audioTrack" });
            const auto name = p.string ("name", true).value_or ("");
            std::optional<Device> device;
            if (auto d = p.get ("instrument", false); ! d.isVoid())
            {
                Device parsed;
                if (auto e = deviceFromJson (d, parsed); e.isNotEmpty())
                    p.errors.add ("instrument: " + e);
                device = parsed;
            }
            auto insert = p.get ("insert", false);
            if (! insert.isVoid() && ! insert.isInt() && insert.toString() != "new" && insert.toString() != "master")
                p.errors.add ("'insert' must be \"new\", \"master\" or an insert ID");
            std::optional<int> index;
            if (auto i = p.number ("index", 0, 100000))
                index = (int)*i;
            const auto audioTrack = p.boolean ("audioTrack").value_or (false);
            if (audioTrack && device)
                p.errors.add ("an audio track has no instrument");
            return std::make_unique<AddChannel> (p.json, name, device, insert, index, audioTrack);
        }));

    list.push_back (info ("channel.remove", "Remove a channel, its notes in every pattern, and automation of it.",
                          ParameterSchema().id ("channel", "Channel ID"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "channel" });
                              return std::make_unique<RemoveChannel> (p.json, p.id<ChannelId> ("channel"));
                          }));

    list.push_back (info (
        "channel.replaceInstrument",
        "Replace a channel's instrument with another (FL's Replace): its notes, Playlist clips, insert, volume, pan, "
        "colour and group stay. Its name stays too unless it was the old instrument's name (then it, and its own "
        "insert's, become the new one's). Automation clips of the old instrument's settings are removed. Result: the "
        "new instrument's device ID.",
        ParameterSchema()
            .id ("channel", "Channel ID")
            .custom ("instrument", "The new instrument device (without id)", true, deviceSchema())
            .string ("name", "A new name for the channel (default: as described above)"),
        [] (Params& p) -> std::unique_ptr<Command>
        {
            p.allowOnly ({ "channel", "instrument", "name" });
            Device device;
            if (auto d = p.get ("instrument", true); ! d.isVoid())
                if (auto e = deviceFromJson (d, device); e.isNotEmpty())
                    p.errors.add ("instrument: " + e);
            auto name = p.string ("name");
            if (name && name->trim().isEmpty())
                p.errors.add ("'name' must not be empty");
            return std::make_unique<ReplaceInstrument> (p.json, p.id<ChannelId> ("channel"), device, name);
        }));

    list.push_back (info ("channel.set",
                          "Change a channel's name, colour, volume, pan, pitch, mute, solo or group (the Channel "
                          "Rack's filter group, e.g. \"Drums\"; empty for none), or an audio track's recording: its "
                          "input, stereo, and whether it is armed.",
                          ParameterSchema()
                              .id ("channel", "Channel ID")
                              .string ("name", "New name")
                              .string ("colour", "Colour as #AARRGGBB")
                              .number ("volumeDb", "Volume in dB", false, -100.0, 12.0)
                              .number ("pan", "Pan, -1 left to 1 right", false, -1.0, 1.0)
                              .number ("pitchCents", "Pitch offset in cents", false, -4800.0, 4800.0)
                              .boolean ("muted", "Muted")
                              .boolean ("solo", "Solo")
                              .string ("group", "Its group in the Channel Rack; empty for none")
                              .integer ("recordInput",
                                        "Audio track: the sound card input it records from, from 1 (0: the "
                                        "input chosen in the audio settings)",
                                        false, 0, 256)
                              .boolean ("recordStereo", "Audio track: records that input and the next as a stereo pair")
                              .boolean ("recordArmed", "Audio track: Record audio records it"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "channel", "name", "colour", "volumeDb", "pan", "pitchCents", "muted",
                                             "solo", "group", "recordInput", "recordStereo", "recordArmed" });
                              SetChannel::Values v { p.string ("name"),
                                                     p.colour ("colour"),
                                                     p.number ("volumeDb", -100.0, 12.0),
                                                     p.number ("pan", -1.0, 1.0),
                                                     p.number ("pitchCents", -4800.0, 4800.0),
                                                     p.boolean ("muted"),
                                                     p.boolean ("solo"),
                                                     p.string ("group"),
                                                     p.number ("recordInput", 0.0, 256.0),
                                                     p.boolean ("recordStereo"),
                                                     p.boolean ("recordArmed") };
                              return std::make_unique<SetChannel> (p.json, p.id<ChannelId> ("channel"), v);
                          }));

    list.push_back (info ("channel.freeze",
                          "Freeze a channel: its instrument is rendered to audio, which plays instead of the plugin "
                          "to save CPU. Unfreeze to edit the instrument again.",
                          ParameterSchema()
                              .id ("channel", "Channel ID")
                              .boolean ("frozen", "true to freeze (the default), false to unfreeze"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "channel", "frozen" });
                              return std::make_unique<SetFrozen<ChannelId>> (p.json, p.id<ChannelId> ("channel"),
                                                                             p.boolean ("frozen").value_or (true));
                          }));

    list.push_back (info ("channel.route", "Send a channel's output to a mixer insert.",
                          ParameterSchema().id ("channel", "Channel ID").id ("insert", "Target insert ID (1 = master)"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "channel", "insert" });
                              return std::make_unique<RouteChannel> (p.json, p.id<ChannelId> ("channel"),
                                                                     p.id<InsertId> ("insert"));
                          }));

    list.push_back (
        info ("channel.move", "Move a channel to another position in the channel rack.",
              ParameterSchema().id ("channel", "Channel ID").integer ("index", "New position", true, 0, 100000),
              [] (Params& p) -> std::unique_ptr<Command>
              {
                  p.allowOnly ({ "channel", "index" });
                  return std::make_unique<MoveItem<ChannelId>> (
                      "channel.move", p.json, p.id<ChannelId> ("channel"),
                      (int)p.number ("index", 0, 100000, true).value_or (0),
                      [] (Project& pr) -> std::vector<Channel>& { return pr.channels; }, "channel");
              }));
}

void addInsertCommands (std::vector<CommandInfo>& list)
{
    list.push_back (info (
        "insert.add", "Add a mixer insert.",
        ParameterSchema().string ("name", "Insert name").integer ("index", "Position in the mixer", false, 0, 100000),
        [] (Params& p) -> std::unique_ptr<Command>
        {
            p.allowOnly ({ "name", "index" });
            std::optional<int> index;
            if (auto i = p.number ("index", 0, 100000))
                index = (int)*i;
            return std::make_unique<AddInsert> (p.json, p.string ("name").value_or (""), index);
        }));

    list.push_back (info (
        "insert.addBus",
        "Add a bus: a new mixer insert (after the last of them) that the given inserts play into instead of where "
        "they went; the bus plays into that place. The result has the new insert ID.",
        ParameterSchema()
            .string ("name", "The bus's name, e.g. Drums")
            .custom ("inserts", "The inserts to play into it (IDs)", true,
                     schemaObject ({ { "type", "array" },
                                     { "items", schemaObject ({ { "type", "integer" }, { "minimum", 1 } }) },
                                     { "minItems", 1 } })),
        [] (Params& p) -> std::unique_ptr<Command>
        {
            p.allowOnly ({ "name", "inserts" });
            std::vector<InsertId> sources;
            if (auto* items = p.get ("inserts", true).getArray())
                for (const auto& item : *items)
                    sources.push_back (InsertId { (uint32_t)juce::jmax (0, (int)item) });
            return std::make_unique<AddBus> (p.json, p.string ("name").value_or ("Bus"), std::move (sources));
        }));

    list.push_back (info ("insert.remove",
                          "Remove a mixer insert. Channels and inserts that fed it go to master; its sends, sidechains "
                          "and automation are removed.",
                          ParameterSchema().id ("insert", "Insert ID"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "insert" });
                              return std::make_unique<RemoveInsert> (p.json, p.id<InsertId> ("insert"));
                          }));

    list.push_back (info (
        "insert.set", "Change an insert's name, colour, icon, volume, pan, stereo width, mute or solo.",
        ParameterSchema()
            .id ("insert", "Insert ID (1 = master)")
            .string ("name", "New name")
            .string ("colour", "Colour as #AARRGGBB")
            .string ("icon", "Icon name")
            .number ("volumeDb", "Volume in dB", false, -100.0, 12.0)
            .number ("pan", "Pan, -1 left to 1 right", false, -1.0, 1.0)
            .number ("stereoWidth", "0 mono, 1 unchanged, 2 extra wide", false, 0.0, 2.0)
            .boolean ("muted", "Muted")
            .boolean ("solo", "Solo"),
        [] (Params& p) -> std::unique_ptr<Command>
        {
            p.allowOnly ({ "insert", "name", "colour", "icon", "volumeDb", "pan", "stereoWidth", "muted", "solo" });
            SetInsert::Values v { p.string ("name"),           p.string ("icon"),
                                  p.colour ("colour"),         p.number ("volumeDb", -100.0, 12.0),
                                  p.number ("pan", -1.0, 1.0), p.number ("stereoWidth", 0.0, 2.0),
                                  p.boolean ("muted"),         p.boolean ("solo") };
            return std::make_unique<SetInsert> (p.json, p.id<InsertId> ("insert"), v);
        }));

    list.push_back (info (
        "insert.move", "Move a mixer insert to another position (routing is kept).",
        ParameterSchema().id ("insert", "Insert ID").integer ("index", "New position", true, 0, 100000),
        [] (Params& p) -> std::unique_ptr<Command>
        {
            p.allowOnly ({ "insert", "index" });
            return std::make_unique<MoveItem<InsertId>> (
                "insert.move", p.json, p.id<InsertId> ("insert"), (int)p.number ("index", 0, 100000, true).value_or (0),
                [] (Project& pr) -> std::vector<Insert>& { return pr.mixer.inserts; }, "insert");
        }));

    list.push_back (info ("insert.freeze",
                          "Freeze a mixer insert: everything playing into it and its effects are rendered to audio, "
                          "which plays instead of those plugins to save CPU. Not possible when something inside it "
                          "also plays elsewhere.",
                          ParameterSchema()
                              .id ("insert", "Insert ID (not the master)")
                              .boolean ("frozen", "true to freeze (the default), false to unfreeze"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "insert", "frozen" });
                              return std::make_unique<SetFrozen<InsertId>> (p.json, p.id<InsertId> ("insert"),
                                                                            p.boolean ("frozen").value_or (true));
                          }));

    list.push_back (info ("insert.setOutput", "Route an insert's output to another insert (no feedback loops).",
                          ParameterSchema().id ("insert", "Insert ID").id ("target", "Target insert ID (1 = master)"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "insert", "target" });
                              return std::make_unique<SetInsertOutput> (p.json, p.id<InsertId> ("insert"),
                                                                        p.id<InsertId> ("target"));
                          }));

    list.push_back (info ("insert.setSidechain",
                          "Feed another insert's sound into this insert's sidechain input (for a ducker or a "
                          "sidechain compressor), or remove it. No feedback loops.",
                          ParameterSchema()
                              .id ("insert", "Insert ID (1 = master)")
                              .id ("source", "Insert whose sound feeds the sidechain; leave out to remove it", false),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "insert", "source" });
                              return std::make_unique<SetSidechain> (p.json, p.id<InsertId> ("insert"),
                                                                     p.id<InsertId> ("source", false));
                          }));

    list.push_back (info ("insert.setNotesFrom",
                          "Send a channel's notes to an insert's effects as MIDI (e.g. the notes Spinus Tune pulls "
                          "a voice to), or stop.",
                          ParameterSchema()
                              .id ("insert", "Insert ID (not the master)")
                              .id ("channel", "Channel whose notes the effects receive; leave out to stop", false),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "insert", "channel" });
                              return std::make_unique<SetNotesFrom> (p.json, p.id<InsertId> ("insert"),
                                                                     p.id<ChannelId> ("channel", false));
                          }));

    list.push_back (info ("insert.setSend", "Add a send from one insert to another, or change its level.",
                          ParameterSchema()
                              .id ("insert", "Source insert ID")
                              .id ("target", "Target insert ID")
                              .number ("levelDb", "Send level in dB", true, -100.0, 12.0),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "insert", "target", "levelDb" });
                              return std::make_unique<SetSend> (
                                  p.json, p.id<InsertId> ("insert"), p.id<InsertId> ("target"),
                                  p.number ("levelDb", -100.0, 12.0, true).value_or (0.0));
                          }));

    list.push_back (info ("insert.removeSend", "Remove the send from one insert to another.",
                          ParameterSchema().id ("insert", "Source insert ID").id ("target", "Target insert ID"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "insert", "target" });
                              return std::make_unique<RemoveSend> (p.json, p.id<InsertId> ("insert"),
                                                                   p.id<InsertId> ("target"));
                          }));
}

void addEffectCommands (std::vector<CommandInfo>& list)
{
    list.push_back (info ("insert.addEffect",
                          "Add an effect to an insert (any number of effects). The result has the new device ID.",
                          ParameterSchema()
                              .id ("insert", "Insert ID (1 = master)")
                              .custom ("device", "Effect device (without id)", true, deviceSchema())
                              .integer ("index", "Slot position; default after the last effect", false, 0,
                                        std::numeric_limits<int>::max()),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "insert", "device", "index" });
                              Device device;
                              if (auto d = p.get ("device", true); ! d.isVoid())
                                  if (auto e = deviceFromJson (d, device); e.isNotEmpty())
                                      p.errors.add ("device: " + e);
                              std::optional<int> index;
                              if (auto i = p.number ("index", 0, std::numeric_limits<int>::max()))
                                  index = (int)*i;
                              return std::make_unique<AddEffect> (p.json, p.id<InsertId> ("insert"), device, index);
                          }));

    list.push_back (info ("insert.removeEffect", "Remove an effect from its insert, with its automation.",
                          ParameterSchema().id ("device", "The effect's device ID"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "device" });
                              return std::make_unique<RemoveEffect> (p.json, p.id<DeviceId> ("device"));
                          }));

    list.push_back (info ("insert.moveEffect",
                          "Move an effect to another slot, on the same insert or another one. It keeps its settings.",
                          ParameterSchema()
                              .id ("device", "The effect's device ID")
                              .id ("insert", "Target insert ID; default the effect's own insert", false)
                              .integer ("index", "New slot position", true, 0, std::numeric_limits<int>::max()),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "device", "insert", "index" });
                              std::optional<InsertId> target;
                              if (p.has ("insert"))
                                  target = p.id<InsertId> ("insert");
                              return std::make_unique<MoveEffect> (
                                  p.json, p.id<DeviceId> ("device"), target,
                                  (int)p.number ("index", 0, std::numeric_limits<int>::max(), true).value_or (0));
                          }));

    list.push_back (info (
        "device.setParameters",
        "Change settings of an instrument or effect, by the parameter ID automation uses (a Spinus device's own; for "
        "other plugins the position: 0, 1...). 'text': a Spinus device's settings as it shows them, \"35%\", "
        "\"220 ms\", \"-6 dB\", \"1.2 kHz\", \"High pass\" (by ID or name; a plain number is in the units shown, "
        "milliseconds for times, hertz for frequencies). 'values': numbers from 0 to 1 over each setting's whole "
        "range, for any device.",
        ParameterSchema()
            .id ("device", "The instrument's or effect's device ID")
            .custom ("values", "Parameter ID to value, 0 to 1 over the setting's whole range", false,
                     schemaObject ({ { "type", "object" },
                                     { "minProperties", 1 },
                                     { "additionalProperties",
                                       schemaObject ({ { "type", "number" }, { "minimum", 0 }, { "maximum", 1 } }) } }))
            .custom ("text",
                     "Spinus devices only: parameter ID or setting name to its value as the device shows it "
                     "(\"35%\", \"220 ms\", \"1.5 s\", \"-6 dB\", \"1.2 kHz\", \"4:1\", \"30% L\", \"Low pass\")",
                     false,
                     schemaObject ({ { "type", "object" },
                                     { "minProperties", 1 },
                                     { "additionalProperties", schemaObject ({ { "type", "string" } }) } })),
        [] (Params& p) -> std::unique_ptr<Command>
        {
            p.allowOnly ({ "device", "values", "text" });
            const auto device = p.id<DeviceId> ("device");
            std::map<juce::String, float> values;
            if (auto* o = p.json["values"].getDynamicObject())
                for (auto& property : o->getProperties())
                {
                    const auto v = property.value;
                    if (! (v.isDouble() || v.isInt() || v.isInt64()) || (double)v < 0.0 || (double)v > 1.0 ||
                        property.name.toString().isEmpty())
                        p.errors.add ("'values' must map parameter IDs to numbers from 0 to 1");
                    values[property.name.toString()] = (float)(double)v;
                }
            else if (p.has ("values"))
                p.errors.add ("'values' must be an object");
            std::map<juce::String, juce::String> texts;
            if (auto* o = p.json["text"].getDynamicObject())
                for (auto& property : o->getProperties())
                {
                    if (! property.value.isString() || property.value.toString().trim().isEmpty() ||
                        property.name.toString().isEmpty())
                        p.errors.add ("'text' must map parameter IDs or names to text such as \"35%\"");
                    texts[property.name.toString()] = property.value.toString();
                }
            else if (p.has ("text"))
                p.errors.add ("'text' must be an object");
            if (values.empty() && texts.empty())
                p.errors.add ("'values' or 'text' needs at least one setting");
            return std::make_unique<SetParameters> (p.json, device, std::move (values), "device.setParameters",
                                                    std::move (texts));
        }));

    list.push_back (info (
        "device.loadPreset",
        "Load one of 3x Osc's built-in presets: every setting of the instrument is set to the preset's (\"Default\": "
        "a new 3x Osc's settings). One undo step.",
        ParameterSchema()
            .id ("device", "The 3x Osc's device ID")
            .custom ("preset", "The preset's name, e.g. \"Reese Bass\" (any case), or \"Default\"", true,
                     []
                     {
                         juce::Array<juce::var> names;
                         names.add (juce::String (dsp::threeOscPresets::defaultName));
                         for (auto& each : dsp::threeOscPresets::all())
                             names.add (each.name);
                         return schemaObject ({ { "type", "string" }, { "examples", names } });
                     }()),
        [] (Params& p) -> std::unique_ptr<Command>
        {
            p.allowOnly ({ "device", "preset" });
            const auto device = p.id<DeviceId> ("device");
            const auto preset = p.string ("preset", true).value_or ("");
            if (p.has ("preset") && preset.trim().isEmpty())
                p.errors.add ("'preset' needs a preset's name");
            return std::make_unique<LoadPreset> (p.json, device, preset);
        }));

    list.push_back (info ("insert.setEffect", "Bypass an effect or enable it again.",
                          ParameterSchema().id ("device", "The effect's device ID").boolean ("bypassed", "Bypassed"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "device", "bypassed" });
                              return std::make_unique<SetEffect> (p.json, p.id<DeviceId> ("device"),
                                                                  p.boolean ("bypassed"));
                          }));
}

void addPlaylistCommands (std::vector<CommandInfo>& list)
{
    list.push_back (
        info ("lane.add", "Add a playlist track (lane).",
              ParameterSchema().string ("name", "Track name").integer ("index", "Position", false, 0, 100000),
              [] (Params& p) -> std::unique_ptr<Command>
              {
                  p.allowOnly ({ "name", "index" });
                  std::optional<int> index;
                  if (auto i = p.number ("index", 0, 100000))
                      index = (int)*i;
                  return std::make_unique<AddLane> (p.json, p.string ("name").value_or (""), index);
              }));

    list.push_back (info ("lane.remove", "Remove a playlist track and the clips on it.",
                          ParameterSchema().id ("lane", "Lane ID"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "lane" });
                              return std::make_unique<RemoveLane> (p.json, p.id<LaneId> ("lane"));
                          }));

    list.push_back (info ("lane.set",
                          "Rename, mute, colour, group or fold a playlist track. A track grouped under another is "
                          "muted and hidden (folded) with it.",
                          ParameterSchema()
                              .id ("lane", "Lane ID")
                              .string ("name", "New name")
                              .boolean ("muted", "Muted")
                              .string ("colour", "Colour as #AARRGGBB")
                              .integer ("group", "The track it is grouped under (lane ID), or 0 for none", false, 0,
                                        std::numeric_limits<int>::max())
                              .boolean ("collapsed", "Folded: the tracks grouped under it are hidden"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "lane", "name", "muted", "colour", "group", "collapsed" });
                              std::optional<LaneId> group;
                              if (auto g = p.number ("group", 0, std::numeric_limits<int>::max()))
                                  group = LaneId { (uint32_t)*g };
                              return std::make_unique<SetLane> (p.json, p.id<LaneId> ("lane"), p.string ("name"),
                                                                p.boolean ("muted"), p.colour ("colour"), group,
                                                                p.boolean ("collapsed"));
                          }));

    list.push_back (info ("marker.add",
                          "Add a named point (a marker) on the playlist's ruler, e.g. where the chorus starts. The "
                          "result has the new marker ID.",
                          ParameterSchema()
                              .integer ("at", "Where, in ticks", true, 0, std::numeric_limits<int>::max())
                              .string ("name", "Its name (default: Marker 1, 2...)"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "at", "name" });
                              const auto at = p.number ("at", 0, std::numeric_limits<int>::max());
                              if (! at)
                                  p.errors.add ("'at' is required");
                              return std::make_unique<AddMarker> (p.json, (Ticks)at.value_or (0),
                                                                  p.string ("name").value_or (""));
                          }));
    list.push_back (info ("marker.set", "Rename or move a marker.",
                          ParameterSchema()
                              .id ("marker", "Marker ID")
                              .string ("name", "New name")
                              .integer ("at", "New place, in ticks", false, 0, std::numeric_limits<int>::max()),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "marker", "name", "at" });
                              auto name = p.string ("name");
                              std::optional<Ticks> at;
                              if (auto a = p.number ("at", 0, std::numeric_limits<int>::max()))
                                  at = (Ticks)*a;
                              if (! name && ! at)
                                  p.errors.add ("give 'name' or 'at'");
                              return std::make_unique<SetMarker> (p.json, p.id<MarkerId> ("marker"), std::move (name),
                                                                  at);
                          }));
    list.push_back (info ("marker.remove", "Remove a marker.", ParameterSchema().id ("marker", "Marker ID"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "marker" });
                              return std::make_unique<RemoveMarker> (p.json, p.id<MarkerId> ("marker"));
                          }));

    list.push_back (info (
        "clip.addPattern", "Place a pattern on the playlist.",
        ParameterSchema()
            .id ("lane", "Lane ID")
            .id ("pattern", "Pattern ID")
            .integer ("start", "Start in ticks", true, 0, std::numeric_limits<int>::max())
            .integer ("length", "Length in ticks (default: the pattern's length)", false, 1,
                      std::numeric_limits<int>::max())
            .integer ("offset", "Where in the pattern to start, in ticks", false, 0, std::numeric_limits<int>::max()),
        [] (Params& p) -> std::unique_ptr<Command>
        {
            p.allowOnly ({ "lane", "pattern", "start", "length", "offset" });
            std::optional<Ticks> length;
            if (auto l = p.number ("length", 1, std::numeric_limits<int>::max()))
                length = (Ticks)*l;
            return std::make_unique<AddPatternClip> (
                p.json, p.id<LaneId> ("lane"), p.id<PatternId> ("pattern"),
                (Ticks)p.number ("start", 0, std::numeric_limits<int>::max(), true).value_or (0), length,
                (Ticks)p.number ("offset", 0, std::numeric_limits<int>::max()).value_or (0));
        }));

    list.push_back (
        info ("clip.addAudio",
              "Place an audio file on the playlist. It plays through `channel`, or through a new channel (with its "
              "own mixer insert) called `name` when no channel is given.",
              ParameterSchema()
                  .id ("lane", "Lane ID")
                  .string ("file", "The audio file: relative to the project's audio/ folder, or an absolute path", true)
                  .integer ("start", "Start in ticks", true, 0, std::numeric_limits<int>::max())
                  .integer ("length", "Length in ticks", true, 1, std::numeric_limits<int>::max())
                  .id ("channel", "Channel to play through (default: a new one)", false)
                  .string ("name", "Name of the new channel (default: the file name)")
                  .integer ("offset", "Where in the file to start, in ticks", false, 0, std::numeric_limits<int>::max())
                  .number ("gainDb", "Clip volume in dB", false, -100.0, 24.0)
                  .integer ("fadeIn", "Fade-in length in ticks", false, 0, std::numeric_limits<int>::max())
                  .integer ("fadeOut", "Fade-out length in ticks", false, 0, std::numeric_limits<int>::max()),
              [] (Params& p) -> std::unique_ptr<Command>
              {
                  p.allowOnly ({ "lane", "file", "start", "length", "channel", "name", "offset", "gainDb", "fadeIn",
                                 "fadeOut" });
                  AddAudioClip::Values v;
                  v.lane = p.id<LaneId> ("lane");
                  v.file = p.string ("file", true).value_or (juce::String());
                  v.start = (Ticks)p.number ("start", 0, std::numeric_limits<int>::max(), true).value_or (0);
                  v.length = (Ticks)p.number ("length", 1, std::numeric_limits<int>::max(), true).value_or (1);
                  if (p.has ("channel"))
                      v.channel = p.id<ChannelId> ("channel");
                  v.name = p.string ("name").value_or (fileStem (v.file));
                  if (v.name.isEmpty())
                      v.name = "Audio";
                  v.offset = (Ticks)p.number ("offset", 0, std::numeric_limits<int>::max()).value_or (0);
                  v.gainDb = (float)p.number ("gainDb", -100.0, 24.0).value_or (0.0);
                  v.fadeIn = (Ticks)p.number ("fadeIn", 0, std::numeric_limits<int>::max()).value_or (0);
                  v.fadeOut = (Ticks)p.number ("fadeOut", 0, std::numeric_limits<int>::max()).value_or (0);
                  return std::make_unique<AddAudioClip> (p.json, std::move (v));
              }));

    list.push_back (info (
        "clip.setAudio",
        "Change an audio clip's file, volume, fades, pitch, speed or direction. A pitch or speed of its own "
        "time-stretches the clip (stretch \"stretch\") unless \"resample\" is chosen.",
        ParameterSchema()
            .id ("clip", "Clip ID")
            .string ("file", "New audio file (relative to audio/, or absolute)")
            .number ("gainDb", "Clip volume in dB", false, -100.0, 24.0)
            .integer ("fadeIn", "Fade-in length in ticks", false, 0, std::numeric_limits<int>::max())
            .integer ("fadeOut", "Fade-out length in ticks", false, 0, std::numeric_limits<int>::max())
            .number ("pitch", "Semitones up or down", false, -24.0, 24.0)
            .number ("speed", "How fast it plays: 2 = twice as fast (half as long), 0.5 = half as fast", false, 0.25,
                     4.0)
            .custom (
                "stretch",
                "off (as recorded), stretch (pitch and speed apart), stretch-drums (the same, keeping hits "
                "sharp) or resample (faster is higher, like a record)",
                false,
                schemaObject ({ { "type", "string" },
                                { "enum", juce::Array<juce::var> { "off", "stretch", "stretch-drums", "resample" } } }))
            .boolean ("reversed", "Play it backwards")
            .boolean ("fit", "Keep the same audio in the clip: its length follows the new speed"),
        [] (Params& p) -> std::unique_ptr<Command>
        {
            p.allowOnly (
                { "clip", "file", "gainDb", "fadeIn", "fadeOut", "pitch", "speed", "stretch", "reversed", "fit" });
            SetAudioClip::Values v;
            v.file = p.string ("file");
            if (auto g = p.number ("gainDb", -100.0, 24.0))
                v.gainDb = (float)*g;
            if (auto f = p.number ("fadeIn", 0, std::numeric_limits<int>::max()))
                v.fadeIn = (Ticks)*f;
            if (auto f = p.number ("fadeOut", 0, std::numeric_limits<int>::max()))
                v.fadeOut = (Ticks)*f;
            if (auto n = p.number ("pitch", -24.0, 24.0))
                v.pitch = (float)*n;
            v.speed = p.number ("speed", 0.25, 4.0);
            v.stretch = p.string ("stretch");
            if (v.stretch &&
                ! juce::StringArray { "off", "stretch", "stretch-drums", "resample" }.contains (*v.stretch))
                p.errors.add ("'stretch' must be off, stretch, stretch-drums or resample");
            v.reversed = p.boolean ("reversed");
            v.fit = p.boolean ("fit").value_or (false);
            if (! v.file && ! v.gainDb && ! v.fadeIn && ! v.fadeOut && ! v.pitch && ! v.speed && ! v.stretch &&
                ! v.reversed)
                p.errors.add ("nothing to change: give file, gainDb, fadeIn, fadeOut, pitch, speed, stretch or "
                              "reversed");
            return std::make_unique<SetAudioClip> (p.json, p.id<ClipId> ("clip"), std::move (v));
        }));

    list.push_back (info (
        "clip.addTake",
        "Record a take into an audio track: the first on a lane of its own, later ones on new lanes (\"Take 2\"...) "
        "grouped under the first take's lane. A take plays where it was recorded; the earlier takes' parts there "
        "are muted (cut where needed). The result has the new clip and lane IDs.",
        ParameterSchema()
            .id ("channel", "The audio track (channel) the takes play through")
            .string ("file", "The audio file: relative to the project's audio/ folder, or an absolute path", true)
            .integer ("start", "Start in ticks", true, 0, std::numeric_limits<int>::max())
            .integer ("length", "Length in ticks", true, 1, std::numeric_limits<int>::max())
            .integer ("offset", "Where in the file to start, in ticks", false, 0, std::numeric_limits<int>::max()),
        [] (Params& p) -> std::unique_ptr<Command>
        {
            p.allowOnly ({ "channel", "file", "start", "length", "offset" });
            AddTake::Values v;
            v.channel = p.id<ChannelId> ("channel");
            v.file = p.string ("file", true).value_or (juce::String());
            v.start = (Ticks)p.number ("start", 0, std::numeric_limits<int>::max(), true).value_or (0);
            v.length = (Ticks)p.number ("length", 1, std::numeric_limits<int>::max(), true).value_or (1);
            v.offset = (Ticks)p.number ("offset", 0, std::numeric_limits<int>::max()).value_or (0);
            return std::make_unique<AddTake> (p.json, std::move (v));
        }));

    list.push_back (info ("clip.useTake",
                          "Comping: this audio clip plays, and the other clips of its audio track (other takes) are "
                          "muted where it plays, cut where they reach past it.",
                          ParameterSchema().id ("clip", "Clip ID"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "clip" });
                              return std::make_unique<UseTake> (p.json, p.id<ClipId> ("clip"));
                          }));

    const auto tuneSchema = juce::JSON::parse (R"json({"type": "array", "items": {"type": "object", "properties": {
        "start": {"type": "number", "minimum": 0, "description": "Seconds into the clip's file"},
        "end": {"type": "number", "minimum": 0, "description": "Seconds into the clip's file (after start)"},
        "pitch": {"type": "number", "minimum": 0, "maximum": 127, "description": "The note as sung (MIDI, fractional)"},
        "shift": {"type": "number", "minimum": -24, "maximum": 24, "description": "Semitones to move it"},
        "drift": {"type": "number", "minimum": 0, "maximum": 1, "description": "1 = as sung, 0 = straight"},
        "vibrato": {"type": "number", "minimum": 0, "maximum": 2, "description": "1 = as sung, 0 = none"}},
        "required": ["start", "end", "pitch"], "additionalProperties": false}})json");

    list.push_back (info ("clip.setTune",
                          "Spinus Tune: replace an audio clip's tuned notes (how each sung note is moved, "
                          "straightened or its vibrato changed). An empty list plays the clip as recorded.",
                          ParameterSchema()
                              .id ("clip", "Audio clip ID")
                              .custom ("notes", "The notes, in time order, not overlapping", true, tuneSchema)
                              .string ("description", "Undo history text, e.g. \"Move note\""),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "clip", "notes", "description" });
                              std::vector<TuneNote> notes;
                              if (auto* items = p.get ("notes", true).getArray())
                                  for (auto& item : *items)
                                  {
                                      TuneNote note;
                                      if (auto e = model::tuneNoteFromJson (item, note); e.isNotEmpty())
                                          p.errors.add ("notes: " + e);
                                      notes.push_back (note);
                                  }
                              else
                                  p.errors.add ("'notes' must be an array");
                              return std::make_unique<SetTune> (p.json, p.id<ClipId> ("clip"), std::move (notes),
                                                                p.string ("description").value_or (""));
                          }));

    const auto pointsSchema = juce::JSON::parse (R"json({"type": "array", "items": {"type": "object", "properties": {
        "time": {"type": "integer", "minimum": 0, "description": "Ticks from the clip's start"},
        "value": {"type": "number", "minimum": 0, "maximum": 1, "description": "0..1 of the parameter's range (scripts run with spinus --run may give \"text\" instead: \"-6 dB\", \"1.2 kHz\")"},
        "curve": {"type": "number", "minimum": -1, "maximum": 1, "description": "Shape of the segment that starts here; 0 = straight"}},
        "required": ["time", "value"], "additionalProperties": false}})json");
    const auto targetSchema = juce::JSON::parse (R"json({"type": "object", "properties": {
        "kind": {"enum": ["channelVolume", "channelPan", "insertVolume", "insertPan", "deviceParameter"], "description": "channelVolume and channelPan need channel; insertVolume and insertPan need insert; deviceParameter needs device and parameterId"},
        "channel": {"type": "integer", "description": "Channel ID, for channelVolume and channelPan"},
        "insert": {"type": "integer", "description": "Mixer insert ID, 1 for the master, for insertVolume and insertPan"},
        "device": {"type": "integer", "description": "Device ID, for deviceParameter"},
        "parameterId": {"type": "string", "description": "The setting's ID as spinus --describe lists it, for deviceParameter"}}, "required": ["kind"], "additionalProperties": false})json");

    auto readPoints = [] (Params& p) -> std::vector<AutomationPoint>
    {
        std::vector<AutomationPoint> points;
        if (auto* items = p.get ("points", true).getArray())
            for (auto& item : *items)
            {
                AutomationPoint point;
                if (auto e = model::pointFromJson (item, point); e.isNotEmpty())
                    p.errors.add ("points: " + e);
                points.push_back (point);
            }
        else
            p.errors.add ("'points' must be an array");
        return points;
    };

    list.push_back (
        info ("clip.addAutomation",
              "Add an automation clip: a curve for one parameter (a channel's or insert's volume or pan, or a device "
              "parameter by its ID). Without `lane`, it goes on a new lane at the bottom called `laneName`.",
              ParameterSchema()
                  .id ("lane", "Lane ID (default: a new lane)", false)
                  .string ("laneName", "Name of the new lane")
                  .integer ("start", "Start in ticks", true, 0, std::numeric_limits<int>::max())
                  .integer ("length", "Length in ticks", true, 1, std::numeric_limits<int>::max())
                  .custom ("target", "What the clip automates", true, targetSchema)
                  .custom ("points", "The curve's points, at least one", true, pointsSchema),
              [readPoints] (Params& p) -> std::unique_ptr<Command>
              {
                  p.allowOnly ({ "lane", "laneName", "start", "length", "target", "points" });
                  std::optional<LaneId> lane;
                  if (p.has ("lane"))
                      lane = p.id<LaneId> ("lane");
                  ParameterTarget target;
                  if (auto e = model::targetFromJson (p.get ("target", true), target); e.isNotEmpty())
                      p.errors.add ("target: " + e);
                  auto points = readPoints (p);
                  return std::make_unique<AddAutomationClip> (
                      p.json, lane, p.string ("laneName").value_or ("Automation"),
                      (Ticks)p.number ("start", 0, std::numeric_limits<int>::max(), true).value_or (0),
                      (Ticks)p.number ("length", 1, std::numeric_limits<int>::max(), true).value_or (1), target,
                      std::move (points));
              }));

    list.push_back (info ("clip.setPoints", "Replace the points of an automation clip.",
                          ParameterSchema()
                              .id ("clip", "Clip ID")
                              .custom ("points", "The curve's points, at least one", true, pointsSchema),
                          [readPoints] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "clip", "points" });
                              auto points = readPoints (p);
                              return std::make_unique<SetAutomationPoints> (p.json, p.id<ClipId> ("clip"),
                                                                            std::move (points));
                          }));

    list.push_back (info ("clip.set", "Move, resize, mute or re-offset a playlist clip.",
                          ParameterSchema()
                              .id ("clip", "Clip ID")
                              .id ("lane", "New lane", false)
                              .integer ("start", "New start in ticks", false, 0, std::numeric_limits<int>::max())
                              .integer ("length", "New length in ticks", false, 1, std::numeric_limits<int>::max())
                              .integer ("offset", "New offset in ticks", false, 0, std::numeric_limits<int>::max())
                              .boolean ("muted", "Muted"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "clip", "lane", "start", "length", "offset", "muted" });
                              SetClip::Values v;
                              if (p.has ("lane"))
                                  v.lane = p.id<LaneId> ("lane");
                              if (auto x = p.number ("start", 0, std::numeric_limits<int>::max()))
                                  v.start = (Ticks)*x;
                              if (auto x = p.number ("length", 1, std::numeric_limits<int>::max()))
                                  v.length = (Ticks)*x;
                              if (auto x = p.number ("offset", 0, std::numeric_limits<int>::max()))
                                  v.offset = (Ticks)*x;
                              v.muted = p.boolean ("muted");
                              return std::make_unique<SetClip> (p.json, p.id<ClipId> ("clip"), v);
                          }));

    list.push_back (info ("clip.remove", "Delete a playlist clip.", ParameterSchema().id ("clip", "Clip ID"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "clip" });
                              return std::make_unique<RemoveClip> (p.json, p.id<ClipId> ("clip"));
                          }));

    list.push_back (info (
        "clip.split",
        "Cut a playlist clip in two at a song time (ticks). The second part is a new clip that "
        "plays on from there; its ID is in the result.",
        ParameterSchema()
            .id ("clip", "Clip ID")
            .integer ("at", "Where to cut, in song ticks (inside the clip)", true, 1, std::numeric_limits<int>::max()),
        [] (Params& p) -> std::unique_ptr<Command>
        {
            p.allowOnly ({ "clip", "at" });
            const auto at = p.number ("at", 1, std::numeric_limits<int>::max(), true);
            return std::make_unique<SplitClip> (p.json, p.id<ClipId> ("clip"), (Ticks)at.value_or (1));
        }));

    const auto clipSchema = juce::JSON::parse (R"({"type": "object", "properties": {
        "type": {"enum": ["pattern", "audio", "automation"]},
        "lane": {"type": "integer", "minimum": 1}, "start": {"type": "integer", "minimum": 0},
        "length": {"type": "integer", "minimum": 1}}, "required": ["type", "lane", "start", "length"]})");
    list.push_back (info ("clip.add",
                          "Place a playlist clip given whole, in the project file's format (without id): a pattern, "
                          "audio or automation clip with all its settings. The new clip's ID is in the result.",
                          ParameterSchema().custom ("clip", "The clip, as project.json writes it", true, clipSchema),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "clip" });
                              Clip clip;
                              if (auto json = p.get ("clip", true); ! json.isObject())
                                  p.errors.add ("'clip' must be an object");
                              else if (auto e = model::clipFromJson (json, clip); e.isNotEmpty())
                                  p.errors.add ("clip: " + e);
                              return std::make_unique<AddClip> (p.json, std::move (clip));
                          }));
}

void addLauncherCommands (std::vector<CommandInfo>& list)
{
    constexpr auto maxTicks = std::numeric_limits<int>::max();

    list.push_back (
        info ("launcher.addTrack", "Add a clip launcher track (a column of slots).",
              ParameterSchema().string ("name", "Track name").integer ("index", "Position", false, 0, 100000),
              [] (Params& p) -> std::unique_ptr<Command>
              {
                  p.allowOnly ({ "name", "index" });
                  const auto name = p.string ("name").value_or ("");
                  const auto index = p.number ("index", 0, 100000);
                  return std::make_unique<LauncherEdit> (
                      "launcher.addTrack", p.json, "Add launcher track",
                      [name, index] (Project& project, LauncherEdit::Made& made)
                      {
                          auto& tracks = project.launcher.tracks;
                          if (index && *index > (double)tracks.size())
                              return juce::Result::fail ("index out of range");
                          if (! made.track)
                              made.track = project.allocateId<LauncherTrackId>();
                          LauncherTrack t;
                          t.id = made.track;
                          t.name = name.isNotEmpty() ? name : "Track " + juce::String ((int)tracks.size() + 1);
                          tracks.insert (tracks.begin() + (index ? (int)*index : (int)tracks.size()), t);
                          return juce::Result::ok();
                      });
              }));

    list.push_back (info ("launcher.removeTrack", "Remove a clip launcher track and the clips in it.",
                          ParameterSchema().id ("track", "Launcher track ID"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "track" });
                              const auto track = p.id<LauncherTrackId> ("track");
                              return std::make_unique<LauncherEdit> (
                                  "launcher.removeTrack", p.json, "Remove launcher track",
                                  [track] (Project& project, LauncherEdit::Made&)
                                  {
                                      auto& tracks = project.launcher.tracks;
                                      const auto i = indexOf (tracks, track);
                                      if (i < 0)
                                          return juce::Result::fail ("launcher track " + juce::String (track.value) +
                                                                     " does not exist");
                                      tracks.erase (tracks.begin() + i);
                                      std::erase_if (project.launcher.clips,
                                                     [track] (const LauncherClip& c) { return c.track == track; });
                                      return juce::Result::ok();
                                  });
                          }));

    list.push_back (info ("launcher.setTrack", "Rename a clip launcher track or change its colour.",
                          ParameterSchema()
                              .id ("track", "Launcher track ID")
                              .string ("name", "New name")
                              .custom ("colour", "Colour as #aarrggbb", false,
                                       schemaObject ({ { "type", "string" }, { "pattern", "^#[0-9a-fA-F]{8}$" } })),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "track", "name", "colour" });
                              const auto track = p.id<LauncherTrackId> ("track");
                              const auto name = p.string ("name");
                              const auto colour = p.colour ("colour");
                              if (! name && ! colour)
                                  p.errors.add ("nothing to change: give name or colour");
                              return std::make_unique<LauncherEdit> (
                                  "launcher.setTrack", p.json,
                                  name ? "Rename launcher track to " + quoted (*name) : "Colour launcher track",
                                  [track, name, colour] (Project& project, LauncherEdit::Made&)
                                  {
                                      const auto i = indexOf (project.launcher.tracks, track);
                                      if (i < 0)
                                          return juce::Result::fail ("launcher track " + juce::String (track.value) +
                                                                     " does not exist");
                                      auto& t = project.launcher.tracks[(size_t)i];
                                      t.name = name.value_or (t.name);
                                      t.colour = colour.value_or (t.colour);
                                      return juce::Result::ok();
                                  });
                          }));

    list.push_back (
        info ("launcher.addScene", "Add a clip launcher scene (a row of slots).",
              ParameterSchema().string ("name", "Scene name").integer ("index", "Position", false, 0, 100000),
              [] (Params& p) -> std::unique_ptr<Command>
              {
                  p.allowOnly ({ "name", "index" });
                  const auto name = p.string ("name").value_or ("");
                  const auto index = p.number ("index", 0, 100000);
                  return std::make_unique<LauncherEdit> (
                      "launcher.addScene", p.json, "Add scene",
                      [name, index] (Project& project, LauncherEdit::Made& made)
                      {
                          auto& scenes = project.launcher.scenes;
                          if (index && *index > (double)scenes.size())
                              return juce::Result::fail ("index out of range");
                          if (! made.scene)
                              made.scene = project.allocateId<SceneId>();
                          Scene scene;
                          scene.id = made.scene;
                          scene.name = name.isNotEmpty() ? name : "Scene " + juce::String ((int)scenes.size() + 1);
                          scenes.insert (scenes.begin() + (index ? (int)*index : (int)scenes.size()), scene);
                          return juce::Result::ok();
                      });
              }));

    list.push_back (info ("launcher.removeScene", "Remove a clip launcher scene and the clips in it.",
                          ParameterSchema().id ("scene", "Scene ID"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "scene" });
                              const auto scene = p.id<SceneId> ("scene");
                              return std::make_unique<LauncherEdit> (
                                  "launcher.removeScene", p.json, "Remove scene",
                                  [scene] (Project& project, LauncherEdit::Made&)
                                  {
                                      auto& scenes = project.launcher.scenes;
                                      const auto i = indexOf (scenes, scene);
                                      if (i < 0)
                                          return juce::Result::fail ("scene " + juce::String (scene.value) +
                                                                     " does not exist");
                                      scenes.erase (scenes.begin() + i);
                                      std::erase_if (project.launcher.clips,
                                                     [scene] (const LauncherClip& c) { return c.scene == scene; });
                                      return juce::Result::ok();
                                  });
                          }));

    list.push_back (info ("launcher.setScene", "Rename a clip launcher scene.",
                          ParameterSchema().id ("scene", "Scene ID").string ("name", "New name", true),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "scene", "name" });
                              const auto scene = p.id<SceneId> ("scene");
                              const auto name = p.string ("name", true).value_or ("");
                              return std::make_unique<LauncherEdit> (
                                  "launcher.setScene", p.json, "Rename scene to " + quoted (name),
                                  [scene, name] (Project& project, LauncherEdit::Made&)
                                  {
                                      const auto i = indexOf (project.launcher.scenes, scene);
                                      if (i < 0)
                                          return juce::Result::fail ("scene " + juce::String (scene.value) +
                                                                     " does not exist");
                                      project.launcher.scenes[(size_t)i].name = name;
                                      return juce::Result::ok();
                                  });
                          }));

    list.push_back (info (
        "launcher.addPattern",
        "Put a pattern in a clip launcher slot (replacing what is there). Launched, it plays on the pattern's "
        "channels, looped over `length`.",
        ParameterSchema()
            .id ("track", "Launcher track ID")
            .id ("scene", "Scene ID")
            .id ("pattern", "Pattern ID")
            .integer ("length", "Length of one pass in ticks (default: the pattern's length)", false, 1, maxTicks)
            .boolean ("loop", "Loop (default true); false plays it once"),
        [] (Params& p) -> std::unique_ptr<Command>
        {
            p.allowOnly ({ "track", "scene", "pattern", "length", "loop" });
            LauncherClip clip;
            clip.track = p.id<LauncherTrackId> ("track");
            clip.scene = p.id<SceneId> ("scene");
            clip.content = PatternClip { p.id<PatternId> ("pattern"), 0 };
            const auto length = p.number ("length", 1, maxTicks);
            clip.loop = p.boolean ("loop").value_or (true);
            return std::make_unique<LauncherEdit> (
                "launcher.addPattern", p.json, "Put pattern in launcher",
                [clip, length] (Project& project, LauncherEdit::Made& made) mutable
                {
                    if (auto r = launcherSlotExists (project, clip.track, clip.scene); r.failed())
                        return r;
                    const auto* pattern = project.findPattern (std::get<PatternClip> (clip.content).pattern);
                    if (pattern == nullptr)
                        return juce::Result::fail ("pattern " +
                                                   juce::String (std::get<PatternClip> (clip.content).pattern.value) +
                                                   " does not exist");
                    if (! made.clip)
                        made.clip = project.allocateId<ClipId>();
                    clip.id = made.clip;
                    clip.length = length ? (Ticks)*length : pattern->length;
                    placeInSlot (project, clip);
                    return juce::Result::ok();
                });
        }));

    list.push_back (info (
        "launcher.addAudio",
        "Put an audio file in a clip launcher slot (replacing what is there). It plays through `channel`, or "
        "through a new channel (with its own mixer insert) called `name` when no channel is given.",
        ParameterSchema()
            .id ("track", "Launcher track ID")
            .id ("scene", "Scene ID")
            .string ("file", "The audio file: relative to the project's audio/ folder, or an absolute path", true)
            .integer ("length", "Length of one pass in ticks", true, 1, maxTicks)
            .id ("channel", "Channel to play through (default: a new one)", false)
            .string ("name", "Name of the new channel (default: the file name)")
            .integer ("offset", "Where in the file to start, in ticks", false, 0, maxTicks)
            .number ("gainDb", "Clip volume in dB", false, -100.0, 24.0)
            .boolean ("loop", "Loop (default true); false plays it once"),
        [] (Params& p) -> std::unique_ptr<Command>
        {
            p.allowOnly ({ "track", "scene", "file", "length", "channel", "name", "offset", "gainDb", "loop" });
            LauncherClip clip;
            clip.track = p.id<LauncherTrackId> ("track");
            clip.scene = p.id<SceneId> ("scene");
            clip.length = (Ticks)p.number ("length", 1, maxTicks, true).value_or (1);
            clip.loop = p.boolean ("loop").value_or (true);
            AudioClip audio;
            audio.file = p.string ("file", true).value_or (juce::String());
            audio.offset = (Ticks)p.number ("offset", 0, maxTicks).value_or (0);
            audio.gainDb = (float)p.number ("gainDb", -100.0, 24.0).value_or (0.0);
            audio.stretchMode = "off"; // as on the Playlist until Rubber Band
            std::optional<ChannelId> channel;
            if (p.has ("channel"))
                channel = p.id<ChannelId> ("channel");
            auto name = p.string ("name").value_or (fileStem (audio.file));
            if (name.isEmpty())
                name = "Audio";
            return std::make_unique<LauncherEdit> (
                "launcher.addAudio", p.json, "Put audio " + quoted (name) + " in launcher",
                [clip, audio, channel, name] (Project& project, LauncherEdit::Made& made) mutable
                {
                    if (auto r = launcherSlotExists (project, clip.track, clip.scene); r.failed())
                        return r;
                    if (channel && project.findChannel (*channel) == nullptr)
                        return juce::Result::fail ("channel " + juce::String (channel->value) + " does not exist");
                    if (audio.file.isEmpty())
                        return juce::Result::fail ("file must not be empty");
                    if (! made.clip)
                    {
                        made.clip = project.allocateId<ClipId>();
                        if (! channel)
                        {
                            made.channel = project.allocateId<ChannelId>();
                            made.insert = project.allocateId<InsertId>();
                        }
                    }
                    if (channel)
                        audio.channel = *channel;
                    else
                    {
                        // Its own channel and insert, as an audio clip dropped on the Playlist gets.
                        Insert insert;
                        insert.id = made.insert;
                        insert.name = name;
                        project.mixer.inserts.push_back (insert);
                        Channel c;
                        c.id = made.channel;
                        c.name = name;
                        c.targetInsert = made.insert;
                        c.audioTrack = true;
                        project.channels.push_back (c);
                        audio.channel = made.channel;
                    }
                    clip.id = made.clip;
                    clip.content = audio;
                    placeInSlot (project, clip);
                    return juce::Result::ok();
                });
        }));

    list.push_back (info (
        "launcher.setClip",
        "Move a clip launcher clip to another slot (replacing what is there), or change its length or looping.",
        ParameterSchema()
            .id ("clip", "Launcher clip ID")
            .id ("track", "Launcher track to move it to", false)
            .id ("scene", "Scene to move it to", false)
            .integer ("length", "Length of one pass in ticks", false, 1, maxTicks)
            .boolean ("loop", "Loop; false plays it once"),
        [] (Params& p) -> std::unique_ptr<Command>
        {
            p.allowOnly ({ "clip", "track", "scene", "length", "loop" });
            const auto id = p.id<ClipId> ("clip");
            std::optional<LauncherTrackId> track;
            std::optional<SceneId> scene;
            if (p.has ("track"))
                track = p.id<LauncherTrackId> ("track");
            if (p.has ("scene"))
                scene = p.id<SceneId> ("scene");
            const auto length = p.number ("length", 1, maxTicks);
            const auto loop = p.boolean ("loop");
            if (! track && ! scene && ! length && ! loop)
                p.errors.add ("nothing to change: give track, scene, length or loop");
            return std::make_unique<LauncherEdit> (
                "launcher.setClip", p.json, track || scene ? "Move launcher clip" : "Change launcher clip",
                [id, track, scene, length, loop] (Project& project, LauncherEdit::Made&)
                {
                    const auto* found = project.launcher.findClip (id);
                    if (found == nullptr)
                        return juce::Result::fail ("launcher clip " + juce::String (id.value) + " does not exist");
                    auto clip = *found;
                    clip.track = track.value_or (clip.track);
                    clip.scene = scene.value_or (clip.scene);
                    if (auto r = launcherSlotExists (project, clip.track, clip.scene); r.failed())
                        return r;
                    if (length)
                        clip.length = (Ticks)*length;
                    clip.loop = loop.value_or (clip.loop);
                    std::erase_if (project.launcher.clips, [id] (const LauncherClip& c) { return c.id == id; });
                    placeInSlot (project, clip);
                    return juce::Result::ok();
                });
        }));

    list.push_back (info (
        "launcher.removeClip", "Empty a clip launcher slot.", ParameterSchema().id ("clip", "Launcher clip ID"),
        [] (Params& p) -> std::unique_ptr<Command>
        {
            p.allowOnly ({ "clip" });
            const auto id = p.id<ClipId> ("clip");
            return std::make_unique<LauncherEdit> (
                "launcher.removeClip", p.json, "Remove launcher clip",
                [id] (Project& project, LauncherEdit::Made&)
                {
                    if (std::erase_if (project.launcher.clips, [id] (const LauncherClip& c) { return c.id == id; }) ==
                        0)
                        return juce::Result::fail ("launcher clip " + juce::String (id.value) + " does not exist");
                    return juce::Result::ok();
                });
        }));

    list.push_back (info (
        "launcher.setQuantize", "When launched clips start: at once, on the next beat or on the next bar.",
        ParameterSchema().custom (
            "quantize", "none, beat or bar", true,
            schemaObject ({ { "type", "string" }, { "enum", juce::Array<juce::var> { "none", "beat", "bar" } } })),
        [] (Params& p) -> std::unique_ptr<Command>
        {
            p.allowOnly ({ "quantize" });
            const auto text = p.string ("quantize", true).value_or ("bar");
            if (text != "none" && text != "beat" && text != "bar")
                p.errors.add ("'quantize' must be none, beat or bar");
            const auto quantize = text == "none"   ? LaunchQuantize::none
                                  : text == "beat" ? LaunchQuantize::beat
                                                   : LaunchQuantize::bar;
            return std::make_unique<LauncherEdit> ("launcher.setQuantize", p.json,
                                                   text == "none" ? juce::String ("Launch clips at once")
                                                                  : "Launch clips on the next " + text,
                                                   [quantize] (Project& project, LauncherEdit::Made&)
                                                   {
                                                       project.launcher.quantize = quantize;
                                                       return juce::Result::ok();
                                                   });
        }));
}

void addPatternCommands (std::vector<CommandInfo>& list)
{
    list.push_back (info (
        "pattern.add", "Add an empty pattern.",
        ParameterSchema()
            .string ("name", "Pattern name")
            .integer ("length", "Length in ticks (3840 = one 4/4 bar)", false, 1, std::numeric_limits<int>::max()),
        [] (Params& p) -> std::unique_ptr<Command>
        {
            p.allowOnly ({ "name", "length" });
            std::optional<Ticks> length;
            if (auto l = p.number ("length", 1, std::numeric_limits<int>::max()))
                length = (Ticks)*l;
            return std::make_unique<AddPattern> (p.json, p.string ("name").value_or (""), length);
        }));

    list.push_back (info ("pattern.clone",
                          "Copy a pattern with all its notes; the copy comes right after it. The result has "
                          "the new pattern ID.",
                          ParameterSchema()
                              .id ("pattern", "Pattern ID")
                              .string ("name", "Name of the copy (default: "
                                               "the name with the next number)"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "pattern", "name" });
                              return std::make_unique<ClonePattern> (p.json, p.id<PatternId> ("pattern"),
                                                                     p.string ("name").value_or (""));
                          }));

    list.push_back (info ("clip.makeUnique",
                          "Give a playlist pattern clip a copy of its pattern of its own, so it can change without "
                          "changing the pattern's other clips. The result has the new pattern ID.",
                          ParameterSchema().id ("clip", "Clip ID (a pattern clip on the playlist)"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "clip" });
                              return std::make_unique<MakeClipUnique> (p.json, p.id<ClipId> ("clip"));
                          }));

    list.push_back (info ("pattern.remove", "Remove a pattern and its clips from the playlist.",
                          ParameterSchema().id ("pattern", "Pattern ID"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "pattern" });
                              return std::make_unique<RemovePattern> (p.json, p.id<PatternId> ("pattern"));
                          }));

    list.push_back (info (
        "pattern.setLength",
        "Set a pattern's length (its notes stay; notes past the end do not play). The next note or time "
        "signature change fits it to its notes again: whole bars, at least one.",
        ParameterSchema()
            .id ("pattern", "Pattern ID")
            .integer ("length", "Length in ticks (3840 = one 4/4 bar)", true, 1, std::numeric_limits<int>::max()),
        [] (Params& p) -> std::unique_ptr<Command>
        {
            p.allowOnly ({ "pattern", "length" });
            const auto length = p.number ("length", 1, std::numeric_limits<int>::max(), true);
            return std::make_unique<SetPatternLength> (p.json, p.id<PatternId> ("pattern"), (Ticks)length.value_or (1));
        }));

    list.push_back (info ("pattern.setTimeSignature",
                          "Give a pattern its own time signature, or make it follow the song's by giving neither. "
                          "The pattern becomes whole bars of it (at least one) that hold its notes.",
                          ParameterSchema()
                              .id ("pattern", "Pattern ID")
                              .integer ("numerator", "Beats per bar", false, 1, 32)
                              .integer ("denominator", "The beat's note value: 1, 2, 4, 8, 16 or 32", false, 1, 32),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "pattern", "numerator", "denominator" });
                              const auto n = p.number ("numerator", 1, 32);
                              const auto d = p.number ("denominator", 1, 32);
                              std::optional<TimeSignature> s;
                              if (n.has_value() != d.has_value())
                                  p.errors.add ("give both 'numerator' and 'denominator', or neither");
                              else if (n)
                              {
                                  s = TimeSignature { (int)*n, (int)*d };
                                  if (! s->isValid())
                                      p.errors.add ("'denominator' must be 1, 2, 4, 8, 16 or 32");
                              }
                              return std::make_unique<SetPatternTimeSignature> (p.json, p.id<PatternId> ("pattern"), s);
                          }));

    list.push_back (info ("pattern.set", "Rename a pattern or change its colour.",
                          ParameterSchema()
                              .id ("pattern", "Pattern ID")
                              .string ("name", "New name")
                              .string ("colour", "Colour as #AARRGGBB"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "pattern", "name", "colour" });
                              auto name = p.string ("name");
                              auto colour = p.colour ("colour");
                              if (! name && ! colour)
                                  p.errors.add ("give 'name' or 'colour'");
                              return std::make_unique<SetPattern> (p.json, p.id<PatternId> ("pattern"),
                                                                   std::move (name), colour);
                          }));

    list.push_back (info (
        "pattern.setSwing",
        "Set a pattern's swing: notes on every second Channel Rack step play later, by up to half a "
        "step at 1 (a dotted feel); 0 is straight. Notes are not moved.",
        ParameterSchema().id ("pattern", "Pattern ID").number ("swing", "0 (straight) to 1", true, 0.0, 1.0),
        [] (Params& p) -> std::unique_ptr<Command>
        {
            p.allowOnly ({ "pattern", "swing" });
            const auto swing = p.number ("swing", 0.0, 1.0);
            if (! swing)
                p.errors.add ("'swing' is required");
            return std::make_unique<SetPatternSwing> (p.json, p.id<PatternId> ("pattern"), (float)swing.value_or (0.0));
        }));

    list.push_back (info ("pattern.setTripletSteps",
                          "Make a pattern's Channel Rack steps sixteenth-note triplets (six per beat) or sixteenths "
                          "(four per beat, the default). Notes are not moved.",
                          ParameterSchema()
                              .id ("pattern", "Pattern ID")
                              .boolean ("triplets", "true for triplet steps, false for sixteenths"),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "pattern", "triplets" });
                              const auto triplets = p.boolean ("triplets");
                              if (! triplets.has_value())
                                  p.errors.add ("'triplets' is required");
                              return std::make_unique<SetPatternTripletSteps> (p.json, p.id<PatternId> ("pattern"),
                                                                               triplets.value_or (false));
                          }));

    list.push_back (info ("pattern.setNotes",
                          "Replace all notes of one channel in one pattern. The pattern's length follows its notes: "
                          "whole bars of its time signature, at least one.",
                          ParameterSchema()
                              .id ("pattern", "Pattern ID")
                              .id ("channel", "Channel ID")
                              .custom ("notes", "The channel's complete note list", true,
                                       schemaObject ({ { "type", "array" }, { "items", noteSchema() } }))
                              .string ("description", "Undo history text, e.g. \"Add note\""),
                          [] (Params& p) -> std::unique_ptr<Command>
                          {
                              p.allowOnly ({ "pattern", "channel", "notes", "description" });
                              std::vector<Note> notes;
                              auto noteList = p.get ("notes", true);
                              if (! noteList.isVoid() && ! noteList.isArray())
                                  p.errors.add ("'notes' must be an array");
                              if (auto* array = noteList.getArray())
                                  for (auto& n : *array)
                                  {
                                      Note note;
                                      if (auto e = noteFromJson (n, note); e.isNotEmpty())
                                          p.errors.add ("note: " + e);
                                      notes.push_back (note);
                                  }
                              return std::make_unique<SetNotes> (p.json, p.id<PatternId> ("pattern"),
                                                                 p.id<ChannelId> ("channel"), notes,
                                                                 p.string ("description").value_or (""));
                          }));
}

} // namespace

//==============================================================================
CommandRegistry::CommandRegistry()
{
    commands.push_back (info ("project.setTempo", "Set the project tempo.",
                              ParameterSchema().number ("bpm", "Tempo in beats per minute", true, 20.0, 999.0),
                              [] (Params& p) -> std::unique_ptr<Command>
                              {
                                  p.allowOnly ({ "bpm" });
                                  return std::make_unique<SetTempo> (
                                      p.json, p.number ("bpm", 20.0, 999.0, true).value_or (120.0));
                              }));

    commands.push_back (info ("project.setTimeSignature",
                              "Set the song's time signature (patterns without their own follow it, as whole "
                              "bars of it that hold their notes).",
                              ParameterSchema()
                                  .integer ("numerator", "Beats per bar", true, 1, 32)
                                  .integer ("denominator", "The beat's note value: 1, 2, 4, 8, 16 or 32", true, 1, 32),
                              [] (Params& p) -> std::unique_ptr<Command>
                              {
                                  p.allowOnly ({ "numerator", "denominator" });
                                  const TimeSignature s { (int)p.number ("numerator", 1, 32, true).value_or (4),
                                                          (int)p.number ("denominator", 1, 32, true).value_or (4) };
                                  if (! s.isValid())
                                      p.errors.add ("'denominator' must be 1, 2, 4, 8, 16 or 32");
                                  return std::make_unique<SetTimeSignature> (p.json, s);
                              }));

    commands.push_back (
        info ("project.setKey", "Set the song's key (its first note and scale), or clear it by giving neither.",
              ParameterSchema()
                  .integer ("tonic", "First note of the key: 0 = C ... 11 = B", false, 0, 11)
                  .custom ("scale", "Scale name", false,
                           juce::JSON::parse (
                               "{\"enum\": " + juce::JSON::toString (juce::var (model::getScaleNames()), true) + "}")),
              [] (Params& p) -> std::unique_ptr<Command>
              {
                  p.allowOnly ({ "tonic", "scale" });
                  const auto tonic = p.number ("tonic", 0, 11);
                  const auto scale = p.string ("scale");
                  if (scale && ! model::getScaleNames().contains (*scale))
                      p.errors.add ("unknown scale '" + *scale + "'");
                  std::optional<Key> key;
                  if (tonic || scale)
                      key = Key { (int)tonic.value_or (0), scale.value_or ("major") };
                  return std::make_unique<SetKey> (p.json, key);
              }));

    addChannelCommands (commands);
    addInsertCommands (commands);
    addEffectCommands (commands);
    addPatternCommands (commands);
    addPlaylistCommands (commands);
    addLauncherCommands (commands);
}

const CommandRegistry& CommandRegistry::get()
{
    static const CommandRegistry registry;
    return registry;
}

const CommandInfo* CommandRegistry::find (const juce::String& name) const
{
    for (auto& c : commands)
        if (c.name == name)
            return &c;
    return nullptr;
}

std::unique_ptr<Command> CommandRegistry::create (const juce::String& name, const juce::var& parameters,
                                                  juce::String& error) const
{
    auto* command = find (name);
    if (command == nullptr)
    {
        error = "unknown command '" + name + "'";
        return nullptr;
    }

    auto result = command->create (parameters, error);
    if (result == nullptr)
        error = name + ": " + error;
    return result;
}

std::unique_ptr<Command> CommandRegistry::createTransaction (const juce::var& json, juce::String& error) const
{
    auto* steps = json.getProperty ("commands", {}).getArray();
    if (steps == nullptr || steps->isEmpty())
    {
        error = "a transaction needs a non-empty 'commands' array";
        return nullptr;
    }

    auto transaction = std::make_unique<Transaction> (json.getProperty ("description", "AI proposal").toString());

    for (auto& step : *steps)
    {
        auto command = create (step.getProperty ("command", "").toString(), step.getProperty ("parameters", {}), error);
        if (command == nullptr)
            return nullptr;
        transaction->add (std::move (command));
    }

    return transaction;
}

juce::var CommandRegistry::getSchema() const
{
    auto* defs = new juce::DynamicObject();
    for (auto& c : commands)
        defs->setProperty (c.name,
                           schemaObject ({ { "description", c.summary }, { "parameters", c.parameterSchema } }));

    return schemaObject ({ { "$schema", "https://json-schema.org/draft/2020-12/schema" },
                           { "title", "Spinus DAW command API" },
                           { "commands", juce::var (defs) } });
}

} // namespace spinus::commands
