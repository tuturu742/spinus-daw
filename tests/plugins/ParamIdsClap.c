/* A test CLAP effect whose parameter IDs are not their positions, as in many real plugins (OB-Xf, Surge XT
   Effects): Spinus knows a plugin's settings by position (engine/ParameterIds.h), so device.setParameters and
   automation must not use these IDs. Some IDs are other settings' positions, so mixing the two up shows.
   It passes its input through; its state is the settings' values, in list order. Never installed or shipped. */

#include <clap/clap.h>
#include <stdlib.h>
#include <string.h>

#define PARAMS 4

static const struct
{
    clap_id id;
    const char* name;
    double value;
} infos[PARAMS] = {
    { 2, "Drive", 0.1 },    /* position 0; its ID is Mix's position */
    { 1000, "Tone", 0.2 },  /* position 1 */
    { 0, "Mix", 0.3 },      /* position 2; its ID is Drive's position */
    { 77, "Level", 0.4 },   /* position 3 */
};

static const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };

static const clap_plugin_descriptor_t descriptor = {
    CLAP_VERSION_INIT, "io.github.tuturu742.spinus.test.param-ids", "Param IDs", "Spinus tests", "", "", "", "1.0",
    "Parameter IDs that are not their positions", features
};

typedef struct
{
    clap_plugin_t plugin;
    double values[PARAMS];
} effect_t;

static effect_t* self (const clap_plugin_t* p) { return (effect_t*) p->plugin_data; }

static int index_of (clap_id id)
{
    for (int i = 0; i < PARAMS; ++i)
        if (infos[i].id == id)
            return i;
    return -1;
}

static uint32_t params_count (const clap_plugin_t* p) { (void) p; return PARAMS; }
static bool params_info (const clap_plugin_t* p, uint32_t index, clap_param_info_t* info)
{
    (void) p;
    if (index >= PARAMS)
        return false;
    memset (info, 0, sizeof (*info));
    info->id = infos[index].id;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    strncpy (info->name, infos[index].name, sizeof (info->name) - 1);
    info->min_value = 0.0;
    info->max_value = 1.0;
    info->default_value = infos[index].value;
    return true;
}
static bool params_value (const clap_plugin_t* p, clap_id id, double* value)
{
    const int i = index_of (id);
    if (i < 0)
        return false;
    *value = self (p)->values[i];
    return true;
}
static bool params_to_text (const clap_plugin_t* p, clap_id id, double value, char* text, uint32_t size)
{
    (void) p;
    (void) id;
    (void) value;
    if (size > 0)
        text[0] = 0;
    return false;
}
static bool params_from_text (const clap_plugin_t* p, clap_id id, const char* text, double* value)
{
    (void) p;
    (void) id;
    (void) text;
    (void) value;
    return false;
}
static void apply_events (effect_t* s, const clap_input_events_t* in)
{
    for (uint32_t k = 0; k < in->size (in); ++k)
    {
        const clap_event_header_t* e = in->get (in, k);
        if (e->space_id == CLAP_CORE_EVENT_SPACE_ID && e->type == CLAP_EVENT_PARAM_VALUE)
        {
            const clap_event_param_value_t* v = (const clap_event_param_value_t*) e;
            const int i = index_of (v->param_id);
            if (i >= 0)
                s->values[i] = v->value;
        }
    }
}
static void params_flush (const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out)
{
    (void) out;
    apply_events (self (p), in);
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

static bool state_save (const clap_plugin_t* p, const clap_ostream_t* stream)
{
    const int64_t size = (int64_t) sizeof (self (p)->values);
    return stream->write (stream, self (p)->values, (uint64_t) size) == size;
}
static bool state_load (const clap_plugin_t* p, const clap_istream_t* stream)
{
    double values[PARAMS];
    const int64_t size = (int64_t) sizeof (values);
    if (stream->read (stream, values, (uint64_t) size) != size)
        return false;
    memcpy (self (p)->values, values, sizeof (values));
    return true;
}
static const clap_plugin_state_t state = { state_save, state_load };

static bool init (const clap_plugin_t* p) { (void) p; return true; }
static void destroy (const clap_plugin_t* p) { free (self (p)); }
static bool activate (const clap_plugin_t* p, double sr, uint32_t min, uint32_t max) { (void) p; (void) sr; (void) min; (void) max; return true; }
static void deactivate (const clap_plugin_t* p) { (void) p; }
static bool start_processing (const clap_plugin_t* p) { (void) p; return true; }
static void stop_processing (const clap_plugin_t* p) { (void) p; }
static void reset (const clap_plugin_t* p) { (void) p; }
static void on_main_thread (const clap_plugin_t* p) { (void) p; }

static clap_process_status process (const clap_plugin_t* p, const clap_process_t* process)
{
    apply_events (self (p), process->in_events);
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
    (void) host;
    if (strcmp (id, descriptor.id) != 0)
        return NULL;
    effect_t* s = (effect_t*) calloc (1, sizeof (effect_t));
    for (int i = 0; i < PARAMS; ++i)
        s->values[i] = infos[i].value;
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
