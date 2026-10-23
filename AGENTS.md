# AGENTS.md: rules for coding agents working on Spinus DAW

Read `DESIGN.md` before starting any task. It is the source of truth. If your task conflicts with it, stop and open an issue labeled `owner-decision` instead of guessing.

## Non-negotiables

1. **Real-time safety.** On the audio thread: no heap allocation, no locks, no file or network I/O, no logging, no system calls, no Python, no AI. Use lock-free FIFOs to talk to the audio thread. Any change under `src/engine/` or `src/dsp/` must pass the real-time checklist in `docs/dev/realtime-checklist.md`.
2. **Command API only.** Every project change goes through a `Command`. UI code and AI code never mutate the model directly. New commands need: schema entry, undo, tests, and a manual page.
3. **AI proposes, the user decides.** AI features never apply changes without an explicit user Apply. Each applied proposal is one undo step.
4. **The DAW works without AI.** Spinus must start, play, save and export with the `spinus-ai` service missing or crashed.
5. **Licenses.** The project is AGPL-3.0-or-later. Before adding any dependency or model, confirm its license is AGPLv3-compatible and add it to `THIRD_PARTY.md`. No non-commercial (NC) model weights as defaults.
6. **Never commit** API keys, model weights, copyrighted audio, or generated build output.

## Every pull request

- One task per branch, named `<area>/<short-task>` (e.g. `piano-roll/chord-stamp`).
- Description sections: **What changed**, **Why**, **Tests**, **How to test** (steps the owner can follow on Arch Linux with no programming), and screenshots or a short recording for UI changes.
- CI green: Release build, Debug + AddressSanitizer build, unit tests, `pluginval` for stock plugins, clang-tidy, `pytest`, docs check.
- Docs: update `docs/user/` for anything a user can see, and `DESIGN.md` for any architectural change.
- Hardware-dependent checks (GPU, audio latency, Windows plugins) go in `tools/owner-checks/` as a script; ask the owner to run it and paste the output.

## Code style

- C++20, JUCE conventions for classes and naming, `clang-format` config at repo root.
- Python 3.12, `ruff` + `mypy --strict` for `ai/`.
- Prefer small, testable units. Headless engine tests over UI tests where possible.

## Owner

The owner tests and steers; they do not write code. Write PR descriptions and questions in plain language.
