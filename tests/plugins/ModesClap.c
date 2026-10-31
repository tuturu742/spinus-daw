/* A test CLAP effect whose type picks what its other settings are, as some real multi-effects do: setting 3,
   "Type" (Echo, Wobble, Crunch; not marked as stepped, shown by name), takes effect only when the plugin next
   processes audio. Only then are settings 0 to 2 renamed and described for that type ("Time", "Feedback",
   "Mix" for Echo; "Rate", "Depth", "Mix" for Wobble; "Drive", "Tone", "Mix" for Crunch), and put back to that
   type's defaults. Before it is first activated they are "Param 0".. showing "-". Its state has the type it
   plays (not one set but not taken in yet) and the three settings. It passes its input through. Never
   installed or shipped. */

#include <clap/clap.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PARAMS 4
#define TYPE 3
#define TYPES 3

static const char* const typeNames[TYPES] = { "Echo", "Wobble", "Crunch" };
static const char* const names[TYPES][3] = { { "Time", "Feedback", "Mix" },
                                             { "Rate", "Depth", "Mix" },
                                             { "Drive", "Tone", "Mix" } };
static const double defaults[TYPES][3] = { { 0.25, 0.4, 0.3 }, { 0.1, 0.5, 0.5 }, { 0.2, 0.5, 1.0 } };

static const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };

static const clap_plugin_descriptor_t descriptor = {
    CLAP_VERSION_INIT, "io.github.tuturu742.spinus.test.modes", "Modes", "Spinus tests", "", "", "", "1.0",
    "An effect whose type picks its other settings", features
};

typedef struct
{
    clap_plugin_t plugin;
    const clap_host_t* host;
    double values[PARAMS]; /* as set; values[TYPE] 0 to 1 */
    int playing;           /* the type it plays */
    int named;             /* activated once: its settings have names */
    int rescan;            /* the names changed: tell the host on the main thread */
} effect_t;

static effect_t* self (const clap_plugin_t* p) { return (effect_t*) p->plugin_data; }

static int type_of (double value)
{
    const int t = (int) floor (value * TYPES);
    return t < 0 ? 0 : (t >= TYPES ? TYPES - 1 : t);
}

static uint32_t params_count (const clap_plugin_t* p) { (void) p; return PARAMS; }
static bool params_info (const clap_plugin_t* p, uint32_t index, clap_param_info_t* info)
{
    const effect_t* s = self (p);
    if (index >= PARAMS)
        return false;
    memset (info, 0, sizeof (*info));
    info->id = index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (index == TYPE)
        strcpy (info->name, "Type");
    else if (s->named)
        strcpy (info->name, names[s->playing][index]);
    else
        snprintf (info->name, sizeof (info->name), "Param %u", index);
    info->min_value = 0.0;
    info->max_value = 1.0;
    info->default_value = index == TYPE ? 0.0 : defaults[0][index];
    return true;
}
static bool params_value (const clap_plugin_t* p, clap_id id, double* value)
{
    if (id >= PARAMS)
        return false;
    *value = self (p)->values[id];
    return true;
}
static bool params_to_text (const clap_plugin_t* p, clap_id id, double value, char* text, uint32_t size)
{
    const effect_t* s = self (p);
    if (size == 0 || id >= PARAMS)
        return false;
    if (id == TYPE)
    {
        snprintf (text, size, "%s", typeNames[type_of (value)]);
        return true;
    }
    if (! s->named)
    {
        snprintf (text, size, "-");
        return true;
    }
    if (id == 2)
        snprintf (text, size, "%.0f %%", value * 100.0);
    else if (s->playing == 0)
        snprintf (text, size, id == 0 ? "%.0f ms" : "%.0f %%", value * (id == 0 ? 1000.0 : 100.0));
    else if (s->playing == 1)
        snprintf (text, size, id == 0 ? "%.2f Hz" : "%.0f %%", value * (id == 0 ? 10.0 : 100.0));
    else
        snprintf (text, size, id == 0 ? "%.1f dB" : "%.0f Hz", id == 0 ? value * 24.0 : 100.0 + value * 9900.0);
    return true;
}
static bool params_from_text (const clap_plugin_t* p, clap_id id, const char* text, double* value)
{
    (void) p;
    (void) id;
    (void) text;
    (void) value;
    return false; /* reads no text back: Spinus finds a value by what it shows */
}
static void apply_events (effect_t* s, const clap_input_events_t* in)
{
    for (uint32_t k = 0; k < in->size (in); ++k)
    {
        const clap_event_header_t* e = in->get (in, k);
        if (e->space_id == CLAP_CORE_EVENT_SPACE_ID && e->type == CLAP_EVENT_PARAM_VALUE)
        {
            const clap_event_param_value_t* v = (const clap_event_param_value_t*) e;
            if (v->param_id < PARAMS)
                s->values[v->param_id] = v->value;
        }
    }
}
static void params_flush (const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out)
{
    (void) out;
    apply_events (self (p), in); /* only stored: the type is taken in when it processes */
}
static const clap_plugin_params_t params = { params_count,   params_info,      params_value,
                                             params_to_text, params_from_text, params_flush };

