/* A test CLAP effect for the plugin sandbox: stereo in, stereo out = in x Gain. Its "Crash"
   switch makes processing crash (SIGSEGV) and its "Hang" switch makes it stop answering, so tests can check
   that Spinus survives both. Gain is saved in its state. Never installed or shipped. */

#include <clap/clap.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
    #include <windows.h>
    #define sleep(seconds) Sleep ((seconds) * 1000)
    /* A real access violation: what a crashing plugin does on Windows (raise() would not reach Spinus's guard). */
    #define CRASH() (*(volatile int*) 0 = 0)
#else
    #include <unistd.h>
    #define CRASH() raise (SIGSEGV)
#endif

enum { paramGain, paramCrash, paramHang, paramCount };

typedef struct
{
    clap_plugin_t plugin;
    double values[paramCount];
} CrashTest;

static const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };

static const clap_plugin_descriptor_t descriptor = {
    CLAP_VERSION_INIT, "io.github.tuturu742.spinus.test.crash-test", "Crash Test", "Spinus tests", "", "", "", "1.0",
    "Gain, and switches that crash or hang it", features
};

static CrashTest* self (const clap_plugin_t* p) { return (CrashTest*) p->plugin_data; }

//==============================================================================
static uint32_t ports_count (const clap_plugin_t* p, bool is_input) { (void) p; (void) is_input; return 1; }

static bool ports_get (const clap_plugin_t* p, uint32_t index, bool is_input, clap_audio_port_info_t* info)
{
    (void) p;
    (void) is_input;
    if (index != 0)
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

//==============================================================================
static uint32_t params_count (const clap_plugin_t* p) { (void) p; return paramCount; }

static bool params_info (const clap_plugin_t* p, uint32_t index, clap_param_info_t* info)
{
    (void) p;
    if (index >= paramCount)
        return false;
    memset (info, 0, sizeof (*info));
    info->id = index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE | (index == paramGain ? 0 : CLAP_PARAM_IS_STEPPED);
    strcpy (info->name, index == paramGain ? "Gain" : index == paramCrash ? "Crash" : "Hang");
    info->min_value = 0.0;
    info->max_value = 1.0;
    info->default_value = index == paramGain ? 1.0 : 0.0;
    return true;
}

static bool params_value (const clap_plugin_t* p, clap_id id, double* value)
{
    if (id >= paramCount)
        return false;
    *value = self (p)->values[id];
    return true;
}

static bool params_value_to_text (const clap_plugin_t* p, clap_id id, double value, char* text, uint32_t size)
{
    (void) p;
    if (id >= paramCount)
        return false;
    if (id == paramGain)
        snprintf (text, size, "%.0f %%", value * 100.0);
    else
        snprintf (text, size, "%s", value > 0.5 ? "On" : "Off");
    return true;
}

static bool params_text_to_value (const clap_plugin_t* p, clap_id id, const char* text, double* value)
{
    (void) p;
    if (id >= paramCount)
        return false;
    *value = id == paramGain ? atof (text) / 100.0 : (strcmp (text, "On") == 0 ? 1.0 : 0.0);
    return true;
}

static void apply_events (CrashTest* t, const clap_input_events_t* events)
{
    for (uint32_t i = 0; events != NULL && i < events->size (events); ++i)
    {
        const clap_event_header_t* header = events->get (events, i);
        if (header->space_id == CLAP_CORE_EVENT_SPACE_ID && header->type == CLAP_EVENT_PARAM_VALUE)
        {
            const clap_event_param_value_t* change = (const clap_event_param_value_t*) header;
            if (change->param_id < paramCount)
                t->values[change->param_id] = change->value;
        }
    }
}

static void params_flush (const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out)
{
    (void) out;
    apply_events (self (p), in);
}

static const clap_plugin_params_t params = { params_count, params_info, params_value, params_value_to_text,
                                             params_text_to_value, params_flush };

//==============================================================================
static bool state_save (const clap_plugin_t* p, const clap_ostream_t* stream)
{
    const float gain = (float) self (p)->values[paramGain];
    return stream->write (stream, &gain, sizeof (gain)) == (int64_t) sizeof (gain);
}

static bool state_load (const clap_plugin_t* p, const clap_istream_t* stream)
{
    float gain = 1.0f;
    if (stream->read (stream, &gain, sizeof (gain)) != (int64_t) sizeof (gain))
        return false;
    self (p)->values[paramGain] = gain;
    return true;
}

static const clap_plugin_state_t state = { state_save, state_load };

//==============================================================================
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
    CrashTest* t = self (p);
    apply_events (t, process->in_events);
    if (t->values[paramCrash] > 0.5)
        CRASH();
    while (t->values[paramHang] > 0.5)
        sleep (1);

    if (process->audio_inputs_count < 1 || process->audio_outputs_count < 1)
        return CLAP_PROCESS_ERROR;
    const clap_audio_buffer_t* in = &process->audio_inputs[0];
    const clap_audio_buffer_t* out = &process->audio_outputs[0];
    const float gain = (float) t->values[paramGain];
    for (uint32_t ch = 0; ch < out->channel_count; ++ch)
        for (uint32_t i = 0; i < process->frames_count; ++i)
            out->data32[ch][i] = in->data32[ch < in->channel_count ? ch : 0][i] * gain;
    return CLAP_PROCESS_CONTINUE;
}

static const void* get_extension (const clap_plugin_t* p, const char* id)
{
    (void) p;
    if (strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0)
        return &audio_ports;
    if (strcmp (id, CLAP_EXT_PARAMS) == 0)
        return &params;
    if (strcmp (id, CLAP_EXT_STATE) == 0)
        return &state;
    return NULL;
}

//==============================================================================
static uint32_t factory_count (const clap_plugin_factory_t* f) { (void) f; return 1; }
static const clap_plugin_descriptor_t* factory_descriptor (const clap_plugin_factory_t* f, uint32_t i) { (void) f; return i == 0 ? &descriptor : NULL; }

static const clap_plugin_t* factory_create (const clap_plugin_factory_t* f, const clap_host_t* host, const char* id)
{
    (void) f;
    (void) host;
    if (strcmp (id, descriptor.id) != 0)
        return NULL;
    CrashTest* t = calloc (1, sizeof (CrashTest));
    t->plugin = (clap_plugin_t) { &descriptor, t, init, destroy, activate, deactivate, start_processing,
                                  stop_processing, reset, process, get_extension, on_main_thread };
    t->values[paramGain] = 1.0;
    return &t->plugin;
}

static const clap_plugin_factory_t factory = { factory_count, factory_descriptor, factory_create };

static bool entry_init (const char* path) { (void) path; return true; }
static void entry_deinit (void) {}
static const void* entry_get_factory (const char* id) { return strcmp (id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : NULL; }

CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory };
