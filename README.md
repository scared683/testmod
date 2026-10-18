# Input Buffer (Geode mod, GD 2.2081, Windows)

When you click or release in a classic level, the mod simulates what happens next. If that input as-is would kill you, it tries moving it up to **N physics steps** later (you clicked early) or earlier (you clicked late), and uses the nearest timing that survives. If you die before clicking at all, the death is held back for N steps in case your click lands in that window.

1 step = 1/240 s ≈ 4.2 ms. The default of 4 steps is about ±17 ms.

Safe mode is built in: while the buffer is on in a level, completions and new-best percentages aren't saved, and a label in the top-left corner shows it's running.

---

## Building it (no compiler needed)

GitHub builds the mod for you. You only need a free GitHub account.

1. **Make a repository.** On github.com, click **New repository**. Any name; Private is fine. Tick nothing else, then **Create repository**.
2. **Upload the files.** On the new repo page, click **uploading an existing file**. Unzip `input-buffer.zip`, then drag *everything inside* the `input-buffer` folder into the browser: `src`, `.github`, `mod.json`, `CMakeLists.txt`, `about.md`, `logo.png`, and the rest. Click **Commit changes**.
   - Check that `.github/workflows/build.yml` shows up in the repo. Without it, nothing builds.
3. **Let it build.** Open the **Actions** tab. A run called *Build Input Buffer* starts by itself; if it doesn't, click it on the left and press **Run workflow**. It takes about 5–10 minutes.
4. **Download.** Open the finished run (green tick). Under **Artifacts**, download **input-buffer**. Unzip it to get `local.input-buffer.geode`.
5. **Install.** In Steam, right-click Geometry Dash → **Manage** → **Browse local files**. Put the `.geode` file in `geode/mods`, then start the game.
   - Geode itself must be **v5.11.2 or newer**. It updates itself; you can check the version in the Geode menu.

If a build fails (red X), open the run, copy the error from the log, and send it to me.

### Building on your own PC instead (optional)

1. Install **Visual Studio 2022 Build Tools** with the *Desktop development with C++* workload, plus **CMake** and **Git**.
2. Install the **Geode CLI** (see docs.geode-sdk.org → Getting Started), then run `geode sdk install` and `geode sdk install-binaries`.
3. In this folder, run `geode build`. The `.geode` file lands in `build/`.

---

## Using it

- **Mod settings** (Geode menu → Input Buffer → gear icon): Enabled, Buffer steps (0–16), Lookahead, Debug log.
- **Keybinds** (in-game): `F8` toggles the buffer, `]` adds a step, `[` removes one. You can rebind these in Geode's keybind settings.
- The label in the corner shows the current step count.
- Works in classic levels (normal and practice). It switches off in platformer and 2-player levels.

## Testing it — please send me feedback

This is a first version. I could compile-check it against the 2.2081 headers, but I can't run Geometry Dash here, so expect a round or two of fixes.

1. Turn on **Debug log** in the mod settings.
2. Turn **off** Click Between Frames, or any other input mod, while testing.
3. Try a known 1-frame spot with 4 steps, then with 0 steps, and compare.
4. If something feels wrong, send me two things: what happened (for example, "jumped late, still died"), and the latest log file from `geode/logs` in the game folder.

Each decision is logged, for example:
```
[IB] step 1532: press early -> delayed 2 step(s)
[IB] step 2210: press late -> moved back 1 step(s)
[IB] step 3005: death held back, waiting up to 4 step(s) for a click
```

## Known limitations (v0.1)

- **Moving objects are frozen in predictions.** Each prediction treats the level as frozen at the current moment. That's accurate for static spikes and blocks, less so for fast-moving objects.
- **Gamemode portals end a prediction.** If a gamemode portal falls inside the prediction window, that prediction stops there and counts as a pass.
- **Rewinds move only the player.** The level keeps going; for the few milliseconds involved, this doesn't matter.
- **Possible flicker.** A predicted orb or pad hit might occasionally flash its effect.
