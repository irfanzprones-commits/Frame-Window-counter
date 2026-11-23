# GDFWR - Frame Window Recorder

Records a run, **proves it by replay**, and measures the **frame window** of every click: how many ticks earlier or later each press and each release could have happened without dying.

- **Record** in Practice Mode with checkpoints. The mod keeps every segment and builds the *final run* of the attempt that completed the level.
- **Analyze** is one process: it first replays the macro at full speed from start to end to check that it survives (if the player dies, the click to blame is moved +1..+10 and then -1..-10 ticks until the run passes), then moves **each press on its own and each release on its own** by up to 12 ticks (optionally further, see *Search beyond 12 ticks*), clusters of close clicks too, and reports the window, pass maps, drift and flags.
- **Replay** plays the macro in the level so you can watch whether it passes. It works as soon as a macro exists; after an analysis it also shows a ring with the window at every action, bucket counters on the left and a precision readout.
- **File panel**: Save, Save As, Load, Import and Export (GDR2).

Open the panel from the pause menu (GDFWR button) or with `Ctrl+Shift+G`.
