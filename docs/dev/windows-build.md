# Building Spinus for Windows

How the Windows build is made, what differs from Linux, and what is still Linux-only. Spinus ships for
Linux and Windows; Windows releases are built by GitHub Actions or on Linux with the cross-build below.

## Compilers

| Route | Use |
|---|---|
| MinGW-w64 | **Not possible.** JUCE dropped MinGW (`external/JUCE/BREAKING_CHANGES.md`); `juce_TargetPlatform.h` stops with `#error "MinGW is not supported"` |
| MSVC (`cl`) on Windows | `.github/workflows/windows.yml` (the `windows` preset); what releases are made with |
| clang-cl + lld-link on Linux, with Microsoft's C runtime and Windows SDK from [xwin](https://github.com/Jake-Shadle/xwin) | The `windows-cross` preset (`cmake/toolchains/windows-msvc-xwin.cmake`): quick checks on Linux, and `packaging/windows/build-on-linux.sh` makes the installer without Windows |

The cross-build needs Microsoft's SDK, fetched by `tools/windows/get-msvc-sdk.sh --accept-license`. That
means accepting the Visual Studio Build Tools licence (https://go.microsoft.com/fwlink/?LinkId=2086102).
The SDK is used to build only, never committed or shipped. Both routes
use the static runtime (`/MT`), so `spinus.exe` needs no Visual C++ Redistributable. Details of the
cross-build:

- xwin fetches release libraries only, so CMake's test projects link the release runtime.
- Windows ignores the case of file names and Linux does not. JUCE asks for `D2d1.lib` and the SDK has
  `d2d1.lib`, so the script links each library the sources name under that spelling.
- Spinus's own devices as VST3/LV2 plugins (for other programs) are left out of the cross-build: JUCE
  describes them with a helper built for the target, which Linux cannot run.
- ctest runs the Windows tests under Wine when Wine is installed (`CMAKE_CROSSCOMPILING_EMULATOR`).
  Catch2 lists them when ctest starts, not after linking.

## What differs on Windows

| Part | Linux | Windows |
|---|---|---|
| Sound | JACK, ALSA (PipeWire through either) | ASIO when a driver is installed (the headers JUCE bundles, GPLv3 option; THIRD_PARTY.md), else WASAPI low latency, WASAPI shared, DirectSound (`engine/AudioSetup.cpp`) |
| Windows plugins | Wine and yabridge (`plugins/windows`) | Native; the Wine bridge is never created (`MainView` hides its menu item) |
| Choosing a microphone | PipeWire routing (`engine/PipeWireInput`) | Not needed: WASAPI names each input |
| CLAP plugins | `dlopen`, X11 windows through XEmbed, posix-fd support | `LoadLibraryW`, Win32 windows in a child window kept in place by `juce::HWNDComponent`; no posix-fd |
| Plugins in a separate process | `shm_open`/`mmap`, process-shared POSIX semaphores, XEmbed editor | A named file mapping and named semaphores behind the same `SharedMemory` calls; no editor window yet |
| Crash guard | Signal handlers, `dladdr`, `backtrace` | An unhandled-exception filter and a SIGABRT handler, `GetModuleHandleEx`, `CaptureStackBackTrace`, `EnumProcessModules` |
| Project lock | `/proc/<pid>` | `OpenProcess`, and the image name must be Spinus's (Windows reuses process numbers soon) |
| AI service | TCP on 127.0.0.1 with a token, started through `ai/run_spinus_ai.py` (the same on both) | The same, with the embeddable Python the installer brings (`ai/python/python.exe`) |
| Free plugin downloads | libcurl, libarchive; the projects' Linux archives | JUCE's `URL` (WinINet), `plugins/ArchiveReader` (zip, tar.gz; tested on Linux); the projects' Windows zips. LSP Plugins has no Windows build and Odin 2 only an installer, so they are not offered there |
| Folders | `~/.config/Spinus DAW`, `$XDG_DATA_HOME` or `~/.local/share` | `%APPDATA%\Spinus DAW`, `%LOCALAPPDATA%` (`getUserDataRoot()`) |
| Helper programs | `spinus-plugin-scanner` | `spinus-plugin-scanner.exe` (`getHelperProgram()`) |
| Terminal output (`--version`, `--self-test`, `--scan-plugins`) | stdout | Attached to the console Spinus was started from, unless stdout already goes somewhere |
| Installing | Arch package, AppImage, Flatpak | `packaging/windows`: `stage.py` gathers the build, Python and its packages; `spinus.nsi` (NSIS) makes a per-user installer with a Start menu entry, the `.spn` association and an uninstaller |
| Warnings | GCC/Clang `-W` flags | `/W4` (clang-cl reads `-Wall` as every warning) |

## A JUCE fix carried by Spinus

JUCE 9's Windows MIDI input calls `std::thread::join` on a thread it never started when opening the
device fails. `std::system_error` then escapes a destructor and the program ends. Windows MIDI inputs are
exclusive, so a keyboard already open in another program triggers it. Under Wine, a fresh setup triggered
it on every first run: the self-test died at its export with exit code 9, and a debug build under winedbg
showed `~InputDevice` → `std::thread::join`. Windows builds compile a copy of `juce_audio_devices` with that
join guarded (`cmake/SpinusJuce.cmake`, generated at configure time like the FST fix). The build stops if
JUCE's text changes. Remove it once JUCE fixes this upstream.

## Still Linux-only

- **Analysis engine** (`ai/spinus_ai/compute`): the locked PyTorch builds are Linux wheels
  (`tools/compute-locks.py`, `x86_64-manylinux_2_28`) and GPU detection reads `/sys`. Windows needs locks
  for `x86_64-pc-windows-msvc` (CPU and CUDA builds exist; AMD's ROCm for Windows does not cover the same
  cards), detection through `nvidia-smi`, and uv bundled in the installer.
- **LSP Plugins' effects** (EQ, Compressor, Limiter, Gate, Multiband, Spectrum Analyzer): the project
  publishes no Windows build. `model::getStandIns()` can make Tracktion built-in effects play them on
  Windows under the same stock types, so projects open the same everywhere. It is empty: no stand-ins
  are chosen.
- **Separated plugins' windows** (the sandbox's editor uses XEmbed). On Windows, the plugin host could make
  its window a child of Spinus's HWND.
- **Tests left out on Windows** (tests/CMakeLists.txt): the Wine bridge, the download tests that write
  archives with libarchive, and the tests that use signals, `pgrep`, environment changes or shell-script
  fakes of Python and PyTorch.
- **Code signing**: the installer is not signed, so SmartScreen warns on first start. A certificate (paid,
  or the SignPath Foundation's free one for open-source projects) can come later.

## Checked under Wine

On Linux, under Wine 11, with no sound device reachable (bubblewrap hides `/dev/snd` and the PipeWire
socket):

- the cross-built `spinus.exe` passes its whole self-test: a song made, saved, reopened and exported with
  sound; both helpers start; the AI service's Python is in place;
- the installer installs silently, the installed copy passes its self-test, `.spn` is associated, and
  uninstalling removes the files and the association;
- the bundled Windows Python runs the AI service over TCP with its token. Wrong tokens get no answer, and
  it stops within a second of `ai.v1.shutdown`. Python 3.12's Server on Windows can wait for ever for a
  connection Spinus has already closed, so the service closes its connections itself.

On a real Windows PC, `tools/owner-checks/windows-build.ps1` checks an installed copy.
