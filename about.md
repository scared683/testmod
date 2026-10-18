# Input Buffer

Snaps an early or late click (or release) to the nearest timing that survives, within a configurable number of physics steps.

- **Buffer steps**: how far an input may move, each way. 1 step = 1/240 s.
- **Keybinds** (change them in Geode's keybind settings): `F8` toggle, `]` more steps, `[` fewer steps.
- Classic levels only. Off in platformer and 2-player levels.

**Safe mode is always on while the buffer is active**: completions and new-best percentages are not saved, and a label is shown in the corner of the screen.
