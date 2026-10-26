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
 * passed through at once.
 *
 * The sound: the page renders it with Web Audio, exactly as in a browser, and
 * an AudioWorklet at the end of its graph sends it here in small chunks,
 * setState("audio", "rate,base64"), 16-bit stereo interleaved. Chunks arrive
 * unevenly (the page's main thread, the host's UI idle), so they wait in a
 * jitter buffer (100 ms to start, more after an underrun) and are read back
 * with linear interpolation at a ratio nudged to keep the buffer at its
 * target: that absorbs the drift between the page's audio clock and the
 * host's, and a page running at another sample rate. The sound therefore
 * lags the MIDI by about the buffer's length; each chunk carries the wall
 * time it was rendered for, so the plugin measures that lag and reports it.
 *
 * Route C (TwirlEngine.hpp): Twirl's sound made here. The page sends each
 * note it would have played, timed like the MIDI: setState("voices",
 * "t,voice,pack,midi,vel,pan,y,nuance;…"), and the volume and tempo as
 * setState("engine", "volume,bpm"); the engine plays them at their sample
 * offsets, mixed with whatever the page still streams.
 *
 * The page belongs to the plugin, not to its window (TwirlPage.hpp): it runs
 * from activate() to the end, window open or not, and its messages come here
 * directly. Twirl's state (its sheet, its Out choice, Follow host transport)
 * is the page's: it sends it as "page" (base64 JSON) and the plugin saves it
 * with the host's project and gives it back on loading.
 *
 * Following the host is the page's doing: run() records the host's tempo,
 * whether it's rolling and its beat at that moment's wall-clock time, and the
 * plugin hands that to the page 20 times a second (window.twirlHost), with
 * the audio lag; the page lines its beats up with the host's.
 */

#include "DistrhoPlugin.hpp"
#include "DistrhoPluginUtils.hpp"
#include "TwirlEngine.hpp"
#include "TwirlPage.hpp"
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

/** Stereo frames, single producer (setState), single consumer (run). */
class AudioRing {
public:
    static constexpr uint32_t kSize = 1 << 16; // frames: 1.4 s at 48 kHz

    uint32_t available() const noexcept
    {
        return (fWrite.load(std::memory_order_acquire) - fRead.load(std::memory_order_relaxed)) & (kSize - 1);
    }

    // Frames that don't fit are dropped (the reader is far behind: it skips ahead anyway).
    uint32_t push(const float* const l, const float* const r, const uint32_t n) noexcept
    {
        uint32_t w = fWrite.load(std::memory_order_relaxed);
        const uint32_t room = kSize - 1 - ((w - fRead.load(std::memory_order_acquire)) & (kSize - 1));
        const uint32_t count = std::min(n, room);
        for (uint32_t i = 0; i < count; ++i, w = (w + 1) & (kSize - 1))
        {
            fL[w] = l[i];
            fR[w] = r[i];
        }
        fWrite.store(w, std::memory_order_release);
        return count;
    }

    bool pop(float& l, float& r) noexcept
    {
        const uint32_t rd = fRead.load(std::memory_order_relaxed);
        if (rd == fWrite.load(std::memory_order_acquire))
            return false;
        l = fL[rd];
        r = fR[rd];
        fRead.store((rd + 1) & (kSize - 1), std::memory_order_release);
        return true;
    }

    uint32_t skip(uint32_t n) noexcept
    {
        n = std::min(n, available());
        const uint32_t rd = fRead.load(std::memory_order_relaxed);
        fRead.store((rd + n) & (kSize - 1), std::memory_order_release);
        return n;
    }

private:
    float fL[kSize], fR[kSize];
    std::atomic<uint32_t> fRead { 0 }, fWrite { 0 };
};

/** When each audio chunk starts (in frames pushed so far) and the wall time it was rendered for. */
struct ChunkStamp {
    uint64_t frame;
    double   wall;
};

/** Single producer (setState), single consumer (run), like NoteRing. */
class StampRing {
public:
    static constexpr uint32_t kSize = 256;

