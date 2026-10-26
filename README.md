# Twirl as a plugin (DPF)

[Twirl](https://enkerli.github.io/music-suite/apps/twirl/), the gesture toy
from [music-suite](https://github.com/Enkerli/music-suite), as an **LV2,
CLAP and VST3 plugin and a JACK standalone**, with no JUCE: built on
[DPF](https://github.com/DISTRHO/DPF) (`develop`) and its web view. You
draw in Twirl's own page; out of the plugin come **Twirl's own sound**
(stereo, exactly as in a browser), its notes as **MIDI** for the host's
instruments (Surge XT, a drum machine, anything), or both: the page's Out
picker (Everything → Sound and room).

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
  until the UI is destroyed.

DPF's web view uses Qt WebEngine where it finds it (Qt 5, then 6), else
WebKitGTK. It runs in its own process over X11 (XWayland is fine; for
`jalv.gtk3`, set `GDK_BACKEND=x11`).

## Channels

As in Twirl's Web MIDI output: **pluck 1, bell 2, blip 3, drums and shaker
10** (General MIDI: kick 36, snare 38, hat 42, shaker 70). Note lengths:
drums 80 ms, pluck and blip 220, bell 600.

## How Twirl's sound gets to the host

The page renders it with Web Audio, as in a browser; an AudioWorklet at the
end of its graph sends it on in 512-frame chunks (16-bit stereo, base64,
`setState("audio", "rate,base64")`), about 94 a second, and plays silence to
the speakers. The plugin buffers it against uneven arrival (100 ms to start;
more only after a short gap, which a bigger buffer would have covered) and
reads it back at a ratio nudged by at most ±0.5 % to hold that level, which
absorbs the drift between the page's audio clock and the host's. After a
stall, it dips for 3 ms rather than click, and catches up. So **the sound
lags the MIDI by about 100 ms**.

`TWIRL_DEBUG=1` in the environment prints the buffer's state every 5 s.

Measured (the JACK standalone, PipeWire, 48 kHz): no gaps and no clicks over
20 s; with the page's main thread stalled 300 ms every 5 s, a gap per stall
(as expected), no clicks, and the buffer back to 100 ms each time.

## How the notes get to the host on time

Twirl keeps its own clock in the page and schedules ahead (about 100 ms).
DPF gives the page `setState()`; Twirl sends each scheduler tick's notes as
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

- Twirl's clock runs in the page: **close the plugin's window and Twirl
  stops** (whatever was already sent still plays out). Hiding it is fine.
- The sheet isn't saved with the host's project yet.
- The host's tempo and transport aren't followed: Twirl has its own.
- Drums are General MIDI (kick 36, snare 38, hat 42, shaker 70).

## License

CC0-1.0, like Twirl. DPF is ISC.
