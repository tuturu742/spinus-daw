/* A test CLAP instrument with one main output of 16 channels and no names for them, as drum plugins that give
   each microphone its own output have: channel k plays a steady (k + 1) / 100, so tests can tell every channel
   apart (and see one that arrives as something else). Never installed or shipped. */

#include <clap/clap.h>
#include <string.h>

#define CHANNELS 16

static const char* const features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, NULL };

static const clap_plugin_descriptor_t descriptor = {
    CLAP_VERSION_INIT, "io.github.tuturu742.spinus.test.many-outputs", "Many Outputs", "Spinus tests", "", "", "",
    "1.0", "Sixteen outputs, each with its own level", features
};

static uint32_t ports_count (const clap_plugin_t* p, bool is_input)
{
    (void) p;
    return is_input ? 0 : 1;
}

static bool ports_get (const clap_plugin_t* p, uint32_t index, bool is_input, clap_audio_port_info_t* info)
{
    (void) p;
    if (is_input || index != 0)
        return false;
    memset (info, 0, sizeof (*info));
    info->id = 0;
    strcpy (info->name, "Outputs");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = CHANNELS;
    info->port_type = NULL; /* no layout: just numbered channels */
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t audio_ports = { ports_count, ports_get };

static uint32_t note_ports_count (const clap_plugin_t* p, bool is_input)
{
    (void) p;
    return is_input ? 1 : 0;
}

static bool note_ports_get (const clap_plugin_t* p, uint32_t index, bool is_input, clap_note_port_info_t* info)
{
    (void) p;
    if (! is_input || index != 0)
        return false;
    memset (info, 0, sizeof (*info));
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_CLAP;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    strcpy (info->name, "Notes");
    return true;
}

static const clap_plugin_note_ports_t note_ports = { note_ports_count, note_ports_get };

static bool init (const clap_plugin_t* p) { (void) p; return true; }
static void destroy (const clap_plugin_t* p) { (void) p; }
static bool activate (const clap_plugin_t* p, double sr, uint32_t min, uint32_t max) { (void) p; (void) sr; (void) min; (void) max; return true; }
static void deactivate (const clap_plugin_t* p) { (void) p; }
static bool start_processing (const clap_plugin_t* p) { (void) p; return true; }
static void stop_processing (const clap_plugin_t* p) { (void) p; }
static void reset (const clap_plugin_t* p) { (void) p; }
static void on_main_thread (const clap_plugin_t* p) { (void) p; }

static clap_process_status process (const clap_plugin_t* p, const clap_process_t* process)
{
    (void) p;
    for (uint32_t port = 0; port < process->audio_outputs_count; ++port)
    {
        const clap_audio_buffer_t* out = &process->audio_outputs[port];
        for (uint32_t ch = 0; ch < out->channel_count; ++ch)
            if (out->data32 != NULL && out->data32[ch] != NULL)
                for (uint32_t i = 0; i < process->frames_count; ++i)
                    out->data32[ch][i] = (float) (ch + 1) / 100.0f;
    }
    return CLAP_PROCESS_CONTINUE;
}

static const void* get_extension (const clap_plugin_t* p, const char* id)
{
    (void) p;
    if (strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0)
        return &audio_ports;
    if (strcmp (id, CLAP_EXT_NOTE_PORTS) == 0)
        return &note_ports;
    return NULL;
}

static clap_plugin_t plugin = { &descriptor, NULL, init, destroy, activate, deactivate, start_processing,
                                stop_processing, reset, process, get_extension, on_main_thread };

static uint32_t factory_count (const clap_plugin_factory_t* f) { (void) f; return 1; }
static const clap_plugin_descriptor_t* factory_descriptor (const clap_plugin_factory_t* f, uint32_t i) { (void) f; return i == 0 ? &descriptor : NULL; }

static const clap_plugin_t* factory_create (const clap_plugin_factory_t* f, const clap_host_t* host, const char* id)
{
    (void) f;
    (void) host;
    return strcmp (id, descriptor.id) == 0 ? &plugin : NULL;
}

static const clap_plugin_factory_t factory = { factory_count, factory_descriptor, factory_create };

static bool entry_init (const char* path) { (void) path; return true; }
static void entry_deinit (void) {}
static const void* entry_get_factory (const char* id) { return strcmp (id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : NULL; }

CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory };
