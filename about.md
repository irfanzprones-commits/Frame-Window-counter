<c-ff4d6d>G</c><c-ffbf3c>D</c><c-9df074>F</c><c-3ba5e9>W</c><c-a855f7>R</c>
<c-11f917>F</c><c-12f727>r</c><c-12f537>a</c><c-12f346>m</c><c-13f156>e</c> <c-14ef66>W</c><c-14ed76>i</c><c-1be78d>n</c><c-21e1a4>d</c><c-28daba>o</c><c-2ed4d1>w</c> <c-35cee8>R</c><c-3bc8ff>e</c><c-45bbff>c</c><c-4fadff>o</c><c-5aa0ff>r</c><c-6493ff>d</c><c-6e85ff>e</c><c-7878ff>r</c>

Records a run, <cj>proves it by replay</c>, and measures the <cp>frame window</c> of every click: how many ticks earlier or later each <ca>press</c> and each <cy>release</c> could have happened without dying. Built around the way <cd>NaN GD</c> counts frame windows, at <cg>240 ticks per second</c>.

<cg>Features</c>
Records in <cj>Practice Mode</c> with checkpoints and builds the <cp>final run</c> of the attempt that completed the level
<ca>Analyze</c> in one go: checks that the macro survives, then measures every click
<cy>Every press and every release has its own window</c>, plus the width in ticks
<cp>Long search</c> for wide windows (optional, up to 120 ticks)
<cj>Long macros are measured in parts</c>, so memory stays small on phones
<cg>Auto-repair</c> moves the click to blame until a failed run passes
<ca>Replay</c> with a ring at every action, <cd>nine colour buckets</c> and a precision readout
<cy>Save, Load, Import and Export</c> (GDR2) in the File panel
Clusters of close clicks, pass maps, drift and flags
Windows, macOS, iOS and Android, <cp>GD 2.2081</c>
<ca>How to use</c>
Open the panel from the pause menu (<cy>GDFWR</c> button) or with <cj>Ctrl+Shift+G</c>
Record a run in Practice Mode, or Import a GDR2 macro
Press <cg>Analyze</c> and wait for the result
Press <cp>Replay</c> to watch the rings and the bucket counters
<cr>Good to know</c>
Hotkeys: <cj>Ctrl+Shift+R</c> start / stop recording, <cj>Ctrl+Shift+P</c> pause / resume
The precision readout is <co>provisional</c>
<co>Experimental</c>: please report anything odd together with the Geode log
