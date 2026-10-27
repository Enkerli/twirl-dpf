/*
 * Twirl as a plugin, the DSP side: Twirl's own sound on a stereo output, and
 * its notes as MIDI.
 *
 * Twirl keeps its own clock in the page. Its notes cross in batches, ahead of
 * time, through setState("notes", …): "t,status,note,velocity;…" with t the
 * wall-clock time each event should sound at (ms since the Unix epoch, from
 * performance.timeOrigin + performance.now() in the page). setState runs on
 * a non-audio thread in every DPF format (UI thread for CLAP, JACK and VST3,
 * the worker for LV2), so it parses into a lock-free ring; run() drains the
 * ring and writes each event at its sample offset (the suite's
 * LiveNoteScheduler pattern, docs/WORKSPACE_PLUGIN.md in music-suite).
 *
 * Note-offs come from the page with Twirl's own lengths; the plugin only
 * counts what's sounding, so "panic" can end it (Start over, the page going).
 * MIDI arriving at the plugin (the host's, and the page's sendNote()) is
 * passed through at once, and, when Out includes sound, played by the engine
 * too (synth mode): channel 1 pluck, 2 bell, 3 blip, 10 drums and shaker, any
 * other pluck but 4 (Twirl's Drone, held notes: left alone); in the sheet's pack, at the event's frame. So one Twirl's MIDI
 * into another plays its notes in the second's sound.
 *
 * The sound is made here (route C, TwirlEngine.hpp). The page sends each
 * note it would have played, timed like the MIDI: setState("voices",
 * "t,voice,pack,midi,vel,pan,y,nuance;…"), the volume, tempo and Warmth as
 * setState("engine", "volume,bpm,warmth"), and the Drone as setState("drone",
 * "on,level,side,rate,pan,trem,dop,split,hz,hz…"); the engine plays the notes
 * at their sample offsets. (Until 2026-10-26 the page streamed its own Web
 * Audio here instead, ~120 ms late; Alex: "The option should leave the
 * plugin (kept in the web versions).")
 *
 * The page belongs to the plugin, not to its window (TwirlPage.hpp): it runs
 * from activate() to the end, window open or not, and its messages come here
 * directly. Twirl's state (its sheet, its Out choice, Follow host transport)
 * is the page's: it sends it as "page" (base64 JSON) and the plugin saves it
 * with the host's project and gives it back on loading.
 *
 * Following the host is the page's doing: run() records the host's tempo,
 * whether it's rolling and its beat at that moment's wall-clock time, and the
 * plugin hands that to the page 20 times a second (window.twirlHost); the
 * page lines its beats up with the host's.
 *
 * The host's parameters (TwirlParams.hpp, route C step 5) are the page's
 * controls. From the host (setParameterValue, on the audio thread): marked,
 * and the page's thread hands them to the page within 10 ms
 * (window.twirlParams), which moves its controls. All of them go once the page
 * is ready and after any restore of its saved state, so the parameters win.
 * From the page (setState("params", "index,value;…"), whatever changed it):
 * kept here, and the UI passes them to the host with a gesture (DPF's
 * editParameter and setParameterValue), the route that works in every format
 * (DPF's CLAP plugin-side change request is a stub). That needs the window
 * open; changes made while it's closed go when it next opens.
 */

#include "DistrhoPlugin.hpp"
#include "DistrhoPluginUtils.hpp"
#include "TwirlEngine.hpp"
#include "TwirlPage.hpp"
#include "TwirlParams.hpp"
#include "TwirlShared.hpp"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

START_NAMESPACE_DISTRHO

// -----------------------------------------------------------------------------------------------------------

/** One timed event from the page. */
struct TimedNote {
    double  t;        // wall-clock ms (Unix epoch)
    uint8_t data[3];  // status, note, velocity
};

/** Single producer (setState), single consumer (run). Fixed size, no allocation. */
template <uint32_t N>
class NoteRing {
public:
    bool push(const TimedNote& n) noexcept
    {
        const uint32_t w = fWrite.load(std::memory_order_relaxed);
        const uint32_t next = (w + 1) % N;
        if (next == fRead.load(std::memory_order_acquire))
            return false; // full
        fBuf[w] = n;
        fWrite.store(next, std::memory_order_release);
        return true;
    }