    void push(const ChunkStamp& c) noexcept
    {
        const uint32_t w = fWrite.load(std::memory_order_relaxed), next = (w + 1) % kSize;
        if (next == fRead.load(std::memory_order_acquire))
            return; // full: this chunk goes unmeasured
        fBuf[w] = c;
        fWrite.store(next, std::memory_order_release);
    }

    const ChunkStamp* front() const noexcept
    {
        const uint32_t r = fRead.load(std::memory_order_relaxed);
        return r == fWrite.load(std::memory_order_acquire) ? nullptr : &fBuf[r];
    }

    void pop() noexcept
    {
        fRead.store((fRead.load(std::memory_order_relaxed) + 1) % kSize, std::memory_order_release);
    }

private:
    ChunkStamp fBuf[kSize];
    std::atomic<uint32_t> fRead { 0 }, fWrite { 0 };
};

static int b64(const char c) noexcept
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

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
        : Plugin(0, 0, 1), // no parameters, no programs, one state: the page's
          fPage(*this)
    {
        std::memset(fSounding, 0, sizeof(fSounding));
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
        return "Twirl, the gesture toy: draw a stroke, it loops on its own clock, and its notes come out as MIDI.";
    }
    const char* getMaker() const override { return "Enkerli"; }
    const char* getHomePage() const override { return "https://github.com/Enkerli/twirl-dpf"; }
    const char* getLicense() const override { return "CC0-1.0"; }
    uint32_t getVersion() const override { return d_version(0, 1, 0); }

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

        if (std::strcmp(key, "audio") == 0)
        {
            parseAudio(value);
            if (fDebug)
                report();
        }
        else if (std::strcmp(key, "voices") == 0)
            parseVoices(value);
        else if (std::strcmp(key, "engine") == 0)
        {
            float volume = 0.8f, bpm = 96.0f;
            const ScopedSafeLocale ssl;
            if (std::sscanf(value, "%f,%f", &volume, &bpm) == 2)
            {
                fEngineVolume.store(volume);
                fEngineBpm.store(bpm);
            }
        }
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
        }
    }

    // Every 10 ms: the host's time for the page, 20 times a second.
    void pageIdle() override
    {
        if (fPageStopping || ! fPageSaidReady || ++fIdleCount % 5 != 0)
            return;
        HostTime h;
        if (! readHostTime(h))
            return;
        char js[256];
        {
            const ScopedSafeLocale ssl;
            std::snprintf(js, sizeof(js),
                          "window.twirlHost && twirlHost({tempo:%.4f,playing:%s,beat:%.6f,at:%.3f,lag:%.2f})",
                          h.tempo, h.playing ? "true" : "false", h.beat, h.at,
                          fHaveLag ? std::max(0.0, std::min(1000.0, fAudioLagMs.load())) : 0.0);
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
        fPriming = true;
        fTargetMs = fTargetBefore = kTargetStartMs;
        fGapFrames = 0.0;
        fGain = 0.0f;
        fFade = 1;
        fPhase = 1.0;
        fPrevL = fPrevR = fNextL = fNextR = 0.0f;
        fHaveLag = false;
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

        renderAudio(outputs[0], outputs[1], frames, blockStart, frameMs);

        // Route C: the page's notes, played here at their sample offsets.
        fEngine.setVolume(fEngineVolume.load(std::memory_order_relaxed));
        fEngine.setTempo(fEngineBpm.load(std::memory_order_relaxed));
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
    static constexpr double kTargetStartMs = 100.0, kTargetMaxMs = 400.0;

    AudioRing fAudio;
    std::atomic<uint32_t> fPageRate { 0 };
    bool fPriming = true;          // waiting for the buffer to fill to its target
    double fTargetMs = kTargetStartMs, fTargetBefore = kTargetStartMs;
    double fGapFrames = 0.0;       // after an underrun, how long nothing came
    double fPhase = 1.0;           // between fPrev and fNext, in source frames
    float fPrevL = 0.0f, fPrevR = 0.0f, fNextL = 0.0f, fNextR = 0.0f;
    static constexpr float kFadeStep = 1.0f / 128.0f; // about 3 ms
    float fGain = 0.0f;            // fades out before a jump, in after one or a wait
    int fFade = 1;                 // -1 fading out, 1 fading in, 0 steady
    float fDecodeL[4096], fDecodeR[4096];
    StampRing fStamps;
    uint64_t fPushed = 0;          // frames into the ring so far (setState's thread)
    uint64_t fPopped = 0;          // frames out of it so far, read or skipped (run's)
    std::atomic<double> fAudioLagMs { 0.0 }; // read by the page's thread
    std::atomic<bool> fHaveLag { false };

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

    // TWIRL_DEBUG=1 in the environment: a line every 5 s on stdout, from setState's thread.
    const bool fDebug = std::getenv("TWIRL_DEBUG") != nullptr;
    std::atomic<uint32_t> fUnderruns { 0 };
    std::atomic<double> fStep { 1.0 }, fTargetNow { kTargetStartMs };
    double fLastReport = 0.0;

    void report() noexcept
    {
        const double now = wallMs();
        if (now - fLastReport < 5000.0)
            return;
        fLastReport = now;
        const uint32_t rate = fPageRate.load(std::memory_order_relaxed);
        d_stdout("twirl: audio buffered %.1f ms (target %.0f), underruns %u, ratio %.5f, page %u Hz, host %.0f Hz",
                 rate ? fAudio.available() * 1000.0 / rate : 0.0, fTargetNow.load(std::memory_order_relaxed),
                 fUnderruns.load(std::memory_order_relaxed), fStep.load(std::memory_order_relaxed), rate, getSampleRate());
    }

    // Route C
    twirl::Engine fEngine;
    VoiceRing<4096> fVoiceRing;
    TimedVoice fVoicePending[kMaxPending];
    uint32_t fVoicePendingCount = 0;
    std::atomic<float> fEngineVolume { 0.8f }, fEngineBpm { 96.0f };

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

    void renderAudio(float* const outL, float* const outR, const uint32_t frames,
                     const double blockStart, const double frameMs) noexcept
    {
        const double sr = getSampleRate();
        const uint32_t pageRate = fPageRate.load(std::memory_order_acquire);
        const uint32_t target = static_cast<uint32_t>(fTargetMs * (pageRate ? pageRate : sr) / 1000.0);
        const uint32_t fill = fAudio.available();

        if (fPriming)
        {
            if (fill >= target)
            {
                fPriming = false;
                // Only a short gap (the sound came back within 50 ms) was jitter
                // a bigger buffer would have covered. A long one was a stall, a
                // pause or the window closing: the buffer goes back to what it was.
                if (fGapFrames > sr / 20)
                    fTargetMs = fTargetBefore;
                // More came while it waited (a burst): start from the target. It's
                // silent now, so the jump can't click; the sound fades in.
                if (fill > target)
                    fPopped += fAudio.skip(fill - target);
                fGain = 0.0f;
                fFade = 1;
            }
            else
            {
                if (fill == 0)
                    fGapFrames += frames;
            }
        }
        // Far too much waiting (a burst after the page stalled): the sound dips
        // for a few ms (no click) while the reader jumps to the target.
        if (! fPriming && fFade == 0 && fill > 2 * target)
            fFade = -1;

        if (fPriming || pageRate == 0)
        {
            std::memset(outL, 0, sizeof(float) * frames);
            std::memset(outR, 0, sizeof(float) * frames);
            return;
        }

        // Read faster when the buffer is above its target, slower below: at most
        // ±0.5 %, a pitch change of under a tenth of a semitone, never heard.
        const double err = (static_cast<double>(fAudio.available()) - target) / target;
        const double step = (pageRate / sr) * (1.0 + std::max(-0.005, std::min(0.005, err * 0.005)));
        fStep.store(step, std::memory_order_relaxed);
        fTargetNow.store(fTargetMs, std::memory_order_relaxed);

        for (uint32_t i = 0; i < frames; ++i)
        {
            while (fPhase >= 1.0)
            {
                fPrevL = fNextL;
                fPrevR = fNextR;
                if (! fAudio.pop(fNextL, fNextR))
                {
                    // Underrun: silence, wait for more, and keep more from now on.
                    std::memset(outL + i, 0, sizeof(float) * (frames - i));
                    std::memset(outR + i, 0, sizeof(float) * (frames - i));
                    fUnderruns.fetch_add(1, std::memory_order_relaxed);
                    fPriming = true;
                    fGapFrames = 0;
                    fTargetBefore = fTargetMs;
                    fTargetMs = std::min(kTargetMaxMs, fTargetMs * 1.25);
                    fNextL = fNextR = 0.0f;
                    return;
                }
                fPhase -= 1.0;
                measureLag(fPopped++, blockStart + i * frameMs);
            }
            if (fFade != 0)
            {
                fGain += fFade * kFadeStep;
                if (fFade < 0 && fGain <= 0.0f)
                {
                    const uint32_t now = fAudio.available();
                    if (now > target)
                        fPopped += fAudio.skip(now - target);
                    fGain = 0.0f;
                    fFade = 1;
                }
                else if (fFade > 0 && fGain >= 1.0f)
                {
                    fGain = 1.0f;
                    fFade = 0;
                }
            }
            const float f = static_cast<float>(fPhase);
            outL[i] = (fPrevL + (fNextL - fPrevL) * f) * fGain;
            outR[i] = (fPrevR + (fNextR - fPrevR) * f) * fGain;
            fPhase += step;
        }
    }

    // The audio frame `frame` (counted from the start) is leaving the plugin at
    // wall time `wall`: if a chunk started there, the lag is how late that is.
    void measureLag(const uint64_t frame, const double wall) noexcept
    {
        while (const ChunkStamp* const c = fStamps.front())
        {
            if (c->frame > frame)
                return;
            if (c->frame == frame)
            {
                const double lag = wall - c->wall, was = fAudioLagMs.load(std::memory_order_relaxed);
                fAudioLagMs.store(fHaveLag.load(std::memory_order_relaxed) ? was + (lag - was) * 0.05 : lag, std::memory_order_relaxed);
                fHaveLag.store(true, std::memory_order_relaxed);
            }
            fStamps.pop(); // used, or skipped over
        }
    }

    // "rate,wall,base64": 16-bit little-endian stereo frames, interleaved; `wall`
    // the ms since the epoch the first frame was rendered for.
    void parseAudio(const char* s) noexcept
    {
        char* end = nullptr;
        const long rate = std::strtol(s, &end, 10);
        if (end == s || *end != ',' || rate < 8000 || rate > 384000)
            return;
        s = end + 1;
        const double wall = std::strtod(s, &end);
        if (end == s || *end != ',')
            return;
        fPageRate.store(static_cast<uint32_t>(rate), std::memory_order_release);
        fStamps.push({ fPushed, wall });
        s = end + 1;

        uint32_t n = 0;
        uint8_t bytes[4];
        int nbytes = 0;
        uint32_t acc = 0;
        int bits = 0;
        for (; *s != '\0' && *s != '='; ++s)
        {
            const int v = b64(*s);
            if (v < 0)
                return;
            acc = (acc << 6) | static_cast<uint32_t>(v);
            bits += 6;
            if (bits < 8)
                continue;
            bits -= 8;
            bytes[nbytes++] = static_cast<uint8_t>(acc >> bits);
            if (nbytes < 4)
                continue;
            nbytes = 0;
            fDecodeL[n] = static_cast<int16_t>(bytes[0] | (bytes[1] << 8)) / 32768.0f;
            fDecodeR[n] = static_cast<int16_t>(bytes[2] | (bytes[3] << 8)) / 32768.0f;
            if (++n == 4096)
            {
                fPushed += fAudio.push(fDecodeL, fDecodeR, n);
                n = 0;
            }
        }
        fPushed += fAudio.push(fDecodeL, fDecodeR, n);
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
