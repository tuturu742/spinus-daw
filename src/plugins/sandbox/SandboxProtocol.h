#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#if ! JUCE_WINDOWS
#include <semaphore.h>
#endif

/*  The plugin sandbox's shared memory: how Spinus and spinus-plugin-host pass audio, MIDI and
    parameter changes without locks.

    Spinus writes each block it is given into the next of `slotCount` slots (input audio, MIDI, parameter
    changes), counts it in `posted` and wakes the host with `work`. The host processes slots in order,
    writes each one's output into the same slot, counts it in `done` and posts `finished`. Spinus collects
    finished slots into a FIFO of its own that starts one block of silence ahead, so in real time it never
    waits: the plugin has a whole block's time to answer, which is the one block of extra latency. Offline
    (rendering), Spinus waits for each block instead.

    Plugin -> Spinus parameter changes (the plugin's own window) travel in `fromPlugin`, a ring Spinus
    drains on its message thread.

    Everything here is plain data and lock-free atomics, valid in two processes at once (Linux: shm_open and
    mmap, process-shared POSIX semaphores; Windows: a named file mapping and two named semaphores). Control
    messages (load, state, editor) go over a pipe instead (juce::ChildProcessCoordinator), as JSON. */

namespace spinus::plugins::sandbox
{

inline constexpr int maxChannels = 8;
inline constexpr int maxBlock = 4096;
inline constexpr int slotCount = 8;
inline constexpr int maxMidiEvents = 512;
inline constexpr int maxParameterChanges = 256;
inline constexpr int ringSize = 1024; // plugin -> Spinus parameter changes

static_assert (std::atomic<uint32_t>::is_always_lock_free);

struct ParameterChange
{
    int32_t index = 0;
    float value = 0.0f;
};

struct MidiEvent
{
    int32_t sample = 0; // within the block
    uint8_t size = 0;
    uint8_t bytes[3] {};
};

struct Slot
{
    int32_t numSamples = 0;
    int32_t numMidi = 0;
    int32_t numChanges = 0;
    MidiEvent midi[maxMidiEvents];
    ParameterChange changes[maxParameterChanges];
    float audio[maxChannels][maxBlock]; // input, replaced by the output
};

struct Shared
{
    uint32_t magic = 0; // `expectedMagic` once set up
    int32_t numChannels = 2;
#if ! JUCE_WINDOWS
    sem_t work, finished; // on Windows: named semaphores, held by SharedMemory
#endif
    std::atomic<uint32_t> posted { 0 }, done { 0 };
    std::atomic<uint32_t> ringWrite { 0 }, ringRead { 0 };
    std::atomic<uint32_t> quit { 0 };
    ParameterChange ring[ringSize];
    Slot slots[slotCount];
};

inline constexpr uint32_t expectedMagic = 0x53504e53; // "SPNS"

/** A block of shared memory, created by Spinus and opened by the host. */
class SharedMemory
{
public:
    /** Creates it (Spinus). */
    static std::unique_ptr<SharedMemory> create();
    /** Opens one created by the other side (the host). */
    static std::unique_ptr<SharedMemory> open (const juce::String& name);
    ~SharedMemory();

    Shared& get() noexcept { return *shared; }
    const juce::String& getName() const noexcept { return name; }
    /** Removes the name (both sides keep their mapping); called once the host has opened it. */
    void unlink();

    /** Wakes the host: a block was posted (Spinus), or it should look at `quit` (either side). */
    void postWork() noexcept;
    /** The host: waits up to `milliseconds` for work; false when the time ran out. */
    bool waitForWork (int milliseconds) noexcept;
    /** The host: a block is done. */
    void postFinished() noexcept;
    /** Spinus (offline rendering): waits up to `milliseconds` for a finished block; false when the time ran
        out. */
    bool waitForFinished (int milliseconds) noexcept;

private:
    SharedMemory (juce::String, Shared*, bool owner);
    juce::String name;
    Shared* shared = nullptr;
    bool owner = false, linked = true;
#if JUCE_WINDOWS
    void* mapping = nullptr; // HANDLEs
    void* work = nullptr;
    void* finished = nullptr;
#endif
};

} // namespace spinus::plugins::sandbox