    bool pop(TimedNote& n) noexcept
    {
        const uint32_t r = fRead.load(std::memory_order_relaxed);
        if (r == fWrite.load(std::memory_order_acquire))
            return false;
        n = fBuf[r];
        fRead.store((r + 1) % N, std::memory_order_release);
        return true;
    }

private:
    TimedNote fBuf[N];
    std::atomic<uint32_t> fRead { 0 }, fWrite { 0 };
};

/** A note for the engine, timed like TimedNote. */
struct TimedVoice {
    double t;
    twirl::NoteSpec spec;
};

/** Single producer (the page's thread), single consumer (run). */
template <uint32_t N>
class VoiceRing {
public:
    bool push(const TimedVoice& v) noexcept
    {
        const uint32_t w = fWrite.load(std::memory_order_relaxed), next = (w + 1) % N;
        if (next == fRead.load(std::memory_order_acquire))
            return false;
        fBuf[w] = v;
        fWrite.store(next, std::memory_order_release);
        return true;
    }
    bool pop(TimedVoice& v) noexcept
    {
        const uint32_t r = fRead.load(std::memory_order_relaxed);
        if (r == fWrite.load(std::memory_order_acquire))
            return false;
        v = fBuf[r];
        fRead.store((r + 1) % N, std::memory_order_release);
        return true;
    }
private:
    TimedVoice fBuf[N];
    std::atomic<uint32_t> fRead { 0 }, fWrite { 0 };
};

static double wallMs() noexcept
{
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

// -----------------------------------------------------------------------------------------------------------

class TwirlPlugin : public Plugin, public TwirlPage::Listener, public TwirlPageAccess
{
public:
    TwirlPlugin()
        : Plugin(twirl::kParamCount, 0, 1), // no programs, one state: the page's
          fPage(*this)
    {
        std::memset(fSounding, 0, sizeof(fSounding));
        for (uint32_t i = 0; i < twirl::kParamCount; ++i)
            fParams[i].store(twirl::kParams[i].def);
    }

    ~TwirlPlugin() override
    {
        // Before the members it calls into go: the page's thread stops here.
        fPageStopping = true;
    }

protected:
   /* --------------------------------------------------------------------------------------------------------
    * Information */

    const char* getLabel() const override { return "twirl"; }
    const char* getDescription() const override
    {
        return "Twirl, the gesture toy: draw a stroke, it loops on its own clock; its sound, its notes as MIDI, or both.";
    }
    const char* getMaker() const override { return "Enkerli"; }
    const char* getHomePage() const override { return "https://github.com/Enkerli/twirl-dpf"; }
    const char* getLicense() const override { return "CC0-1.0"; }
    uint32_t getVersion() const override { return d_version(0, 1, 0); }

   /* --------------------------------------------------------------------------------------------------------
    * Parameters: the page's controls (TwirlParams.hpp) */

    void initParameter(const uint32_t index, Parameter& parameter) override
    {
        if (index >= twirl::kParamCount)
            return;
        const twirl::ParamSpec& p = twirl::kParams[index];
        parameter.hints = kParameterIsAutomatable;
        if (p.kind != twirl::kFloat)
            parameter.hints |= kParameterIsInteger;
        if (p.kind == twirl::kSwitch)
            parameter.hints |= kParameterIsBoolean;
        parameter.name = p.name;
        parameter.symbol = p.symbol;
        parameter.unit = p.unit;
        parameter.ranges.min = p.min;
        parameter.ranges.max = p.max;
        parameter.ranges.def = p.def;
        if (p.kind == twirl::kChoice)
        {
            const uint32_t n = static_cast<uint32_t>(p.max) + 1;
            parameter.enumValues.count = n;
            parameter.enumValues.restrictedMode = true;
            parameter.enumValues.values = new ParameterEnumerationValue[n];
            for (uint32_t i = 0; i < n; ++i)
            {
                parameter.enumValues.values[i].value = static_cast<float>(i);
                parameter.enumValues.values[i].label = p.choices[i];
            }
        }
    }

    float getParameterValue(const uint32_t index) const override
    {
        return index < twirl::kParamCount ? fParams[index].load() : 0.0f;
    }

    // From the host, perhaps on the audio thread: for the page. What the
    // window told the host comes back as echoes: equal, or, while it's still
    // moving, a step behind (ignored for a moment after the window's last).
    void setParameterValue(const uint32_t index, const float value) override
    {
        if (index >= twirl::kParamCount || value == fParams[index].load())
            return;
        if (wallMs() - fParamToHostAt[index].load() < 300.0)
            return;
        fParams[index].store(value);
        fParamsToPage.fetch_or(1u << index);
    }

   /* --------------------------------------------------------------------------------------------------------
    * State: the page's, saved with the project */

    void initState(const uint32_t index, State& state) override
    {
        if (index != 0)
            return;
        state.key = "page";
        state.defaultValue = "";
        state.hints = kStateIsHostReadable | kStateIsOnlyForDSP;
    }

    String getState(const char* const key) const override
    {
        if (std::strcmp(key, "page") != 0)
            return String();
        const std::lock_guard<std::mutex> lock(fSavedMutex);
        return String(fSaved.c_str());
    }

    // From the host: a project loading.
    void setState(const char* const key, const char* const value) override
    {
        if (std::strcmp(key, "page") != 0)
            return;
        {
            const std::lock_guard<std::mutex> lock(fSavedMutex);
            fSaved = value;
        }
        if (fPageSaidReady)
            restorePage();
        fParamsToPage.fetch_or(kAllParams);                      // after it: the parameters win
    }

   /* --------------------------------------------------------------------------------------------------------
    * The page: its messages, on its thread */

    void pageMessage(char* const msg) override
    {
        if (fPageStopping)
            return;
        if (std::strncmp(msg, "sendnote ", 9) == 0)
        {
            long c = 0, n = 0, v = 0;
            if (std::sscanf(msg + 9, "%ld %ld %ld", &c, &n, &v) == 3 && c >= 0 && c < 16 && n >= 0 && n < 128 && v >= 0 && v < 128)
                fRing.push({ wallMs(), { static_cast<uint8_t>((v ? 0x90 : 0x80) | c), static_cast<uint8_t>(n), static_cast<uint8_t>(v) } });
            return;
        }
        if (std::strncmp(msg, "setstate ", 9) != 0)
            return;
        char* const key = msg + 9;
        char* const sep = std::strchr(key, ' ');
        const char* const value = sep != nullptr ? sep + 1 : "";
        if (sep != nullptr)
            *sep = '\0';

        if (std::strcmp(key, "voices") == 0)
            parseVoices(value);
        else if (std::strcmp(key, "engine") == 0)
        {
            float volume = 0.8f, bpm = 96.0f, warmth = 0.5f;
            int pack = 0, sound = 1;
            const ScopedSafeLocale ssl;
            if (std::sscanf(value, "%f,%f,%f,%d,%d", &volume, &bpm, &warmth, &pack, &sound) >= 2)
            {
                fEngineVolume.store(volume);
                fEngineBpm.store(bpm);
                fEngineWarmth.store(warmth);
                fEnginePack.store(static_cast<uint8_t>(std::max(0, std::min(pack, 6))));
                fEngineSound.store(sound != 0);
            }
        }
        else if (std::strcmp(key, "drone") == 0)
            parseDrone(value);
        else if (std::strcmp(key, "notes") == 0)
            parseNotes(value);
        else if (std::strcmp(key, "panic") == 0)
            fPanic.store(true, std::memory_order_release);
        else if (std::strcmp(key, "page") == 0)
        {
            const std::lock_guard<std::mutex> lock(fSavedMutex);
            fSaved = value;
        }
        else if (std::strcmp(key, "ready") == 0)
        {
            // The page has (re)loaded: it gets its saved state, if there is one.
            fPageSaidReady = true;
            restorePage();
            fParamsToPage.fetch_or(kAllParams);                  // and then the host's parameters
        }
        else if (std::strcmp(key, "params") == 0)
            parseParams(value);
    }

    // Every 10 ms: the host's time for the page, 20 times a second.
    void pageIdle() override
    {
        if (fPageStopping || ! fPageSaidReady)
            return;
        sendParamsToPage();
        if (++fIdleCount % 5 != 0)
            return;
        HostTime h;
        if (! readHostTime(h))
            return;
        char js[512];
        {
            const ScopedSafeLocale ssl;
            int n = std::snprintf(js, sizeof(js),
                                  "window.twirlHost && twirlHost({tempo:%.4f,playing:%s,beat:%.6f,at:%.3f,midiIn:[",
                                  h.tempo, h.playing ? "true" : "false", h.beat, h.at);
            for (int c = 0; c < 16 && n > 0 && n < static_cast<int>(sizeof(js)); ++c)
                n += std::snprintf(js + n, sizeof(js) - n, c ? ",%u" : "%u", fMidiInNotes[c].load(std::memory_order_relaxed));
            if (n > 0 && n < static_cast<int>(sizeof(js)))
                std::snprintf(js + n, sizeof(js) - n, "]})");
        }
        fPage.evaluate(js);
    }

   /* --------------------------------------------------------------------------------------------------------
    * The UI (direct access): show the page in its window */

    void uiAttach(const uintptr_t window, const unsigned width, const unsigned height) override
    {
        fPage.attach(window, width, height);
    }

    void uiDetach() override
    {
        fPage.detach();
    }

    void uiResize(const unsigned width, const unsigned height) override
    {
        fPage.resize(width, height);
    }

    uint32_t uiTakeParamChanges() override
    {
        return fParamsToHost.exchange(0);
    }

    float uiParamValue(const uint32_t index) override
    {
        if (index >= twirl::kParamCount)
            return 0.0f;
        fParamToHostAt[index].store(wallMs());
        return fParams[index].load();
    }

   /* --------------------------------------------------------------------------------------------------------
    * Process */

    void activate() override
    {
        startPage();
        fEngine.setSampleRate(getSampleRate());
        fVoicePendingCount = 0;
        fFrames = 0;
        fHaveOffset = false;
        fPendingCount = 0;
    }

    void run(const float**, float** const outputs, const uint32_t frames,
             const MidiEvent* const midiIn, const uint32_t midiInCount) override
    {
        // Where this block sits on the wall clock. Measured each block, but
        // smoothed against the sample count so that one late callback doesn't
        // move every note in it (re-anchored after a jump: an xrun, a stall).
        const double sr = getSampleRate();
        const double frameMs = 1000.0 / sr;
        const double measured = wallMs() - fFrames * frameMs;
        if (! fHaveOffset || std::fabs(measured - fOffset) > 50.0)
        {
            fOffset = measured;
            fHaveOffset = true;
        }
        else
        {
            fOffset += (measured - fOffset) * 0.01;
        }
        const double blockStart = fOffset + fFrames * frameMs;
        const double blockEnd = blockStart + frames * frameMs;
        fFrames += frames;

        std::memset(outputs[0], 0, sizeof(float) * frames);
        std::memset(outputs[1], 0, sizeof(float) * frames);

        // The page's notes, played here at their sample offsets.
        fEngine.setVolume(fEngineVolume.load(std::memory_order_relaxed));
        fEngine.setTempo(fEngineBpm.load(std::memory_order_relaxed));
        fEngine.setWarmth(fEngineWarmth.load(std::memory_order_relaxed));
        {
            // The Drone, when the page has changed it (a seqlock: retried next block if mid-write).
            const uint32_t seq = fDroneSeq.load(std::memory_order_acquire);
            if (seq != fDroneSeen && (seq & 1) == 0)
            {
                const twirl::DroneSpec d = fDroneShared;
                if (fDroneSeq.load(std::memory_order_acquire) == seq)
                {
                    fEngine.setDrone(d);
                    fDroneSeen = seq;
                }
            }
        }
        TimedVoice tv;
        while (fVoicePendingCount < kMaxPending && fVoiceRing.pop(tv))
            fVoicePending[fVoicePendingCount++] = tv;
        for (uint32_t i = 0; i < fVoicePendingCount;)
        {
            const TimedVoice& p(fVoicePending[i]);
            if (p.t < blockEnd)
            {
                if (p.t > blockStart - kStaleMs)
                {
                    const double off = (p.t - blockStart) / frameMs;
                    fEngine.play(p.spec, off <= 0.0 ? 0 : std::min<int>(static_cast<int>(frames) - 1, static_cast<int>(off)));
                }
                fVoicePending[i] = fVoicePending[--fVoicePendingCount];
                continue;
            }
            ++i;
        }
        // Synth mode: MIDI coming in, played by the engine at its frame. Counted by
        // channel either way, for the page's report (⇧Q: did channel 10 arrive?).
        for (uint32_t i = 0; i < midiInCount; ++i)
            if (midiIn[i].size >= 3 && (midiIn[i].data[0] & 0xF0) == 0x90 && midiIn[i].data[2] != 0)
                fMidiInNotes[midiIn[i].data[0] & 0x0F].fetch_add(1, std::memory_order_relaxed);
        if (fEngineSound.load(std::memory_order_relaxed))
            for (uint32_t i = 0; i < midiInCount; ++i)
                synthNote(midiIn[i]);
        fEngine.render(outputs[0], outputs[1], frames);

        // The host's time, for the page: its beat at this block's wall-clock time.
        const TimePosition& pos(getTimePosition());
        HostTime h;
        h.playing = pos.playing;
        h.at = blockStart;
        if (pos.bbt.valid && pos.bbt.beatsPerMinute > 0.0 && pos.bbt.ticksPerBeat > 0.0)
        {
            h.tempo = pos.bbt.beatsPerMinute;
            h.beat = (pos.bbt.barStartTick + pos.bbt.tick) / pos.bbt.ticksPerBeat + (pos.bbt.beat - 1);
        }
        writeHostTime(h);

        // New events from the page join the pending list.
        TimedNote n;
        while (fPendingCount < kMaxPending && fRing.pop(n))
            fPending[fPendingCount++] = n;

        uint32_t outCount = 0;

        if (fPanic.exchange(false, std::memory_order_acq_rel))
        {
            fPendingCount = 0;
            for (uint8_t ch = 0; ch < 16; ++ch)
                for (uint8_t note = 0; note < 128; ++note)
                    for (; fSounding[ch][note] > 0 && outCount < kMaxOut; --fSounding[ch][note])
                        addOut(outCount, 0, uint8_t(0x80 | ch), note, 0);
        }

        // Passed through: the host's MIDI, and notes from the page's sendNote().
        for (uint32_t i = 0; i < midiInCount && outCount < kMaxOut; ++i)
        {
            const MidiEvent& ev(midiIn[i]);
            if (ev.size > MidiEvent::kDataSize)
                continue; // no sysex
            MidiEvent& out(fOut[outCount++]);
            out = ev;
            out.dataExt = nullptr;
            count(out.data, out.size);
        }

        // Pending events due in this block, each at its frame; too old ones dropped.
        for (uint32_t i = 0; i < fPendingCount;)
        {
            const TimedNote& p(fPending[i]);
            if (p.t < blockEnd && outCount < kMaxOut)
            {
                if (p.t > blockStart - kStaleMs)
                {
                    const double off = (p.t - blockStart) / frameMs;
                    const uint32_t frame = off <= 0.0 ? 0 : std::min<uint32_t>(frames - 1, static_cast<uint32_t>(off));
                    addOut(outCount, frame, p.data[0], p.data[1], p.data[2]);
                    count(p.data, 3);
                }
                fPending[i] = fPending[--fPendingCount];
                continue;
            }
            ++i;
        }

        // Hosts want a block's events in time order (stable: an off before an
        // on at the same frame stays first).
        for (uint32_t i = 1; i < outCount; ++i)
        {
            const MidiEvent ev(fOut[i]);
            uint32_t j = i;
            for (; j > 0 && fOut[j - 1].frame > ev.frame; --j)
                fOut[j] = fOut[j - 1];
            fOut[j] = ev;
        }

        for (uint32_t i = 0; i < outCount; ++i)
            writeMidiEvent(fOut[i]);
    }

    // -------------------------------------------------------------------------------------------------------

private:
    static constexpr uint32_t kMaxPending = 2048;
    static constexpr uint32_t kMaxOut = 512;
    static constexpr double kStaleMs = 1000.0; // older than this is from a stall or a restore: dropped
    // The host's time, written by run(), read by the page's thread (a seqlock).
    struct HostTime {
        double tempo = 0.0;        // 0: the host gives none
        double beat = 0.0;         // its beat (quarter notes, say) at wall time `at`
        double at = 0.0;
        bool playing = false;
    };
    std::atomic<uint32_t> fHostSeq { 0 };
    std::atomic<double> fHostTempo { 0.0 }, fHostBeat { 0.0 }, fHostAt { 0.0 };
    std::atomic<bool> fHostPlaying { false };

    void writeHostTime(const HostTime& h) noexcept
    {
        fHostSeq.fetch_add(1, std::memory_order_acq_rel); // odd: writing
        fHostTempo.store(h.tempo, std::memory_order_relaxed);
        fHostBeat.store(h.beat, std::memory_order_relaxed);
        fHostAt.store(h.at, std::memory_order_relaxed);
        fHostPlaying.store(h.playing, std::memory_order_relaxed);
        fHostSeq.fetch_add(1, std::memory_order_acq_rel); // even: done
    }

    bool readHostTime(HostTime& h) const noexcept
    {
        for (int tries = 0; tries < 4; ++tries)
        {
            const uint32_t seq = fHostSeq.load(std::memory_order_acquire);
            if (seq == 0 || (seq & 1) != 0)
                continue;
            h.tempo = fHostTempo.load(std::memory_order_relaxed);
            h.beat = fHostBeat.load(std::memory_order_relaxed);
            h.at = fHostAt.load(std::memory_order_relaxed);
            h.playing = fHostPlaying.load(std::memory_order_relaxed);
            if (fHostSeq.load(std::memory_order_acquire) == seq)
                return true;
        }
        return false;
    }

    // The parameters: their values (as the page has them, or about to), which
    // changed for the page and for the host, and when the window last told the host.
    static constexpr uint32_t kAllParams = twirl::kParamCount == 32 ? ~0u : (1u << twirl::kParamCount) - 1;
    std::atomic<float> fParams[twirl::kParamCount];
    std::atomic<double> fParamToHostAt[twirl::kParamCount] {};
    std::atomic<uint32_t> fParamsToPage { 0 }, fParamsToHost { 0 };

    void sendParamsToPage()
    {
        const uint32_t changed = fParamsToPage.exchange(0);
        if (changed == 0)
            return;
        std::string js = "window.twirlParams && twirlParams({";
        char one[48];
        const ScopedSafeLocale ssl;
        for (uint32_t i = 0; i < twirl::kParamCount; ++i)
            if (changed & (1u << i))
            {
                std::snprintf(one, sizeof(one), "%u:%.6g,", i, fParams[i].load());
                js += one;
            }
        js += "})";
        fPage.evaluate(js);
    }

    void parseParams(const char* s)
    {
        const ScopedSafeLocale ssl;
        while (*s != '\0')
        {
            char* end;
            const long i = std::strtol(s, &end, 10);
            if (end == s || *end != ',')
                return;
            s = end + 1;
            const float v = std::strtof(s, &end);
            if (end == s)
                return;
            s = *end == ';' ? end + 1 : end;
            if (i < 0 || i >= static_cast<long>(twirl::kParamCount) || ! std::isfinite(v))
                continue;
            const twirl::ParamSpec& p = twirl::kParams[i];
            fParams[i].store(std::max(p.min, std::min(p.max, v)));
            fParamsToHost.fetch_or(1u << i);
        }
    }

    // The page and its saved state.
    TwirlPage fPage;
    std::atomic<bool> fPageStopping { false }, fPageSaidReady { false };
    uint32_t fIdleCount = 0;
    mutable std::mutex fSavedMutex;
    std::string fSaved;             // base64 JSON, the page's to read

    void startPage()
    {
        String path;
        if (const char* const bundle = getBundlePath())
        {
            path = getResourcePath(bundle);
        }
        else
        {
            path = getBinaryFilename();
            path.truncate(path.rfind(DISTRHO_OS_SEP));
            path += "/resources";
        }
        path.urlEncode();
        fPage.start("file://" + path + "/index.html");
    }

    void restorePage()
    {
        std::string saved;
        {
            const std::lock_guard<std::mutex> lock(fSavedMutex);
            saved = fSaved;
        }
        // base64: no quotes or backslashes to escape
        for (const char c : saved)
            if (! (std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/' || c == '='))
                return;
        fPage.evaluate("window.twirlRestore && twirlRestore('" + saved + "')");
    }

    // The sound (route C)
    twirl::Engine fEngine;
    VoiceRing<4096> fVoiceRing;
    TimedVoice fVoicePending[kMaxPending];
    uint32_t fVoicePendingCount = 0;
    std::atomic<float> fEngineVolume { 0.8f }, fEngineBpm { 96.0f }, fEngineWarmth { 0.5f };
    std::atomic<uint8_t> fEnginePack { 0 };
    std::atomic<bool> fEngineSound { true };
    std::atomic<uint32_t> fMidiInNotes[16] {};   // note-ons coming in, by channel (the page's report)

    /** A note-on coming in, as Twirl would have played it (note-offs: its voices ring out). */
    void synthNote(const MidiEvent& ev) noexcept
    {
        if (ev.size < 3 || (ev.data[0] & 0xF0) != 0x90 || ev.data[2] == 0)
            return;
        const uint8_t ch = ev.data[0] & 0x0F, note = ev.data[1] & 0x7F;
        if (ch == 3)                // Twirl's Drone (held notes): not one-shots; left alone for now
            return;
        twirl::NoteSpec spec { twirl::kPluck, fEnginePack.load(std::memory_order_relaxed), static_cast<float>(note),
                               ev.data[2] / 127.0f, 0.0f, 0.5f, 0.0f };
        switch (ch)
        {
        case 1: spec.voice = twirl::kBell; spec.midi -= 12; break; // Twirl sends bell and blip an octave up
        case 2: spec.voice = twirl::kBlip; spec.midi -= 12; break;
        case 9:
            // Drums by note: Twirl's (36 kick, 38 snare, 42 hat, 70 shaker) and General MIDI's neighbours.
            // Their height picks the kind; the pitch tunes it (Twirl's drums are pitched).
            spec.midi = 60;
            if (note == 69 || note == 70 || note == 82) { spec.voice = twirl::kShake; spec.y = 0.5f; }
            else if (note <= 36) { spec.voice = twirl::kDrum; spec.y = 0.8f; }
            else if (note == 42 || note == 44 || note == 46 || note >= 49) { spec.voice = twirl::kDrum; spec.y = 0.1f; }
            else { spec.voice = twirl::kDrum; spec.y = 0.45f; }
            break;
        default: break; // channel 1, and any other: pluck
        }
        fEngine.play(spec, static_cast<int>(ev.frame));
    }
    std::atomic<uint32_t> fDroneSeq { 0 };
    twirl::DroneSpec fDroneShared;   // written by the page's thread inside the seqlock
    uint32_t fDroneSeen = 0;

    // "on,level,side,rate,pan,trem,dop,split,hz,hz…"
    void parseDrone(const char* s) noexcept
    {
        twirl::DroneSpec d;
        const ScopedSafeLocale ssl;
        int on = 0, used = 0;
        if (std::sscanf(s, "%d,%f,%f,%f,%f,%f,%f,%f%n", &on, &d.level, &d.side, &d.rate, &d.panSwing, &d.trem, &d.dop, &d.split, &used) != 8)
            return;
        d.on = on != 0;
        s += used;
        while (*s == ',' && d.count < twirl::DroneSpec::kMaxNotes)
        {
            char* end = nullptr;
            const float hz = std::strtof(s + 1, &end);
            if (end == s + 1 || ! std::isfinite(hz) || hz <= 0.0f)
                break;
            d.freq[d.count++] = hz;
            s = end;
        }
        fDroneSeq.fetch_add(1, std::memory_order_acq_rel);
        fDroneShared = d;
        fDroneSeq.fetch_add(1, std::memory_order_acq_rel);
    }

    // "t,voice,pack,midi,vel,pan,y,nuance;…"; anything malformed is skipped.
    void parseVoices(const char* s) noexcept
    {
        const ScopedSafeLocale ssl;
        while (*s != '\0')
        {
            double t = 0;
            int voice = 0, pack = 0, used = 0;
            float midi = 60, vel = 0, pan = 0, y = 0.5f, nuance = 0;
            if (std::sscanf(s, "%lf,%d,%d,%f,%f,%f,%f,%f%n", &t, &voice, &pack, &midi, &vel, &pan, &y, &nuance, &used) == 8
                && voice >= 0 && voice <= 4 && std::isfinite(midi) && vel >= 0.0f)
            {
                fVoiceRing.push({ t, { static_cast<uint8_t>(voice), static_cast<uint8_t>(std::max(0, pack)), midi,
                                       std::min(vel, 1.5f), std::max(-1.0f, std::min(1.0f, pan)), std::max(0.0f, std::min(1.0f, y)),
                                       std::max(-1.0f, std::min(1.0f, nuance)) } });
            }
            const char* const semi = std::strchr(s, ';');
            if (semi == nullptr)
                break;
            s = semi + 1;
        }
    }

    NoteRing<4096> fRing;
    std::atomic<bool> fPanic { false };

    TimedNote fPending[kMaxPending];
    uint32_t fPendingCount = 0;
    MidiEvent fOut[kMaxOut];
    uint8_t fSounding[16][128]; // note-ons not yet ended, per channel and note

    uint64_t fFrames = 0;
    double fOffset = 0.0;
    bool fHaveOffset = false;

    void addOut(uint32_t& outCount, const uint32_t frame, const uint8_t status, const uint8_t note, const uint8_t vel) noexcept
    {
        MidiEvent& out(fOut[outCount++]);
        out.frame = frame;
        out.size = 3;
        out.data[0] = status;
        out.data[1] = note;
        out.data[2] = vel;
        out.data[3] = 0;
        out.dataExt = nullptr;
    }

    void count(const uint8_t* const data, const uint32_t size) noexcept
    {
        if (size < 3)
            return;
        const uint8_t kind = data[0] & 0xF0, ch = data[0] & 0x0F, note = data[1] & 0x7F;
        if (kind == 0x90 && data[2] > 0)
        {
            if (fSounding[ch][note] < 255)
                ++fSounding[ch][note];
        }
        else if ((kind == 0x80 || kind == 0x90) && fSounding[ch][note] > 0)
        {
            --fSounding[ch][note];
        }
    }

    // "t,status,note,velocity;…"; anything malformed is skipped.
    void parseNotes(const char* s) noexcept
    {
        while (*s != '\0')
        {
            char* end = nullptr;
            TimedNote n;
            n.t = std::strtod(s, &end);
            bool ok = end != s && *end == ',';
            long v[3] = { 0, 0, 0 };
            for (int k = 0; ok && k < 3; ++k)
            {
                s = end + 1;
                v[k] = std::strtol(s, &end, 10);
                ok = end != s && (k < 2 ? *end == ',' : (*end == ';' || *end == '\0'));
            }
            if (ok && (v[0] & 0xF0) >= 0x80 && (v[0] & 0xF0) <= 0x90 && v[0] <= 0x9F
                && v[1] >= 0 && v[1] < 128 && v[2] >= 0 && v[2] < 128)
            {
                n.data[0] = static_cast<uint8_t>(v[0]);
                n.data[1] = static_cast<uint8_t>(v[1]);
                n.data[2] = static_cast<uint8_t>(v[2]);
                fRing.push(n); // full: dropped (the page sends far less than this)
            }
            const char* const semi = std::strchr(end != nullptr ? end : s, ';');
            if (semi == nullptr)
                break;
            s = semi + 1;
        }
    }

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TwirlPlugin)
};

// -----------------------------------------------------------------------------------------------------------

Plugin* createPlugin()
{
    return new TwirlPlugin();
}

END_NAMESPACE_DISTRHO
