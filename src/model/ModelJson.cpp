#include "ModelJson.h"

#include "Migrations.h"
#include "Validation.h"

namespace spinus::model
{

namespace
{

//==============================================================================
// Writing

struct Obj
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();

    Obj& set (const char* key, const juce::var& value)
    {
        o->setProperty (key, value);
        return *this;
    }

    operator juce::var() const { return juce::var (o.get()); }
};

juce::var ticks (Ticks t) { return juce::var ((juce::int64)t); }
juce::var colour (juce::uint32 argb) { return "#" + juce::String::toHexString ((juce::int64)argb).paddedLeft ('0', 8); }

template <typename IdType> juce::var id (IdType i) { return juce::var ((int)i.value); }

juce::var deviceKindName (Device::Kind k) { return k == Device::Kind::plugin ? "plugin" : "stock"; }

const std::pair<ParameterTarget::Kind, const char*> targetKinds[] = {
    { ParameterTarget::Kind::channelVolume, "channelVolume" },     { ParameterTarget::Kind::channelPan, "channelPan" },
    { ParameterTarget::Kind::insertVolume, "insertVolume" },       { ParameterTarget::Kind::insertPan, "insertPan" },
    { ParameterTarget::Kind::deviceParameter, "deviceParameter" },
};

juce::var write (const Device& d)
{
    Obj o;
    o.set ("id", id (d.id)).set ("kind", deviceKindName (d.kind));

    if (d.kind == Device::Kind::stock)
        o.set ("stockType", d.stockType);
    else
        o.set ("plugin", Obj()
                             .set ("format", d.plugin.format)
                             .set ("name", d.plugin.name)
                             .set ("manufacturer", d.plugin.manufacturer)
                             .set ("fileOrIdentifier", d.plugin.fileOrIdentifier)
                             .set ("uniqueId", d.plugin.uniqueId));

    o.set ("bypassed", d.bypassed);
    if (! d.parameters.empty())
    {
        juce::DynamicObject::Ptr values = new juce::DynamicObject();
        for (auto& [parameter, value] : d.parameters)
            values->setProperty (parameter, value);
        o.set ("parameters", juce::var (values.get()));
    }
    return o;
}

juce::var write (const Note& n)
{
    Obj o;
    o.set ("start", ticks (n.start))
        .set ("length", ticks (n.length))
        .set ("pitch", n.pitch)
        .set ("velocity", n.velocity)
        .set ("releaseVelocity", n.releaseVelocity)
        .set ("pan", n.pan)
        .set ("finePitchCents", n.finePitchCents);

    if (! n.expressions.empty())
    {
        Obj e;
        for (auto& [name, value] : n.expressions)
            e.set (name.toRawUTF8(), value);
        o.set ("expressions", e);
    }

    return o;
}

juce::var write (const ParameterTarget& t)
{
    Obj o;
    for (auto& [kind, name] : targetKinds)
        if (kind == t.kind)
            o.set ("kind", name);

    using K = ParameterTarget::Kind;
    if (t.kind == K::channelVolume || t.kind == K::channelPan)
        o.set ("channel", id (t.channel));
    else if (t.kind == K::insertVolume || t.kind == K::insertPan)
        o.set ("insert", id (t.insert));
    else
        o.set ("device", id (t.device)).set ("parameterId", t.parameterId);

    return o;
}

juce::var write (const Clip& c)
{
    Obj o;
    o.set ("id", id (c.id))
        .set ("lane", id (c.lane))
        .set ("start", ticks (c.start))
        .set ("length", ticks (c.length))
        .set ("muted", c.muted);

    if (auto* p = std::get_if<PatternClip> (&c.content))
        o.set ("type", "pattern").set ("pattern", id (p->pattern)).set ("offset", ticks (p->offset));
    else if (auto* a = std::get_if<AudioClip> (&c.content))
        o.set ("type", "audio")
            .set ("file", a->file)
            .set ("channel", id (a->channel))
            .set ("offset", ticks (a->offset))
            .set ("gainDb", a->gainDb)
            .set ("pitchSemitones", a->pitchSemitones)
            .set ("speed", a->speed)
            .set ("reversed", a->reversed)
            .set ("stretchMode", a->stretchMode)
            .set ("fadeIn", ticks (a->fadeIn))
            .set ("fadeOut", ticks (a->fadeOut));
    if (auto* a = std::get_if<AudioClip> (&c.content); a != nullptr && ! a->tune.empty())
    {
        juce::Array<juce::var> notes;
        for (auto& n : a->tune)
            notes.add (tuneNoteToJson (n));
        o.set ("tune", notes);
    }
    else if (auto* m = std::get_if<AutomationClip> (&c.content))
    {
        juce::Array<juce::var> points;
        for (auto& point : m->points)
            points.add (Obj().set ("time", ticks (point.time)).set ("value", point.value).set ("curve", point.curve));

        o.set ("type", "automation").set ("target", write (m->target)).set ("points", points);
    }

    return o;
}

juce::var write (const Launcher& l)
{
    juce::Array<juce::var> tracks, scenes, clips;
    for (auto& t : l.tracks)
        tracks.add (Obj().set ("id", id (t.id)).set ("name", t.name).set ("colour", colour (t.colour)));
    for (auto& scene : l.scenes)
        scenes.add (Obj().set ("id", id (scene.id)).set ("name", scene.name));
    for (auto& c : l.clips)
    {
        // The content as a Playlist clip writes it, placed by track and scene instead of lane and start.
        Clip asPlaylistClip;
        asPlaylistClip.id = c.id;
        asPlaylistClip.length = c.length;
        std::visit ([&] (auto& content) { asPlaylistClip.content = content; }, c.content);
        auto o = write (asPlaylistClip);
        auto* object = o.getDynamicObject();
        for (auto* key : { "lane", "start", "muted" })
            object->removeProperty (key);
        object->setProperty ("track", id (c.track));
        object->setProperty ("scene", id (c.scene));
        object->setProperty ("loop", c.loop);
        clips.add (o);
    }
    const char* quantize = l.quantize == LaunchQuantize::none   ? "none"
                           : l.quantize == LaunchQuantize::beat ? "beat"
                                                                : "bar";
    return Obj().set ("tracks", tracks).set ("scenes", scenes).set ("clips", clips).set ("quantize", quantize);
}

juce::var write (const Insert& i)
{
    juce::Array<juce::var> effects, sends, sidechains;

    for (auto& slot : i.effects)
        effects.add (Obj().set ("device", write (slot.device)).set ("mix", slot.mix));

    for (auto& s : i.sends)
        sends.add (Obj().set ("target", id (s.target)).set ("levelDb", s.levelDb));

    for (auto source : i.sidechainSources)
        sidechains.add (id (source));

    Obj o;
    o.set ("id", id (i.id))
        .set ("name", i.name)
        .set ("colour", colour (i.colour))
        .set ("icon", i.icon)
        .set ("volumeDb", i.volumeDb)
        .set ("pan", i.pan)
        .set ("stereoWidth", i.stereoWidth)
        .set ("muted", i.muted)
        .set ("solo", i.solo);

    if (i.frozen)
        o.set ("frozen", true);

    if (i.output)
        o.set ("output", id (i.output));
    if (i.notesFrom)
        o.set ("notesFrom", id (i.notesFrom));

    return o.set ("effects", effects).set ("sends", sends).set ("sidechainSources", sidechains);
}

//==============================================================================
// Reading: lenient about missing optional fields, strict about types and references (validate()).

struct Reader
{
    juce::StringArray errors;

