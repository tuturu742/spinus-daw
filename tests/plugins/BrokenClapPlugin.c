/* Deliberately broken CLAP "plugins" for the plugin scanner tests: one crashes and one hangs as soon
   as a host loads it (BROKEN_BEHAVIOUR 1 = crash, 2 = hang). Never installed or shipped. */

#include <clap/clap.h>
#include <signal.h>
#ifdef _WIN32
    #include <windows.h>
    #define sleep(seconds) Sleep ((seconds) * 1000)
    /* A real access violation: what a crashing plugin does on Windows (raise() would not reach Spinus's guard). */
    #define CRASH() (*(volatile int*) 0 = 0)
#else
    #include <unistd.h>
    #define CRASH() raise (SIGSEGV)
#endif

static bool init (const char* path)
{
    (void) path;
#if BROKEN_BEHAVIOUR == 1
    CRASH();
#else
    for (;;)
        sleep (1);
#endif
    return false;
}

static void deinit (void) {}
static const void* get_factory (const char* id) { (void) id; return NULL; }

CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, init, deinit, get_factory };
