# Twirl as a plugin (DPF)

[Twirl](https://enkerli.github.io/music-suite/apps/twirl/), the gesture toy
from [music-suite](https://github.com/Enkerli/music-suite), as an **LV2,
CLAP and VST3 plugin and a JACK standalone**, with no JUCE: built on
[DPF](https://github.com/DISTRHO/DPF) (`develop`) and its web view. You
draw in Twirl's own page; out of the plugin come **Twirl's own sound**
(stereo, exactly as in a browser), its notes as **MIDI** for the host's
instruments (Surge XT, a drum machine, anything), or both (the default): the
page's Out picker (Everything → Sound and room). Beside it, **Follow host
transport** takes the host's tempo, plays while its transport plays and on
its beats.

**Twirl keeps playing with its window closed**, and its sheet, Out choice
and Follow host transport are **saved with the host's project**.

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

## Following the host

`run()` records the host's tempo, whether it's rolling, and its beat at
that block's wall-clock time; the plugin hands this to the page 20 times a
second (`twirlHost`), with the measured audio lag. With **Follow host
transport** on, Twirl takes the tempo, holds its loops (and the Drone) while
the transport is stopped (strokes still sound as you draw), and keeps its
beat on the host's, modulo 48 beats (whole bars in 2/4, 3/4, 4/4, 6/8):
jumping when the host starts or moves, easing a quarter of the way each
report otherwise. With its own sound going to the host, it plays early by
the audio lag, so the sound lands on the beat; with sound and MIDI, the MIDI
is delayed by the same, so the two agree.

Measured against Carla's transport: on the host's beat within 0.3–2 ms at
120 and 140 BPM and after a jump; held while stopped. The page's clock and
the plugin's agree (host reports arrive 12–37 ms after they're stamped).

## Channels

As in Twirl's Web MIDI output: **pluck 1, bell 2, blip 3, drums and shaker
10** (General MIDI: kick 36, snare 38, hat 42, shaker 70). Note lengths:
drums 80 ms, pluck and blip 220, bell 600.

## How Twirl's sound gets to the host

The page renders it with Web Audio, as in a browser; an AudioWorklet at the
end of its graph sends it on in 512-frame chunks (16-bit stereo, base64,
`setState("audio", "rate,wall,base64")`, `wall` when its first frame was
rendered), about 94 a second, and plays silence to the speakers. The plugin buffers it against uneven arrival (100 ms to start;
more only after a short gap, which a bigger buffer would have covered) and
reads it back at a ratio nudged by at most ±0.5 % to hold that level, which
absorbs the drift between the page's audio clock and the host's. After a
stall, it dips for 3 ms rather than click, and catches up. The sound reaches
the host about 100–120 ms after it was rendered: the plugin measures this
from the chunks' times, and the page allows for it (above).

`TWIRL_DEBUG=1` in the environment prints the buffer's state every 5 s.

Measured (the JACK standalone, PipeWire, 48 kHz): no gaps and no clicks over
20 s; with the page's main thread stalled 300 ms every 5 s, a gap per stall
(as expected), no clicks, and the buffer back to 100 ms each time.

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
- Follow host transport is saved with the project but isn't a host
  parameter (not automatable).
- Drums are General MIDI (kick 36, snare 38, hat 42, shaker 70): Bitwig's
  drum machine needs a note map for now.

## License

CC0-1.0, like Twirl. DPF is ISC.