    static juce::var get (const juce::var& o, const char* key) { return o.getProperty (key, {}); }

    double number (const juce::var& o, const char* key, double fallback)
    {
        auto v = get (o, key);
        if (v.isVoid())
            return fallback;
        if (! (v.isDouble() || v.isInt() || v.isInt64()))
        {
            errors.add (juce::String ("'") + key + "' must be a number");
            return fallback;
        }
        return (double)v;
    }

    float real (const juce::var& o, const char* key, float fallback) { return (float)number (o, key, fallback); }
    int integer (const juce::var& o, const char* key, int fallback) { return (int)number (o, key, fallback); }
    Ticks tick (const juce::var& o, const char* key, Ticks fallback)
    {
        auto v = get (o, key);
        return v.isVoid() ? fallback : (v.isInt() || v.isInt64() ? (Ticks)(juce::int64)v : (Ticks)number (o, key, 0));
    }

    bool boolean (const juce::var& o, const char* key, bool fallback)
    {
        auto v = get (o, key);
        return v.isVoid() ? fallback : (bool)v;
    }

    juce::String text (const juce::var& o, const char* key, const juce::String& fallback = {})
    {
        auto v = get (o, key);
        return v.isVoid() ? fallback : v.toString();
    }

    juce::uint32 colour (const juce::var& o, const char* key, juce::uint32 fallback)
    {
        auto s = text (o, key);
        return s.startsWith ("#") ? (juce::uint32)s.substring (1).getHexValue64() : fallback;
    }

