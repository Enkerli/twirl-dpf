# Twirl as a plugin (DPF)

[Twirl](https://enkerli.github.io/music-suite/apps/twirl/), the gesture toy
from [music-suite](https://github.com/Enkerli/music-suite), as an **LV2,
CLAP and VST3 plugin and a JACK standalone**, with no JUCE: built on
[DPF](https://github.com/DISTRHO/DPF) (`develop`) and its web view. You
draw in Twirl's own page, and its notes come out as **MIDI** for the host's
instruments (Surge XT, a GM soundfont, anything). Twirl itself makes no
sound here.

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

`make` applies `patches/` to DPF first (a fix not yet upstream: on Linux,
Bitwig gives a CLAP the path of its binary, and DPF looked for `resources/`
one folder too deep).

DPF's web view uses Qt WebEngine where it finds it (Qt 5, then 6), else
WebKitGTK. It runs in its own process over X11 (XWayland is fine; for
`jalv.gtk3`, set `GDK_BACKEND=x11`).

## Channels

As in Twirl's Web MIDI output: **pluck 1, bell 2, blip 3, drums and shaker
10** (General MIDI: kick 36, snare 38, hat 42, shaker 70). Note lengths:
drums 80 ms, pluck and blip 220, bell 600.

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
  stops** (whatever was already sent still plays out).
- The sheet isn't saved with the host's project yet.
- The host's tempo and transport aren't followed: Twirl has its own.

## License

CC0-1.0, like Twirl. DPF is ISC.
