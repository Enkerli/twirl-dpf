# Twirl as a plugin (DPF)

[Twirl](https://enkerli.github.io/music-suite/apps/twirl/), the gesture toy
from [music-suite](https://github.com/Enkerli/music-suite), as an **LV2,
CLAP and VST3 plugin and a JACK standalone**, with no JUCE: built on
[DPF](https://github.com/DISTRHO/DPF) (`develop`) and its web view. You
draw in Twirl's own page; out of the plugin come **Twirl's sound** (stereo,
made in the plugin from the page's notes, all seven sound packs, the Drone
and Warmth), its notes as **MIDI** for the host's instruments (Surge XT, a
drum machine, anything), or both (the default): the page's Out picker
(Everything → Sound and room). Beside it, **Follow host
transport** takes the host's tempo, plays while its transport plays and on
its beats.

**Twirl keeps playing with its window closed**, and its sheet, Out choice
and Follow host transport are **saved with the host's project**. Twenty-nine of
its controls are **host parameters**, automatable (below), among them a
level and a mute for each of four groups of loops.

Twirl's page isn't forked: `make` copies `apps/twirl/index.html` from a
music-suite checkout beside this one (or `MUSIC_SUITE=…`), else downloads it
from music-suite's `main`.

## Build (Linux)

```sh
sudo apt install build-essential pkg-config libgl1-mesa-dev libx11-dev libxext-dev \
  libxrandr-dev libxcursor-dev libdbus-1-dev libjack-jackd2-dev libgtk-3-dev libwebkit2gtk-4.1-dev
git clone --recursive https://github.com/Enkerli/twirl-dpf
cd twirl-dpf
make            # bin/: twirl (JACK), twirl.lv2, twirl.clap, twirl.vst3
make install    # into ~/.lv2, ~/.clap, ~/.vst3
pw-jack bin/twirl   # the standalone (pw-jack with PipeWire)
```

`make` applies `patches/` to DPF first (fixes not yet upstream):
- `0001`: on Linux, Bitwig gives a CLAP the path of its binary, and DPF
  looked for `resources/` one folder too deep.
- `0002`: closing the view, the web view's child process called into Qt from
  its SIGTERM handler; it hung until SIGKILL (2 s) or crashed (SIGSEGV in
  Qt's exit handlers, as Bitwig showed). It now leaves with `_exit(0)`.
- `0003`: keyboard focus. The web view is a window of another process inside
  the plugin's; a click left X's focus on the host's window, so keys never
  reached the page. Now a press in the page (DPF's initial JS) has the web
  view take the focus.
- `0004`: a hidden CLAP window kept its page running but stopped the timer
  that collects the page's messages, so nothing reached the plugin (Bitwig
  hides plugin windows when another device's opens). The timer now runs
  until the UI is destroyed. (Twirl no longer depends on it: see below.)

DPF's web view uses Qt WebEngine where it finds it (Qt 5, then 6), else
WebKitGTK. It runs in its own process over X11 (XWayland is fine; for
`jalv.gtk3`, set `GDK_BACKEND=x11`).

## The page belongs to the plugin, not to its window

DPF's web view normally lives in the UI: close the window and the page goes,
with Twirl's clock, its loops and its sheet. Here (`TwirlPage.hpp`) the
plugin starts the page itself when it's activated, in a window that's never
shown, and keeps it for its whole life. The UI is an empty window: opening,
it moves the page's window into itself; closing, it moves it back out before
the host destroys it. The page's messages come to the plugin directly (a
thread collects them every 10 ms), not through the host's UI-to-plugin
channel, so this needs the plugin and its UI in one process (DPF's direct
access; the LV2 is a single binary).

Two settings go to the web view's process alone, while it starts:
`--autoplay-policy=no-user-gesture-required` (the page starts its sound by
itself, its window perhaps never opened), and `QT_XCB_GL_INTEGRATION=none`:
with GL (Mesa, DRI3), a view that isn't on screen waits forever in its
buffer swap, and everything on that thread stops, the page's messages
included. Without GL, Twirl measured 60 fps.

Checked in Carla's engine (the LV2): Twirl plays from the start with no
window; open, close and reopen the window and it's the same page, still
playing.

## Saved with the project

The page sends its state as `setState("page", base64 JSON)` (its sheet, Out,
Follow host transport) when it autosaves (every 4 s) and when Out or the
toggle change; the plugin keeps it as its one state and gives it back when a
project loads (`twirlRestore`). Checked in Carla: saved from one instance,
loaded into a fresh one, the tempo and Out came back.

## Scenes, My toys and the look: shared files

In a browser, Twirl keeps its scene slots, My toys and how it looks (the
level, Numbers, Beats…) in the browser's storage. The plugin's web view
has none that lasts (Qt 6's default profile is off the record) and none
shared between instances, so the plugin keeps them as files instead
(`TwirlFiles.hpp`), every instance's: `$XDG_DATA_HOME/twirl`, else
`~/.local/share/twirl`: `scenes/Scene 1.json`…, `My toys.json`,
`settings/….json`, the page's own JSON. The page gets them all before it
starts (its initial JavaScript), saves with `setState("file", "path\n…")`
(written whole: a temporary file, renamed), and hears of other instances'
saves within a second (the plugin looks for changes). A scene file's text
pastes into Scenes → Paste a code, in the plugin or a browser: a way to
share one. The sheet itself stays each instance's own, saved with the
project. Checked in Carla: a save lands on disk, another instance's file is
picked up, and a fresh instance starts with them.

## Following the host

`run()` records the host's tempo, whether it's rolling, and its beat at
that block's wall-clock time; the plugin hands this to the page 20 times a
second (`twirlHost`). With **Follow host
transport** on, Twirl takes the tempo, holds its loops (and the Drone) while
the transport is stopped (strokes still sound as you draw), and keeps its
beat on the host's, modulo 48 beats (whole bars in 2/4, 3/4, 4/4, 6/8):
jumping when the host starts or moves; otherwise averaging the error over
about a second and correcting it only past 2 ms (each report's measure is
noisy, ±10 ms; correcting every one moved every note by a few ms, which
sounded unquantized). While the host is stopped, gives no transport, or
stops calling the plugin, Twirl's clock stands still. The sound and the MIDI
are both played at their exact times, so they agree with each other and with
the host.

Measured against Carla's transport at 120 BPM: Twirl's timing moves 0.02–0.08
ms per 100 ms on average (about 1 ms at most), and sits on the host's beat
within about 1 ms on average; follows a tempo change and a jump; held while
stopped. The page's clock and the plugin's agree (host reports arrive 12–37
ms after they're stamped).

## Channels

As in Twirl's Web MIDI output: **pluck 1, bell 2, blip 3, drums and shaker
10** (General MIDI: kick 36, snare 38, hat 42, shaker 70), and **the Drone
4** (its notes held while it plays, changing as Walk moves them). Note
lengths: drums 80 ms, pluck and blip 220, bell 600.

## Twirl's sound, made in the plugin

`TwirlEngine.hpp`: the page sends each note as it would have played it
(voice, sound pack, exact pitch, velocity, pan, height, Vary's nuance), timed
like the MIDI (`setState("voices", …)`), the volume, tempo and Warmth
(`"engine"`), and the Drone (`"drone"`: its notes as Walk moves them, its
level and space); the engine plays the notes at their sample offsets, so the
sound is in step with the MIDI and the host. Microtonal pitches stay exact
(the MIDI rounds them).

It's built from what Twirl's Web Audio recipes use: band-limited
oscillators, Web Audio's biquads and ramps, its panner, the feedback delay, a
compressor; struck tones as sines from rotating phasors. All seven packs
(Paper, Mallets, Felt, Steel, Kalimba, Chip, Chimes; a voice a pack lacks is
Paper's), the Drone (saw pairs, a breathing lowpass, the space's rotor, tape
wobble) and Warmth's room (hiss, crackle, wow and flutter). A pool of 384
voices, no allocation while playing. The bar (Alex): "Doesn't even need to
sound that close to the current one, as long as it sounds ok."

`make test` renders every pack's every voice at 8 pitches and 4 loudnesses,
offline (with the plugin's own compiler flags), and fails if one is silent,
clips, isn't finite or doesn't die away; the Drone (swells, glides to a new
chord, fades out when off) and Warmth (about -60 dB, silent at 0) too; and
a busy sheet in the heaviest pack (Chimes, 50 notes a second, ~120 voices)
must take under a quarter of one core (it takes about 11 %). It writes
`build/render/*.wav` for listening.

**Synth mode.** MIDI coming into the plugin is played by the engine too,
when Out includes sound: channel 1 pluck, 2 bell, 3 blip (these two an
octave down, as Twirl sends them an octave up), 10 drums and shaker (36
kick, 38 snare, 42 hat, 70 shaker, and General MIDI's neighbours by kind),
any other channel pluck but 4 (Twirl's Drone, held notes, which Twirl now
also sends as MIDI: left alone for now); in the sheet's pack, at the event's frame, window
open or not. Note-offs are ignored: Twirl's voices are one-shots. It still
passes through to the MIDI out. So one Twirl's MIDI into another plays its
notes in the second's sound. Checked with two standalones: B, with no loops
of its own, played A's notes in Kalimba while connected, and only Warmth's
hiss once disconnected.

**Incoming MIDI only** (Everything → Sound and room, and a host parameter):
the sheet stays silent (every loop as if muted, no meetings; their passes go
on, so it comes back in step) and Twirl is an instrument: it plays only what
comes in, in its sheet's sounds (pack, Warmth, the Drone). A second Twirl
set so, on channel 10, is a drum kit in one instance, in any pack. Without
it, the receiving Twirl's own sheet plays too (Alex, in Bitwig: channel 10
into a second Twirl "played its own scene instead of playing the drum
notes"). ⇧Q's report shows the note-ons that came in, by channel (`midiIn`),
to see what the host really sends. Checked in Carla (LV2): the parameter
silences the sheet and back, eight channel-10 notes counted as `ch10: 8`.

Until 2026-10-26 the page streamed its own Web Audio into the plugin
instead (exact, but ~120 ms late, through a jitter buffer); that left the
plugin (Alex: "The option should leave the plugin (kept in the web
versions)").

## Parameters

Twenty-nine of the page's controls are the host's parameters, automatable
and saved with the project (`TwirlParams.hpp`): Volume, Tempo, Follow host
transport, Swing, Warmth, Air, Drone (on, level, voicing, space), Lock,
Walk (on, way, pace), Sounds (the pack), Scale, Root, Vary, Evolve, Morph,
Incoming MIDI only, and the level and mute of Groups A to D (a loop joins
one in its settings; the group's level multiplies its loops' volume).
Choices are lists with their names, switches on/off. The page holds the same
list in the same order (`HOST_PARAMS`); both append only, as hosts keep
parameters by number.

- **Host → Twirl:** the plugin passes each change to the page within 10 ms,
  window open or not; the page applies it as if its control had moved (so
  the sound follows: the pack, the Drone…). Tempo does nothing while Follow
  host transport is on.
- **Twirl → host:** whatever changes a control in the page (a control, a
  key, a scene, Start over) is reported to the plugin (checked 20 times a
  second), and the plugin's window tells the host, each as a gesture, as a
  knob would (DPF's `editParameter` and `setParameterValue`: the route that
  works in every format; DPF's CLAP plugin-side change request is a stub).
  That needs the window open: changes made while it's closed go when it next
  opens. For 300 ms after the window tells the host a value, the host's
  echoes of it are ignored (a slider being dragged doesn't jump back).
- **With the project:** the page's saved state keeps the sheet; the plugin
  sends the page all the parameters once it's ready and after any restore,
  so **the parameters win**. (A project saved before the parameters existed
  gets their defaults.)

Checked in Carla (LV2 and VST3): the host sets them and the page follows
(Kalimba, Dorian on G, the Drone and Walk on); with the window open, the
page's own changes (tempo, pack) reach the host; saved, then loaded into a
fresh instance whose tempo had been changed, the saved parameters came back.
In VST3, DPF lists its 2,080 MIDI CC parameters (16 channels × 130) before
Twirl's twenty.

## How the notes get to the host on time

Twirl keeps its own clock in the page and schedules ahead (about 100 ms).
The page has `setState()` (as DPF's web view UIs do); Twirl sends each scheduler tick's notes as
one message, `setState("notes", "t,status,note,velocity;…")`, `t` being the
wall-clock time the event should sound at (ms since the epoch). The plugin
parses them into a lock-free ring; `run()` maps each block onto the wall
clock (smoothed against the sample count) and writes every event at its
sample offset. Late events play at the block's start; anything over a second
old is dropped. `setState("panic", …)` ends whatever is sounding (Twirl's
Start over, the page closing). MIDI coming into the plugin passes through.

Measured on Linux (PipeWire, 48 kHz, 1024-frame buffer), the JACK standalone
and the LV2 in `jalv`: every event within 0.1 ms of Twirl's own timing, sent
60–140 ms ahead of when it sounds.

## Limits, for now

- One browser engine per plugin instance, running from activation, window
  open or not.
- The plugin and its UI must share a process (DPF's direct access); hosts
  that run plugin UIs apart from the plugin won't show the page.
- The page tells the host about its own changes only while the window is
  open (they go when it next opens).
- Scene files are read when an instance starts and whenever one changes;
  two instances saving the same slot in the same second: the last wins.
- Drums are General MIDI (kick 36, snare 38, hat 42, shaker 70): Bitwig's
  drum machine needs a note map for now.

## License

CC0-1.0, like Twirl. DPF is ISC.