    template <typename IdType> IdType id (const juce::var& o, const char* key)
    {
        return IdType { (uint32_t)juce::jmax (0, integer (o, key, 0)) };
    }

    const juce::Array<juce::var>& array (const juce::var& o, const char* key)
    {
        static const juce::Array<juce::var> empty;
        auto* a = get (o, key).getArray();
        return a != nullptr ? *a : empty;
    }

    Device device (const juce::var& o)
    {
        Device d;
        d.id = id<DeviceId> (o, "id");
        d.kind = text (o, "kind") == "plugin" ? Device::Kind::plugin : Device::Kind::stock;
        d.stockType = text (o, "stockType");
        auto p = get (o, "plugin");
        d.plugin.format = text (p, "format");
        d.plugin.name = text (p, "name");
        d.plugin.manufacturer = text (p, "manufacturer");
        d.plugin.fileOrIdentifier = text (p, "fileOrIdentifier");
        d.plugin.uniqueId = integer (p, "uniqueId", 0);
        d.bypassed = boolean (o, "bypassed", false);
        if (auto* values = get (o, "parameters").getDynamicObject())
            for (auto& property : values->getProperties())
                d.parameters[property.name.toString()] = (float)(double)property.value;
        return d;
    }

    Note note (const juce::var& o)
    {
        Note n;
        n.start = tick (o, "start", 0);
        n.length = tick (o, "length", n.length);
        n.pitch = integer (o, "pitch", n.pitch);
        n.velocity = real (o, "velocity", n.velocity);
        n.releaseVelocity = real (o, "releaseVelocity", n.releaseVelocity);
        n.pan = real (o, "pan", 0.0f);
        n.finePitchCents = real (o, "finePitchCents", 0.0f);

        if (auto* e = get (o, "expressions").getDynamicObject())
            for (auto& property : e->getProperties())
                n.expressions[property.name.toString()] = (float)(double)property.value;

        return n;
    }

    ParameterTarget target (const juce::var& o)
    {
        ParameterTarget t;
        const auto kind = text (o, "kind");
        bool known = false;

        for (auto& [k, name] : targetKinds)
            if (kind == name)
            {
                t.kind = k;
                known = true;
            }

        if (! known)
            errors.add ("unknown automation target '" + kind + "'");

        t.channel = id<ChannelId> (o, "channel");
        t.insert = id<InsertId> (o, "insert");
        t.device = id<DeviceId> (o, "device");
        t.parameterId = text (o, "parameterId");
        return t;
    }

    Clip clip (const juce::var& o)
    {
        Clip c;
        c.id = id<ClipId> (o, "id");
        c.lane = id<LaneId> (o, "lane");
        c.start = tick (o, "start", 0);
        c.length = tick (o, "length", c.length);
        c.muted = boolean (o, "muted", false);
        const auto type = text (o, "type");

        if (type == "pattern")
            c.content = PatternClip { id<PatternId> (o, "pattern"), tick (o, "offset", 0) };
        else if (type == "audio")
        {
            AudioClip a;
            a.file = text (o, "file");
            a.channel = id<ChannelId> (o, "channel");
            a.offset = tick (o, "offset", 0);
            a.gainDb = real (o, "gainDb", 0.0f);
            a.pitchSemitones = real (o, "pitchSemitones", 0.0f);
            a.speed = number (o, "speed", 1.0);
            a.reversed = (bool)o.getProperty ("reversed", false);
            a.stretchMode = text (o, "stretchMode", a.stretchMode);
            a.fadeIn = tick (o, "fadeIn", 0);
            a.fadeOut = tick (o, "fadeOut", 0);
            for (auto& n : array (o, "tune"))
            {
                TuneNote note;
                if (auto e = tuneNoteFromJson (n, note); e.isNotEmpty())
                    errors.add ("clip " + juce::String (c.id.value) + ": " + e);
                a.tune.push_back (note);
            }
            c.content = a;
        }
        else if (type == "automation")
        {
            AutomationClip a;
            a.target = target (get (o, "target"));
            for (auto& p : array (o, "points"))
                a.points.push_back ({ tick (p, "time", 0), real (p, "value", 0.0f), real (p, "curve", 0.0f) });
            c.content = a;
        }
        else
            errors.add ("clip " + juce::String (c.id.value) + " has unknown type '" + type + "'");

        return c;
    }

    Launcher launcher (const juce::var& o)
    {
        Launcher l;
        for (auto& t : array (o, "tracks"))
            l.tracks.push_back ({ id<LauncherTrackId> (t, "id"), text (t, "name"), colour (t, "colour", 0xff3d4450) });
        for (auto& scene : array (o, "scenes"))
            l.scenes.push_back ({ id<SceneId> (scene, "id"), text (scene, "name") });
        for (auto& c : array (o, "clips"))
        {
            const auto read = clip (c);
            LauncherClip lc;
            lc.id = read.id;
            lc.track = id<LauncherTrackId> (c, "track");
            lc.scene = id<SceneId> (c, "scene");
            lc.length = read.length;
            lc.loop = boolean (c, "loop", true);
            if (auto* pattern = std::get_if<PatternClip> (&read.content))
                lc.content = *pattern;
            else if (auto* audio = std::get_if<AudioClip> (&read.content))
                lc.content = *audio;
            else
                errors.add ("launcher clip " + juce::String (lc.id.value) + " must hold a pattern or audio");
            l.clips.push_back (lc);
        }
        const auto quantize = text (o, "quantize", "bar");
        l.quantize = quantize == "none"   ? LaunchQuantize::none
                     : quantize == "beat" ? LaunchQuantize::beat
                                          : LaunchQuantize::bar;
        if (quantize != "none" && quantize != "beat" && quantize != "bar")
            errors.add ("launcher: unknown quantize '" + quantize + "'");
        return l;
    }

    Insert insert (const juce::var& o)
    {
        Insert i;
        i.id = id<InsertId> (o, "id");
        i.name = text (o, "name");
        i.colour = colour (o, "colour", i.colour);
        i.icon = text (o, "icon");
        i.volumeDb = real (o, "volumeDb", 0.0f);
        i.pan = real (o, "pan", 0.0f);
        i.stereoWidth = real (o, "stereoWidth", 1.0f);
        i.muted = boolean (o, "muted", false);
        i.solo = boolean (o, "solo", false);
        i.frozen = boolean (o, "frozen", false);
        i.output = id<InsertId> (o, "output");
        i.notesFrom = id<ChannelId> (o, "notesFrom");

        for (auto& e : array (o, "effects"))
            i.effects.push_back ({ device (get (e, "device")), real (e, "mix", 1.0f) });

        for (auto& s : array (o, "sends"))
            i.sends.push_back ({ id<InsertId> (s, "target"), real (s, "levelDb", 0.0f) });

        for (auto& s : array (o, "sidechainSources"))
            i.sidechainSources.push_back (InsertId { (uint32_t)(int)s });

        return i;
    }
};

} // namespace

//==============================================================================
juce::var toJson (const Project& p)
{
    Obj settings;
    settings.set ("tempoBpm", p.settings.tempoBpm)
        .set ("timeSignature", Obj()
                                   .set ("numerator", p.settings.timeSignature.numerator)
                                   .set ("denominator", p.settings.timeSignature.denominator))
        .set ("sampleRate", p.settings.sampleRate)
        .set ("title", p.settings.title)
        .set ("author", p.settings.author)
        .set ("comments", p.settings.comments);

    if (p.settings.key)
        settings.set ("key", Obj().set ("tonic", p.settings.key->tonic).set ("scale", p.settings.key->scale));

    juce::Array<juce::var> channels, patterns, lanes, clips, inserts;

    for (auto& c : p.channels)
    {
        Obj o;
        o.set ("id", id (c.id)).set ("name", c.name).set ("colour", colour (c.colour));

        if (c.instrument)
            o.set ("instrument", write (*c.instrument));

        o.set ("volumeDb", c.volumeDb)
            .set ("pan", c.pan)
            .set ("pitchCents", c.pitchCents)
            .set ("muted", c.muted)
            .set ("solo", c.solo);
        if (c.frozen)
            o.set ("frozen", true);
        if (c.audioTrack)
            o.set ("audioTrack", true);
        if (c.group.isNotEmpty())
            o.set ("group", c.group);
        if (c.recordInput > 0)
            o.set ("recordInput", c.recordInput);
        if (c.recordStereo)
            o.set ("recordStereo", true);
        if (c.recordArmed)
            o.set ("recordArmed", true);
        channels.add (o.set ("targetInsert", id (c.targetInsert)));
    }

    for (auto& pattern : p.patterns)
    {
        juce::Array<juce::var> perChannel;
        for (auto& [channelId, notes] : pattern.notes)
        {
            juce::Array<juce::var> noteList;
            for (auto& n : notes)
                noteList.add (write (n));
            perChannel.add (Obj().set ("channel", id (channelId)).set ("notes", noteList));
        }

        auto patternJson = Obj()
                               .set ("id", id (pattern.id))
                               .set ("name", pattern.name)
                               .set ("colour", colour (pattern.colour))
                               .set ("length", ticks (pattern.length));
        if (pattern.timeSignature)
        {
            const auto signature = pattern.timeSignature.value_or (TimeSignature {});
            patternJson.set ("timeSignature",
                             Obj().set ("numerator", signature.numerator).set ("denominator", signature.denominator));
        }
        if (pattern.tripletSteps)
            patternJson.set ("tripletSteps", true);
        if (pattern.swing > 0.0f)
            patternJson.set ("swing", (double)pattern.swing);
        patterns.add (patternJson.set ("channels", perChannel));
    }

    for (auto& lane : p.playlist.lanes)
    {
        auto o = Obj()
                     .set ("id", id (lane.id))
                     .set ("name", lane.name)
                     .set ("colour", colour (lane.colour))
                     .set ("muted", lane.muted);
        if (lane.group.isValid()) // only for grouped and folded tracks
            o.set ("group", id (lane.group));
        if (lane.collapsed)
            o.set ("collapsed", true);
        lanes.add (o);
    }

    for (auto& clip : p.playlist.clips)
        clips.add (write (clip));

    for (auto& insert : p.mixer.inserts)
        inserts.add (write (insert));

    // Markers only when there are some: projects without them are written as before.
    Obj playlistObject;
    playlistObject.set ("lanes", lanes).set ("clips", clips);
    if (! p.playlist.markers.empty())
    {
        juce::Array<juce::var> markers;
        for (auto& marker : p.playlist.markers)
            markers.add (Obj()
                             .set ("id", (juce::int64)marker.id.value)
                             .set ("at", (juce::int64)marker.at)
                             .set ("name", marker.name));
        playlistObject.set ("markers", markers);
    }

    Obj root;
    root.set ("schemaVersion", Project::currentSchemaVersion)
        .set ("settings", settings)
        .set ("channels", channels)
        .set ("patterns", patterns)
        .set ("playlist", playlistObject)
        .set ("mixer", Obj().set ("master", write (p.mixer.master)).set ("inserts", inserts))
        .set ("nextId", (juce::int64)p.nextId);

    if (! p.launcher.isEmpty())
        root.set ("launcher", write (p.launcher));

    return root;
}

juce::String toJsonText (const Project& p)
{
    // JUCE writes \r\n line breaks by default; project files use \n on every platform so they diff cleanly.
    // Written so by the stream: replacing them afterwards copies the whole text once per line, which took
    // minutes for a song of many instruments (and the save never finished).
    juce::MemoryOutputStream out (65536); // 64 KiB to start with
    out.setNewLineString ("\n");
    juce::JSON::writeToStream (out, toJson (p),
                               juce::JSON::FormatOptions().withSpacing (juce::JSON::Spacing::multiLine));
    return out.toUTF8();
}

juce::Result fromJson (const juce::var& input, Project& result)
{
    if (! input.isObject())
        return juce::Result::fail ("project.json is not a JSON object");

    auto json = input;
    if (auto migrated = migrateToCurrent (json); migrated.failed())
        return migrated;

    Reader r;
    Project p;
    auto settings = Reader::get (json, "settings");
    p.settings.tempoBpm = r.number (settings, "tempoBpm", 120.0);
    auto sig = Reader::get (settings, "timeSignature");
    p.settings.timeSignature = { r.integer (sig, "numerator", 4), r.integer (sig, "denominator", 4) };
    p.settings.sampleRate = r.number (settings, "sampleRate", 48000.0);
    p.settings.title = r.text (settings, "title");
    p.settings.author = r.text (settings, "author");
    p.settings.comments = r.text (settings, "comments");

    if (auto key = Reader::get (settings, "key"); key.isObject())
        p.settings.key = Key { r.integer (key, "tonic", 0), r.text (key, "scale", "major") };

    for (auto& c : r.array (json, "channels"))
    {
        Channel ch;
        ch.id = r.id<ChannelId> (c, "id");
        ch.name = r.text (c, "name");
        ch.colour = r.colour (c, "colour", ch.colour);
        if (auto instrument = Reader::get (c, "instrument"); instrument.isObject())
            ch.instrument = r.device (instrument);
        ch.volumeDb = r.real (c, "volumeDb", 0.0f);
        ch.pan = r.real (c, "pan", 0.0f);
        ch.pitchCents = r.real (c, "pitchCents", 0.0f);
        ch.muted = r.boolean (c, "muted", false);
        ch.solo = r.boolean (c, "solo", false);
        ch.frozen = r.boolean (c, "frozen", false);
        ch.audioTrack = r.boolean (c, "audioTrack", false);
        ch.targetInsert = r.id<InsertId> (c, "targetInsert");
        ch.group = r.text (c, "group");
        ch.recordInput = juce::jmax (0, r.integer (c, "recordInput", 0));
        ch.recordStereo = r.boolean (c, "recordStereo", false);
        ch.recordArmed = r.boolean (c, "recordArmed", false);
        p.channels.push_back (ch);
    }

    for (auto& pj : r.array (json, "patterns"))
    {
        Pattern pattern;
        pattern.id = r.id<PatternId> (pj, "id");
        pattern.name = r.text (pj, "name");
        pattern.colour = r.colour (pj, "colour", pattern.colour);
        pattern.length = r.tick (pj, "length", pattern.length);
        if (auto own = Reader::get (pj, "timeSignature"); own.isObject())
            pattern.timeSignature =
                TimeSignature { r.integer (own, "numerator", 4), r.integer (own, "denominator", 4) };
        pattern.tripletSteps = r.boolean (pj, "tripletSteps", false);
        pattern.swing = (float)juce::jlimit (0.0, 1.0, r.number (pj, "swing", 0.0));

        for (auto& perChannel : r.array (pj, "channels"))
        {
            auto& notes = pattern.notes[r.id<ChannelId> (perChannel, "channel")];
            for (auto& n : r.array (perChannel, "notes"))
                notes.push_back (r.note (n));
        }

        p.patterns.push_back (pattern);
    }

    auto playlist = Reader::get (json, "playlist");
    for (auto& l : r.array (playlist, "lanes"))
        p.playlist.lanes.push_back ({ r.id<LaneId> (l, "id"), r.text (l, "name"), r.colour (l, "colour", 0xff3d4450),
                                      r.boolean (l, "muted", false), r.id<LaneId> (l, "group"),
                                      r.boolean (l, "collapsed", false) });

    for (auto& c : r.array (playlist, "clips"))
        p.playlist.clips.push_back (r.clip (c));
    if (Reader::get (playlist, "markers").isArray())
        for (auto& m : r.array (playlist, "markers"))
            p.playlist.markers.push_back ({ r.id<MarkerId> (m, "id"), r.tick (m, "at", 0), r.text (m, "name") });

    auto mixer = Reader::get (json, "mixer");
    p.mixer.master = r.insert (Reader::get (mixer, "master"));
    for (auto& i : r.array (mixer, "inserts"))
        p.mixer.inserts.push_back (r.insert (i));

    if (auto launcher = Reader::get (json, "launcher"); launcher.isObject())
        p.launcher = r.launcher (launcher);
    p.nextId = (uint32_t)r.number (json, "nextId", 0);

    // Projects saved before audio tracks: a channel without an instrument that audio clips play
    // through was made for them, so it is one.
    auto markAudioTrack = [&p] (const AudioClip& audio)
    {
        if (auto* channel = p.findChannel (audio.channel); channel != nullptr && ! channel->instrument)
            channel->audioTrack = true;
    };
    for (auto& clip : p.playlist.clips)
        if (auto* audio = std::get_if<AudioClip> (&clip.content))
            markAudioTrack (*audio);
    for (auto& clip : p.launcher.clips)
        if (auto* audio = std::get_if<AudioClip> (&clip.content))
            markAudioTrack (*audio);

    if (! r.errors.isEmpty())
        return juce::Result::fail (r.errors.joinIntoString ("\n"));

    if (auto problems = validate (p); ! problems.isEmpty())
        return juce::Result::fail ("invalid project:\n" + problems.joinIntoString ("\n"));

    result = std::move (p);
    return juce::Result::ok();
}

juce::var noteToJson (const Note& n) { return write (n); }
juce::var deviceToJson (const Device& d) { return write (d); }

juce::String noteFromJson (const juce::var& json, Note& result)
{
    Reader r;
    result = r.note (json);
    return r.errors.joinIntoString ("; ");
}

juce::String deviceFromJson (const juce::var& json, Device& result)
{
    Reader r;
    result = r.device (json);
    return r.errors.joinIntoString ("; ");
}

juce::var targetToJson (const ParameterTarget& t) { return write (t); }

juce::var clipToJson (const Clip& c) { return write (c); }

juce::String clipFromJson (const juce::var& json, Clip& result)
{
    Reader r;
    result = r.clip (json);
    return r.errors.joinIntoString ("; ");
}

juce::String targetFromJson (const juce::var& json, ParameterTarget& result)
{
    Reader r;
    result = r.target (json);
    return r.errors.joinIntoString ("; ");
}

juce::var pointToJson (const AutomationPoint& p)
{
    return Obj().set ("time", ticks (p.time)).set ("value", p.value).set ("curve", p.curve);
}

juce::String pointFromJson (const juce::var& json, AutomationPoint& result)
{
    Reader r;
    result = { r.tick (json, "time", 0), r.real (json, "value", 0.0f), r.real (json, "curve", 0.0f) };
    return r.errors.joinIntoString ("; ");
}

juce::var tuneNoteToJson (const TuneNote& n)
{
    return Obj()
        .set ("start", n.start)
        .set ("end", n.end)
        .set ("pitch", n.pitch)
        .set ("shift", n.shift)
        .set ("drift", n.drift)
        .set ("vibrato", n.vibrato);
}

juce::String tuneNoteFromJson (const juce::var& json, TuneNote& result)
{
    Reader r;
    if (! json.isObject())
        return "a tune note must be an object";
    result.start = (double)Reader::get (json, "start");
    result.end = (double)Reader::get (json, "end");
    result.pitch = r.real (json, "pitch", 60.0f);
    result.shift = r.real (json, "shift", 0.0f);
    result.drift = r.real (json, "drift", 1.0f);
    result.vibrato = r.real (json, "vibrato", 1.0f);
    if (! (result.start >= 0.0 && result.end > result.start))
        r.errors.add ("a tune note needs 0 <= start < end (seconds)");
    if (result.pitch < 0.0f || result.pitch > 127.0f || std::abs (result.shift) > 24.0f || result.drift < 0.0f ||
        result.drift > 1.0f || result.vibrato < 0.0f || result.vibrato > 2.0f)
        r.errors.add ("a tune note needs pitch 0..127, shift -24..24, drift 0..1 and vibrato 0..2");
    return r.errors.joinIntoString ("; ");
}

juce::Result fromJsonText (const juce::String& text, Project& result)
{
    juce::var json;
    if (auto parsed = juce::JSON::parse (text, json); parsed.failed())
        return juce::Result::fail ("project.json is not valid JSON: " + parsed.getErrorMessage());

    return fromJson (json, result);
}

} // namespace spinus::model
