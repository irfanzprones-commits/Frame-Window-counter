# GDFWR - Frame Window Recorder (Geode mod)

Target: **Geometry Dash 2.2081**, **Geode SDK v5.11.2** (newest stable at the time of writing, 15 Oct 2026; it still targets 2.2081).
Geode v6 is only an alpha for GD 2.209x, so it is NOT used. Language: C++23.
Concept and specification: the owner. Specification review and this implementation: Claude.

## Build
```
geode sdk update            # make sure the SDK is v5.11.2 or newer v5.x:  geode sdk version
geode sdk install-binaries
geode build                 # or: cmake + GEODE_SDK, see CMakeLists.txt
```
Before publishing, change `id` and `developer` in `mod.json` (`yourname.gdfwr`).

## What was tested, and what was not (please read)
| Part | Code | Status |
|---|---|---|
| Recording model, segments, bookmarks, final run, junction rule, validation, click edits (Part 1) | `src/core/Session.*` | **Compiled and unit-tested** (271 checks, also under ASan/UBSan) |
| `.gdfwr` file: sections, CRC32, stale rule, damaged / newer file, atomic save + `.bak` (Parts 2, 8.4, 8.5) | `src/core/FileIO.*` | **Compiled and unit-tested**, parsers fuzzed with 20 000 mutated files |
| Analysis engine: walk, clamps, fast + full scan, long search, bisection, holes/traps, clusters, drift, flags, summary, cancel (Part 4) | `src/core/Analysis.*` | **Compiled and unit-tested** against a mock game, including the worked example of the spec (old rule) and the per-action rule |
| Import / export, GDR2 reader + writer, cleaning rules, loss list (Part 8.6, 8.7) | `src/core/Formats.*` | **Compiled and unit-tested** (I-2, I-3, I-4 style cases, lossless round trip) |
| Replay buckets, rings plan, precision model (Part 9) | `src/core/Metrics.*` | **Compiled and unit-tested** (precision model is provisional, see below) |
| Game hooks, game bridge, real-game simulator, verification pass, replay, UI | `src/game/*`, `src/ui/*` | **Written, NOT compiled against the real SDK and NOT run in the game.** Syntax-checked against a stub (`tests/syntax_check.sh`); the verification / auto-repair state machine is also run against a fake game (`tests/verify_sim.sh`), which proves our logic but nothing about Geometry Dash itself. |

The Geode SDK could not be installed in the environment where this was written (no network for the build tools), so the
last row is the honest gap. Expect a few compile errors on the first `geode build`; they will be small and local.

### Check these first if the build or the first run misbehaves
Everything that depends on a GD 2.2081 / Geode binding name is marked `[BINDING]`:
1. `Bridge::tickOf` - tick = `m_gameState.m_levelTime * 240` (read inside `processCommands`, before the original runs).
2. `Bridge::holding` - `PlayerObject::m_holdingButtons[Jump]`.
3. `Bridge::stepTick` - one physics tick = `processCommands(1/240)` + `checkForEnd()` (the end test normally lives in `postUpdate`), or the game's
   own `update(1/240)`. Nobody has confirmed which of the two really moves the player in 2.2081, so `Bridge::calibrateStepper` tries both (and
   `PlayLayer::startGame()`, which the game normally calls a moment after a restart) for 60 ticks before every verification and keeps the one
   that moves the player. The result is in the failure message ("stepper: ...") and in the Geode log (`GDFWR: stepper check`).
   The END of the level in our own run (`Bridge::levelEndReason`, `PlayLayer::playEndAnimationToPos` hook): the game calls `levelComplete()` only
   some real time after the end animation starts, so the simulation never waits for it. Signs used: the `playEndAnimationToPos` / `levelComplete`
   hooks, `GJBaseGameLayer::m_levelEndAnimationStarted`, and the player reaching `GJBaseGameLayer::m_endPortal`. A failed verification prints the
   player x and the portal x, and every verification writes one line to the Geode log (`GDFWR: verification ...`).
4. `GameSimulator::run` - `createCheckpoint()` / `loadFromCheckpoint()` and setting `m_levelTime` after the load.
5. Hook signatures in `Hooks.cpp`: `removeCheckpoint(bool)`, `storeCheckpoint(CheckpointObject*)`, `pauseGame(bool)`, `resume()`, `PauseLayer::customSetup/onQuit`.
6. Geode v5 UI/API: `Popup::init(w,h)`, virtual `Popup::onClose`, `addChildAtPosition(node, Anchor, offset, bool)`, `file::pick` + `async::spawn`, `listenForKeybindSettingPresses`, the keybind default format in `mod.json`, the 4-argument `ButtonSprite::create`.
7. `GJGameLevel::m_levelVersion`, `m_levelID.value()`, `m_levelString` (level identity and hash), random-trigger object IDs 1912 / 2068.
8. (0.2.1, optional) `PlayLayer::m_anticheatSpike`: `Bridge::isAnticheatSpike`. Written so that it compiles without the member (it then answers "no"). If `Analyze`
   dies at tick 2 even now, the failure message names the object that killed the player; send that line and the Geode log lines `GDFWR: stepper check` / `GDFWR: death at tick`.

### The one real risk of the design
Verification and analysis restore game states with the game's own practice checkpoints. If 2.2081 checkpoints do not restore
some internal field, the "offset 0" check (Analysis 4.10) no longer flags those clicks `UNRELIABLE` (0.3.0): `GameSimulator` marks the
saved state as untrusted and measures what starts there by replaying the level from tick 0 (slower, exact), and the result says
`RESTORE_REPLAYED`. If that happens a lot, extend `GameSimulator::runOnce` (state restore) and `Bridge::stateHash`; the engine does not change.

## What changed in 0.3.0 (Analyze reworked)
Reported from a real run: a tight jump recorded at 14-29 FPS. Analyze said "Macro repaired" (190 -> 192, on a second recording 187 -> 190), then showed
`none` for every window and grey `?` rings. Findings, and what was changed. **None of this was run in Geometry Dash**: the logic is tested against the fake game.
- **Why everything was `none`.** A recording made at a low frame rate gets the warnings COARSE_INPUT and UNRELIABLE_TICK, and `makeAnalysisInput`
  marked every click near such a flag `UNRELIABLE`, which is never counted (and gives a grey `?` ring), even though the same popup said "the windows are still
  measured correctly". Now the windows are measured by REPLAYING the level, so the flags of the RECORDING no longer exclude a click (setting "Count clicks of a recording
  with a flagged tick counter", on by default; off = the old rule). `UNRELIABLE` now only means that the replay itself could not be reproduced.
- **Why 190 became 192.** At 14-29 FPS one frame is 8-17 ticks and the recorder can only store the tick at the start of the frame, so a recorded click can be that far from
  the tick the game used. The fit moves a click that does not pass by the smallest shift that gets past the failure (+1, -1, +2, -2 ...; it used to try ten later shifts before the
  first earlier one), searches at least one recorded frame each way, no longer gives up after 5 clicks, and the result lists every moved click as `recorded -> now`
  (and the recorded ticks next to each row). No "Macro repaired" dialog, no question before Analyze; `Edit clicks > Restore all` still brings the recorded ticks back.
- **Saved states that do not restore exactly** no longer ruin a click: see "The one real risk" above.
- **Faster**: a run that is back on the verified run for 4 ticks stops there (setting "Stop a run that is back on the verified run"; 30273 -> 3372 simulated ticks in test T24
  with identical windows), the second tick-by-tick scan of the 20 tightest clicks is off by default (setting "Tick-by-tick scan of the tightest clicks"), and the progress popup
  gives the work about half of each frame (8-40 ms) instead of a fixed 8 ms, which was a tenth of the time at 14 FPS.
- **Rings**: no growing animation; a ring appears at full strength, stays fully visible for 1 s and fades out over 2 s (settings "Ring: seconds fully visible / of fading"). It is drawn
  on the screen where the player was when it clicked, as a child of the replay HUD: before, it lived in the level (scrolled out of the screen) and a ring that was still alive when
  the replay stopped or the level restarted stayed behind in the level for good. A release that never mattered shows a grey `any`, a click that could not be measured a grey `?`.

## What changed in 0.2.0 (analysis rework)
- **Per-action windows**: the press window moves only the press, the release window only the release (old rule: setting "Press window moves the release too").
- **Width, not only a count**: each window stores where the surviving timings end on each side. Whole-tick runs give "between n-1 and n+1 ticks"; the engine narrows it to 1/16 tick by bisection when the simulator can place an input between two steps. **`GameSimulator` cannot yet**, so this is covered by the mock tests only. Implementing it means splitting the physics step the way click-between-frames does ([BINDING], not attempted blind).
- **Long search** (setting "Search beyond 12 ticks"): steps of 4 after the map, then bisection. Off by default.
- **Nine replay buckets** (1, 2, 3, 4, 5-6, 7-8, 9-12, 13-24, 25+) and rings up to 99.
- **File**: MACR v2, ANAL v2, old files still load.
- **Bounded memory**: long macros are measured in parts (replay from tick 0 per part, only that part's states kept) instead of being refused; see the changelog for why this is not an inline ring buffer.
- **Still open**: sub-tick input in the real game and a precision model confirmed against NaN's calculator.

## Decisions I had to make (all visible in the code)
- **Versions**: Geode v5.11.2, not 5.11.1 and not the v6 alpha.
- **GDR**: GDR 2 (binary, magic `GDR`, layout from the GDReplayFormat readme) is read and written. GDR 1 (msgpack/JSON) is **not** supported. The writer version byte `kGdr2WriterVersion = 2` and the packed-input varint continuation are from the readme and must be confirmed with a real file (spec test I-1).
- **xdBot**: **not enabled.** Its layout is not publicly documented and the spec says "do not guess". `XdBotReader` is a stub; send me a sample `.xd` file or its spec and it is a small addition.
- **Frame offset per format (D9)** is `0` until test I-5 is measured on a real file.
- **Precision readout** ("Raw Precision (24h) in sigma/s") uses my reading of Appendix A item 5: sigma = 1/L seconds, p_i = erf(z/sqrt2) with z = N_i/240/2 * L, an attempt costs the full level time, L solved for 24 h. It is labelled provisional in the settings and must be confirmed before you trust the number.
- Replay numbers come from the stored ANALYSIS (must be CURRENT); windows above 12 land in bucket 9-12 and the ring shows `N+`; unreliable / random / not-analysed windows get a grey `?` ring and are not counted; a release that never mattered gets a grey `any` ring (not counted).
- **Replay needs only a macro** (no analysis, no verification): the button is enabled whenever the file has a macro and grey when it has none. Rings carry numbers only when the analysis is CURRENT and the file VERIFIED; otherwise they are grey.
- **Fit (was auto-repair)**: if the player dies during the check, the click to blame (the one holding at that moment, then the next one) is moved by the smallest shift (+1, -1, +2, -2 ... up to the setting, but at least one recorded frame) and the run is tried again; the first shift that gets past the failure is kept and the file becomes EDITED. If nothing helps the file is not changed. See the settings "Fit the clicks to the game when the macro does not pass" and "Fit: largest shift".
- There is **no Verify button**. **Analyze** is one process: 1. check the macro from start to end (verification, which also saves the game states the analysis starts from), 2. measure every click (Appendix A item 8). An imported file (PARTIAL until a verification reaches the end of the level) is analysed the same way and becomes COMPLETED when step 1 passes.
- A synthetic **last release** (import or pause fix) is treated as "run ends while holding": release window N/A; the press window shifts the press only and cannot pass the end of the level.
- Starting a level with the button already held adds a synthetic press at tick 0 (flagged `SYNTHETIC_EVENTS`).
- Click **source** is stored as `other`: the game-level hook cannot tell keyboard from mouse.
- Panels are full screen (Part 10); every position is a fraction of the window, so everything scales with it. Import, Analyze and Replay need the level open; Save, Save As, Load and Export work anywhere.

## Not in v1 (core is ready where noted)
- Platformer levels, TPS other than 240, seed / random-branch capture (levels with random triggers get `RANDOM` flags instead).
- Early-out on equal state (4.5), chained check (4.12 optional), memoisation (4.15): the engine has the hooks, they are off.
- Platforms: Windows, macOS, iOS, Android 32 and 64 are all built by `.github/workflows/build.yml`. Nothing in `src/` is platform-specific.
- UI to request an extra full scan for a chosen click: `AnalysisParams::selectedClicks` exists, no button yet.

## Layout
```
src/core/   Geode-free C++: model, session, file, analysis, formats, metrics   (tests/test_core.cpp)
src/game/   hooks, bridge to the game, jobs (verify / analyze / replay)
src/ui/     panel, click editor, overlays, result view
tests/      run.sh (core tests), verify_sim.sh (verification / fit / analysis jobs vs. a fake game), syntax_check.sh (glue vs. stub), syntax-stubs/ (NOT the SDK)
```
Run the core tests anywhere with a C++23 compiler: `./tests/run.sh`.
