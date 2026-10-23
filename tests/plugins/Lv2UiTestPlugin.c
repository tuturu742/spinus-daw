/* A tiny LV2 plugin with X11 UIs that behave like real ones Spinus must survive (tests/plugins/Lv2GuiTests.cpp).
   One plugin per UI, all in this one library (the bundle's .ttl files are written by tests/CMakeLists.txt):

   - "unsupported": the UI requires a host feature no host has, so it must never be instantiated.
   - "null-resize": the manifest lists ui:resize as extension data, but extension_data returns NULL for it,
     as DrumGizmo 0.9.20 does. Unpatched JUCE reads through that NULL pointer (SIGSEGV).
   - "fails": instantiate returns NULL.
   - "instance-access": the UI requires instance-access and only opens when the feature's data is this
     plugin's own LV2_Handle (as DrumGizmo's UI does).

   The plugin itself is one gain control and one audio output (silence). */

#include <lv2/core/lv2.h>
#include <lv2/instance-access/instance-access.h>
#include <lv2/ui/ui.h>

#include <X11/Xlib.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TEST_URI "urn:spinus:test:lv2-ui#"
#define PLUGIN_MAGIC 0x53504c56u /* "SPLV" */

typedef struct
{
    uint32_t magic;
    const float* gain;
    float* out;
} Plugin;

static LV2_Handle instantiate (const LV2_Descriptor* d, double rate, const char* path, const LV2_Feature* const* f)
{
    (void)d;
    (void)rate;
    (void)path;
    (void)f;
    Plugin* p = (Plugin*)calloc (1, sizeof (Plugin));
    if (p != NULL)
        p->magic = PLUGIN_MAGIC;
    return p;
}

static void connectPort (LV2_Handle h, uint32_t port, void* data)
{
    Plugin* p = (Plugin*)h;
    if (port == 0)
        p->gain = (const float*)data;
    else if (port == 1)
        p->out = (float*)data;
}

static void run (LV2_Handle h, uint32_t frames)
{
    Plugin* p = (Plugin*)h;
    if (p->out != NULL)
        memset (p->out, 0, frames * sizeof (float));
}

static void cleanup (LV2_Handle h)
{
    ((Plugin*)h)->magic = 0;
    free (h);
}

static const void* noExtensionData (const char* uri)
{
    (void)uri;
    return NULL;
}

#define PLUGIN(name) { TEST_URI name, instantiate, connectPort, NULL, run, NULL, cleanup, noExtensionData }

static const LV2_Descriptor plugins[] = { PLUGIN ("unsupported"), PLUGIN ("null-resize"), PLUGIN ("fails"),
                                          PLUGIN ("instance-access") };

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor (uint32_t index)
{
    return index < sizeof (plugins) / sizeof (plugins[0]) ? &plugins[index] : NULL;
}

/*==============================================================================================================*/
typedef struct
{
    Display* display;
    Window window;
} Ui;

static const void* findFeature (const LV2_Feature* const* features, const char* uri)
{
    for (; features != NULL && *features != NULL; ++features)
        if (strcmp ((*features)->URI, uri) == 0)
            return (*features)->data;
    return NULL;
}

static LV2UI_Handle uiInstantiate (const LV2UI_Descriptor* d, const char* pluginUri, const char* bundlePath,
                                   LV2UI_Write_Function write, LV2UI_Controller controller, LV2UI_Widget* widget,
                                   const LV2_Feature* const* features)
{
    (void)pluginUri;
    (void)bundlePath;
    (void)write;
    (void)controller;

    if (strcmp (d->URI, TEST_URI "fails-ui") == 0)
        return NULL;

    if (strcmp (d->URI, TEST_URI "instance-access-ui") == 0)
    {
        const Plugin* plugin = (const Plugin*)findFeature (features, LV2_INSTANCE_ACCESS_URI);
        if (plugin == NULL || plugin->magic != PLUGIN_MAGIC)
            return NULL;
    }

    const Window parent = (Window)(uintptr_t)findFeature (features, LV2_UI__parent);
    Display* display = XOpenDisplay (NULL);
    if (parent == 0 || display == NULL)
    {
        if (display != NULL)
            XCloseDisplay (display);
        return NULL;
    }

    Ui* ui = (Ui*)calloc (1, sizeof (Ui));
    if (ui == NULL)
    {
        XCloseDisplay (display);
        return NULL;
    }
    ui->display = display;
    ui->window = XCreateSimpleWindow (display, parent, 0, 0, 320, 200, 0, 0, 0x2a7ab0);
    XMapWindow (display, ui->window);
    XSync (display, False); /* the host asks for its size at once, on its own connection */
    *widget = (LV2UI_Widget)(uintptr_t)ui->window;
    return ui;
}

static void uiCleanup (LV2UI_Handle h)
{
    Ui* ui = (Ui*)h;
    XDestroyWindow (ui->display, ui->window);
    XCloseDisplay (ui->display);
    free (ui);
}

static void uiPortEvent (LV2UI_Handle h, uint32_t port, uint32_t size, uint32_t format, const void* buffer)
{
    (void)h;
    (void)port;
    (void)size;
    (void)format;
    (void)buffer;
}

static int uiIdle (LV2UI_Handle h)
{
    Ui* ui = (Ui*)h;
    while (XPending (ui->display) > 0)
    {
        XEvent event;
        XNextEvent (ui->display, &event);
    }
    return 0;
}

static const void* uiExtensionData (const char* uri)
{
    static const LV2UI_Idle_Interface idle = { uiIdle };
    if (strcmp (uri, LV2_UI__idleInterface) == 0)
        return &idle;
    return NULL; /* ui:resize too, although the manifest lists it (DrumGizmo does the same) */
}

#define UI(name) { TEST_URI name "-ui", uiInstantiate, uiCleanup, uiPortEvent, uiExtensionData }

static const LV2UI_Descriptor uis[] = { UI ("unsupported"), UI ("null-resize"), UI ("fails"), UI ("instance-access") };

LV2_SYMBOL_EXPORT const LV2UI_Descriptor* lv2ui_descriptor (uint32_t index)
{
    return index < sizeof (uis) / sizeof (uis[0]) ? &uis[index] : NULL;
}
