/* Two small test CLAP instruments for synth match. Never installed or shipped.

   "Spinus Test Synth": one voice, a saw, square or sine through a resonant low pass, with an ADSR. Its
   parameters have the kinds of names real synths use, so an automatic profile finds Wave, Cutoff,
   Resonance, Attack, Decay, Sustain and Release, and leaves out Master Volume, Bypass, Voices, LFO Rate and
   Delay Mix.

   "Spinus Test Organ": a sine with only Volume, Bypass and Transpose: nothing to tune.

   The synth has six presets of its own, listed through CLAP preset discovery (inside the plugin, by load
   key) and loaded through preset-load, for closest preset; the organ has none.

   Built with MISBEHAVING=1 it is "Spinus Misbehaving Synth" (misbehaving-synth.clap), for the sound index's
   tests: two more presets, "Crashes" (its sound crashes the process) and "Hangs" (its sound never comes:
   processing stops answering).

   Built with DIFFICULT=1 it is "Spinus Difficult Synth" (difficult-synth.clap), behaving as some big sample
   instruments do: it crashes when its presets are listed, and stays silent for its first 3 seconds (as if
   loading its samples in the background). */

#include <clap/clap.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if MISBEHAVING || DIFFICULT
    #ifdef _WIN32
        #include <windows.h>
        #define CRASH() (*(volatile int*)0 = 0)
        #define PAUSE() Sleep (1000)
        #define NOW() ((double)GetTickCount64() / 1000.0)
    #else
        #include <signal.h>
        #include <time.h>
        #include <unistd.h>
        #define CRASH() raise (SIGSEGV)
        #define PAUSE() sleep (1)
static double now_seconds (void)
{
    struct timespec t;
    clock_gettime (CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
        #define NOW() now_seconds()
    #endif
#endif

#define PI 3.14159265358979323846

enum
{
    P_WAVE,
    P_CUTOFF,
    P_RESONANCE,
    P_ATTACK,
    P_DECAY,
    P_SUSTAIN,
    P_RELEASE,
    P_MASTER,
    P_BYPASS,
    P_VOICES,
    P_LFO_RATE,
    P_DELAY_MIX,
    SYNTH_PARAMS
};

enum
{
    O_VOLUME,
    O_BYPASS,
    O_TRANSPOSE,
    ORGAN_PARAMS
};

typedef struct
{
    const char* name;
    double min, max, value;
    int stepped;
} param_t;

static const param_t synth_params[SYNTH_PARAMS] = {
    { "Wave", 0, 2, 0, 1 },      { "Cutoff", 0, 1, 0.8, 0 },        { "Resonance", 0, 1, 0.1, 0 },
    { "Attack", 0, 1, 0.0, 0 },  { "Decay", 0, 1, 0.3, 0 },         { "Sustain", 0, 1, 0.8, 0 },
    { "Release", 0, 1, 0.1, 0 }, { "Master Volume", 0, 1, 0.8, 0 }, { "Bypass", 0, 1, 0, 1 },
    { "Voices", 1, 8, 1, 1 },    { "LFO Rate", 0, 1, 0.3, 0 },      { "Delay Mix", 0, 1, 0, 0 },
};

static const param_t organ_params[ORGAN_PARAMS] = {
    { "Volume", 0, 1, 0.8, 0 },
    { "Bypass", 0, 1, 0, 1 },
    { "Transpose", -12, 12, 0, 1 },
};

/* The synth's presets: Wave, Cutoff, Resonance, Attack, Decay, Sustain, Release (the rest as by default). */
#if MISBEHAVING
    #define PRESETS 8
#else
    #define PRESETS 6
#endif
static const struct
{
    const char* name;
    double values[7];
} presets[PRESETS] = {
    { "Bright Saw", { 0, 1.0, 0.1, 0.0, 0.3, 0.8, 0.1 } },     { "Dark Square", { 1, 0.3, 0.2, 0.0, 0.3, 0.8, 0.1 } },
    { "Soft Sine Pad", { 2, 0.8, 0.0, 0.6, 0.5, 0.9, 0.5 } },  { "Square Pluck", { 1, 0.7, 0.3, 0.0, 0.15, 0.0, 0.1 } },
    { "Resonant Saw", { 0, 0.45, 0.85, 0.0, 0.4, 0.7, 0.1 } }, { "Sine Pluck", { 2, 0.5, 0.0, 0.0, 0.1, 0.0, 0.05 } },
#if MISBEHAVING
    { "Crashes", { 0, 1.0, 0.1, 0.0, 0.3, 0.8, 0.1 } },        { "Hangs", { 0, 1.0, 0.1, 0.0, 0.3, 0.8, 0.1 } },
#endif
};

static const char* const features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, NULL };

#if MISBEHAVING
    #define SYNTH_ID "io.github.tuturu742.spinus.test.misbehaving-synth"
    #define SYNTH_NAME "Spinus Misbehaving Synth"
#elif DIFFICULT
    #define SYNTH_ID "io.github.tuturu742.spinus.test.difficult-synth"
    #define SYNTH_NAME "Spinus Difficult Synth"
#else
    #define SYNTH_ID "io.github.tuturu742.spinus.test.synth"
    #define SYNTH_NAME "Spinus Test Synth"
#endif

static const clap_plugin_descriptor_t synth_descriptor = { CLAP_VERSION_INIT,
                                                           SYNTH_ID,
                                                           SYNTH_NAME,
                                                           "Spinus tests",
                                                           "",
                                                           "",
                                                           "",
                                                           "1.0",
                                                           "A tiny synth with named settings",
                                                           features };
static const clap_plugin_descriptor_t organ_descriptor = { CLAP_VERSION_INIT,
                                                           "io.github.tuturu742.spinus.test.organ",
                                                           "Spinus Test Organ",
                                                           "Spinus tests",
                                                           "",
                                                           "",
                                                           "",
                                                           "1.0",
                                                           "A sine with nothing to tune",
                                                           features };

typedef struct
{
    clap_plugin_t plugin;
    int organ;
    int count;
    const param_t* infos;
    double values[SYNTH_PARAMS];
    double rate, phase, low, band, level;
    int note, gate; /* the note playing (-1: none), whether its key is down */
    int stage;      /* 0 attack, 1 decay, 2 sustain, 3 release */
    int misbehave;  /* MISBEHAVING: 1 crash, 2 hang while processing */
    double created; /* DIFFICULT: when (NOW()), to stay silent for 3 seconds after */
} synth_t;

static synth_t* self (const clap_plugin_t* p) { return (synth_t*)p->plugin_data; }

/* Parameters */
static uint32_t params_count (const clap_plugin_t* p) { return (uint32_t)self (p)->count; }
static bool params_info (const clap_plugin_t* p, uint32_t index, clap_param_info_t* info)
{
    synth_t* s = self (p);
    if ((int)index >= s->count)
        return false;
    memset (info, 0, sizeof (*info));
    info->id = index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE | (s->infos[index].stepped ? CLAP_PARAM_IS_STEPPED : 0);
    strncpy (info->name, s->infos[index].name, sizeof (info->name) - 1);
    info->min_value = s->infos[index].min;
    info->max_value = s->infos[index].max;
    info->default_value = s->infos[index].value;
    return true;
}
static bool params_value (const clap_plugin_t* p, clap_id id, double* value)
{
    synth_t* s = self (p);
    if ((int)id >= s->count)
        return false;
    *value = s->values[id];
    return true;
}
static bool params_to_text (const clap_plugin_t* p, clap_id id, double value, char* text, uint32_t size)
{
    (void)p;
    (void)id;
    snprintf (text, size, "%g", value);
    return true;
}
static bool params_from_text (const clap_plugin_t* p, clap_id id, const char* text, double* value)
{
    (void)p;
    (void)id;
    *value = atof (text);
    return true;
}
static void apply_event (synth_t* s, const clap_event_header_t* e)
{
    if (e->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;
    if (e->type == CLAP_EVENT_PARAM_VALUE)
    {
        const clap_event_param_value_t* v = (const clap_event_param_value_t*)e;
        if ((int)v->param_id < s->count)
            s->values[v->param_id] = v->value;
    }
    else if (e->type == CLAP_EVENT_NOTE_ON)
    {
        const clap_event_note_t* n = (const clap_event_note_t*)e;
        s->note = n->key;
        s->gate = 1;
        s->stage = 0;
    }
    else if (e->type == CLAP_EVENT_NOTE_OFF)
    {
        const clap_event_note_t* n = (const clap_event_note_t*)e;
        if (n->key == s->note || n->key < 0)
        {
            s->gate = 0;
            s->stage = 3;
        }
    }
    else if (e->type == CLAP_EVENT_NOTE_CHOKE)
    {
        s->note = -1;
        s->gate = 0;
        s->level = 0;
        s->low = s->band = 0;
    }
    else if (e->type == CLAP_EVENT_MIDI)
    {
        const clap_event_midi_t* m = (const clap_event_midi_t*)e;
        const int status = m->data[0] & 0xf0;
        if (status == 0x90 && m->data[2] > 0)
        {
            s->note = m->data[1];
            s->gate = 1;
            s->stage = 0;
        }
        else if ((status == 0x80 || (status == 0x90 && m->data[2] == 0)) && m->data[1] == s->note)
        {
            s->gate = 0;
            s->stage = 3;
        }
        else if (status == 0xb0 && (m->data[1] == 120 || m->data[1] == 123))
        {
            s->gate = 0;
            s->stage = 3;
            if (m->data[1] == 120)
            {
                s->note = -1;
                s->level = 0;
                s->low = s->band = 0;
            }
        }
    }
}
static void params_flush (const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out)
{
    (void)out;
    for (uint32_t i = 0; i < in->size (in); ++i)
        apply_event (self (p), in->get (in, i));
}
static const clap_plugin_params_t params = { params_count,   params_info,      params_value,
                                             params_to_text, params_from_text, params_flush };

/* Ports */
static uint32_t audio_count (const clap_plugin_t* p, bool is_input)
{
    (void)p;
    return is_input ? 0 : 1;
}
static bool audio_get (const clap_plugin_t* p, uint32_t index, bool is_input, clap_audio_port_info_t* info)
{
    (void)p;
    if (is_input || index > 0)
        return false;
    memset (info, 0, sizeof (*info));
    strcpy (info->name, "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
static const clap_plugin_audio_ports_t audio_ports = { audio_count, audio_get };

static uint32_t note_count (const clap_plugin_t* p, bool is_input)
{
    (void)p;
    return is_input ? 1 : 0;
}
static bool note_get (const clap_plugin_t* p, uint32_t index, bool is_input, clap_note_port_info_t* info)
{
    (void)p;
    if (! is_input || index > 0)
        return false;
    memset (info, 0, sizeof (*info));
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    strcpy (info->name, "Notes");
    return true;
}
static const clap_plugin_note_ports_t note_ports = { note_count, note_get };

/* State: the parameter values. */
static bool state_save (const clap_plugin_t* p, const clap_ostream_t* stream)
{
    synth_t* s = self (p);
    const int64_t size = (int64_t)(sizeof (double) * (size_t)s->count);
    return stream->write (stream, s->values, (uint64_t)size) == size;
}
static bool state_load (const clap_plugin_t* p, const clap_istream_t* stream)
{
    synth_t* s = self (p);
    double values[SYNTH_PARAMS];
    const int64_t size = (int64_t)(sizeof (double) * (size_t)s->count);
    if (stream->read (stream, values, (uint64_t)size) != size)
        return false;
    memcpy (s->values, values, (size_t)size);
    return true;
}
static const clap_plugin_state_t state = { state_save, state_load };

/* Presets: one of the synth's own, by its load key ("0" to "5"). */
static bool preset_from_location (const clap_plugin_t* p, uint32_t kind, const char* location, const char* key)
{
    synth_t* s = self (p);
    (void)location;
    if (s->organ || kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || key == NULL)
        return false;
    const int index = atoi (key);
    if (index < 0 || index >= PRESETS)
        return false;
    for (int i = 0; i < 7; ++i)
        s->values[i] = presets[index].values[i];
    for (int i = 7; i < s->count; ++i)
        s->values[i] = s->infos[i].value;
    s->misbehave = index >= 6 ? index - 5 : 0;
    return true;
}
static const clap_plugin_preset_load_t preset_load = { preset_from_location };

/* The plugin */
static bool init (const clap_plugin_t* p)
{
    (void)p;
    return true;
}
static void destroy (const clap_plugin_t* p) { free (self (p)); }
static bool activate (const clap_plugin_t* p, double sr, uint32_t min, uint32_t max)
{
    (void)min;
    (void)max;
    self (p)->rate = sr;
    return true;
}
static void deactivate (const clap_plugin_t* p) { (void)p; }
static bool start_processing (const clap_plugin_t* p)
{
    (void)p;
    return true;
}
static void stop_processing (const clap_plugin_t* p) { (void)p; }
static void reset (const clap_plugin_t* p)
{
    synth_t* s = self (p);
    s->note = -1;
    s->level = s->low = s->band = 0;
}
static void on_main_thread (const clap_plugin_t* p) { (void)p; }

static double seconds (double v) { return 0.001 + 2.0 * v * v; } /* 1 ms to 2 s */

static float next_sample (synth_t* s)
{
    if (s->note < 0)
        return 0.0f;
    const double* v = s->values;
    const double frequency = 440.0 * pow (2.0, (s->note - 69) / 12.0);
    s->phase += frequency / s->rate;
    s->phase -= floor (s->phase);
    double x;
    if (s->organ)
        x = sin (2.0 * PI * s->phase);
    else
    {
        const int wave = (int)(v[P_WAVE] + 0.5);
        x = wave == 0 ? 2.0 * s->phase - 1.0 : wave == 1 ? (s->phase < 0.5 ? 1.0 : -1.0) : sin (2.0 * PI * s->phase);
    }

    double attack = 0.005, decay = 0.3, sustain = 1.0, release = 0.05;
    if (! s->organ)
    {
        attack = seconds (v[P_ATTACK]);
        decay = seconds (v[P_DECAY]);
        sustain = v[P_SUSTAIN];
        release = seconds (v[P_RELEASE]);
    }
    if (s->stage == 0)
    {
        s->level += 1.0 / (attack * s->rate);
        if (s->level >= 1.0)
        {
            s->level = 1.0;
            s->stage = 1;
        }
    }
    else if (s->stage == 1)
    {
        s->level -= (1.0 - sustain) / (decay * s->rate);
        if (s->level <= sustain)
        {
            s->level = sustain;
            s->stage = 2;
        }
    }
    else if (s->stage == 3)
    {
        s->level -= 1.0 / (release * s->rate);
        if (s->level <= 0.0)
        {
            s->level = 0.0;
            s->note = -1;
        }
    }

    if (! s->organ)
    {
        /* A state-variable low pass, 30 Hz to 15 kHz. */
        const double cutoff = 30.0 * pow (500.0, v[P_CUTOFF]);
        const double f = 2.0 * sin (PI * fmin (cutoff, s->rate / 6.0) / s->rate);
        const double q = 2.0 - 1.9 * v[P_RESONANCE];
        s->low += f * s->band;
        const double high = x - s->low - q * s->band;
        s->band += f * high;
        x = s->low;
        return (float)(0.3 * x * s->level * v[P_MASTER]);
    }
    return (float)(0.3 * x * s->level * v[O_VOLUME]);
}

static clap_process_status process (const clap_plugin_t* p, const clap_process_t* process)
{
    synth_t* s = self (p);
#if MISBEHAVING
    if (s->misbehave == 1)
        CRASH();
    while (s->misbehave == 2)
        PAUSE();
#endif
#if DIFFICULT
    if (NOW() - s->created < 3.0)
    {
        /* Still "loading its samples": silence. */
        const clap_audio_buffer_t* silent = &process->audio_outputs[0];
        for (uint32_t c = 0; c < silent->channel_count; ++c)
            memset (silent->data32[c], 0, sizeof (float) * process->frames_count);
        return CLAP_PROCESS_CONTINUE;
    }
#endif
    const uint32_t frames = process->frames_count;
    const uint32_t events = process->in_events->size (process->in_events);
    uint32_t next = 0;
    const clap_audio_buffer_t* out = &process->audio_outputs[0];
    for (uint32_t i = 0; i < frames; ++i)
    {
        while (next < events)
        {
            const clap_event_header_t* e = process->in_events->get (process->in_events, next);
            if (e->time > i)
                break;
            apply_event (s, e);
            ++next;
        }
        const float x = next_sample (s);
        for (uint32_t c = 0; c < out->channel_count; ++c)
            out->data32[c][i] = x;
    }
    while (next < events)
        apply_event (s, process->in_events->get (process->in_events, next++));
    return CLAP_PROCESS_CONTINUE;
}

static const void* get_extension (const clap_plugin_t* p, const char* id)
{
    if (strcmp (id, CLAP_EXT_PRESET_LOAD) == 0)
        return self (p)->organ ? NULL : &preset_load;
    if (strcmp (id, CLAP_EXT_PARAMS) == 0)
        return &params;
    if (strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0)
        return &audio_ports;
    if (strcmp (id, CLAP_EXT_NOTE_PORTS) == 0)
        return &note_ports;
    if (strcmp (id, CLAP_EXT_STATE) == 0)
        return &state;
    return NULL;
}

static uint32_t factory_count (const clap_plugin_factory_t* f)
{
    (void)f;
    return 2;
}
static const clap_plugin_descriptor_t* factory_descriptor (const clap_plugin_factory_t* f, uint32_t i)
{
    (void)f;
    return i == 0 ? &synth_descriptor : i == 1 ? &organ_descriptor : NULL;
}
static const clap_plugin_t* factory_create (const clap_plugin_factory_t* f, const clap_host_t* host, const char* id)
{
    (void)f;
    (void)host;
    const int organ = strcmp (id, organ_descriptor.id) == 0;
    if (! organ && strcmp (id, synth_descriptor.id) != 0)
        return NULL;
    synth_t* s = (synth_t*)calloc (1, sizeof (synth_t));
    s->organ = organ;
    s->count = organ ? ORGAN_PARAMS : SYNTH_PARAMS;
    s->infos = organ ? organ_params : synth_params;
    for (int i = 0; i < s->count; ++i)
        s->values[i] = s->infos[i].value;
    s->rate = 48000.0;
    s->note = -1;
#if DIFFICULT
    s->created = NOW();
#endif
    s->plugin.desc = organ ? &organ_descriptor : &synth_descriptor;
    s->plugin.plugin_data = s;
    s->plugin.init = init;
    s->plugin.destroy = destroy;
    s->plugin.activate = activate;
    s->plugin.deactivate = deactivate;
    s->plugin.start_processing = start_processing;
    s->plugin.stop_processing = stop_processing;
    s->plugin.reset = reset;
    s->plugin.process = process;
    s->plugin.get_extension = get_extension;
    s->plugin.on_main_thread = on_main_thread;
    return &s->plugin;
}

static const clap_plugin_factory_t factory = { factory_count, factory_descriptor, factory_create };

/* Preset discovery: the synth's presets, inside the plugin. */
static const clap_preset_discovery_provider_descriptor_t provider_descriptor = {
    CLAP_VERSION_INIT, "io.github.tuturu742.spinus.test.presets", "Spinus Test Synth presets", "Spinus tests"
};
static const clap_preset_discovery_indexer_t* provider_indexer = NULL;
static bool provider_init (const clap_preset_discovery_provider_t* provider)
{
    (void)provider;
    const clap_preset_discovery_location_t location = { CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, "Factory",
                                                        CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, NULL };
    return provider_indexer->declare_location (provider_indexer, &location);
}
static void provider_destroy (const clap_preset_discovery_provider_t* provider) { (void)provider; }
static bool provider_metadata (const clap_preset_discovery_provider_t* provider, uint32_t kind, const char* location,
                               const clap_preset_discovery_metadata_receiver_t* receiver)
{
    (void)provider;
    (void)location;
    if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)
        return false;
#if DIFFICULT
    CRASH(); /* listing its presets crashes it */
#endif
    const clap_universal_plugin_id_t id = { "clap", SYNTH_ID };
    for (int i = 0; i < PRESETS; ++i)
    {
        char key[8];
        snprintf (key, sizeof (key), "%d", i);
        if (! receiver->begin_preset (receiver, presets[i].name, key))
            break;
        receiver->add_plugin_id (receiver, &id);
        receiver->set_flags (receiver, CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT);
    }
    return true;
}
static const void* provider_extension (const clap_preset_discovery_provider_t* provider, const char* id)
{
    (void)provider;
    (void)id;
    return NULL;
}
static const clap_preset_discovery_provider_t provider = { &provider_descriptor, NULL,
                                                           provider_init,        provider_destroy,
                                                           provider_metadata,    provider_extension };

static uint32_t discovery_count (const clap_preset_discovery_factory_t* f)
{
    (void)f;
    return 1;
}
static const clap_preset_discovery_provider_descriptor_t*
discovery_descriptor (const clap_preset_discovery_factory_t* f, uint32_t index)
{
    (void)f;
    return index == 0 ? &provider_descriptor : NULL;
}
static const clap_preset_discovery_provider_t* discovery_create (const clap_preset_discovery_factory_t* f,
                                                                 const clap_preset_discovery_indexer_t* indexer,
                                                                 const char* id)
{
    (void)f;
    if (strcmp (id, provider_descriptor.id) != 0)
        return NULL;
    provider_indexer = indexer;
    return &provider;
}
static const clap_preset_discovery_factory_t discovery = { discovery_count, discovery_descriptor, discovery_create };

static bool entry_init (const char* path)
{
    (void)path;
    return true;
}
static void entry_deinit (void) {}
static const void* entry_get_factory (const char* id)
{
    if (strcmp (id, CLAP_PRESET_DISCOVERY_FACTORY_ID) == 0)
        return &discovery;
    return strcmp (id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory };
