# Command API (developer notes)

Every change to a project is a `Command` (AGENTS rule 2). Code lives in `src/commands/`.

- `Command`: `perform()` applies the whole change or nothing, `undo()` restores the exact previous state,
  `getChanges()` says what was touched (a `ChangeSet`), `getResult()` returns IDs of created objects.
  Redo calls `perform()` again and reuses the IDs allocated the first time.
- `Transaction`: several commands as one undo step, all or nothing. Each applied AI proposal is one
  transaction (AGENTS rule 3).
- `CommandHistory`: owns the project and the undo/redo stacks; the only way to change a project. After
  every command it validates the project and rolls back if a command ever breaks an invariant.
  `performJoined()` adds a command to the newest undo step when that step is the same command: the UI
  applies every move of a fader drag (so it is heard) and the whole drag is one undo step.
- `CommandRegistry`: every command by name, with a JSON Schema for its parameters. The UI, the AI
  service (`project.v1.apply`) and scripted tests all create commands here.

The full list of commands and parameters is [`commands.schema.json`](commands.schema.json), generated
from the registry; a test fails when it is out of date.

## Adding a command

1. Implement it in `src/commands/CommandRegistry.cpp`. Use `SnapshotCommand` for rare structural edits
   (add/remove; undo restores whole sections), or a targeted undo for frequent edits (knobs, notes).
2. Register it with a one-sentence summary and a `ParameterSchema`. Reject unknown parameters with
   `allowOnly`.
3. Add an example call to `exampleCalls()` in `tests/commands/CommandTests.cpp` (the generic test checks
   perform, exact undo and redo) plus tests for its special cases.
4. Regenerate the schema: `SPINUS_WRITE_COMMAND_SCHEMA=1 ./build/release/tests/spinus_tests "[commands]"`.
5. If users can trigger it, document the feature in `docs/user/`.

## Device settings: numbers and text

A device's settings have one stored form and one typed form, and nothing else:

- **Stored: 0 to 1 over the setting's whole range.** `Device::parameters` in the project file, automation
  points, synth profiles' `min`/`max`, and `device.setParameters`' `values`. A `values` number is never
  read as percent, milliseconds or hertz: `{"amount": 0.35}` is 35% of the way along the knob.
- **Typed: text in the units the device shows** (`src/dsp/ParameterText.h`). `"35%"`, `"220 ms"`,
  `"1.5 s"`, `"-6 dB"`, `"1.2 kHz"`, `"4:1"`, `"30% L"`, a choice by name (`"High pass"`). The unit may be
  left out; a plain number is then in the setting's smallest unit: milliseconds for times, hertz for
  frequencies, percent for settings shown in percent (`"35"` is 35%), the shown unit otherwise. A unit of
  another kind than the setting shows (`"50%"` for a setting in dB) is refused, not guessed.

Everything that turns text into a setting follows the typed rule: the knobs' text boxes in Spinus's own
devices (every number setting is a `param::FloatSetting`, and a test checks each one reads back what it
shows), the Automation editor's Value box (`Session::parseTargetValue`), `device.setParameters`' `text` (read once
into stored values: a Spinus device's on a fresh copy of the device, another plugin's on the plugin as it
plays, or on a copy when the same proposal adds it), the Automixer, the assistant (its prompt and the schema
tell it to use `text`) and the demo song generators. Another plugin's text is read by what the plugin shows,
once the unit agrees with it (`param::byPluginText`): the plugin's own reading when it shows what was typed,
else the value is found from what it shows at 200 points over its range (a VST3's separate unit label
counts), narrowed between the two the number lies between, to the middle of the values that show the same;
a word it shows ("Hall", "Off") reads too. A plugin that shows bare numbers ("2.575" for a decay in seconds)
takes "2.5 s" as 2.5 when only that is in its range (else as 2500, in milliseconds). A plugin that shows no numbers is refused, with what it shows at
a few points.
