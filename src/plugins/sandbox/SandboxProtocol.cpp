#include "SandboxProtocol.h"

#if JUCE_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace spinus::plugins::sandbox
{

SharedMemory::SharedMemory (juce::String n, Shared* s, bool o) : name (std::move (n)), shared (s), owner (o) {}

#if JUCE_WINDOWS
namespace
{
std::wstring wide (const juce::String& s) { return s.toWideCharPointer(); }
} // namespace

std::unique_ptr<SharedMemory> SharedMemory::create()
{
    static std::atomic<int> counter { 0 };
    const auto name = "Local\\spinus-sandbox-" + juce::String ((int)GetCurrentProcessId()) + "-" +
                      juce::String (++counter) + "-" +
                      juce::String::toHexString (juce::Random::getSystemRandom().nextInt());

    auto* mapping = CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)sizeof (Shared),
                                        wide (name).c_str());
    if (mapping == nullptr || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (mapping != nullptr)
            CloseHandle (mapping);
        return nullptr;
    }
    auto* work = CreateSemaphoreW (nullptr, 0, 0x7fffffff, wide (name + "-work").c_str());
    auto* finished = CreateSemaphoreW (nullptr, 0, 0x7fffffff, wide (name + "-finished").c_str());
    auto* memory = MapViewOfFile (mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (Shared));
    if (work == nullptr || finished == nullptr || memory == nullptr)
    {
        for (auto* h : { mapping, work, finished })
            if (h != nullptr)
                CloseHandle (h);
        return nullptr;
    }

    auto* shared = new (memory) Shared();
    shared->magic = expectedMagic;
    std::unique_ptr<SharedMemory> result (new SharedMemory (name, shared, true));
    result->mapping = mapping;
    result->work = work;
    result->finished = finished;
    return result;
}

std::unique_ptr<SharedMemory> SharedMemory::open (const juce::String& name)
{
    auto* mapping = OpenFileMappingW (FILE_MAP_ALL_ACCESS, FALSE, wide (name).c_str());
    if (mapping == nullptr)
        return nullptr;
    auto* memory = MapViewOfFile (mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (Shared));
    auto* work = OpenSemaphoreW (SEMAPHORE_ALL_ACCESS, FALSE, wide (name + "-work").c_str());
    auto* finished = OpenSemaphoreW (SEMAPHORE_ALL_ACCESS, FALSE, wide (name + "-finished").c_str());
    auto* shared = static_cast<Shared*> (memory);
    if (memory == nullptr || work == nullptr || finished == nullptr || shared->magic != expectedMagic)
    {
        if (memory != nullptr)
            UnmapViewOfFile (memory);
        for (auto* h : { mapping, work, finished })
            if (h != nullptr)
                CloseHandle (h);
        return nullptr;
    }
    std::unique_ptr<SharedMemory> result (new SharedMemory (name, shared, false));
    result->mapping = mapping;
    result->work = work;
    result->finished = finished;
    return result;
}

// Windows removes the name with the last handle; each side holds its own until it is done.
void SharedMemory::unlink() { linked = false; }

SharedMemory::~SharedMemory()
{
    if (owner)
        shared->~Shared();
    UnmapViewOfFile (shared);
    for (auto* h : { mapping, work, finished })
        if (h != nullptr)
            CloseHandle (h);
}

void SharedMemory::postWork() noexcept { ReleaseSemaphore (work, 1, nullptr); }
void SharedMemory::postFinished() noexcept { ReleaseSemaphore (finished, 1, nullptr); }
bool SharedMemory::waitForWork (int milliseconds) noexcept
{
    return WaitForSingleObject (work, (DWORD)milliseconds) == WAIT_OBJECT_0;
}
bool SharedMemory::waitForFinished (int milliseconds) noexcept
{
    return WaitForSingleObject (finished, (DWORD)milliseconds) == WAIT_OBJECT_0;
}

#else
namespace
{
/** Waits on a semaphore for up to `milliseconds` (sem_timedwait takes a time of day). */
bool waitFor (sem_t& semaphore, int milliseconds) noexcept
{
    timespec until {};
    clock_gettime (CLOCK_REALTIME, &until);
    until.tv_sec += milliseconds / 1000;
    until.tv_nsec += (long)(milliseconds % 1000) * 1'000'000;
    if (until.tv_nsec >= 1'000'000'000)
    {
        until.tv_sec += 1;
        until.tv_nsec -= 1'000'000'000;
    }
    while (sem_timedwait (&semaphore, &until) != 0)
        if (errno != EINTR)
            return false;
    return true;
}
} // namespace

std::unique_ptr<SharedMemory> SharedMemory::create()
{
    static std::atomic<int> counter { 0 };
    const auto name = "/spinus-sandbox-" + juce::String (getpid()) + "-" + juce::String (++counter) + "-" +
                      juce::String::toHexString (juce::Random::getSystemRandom().nextInt());
    const int fd = shm_open (name.toRawUTF8(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0)
        return nullptr;
    if (ftruncate (fd, (off_t)sizeof (Shared)) != 0)
    {
        close (fd);
        shm_unlink (name.toRawUTF8());
        return nullptr;
    }
    auto* memory = mmap (nullptr, sizeof (Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close (fd);
    if (memory == MAP_FAILED)
    {
        shm_unlink (name.toRawUTF8());
        return nullptr;
    }

    auto* shared = new (memory) Shared();
    sem_init (&shared->work, 1, 0);
    sem_init (&shared->finished, 1, 0);
    shared->magic = expectedMagic;
    return std::unique_ptr<SharedMemory> (new SharedMemory (name, shared, true));
}

std::unique_ptr<SharedMemory> SharedMemory::open (const juce::String& name)
{
    const int fd = shm_open (name.toRawUTF8(), O_RDWR, 0600);
    if (fd < 0)
        return nullptr;
    auto* memory = mmap (nullptr, sizeof (Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close (fd);
    if (memory == MAP_FAILED)
        return nullptr;
    auto* shared = static_cast<Shared*> (memory);
    if (shared->magic != expectedMagic)
    {
        munmap (memory, sizeof (Shared));
        return nullptr;
    }
    return std::unique_ptr<SharedMemory> (new SharedMemory (name, shared, false));
}

void SharedMemory::unlink()
{
    if (owner && linked)
        shm_unlink (name.toRawUTF8());
    linked = false;
}

SharedMemory::~SharedMemory()
{
    if (owner)
    {
        sem_destroy (&shared->work);
        sem_destroy (&shared->finished);
        shared->~Shared();
        unlink();
    }
    munmap (shared, sizeof (Shared));
}

void SharedMemory::postWork() noexcept { sem_post (&shared->work); }
void SharedMemory::postFinished() noexcept { sem_post (&shared->finished); }
bool SharedMemory::waitForWork (int milliseconds) noexcept { return waitFor (shared->work, milliseconds); }
bool SharedMemory::waitForFinished (int milliseconds) noexcept { return waitFor (shared->finished, milliseconds); }
#endif

} // namespace spinus::plugins::sandbox
