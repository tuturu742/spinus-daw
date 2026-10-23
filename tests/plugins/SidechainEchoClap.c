/* A test CLAP effect with a stereo main input, a stereo sidechain input and a stereo output. It ignores
   the main input and copies the sidechain to the output, so tests can check that sidechain audio arrives.
   Like strict plugins (Odin 2), it refuses to start processing unless the host says start_processing
   runs on the audio thread. Never installed or shipped. */

#include <clap/clap.h>
#include <string.h>

static const clap_host_t* the_host = NULL;
static int started = 0; /* instances processing; the plugin object is shared */

static const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };

static const clap_plugin_descriptor_t descriptor = {
    CLAP_VERSION_INIT, "io.github.tuturu742.spinus.test.sidechain-echo", "Sidechain Echo", "Spinus tests", "", "", "", "1.0",
    "Outputs its sidechain input", features
};

static uint32_t ports_count (const clap_plugin_t* p, bool is_input)
{
    (void) p;
    return is_input ? 2 : 1;
}

static bool ports_get (const clap_plugin_t* p, uint32_t index, bool is_input, clap_audio_port_info_t* info)
{
    (void) p;
    if (index >= ports_count (p, is_input))
        return false;
    memset (info, 0, sizeof (*info));
    info->id = index;
    strcpy (info->name, index == 0 ? "Main" : "Sidechain");
    info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t audio_ports = { ports_count, ports_get };

static bool init (const clap_plugin_t* p) { (void) p; return true; }
static void destroy (const clap_plugin_t* p) { (void) p; }
static bool activate (const clap_plugin_t* p, double sr, uint32_t min, uint32_t max) { (void) p; (void) sr; (void) min; (void) max; return true; }
static void deactivate (const clap_plugin_t* p) { (void) p; }
static bool start_processing (const clap_plugin_t* p)
{
    (void) p;
    const clap_host_thread_check_t* check =
        the_host != NULL ? (const clap_host_thread_check_t*) the_host->get_extension (the_host, CLAP_EXT_THREAD_CHECK) : NULL;
    if (check != NULL && ! check->is_audio_thread (the_host))
        return false;
    ++started;
    return true;
}
static void stop_processing (const clap_plugin_t* p) { (void) p; --started; }
static void reset (const clap_plugin_t* p) { (void) p; }
static void on_main_thread (const clap_plugin_t* p) { (void) p; }

static clap_process_status process (const clap_plugin_t* p, const clap_process_t* process)
{
    (void) p;
    if (process->audio_inputs_count < 2 || process->audio_outputs_count < 1)
        return CLAP_PROCESS_ERROR;

    const clap_audio_buffer_t* side = &process->audio_inputs[1];
    const clap_audio_buffer_t* out = &process->audio_outputs[0];
    if (started <= 0) // silence, so a host that skipped start_processing fails the sidechain tests
    {
        for (uint32_t ch = 0; ch < out->channel_count; ++ch)
            memset (out->data32[ch], 0, process->frames_count * sizeof (float));
        return CLAP_PROCESS_CONTINUE;
    }
    for (uint32_t ch = 0; ch < out->channel_count; ++ch)
        memcpy (out->data32[ch], side->data32[ch < side->channel_count ? ch : 0], process->frames_count * sizeof (float));
    return CLAP_PROCESS_CONTINUE;
}

static const void* get_extension (const clap_plugin_t* p, const char* id)
{
    (void) p;
    return strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0 ? &audio_ports : NULL;
}

static clap_plugin_t plugin = { &descriptor, NULL, init, destroy, activate, deactivate, start_processing,
                                stop_processing, reset, process, get_extension, on_main_thread };

static uint32_t factory_count (const clap_plugin_factory_t* f) { (void) f; return 1; }
static const clap_plugin_descriptor_t* factory_descriptor (const clap_plugin_factory_t* f, uint32_t i) { (void) f; return i == 0 ? &descriptor : NULL; }

static const clap_plugin_t* factory_create (const clap_plugin_factory_t* f, const clap_host_t* host, const char* id)
{
    (void) f;
    the_host = host;
    return strcmp (id, descriptor.id) == 0 ? &plugin : NULL;
}

static const clap_plugin_factory_t factory = { factory_count, factory_descriptor, factory_create };

static bool entry_init (const char* path) { (void) path; return true; }
static void entry_deinit (void) {}
static const void* entry_get_factory (const char* id) { return strcmp (id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : NULL; }

CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory };
