# Spinus on your Windows PC: checks an installed Spinus and prints what to paste back.
#
# 1. Install Spinus with Spinus-<version>-Setup.exe (if Windows says "Windows protected your PC", click
#    More info, then Run anyway).
# 2. Open PowerShell (Start menu, type "PowerShell") and run, with the folder you saved this file in:
#      powershell -ExecutionPolicy Bypass -File "$HOME\Downloads\windows-build.ps1"
# 3. Paste everything it prints into the issue, then do the checks it lists at the end.

$ErrorActionPreference = "Continue"
function Say($text) { Write-Host "== $text" }

$exe = "$env:LOCALAPPDATA\Programs\Spinus DAW\spinus.exe"
Say "Windows: $([Environment]::OSVersion.VersionString), $env:PROCESSOR_ARCHITECTURE"
if (-not (Test-Path $exe)) {
    Say "Spinus is not installed in $exe. Run the installer first."
    exit 1
}
Say "Version: $(& $exe --version | Out-String)".Trim()

Say "Self-test (about a minute)"
$check = Join-Path $env:TEMP "spinus-check"
Remove-Item -Recurse -Force $check -ErrorAction SilentlyContinue
& $exe --self-test $check | Out-Host
Remove-Item -Recurse -Force $check -ErrorAction SilentlyContinue

Say ".spn projects open with"
(Get-ItemProperty "HKCU:\Software\Classes\SpinusDAW.Project\shell\open\command" -ErrorAction SilentlyContinue)."(default)"

Say "ASIO drivers on this PC"
$asio = Get-ChildItem "HKLM:\SOFTWARE\ASIO" -ErrorAction SilentlyContinue | ForEach-Object { $_.PSChildName }
if ($asio) { $asio } else { "(none: Spinus uses Windows Audio)" }

Say "Sound cards"
Get-CimInstance Win32_SoundDevice | ForEach-Object { "$($_.Name) ($($_.Status))" }

Say "Plugin folders"
foreach ($folder in "$env:CommonProgramFiles\VST3", "$env:CommonProgramFiles\CLAP", "$env:ProgramFiles\VSTPlugins",
                    "$env:ProgramFiles\Steinberg\VstPlugins") {
    if (Test-Path $folder) { "$folder : $((Get-ChildItem $folder -ErrorAction SilentlyContinue).Count) items" }
}

Say "Done. Now please try, and say what happened:"
@"
  1. Start Spinus from the Start menu. Get free plugins: download Surge XT and Dragonfly.
  2. F10: which sound system and device it chose. Click the metronome and press Space: even clicks?
  3. Add Surge XT, play notes in the Piano Roll, open its window (double-click the channel). Does it show
     inside Spinus's window, and can you turn its knobs?
  4. Scan plugins (bottom of the Browser): your own VST3 plugins appear? Add one and open its window.
  5. Save a project, close Spinus, double-click the .spn in Explorer: it opens?
  6. Export the song (Ctrl+R) as WAV and MP3, and listen.
  7. Options > AI: turn AI on, add a connection, ask the Assistant something.
"@ | Write-Host