static uint32_t ports_count (const clap_plugin_t* p, bool is_input) { (void) p; (void) is_input; return 1; }
static bool ports_get (const clap_plugin_t* p, uint32_t index, bool is_input, clap_audio_port_info_t* info)
{
    (void) p;
    (void) is_input;
    if (index > 0)
        return false;
    memset (info, 0, sizeof (*info));
    strcpy (info->name, "Main");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
static const clap_plugin_audio_ports_t audio_ports = { ports_count, ports_get };

/* The type it plays, then settings 0 to 2. */
static bool state_save (const clap_plugin_t* p, const clap_ostream_t* stream)
{
    const effect_t* s = self (p);
    double data[PARAMS] = { (double) s->playing, s->values[0], s->values[1], s->values[2] };
    const int64_t size = (int64_t) sizeof (data);
    return stream->write (stream, data, (uint64_t) size) == size;
}
static bool state_load (const clap_plugin_t* p, const clap_istream_t* stream)
{
    effect_t* s = self (p);
    double data[PARAMS];
    const int64_t size = (int64_t) sizeof (data);
    if (stream->read (stream, data, (uint64_t) size) != size)
        return false;
    s->playing = type_of ((data[0] + 0.5) / TYPES);
    s->values[TYPE] = (s->playing + 0.5) / TYPES;
    for (int i = 0; i < 3; ++i)
        s->values[i] = data[i + 1];
    s->named = 1;
    return true;
}
static const clap_plugin_state_t state = { state_save, state_load };

static bool init (const clap_plugin_t* p) { (void) p; return true; }
static void destroy (const clap_plugin_t* p) { free (self (p)); }
static bool activate (const clap_plugin_t* p, double sr, uint32_t min, uint32_t max)
{
    (void) sr;
    (void) min;
    (void) max;
    effect_t* s = self (p);
    if (! s->named)
    {
        s->named = 1;
        s->rescan = 1;
        s->host->request_callback (s->host);
    }
    return true;
}
static void deactivate (const clap_plugin_t* p) { (void) p; }
static bool start_processing (const clap_plugin_t* p) { (void) p; return true; }
static void stop_processing (const clap_plugin_t* p) { (void) p; }
static void reset (const clap_plugin_t* p) { (void) p; }
static void on_main_thread (const clap_plugin_t* p)
{
    effect_t* s = self (p);
    if (! s->rescan)
        return;
    s->rescan = 0;
    const clap_host_params_t* hp = (const clap_host_params_t*) s->host->get_extension (s->host, CLAP_EXT_PARAMS);
    if (hp != NULL)
        hp->rescan (s->host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT | CLAP_PARAM_RESCAN_INFO);
}

static clap_process_status process (const clap_plugin_t* p, const clap_process_t* process)
{
    effect_t* s = self (p);
    apply_events (s, process->in_events);
    const int wanted = type_of (s->values[TYPE]);
    if (wanted != s->playing)
    {
        /* A new type: its own settings, at its defaults. */
        s->playing = wanted;
        for (int i = 0; i < 3; ++i)
            s->values[i] = defaults[wanted][i];
        s->rescan = 1;
        s->host->request_callback (s->host);
    }
    const clap_audio_buffer_t* in = &process->audio_inputs[0];
    const clap_audio_buffer_t* out = &process->audio_outputs[0];
    for (uint32_t c = 0; c < out->channel_count && c < in->channel_count; ++c)
        if (out->data32[c] != in->data32[c])
            memcpy (out->data32[c], in->data32[c], sizeof (float) * process->frames_count);
    return CLAP_PROCESS_CONTINUE;
}

static const void* get_extension (const clap_plugin_t* p, const char* id)
{
    (void) p;
    if (strcmp (id, CLAP_EXT_PARAMS) == 0)
        return &params;
    if (strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0)
        return &audio_ports;
    if (strcmp (id, CLAP_EXT_STATE) == 0)
        return &state;
    return NULL;
}

static uint32_t factory_count (const clap_plugin_factory_t* f) { (void) f; return 1; }
static const clap_plugin_descriptor_t* factory_descriptor (const clap_plugin_factory_t* f, uint32_t i)
{
    (void) f;
    return i == 0 ? &descriptor : NULL;
}
static const clap_plugin_t* factory_create (const clap_plugin_factory_t* f, const clap_host_t* host, const char* id)
{
    (void) f;
    if (strcmp (id, descriptor.id) != 0)
        return NULL;
    effect_t* s = (effect_t*) calloc (1, sizeof (effect_t));
    s->host = host;
    for (int i = 0; i < 3; ++i)
        s->values[i] = defaults[0][i];
    s->plugin.desc = &descriptor;
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

static bool entry_init (const char* path) { (void) path; return true; }
static void entry_deinit (void) {}
static const void* entry_get_factory (const char* id) { return strcmp (id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : NULL; }

CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory };
