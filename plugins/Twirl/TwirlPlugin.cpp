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
 * lags the MIDI by about the buffer's length.
 */

#include "DistrhoPlugin.hpp"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>

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

/** Stereo frames, single producer (setState), single consumer (run). */
class AudioRing {
public:
    static constexpr uint32_t kSize = 1 << 16; // frames: 1.4 s at 48 kHz

    uint32_t available() const noexcept
    {
        return (fWrite.load(std::memory_order_acquire) - fRead.load(std::memory_order_relaxed)) & (kSize - 1);
    }

    // Frames that don't fit are dropped (the reader is far behind: it skips ahead anyway).
    void push(const float* const l, const float* const r, const uint32_t n) noexcept
    {
        uint32_t w = fWrite.load(std::memory_order_relaxed);
        const uint32_t room = kSize - 1 - ((w - fRead.load(std::memory_order_acquire)) & (kSize - 1));
        for (uint32_t i = 0; i < n && i < room; ++i, w = (w + 1) & (kSize - 1))
        {
            fL[w] = l[i];
            fR[w] = r[i];
        }
        fWrite.store(w, std::memory_order_release);
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

    void skip(const uint32_t n) noexcept
    {
        const uint32_t rd = fRead.load(std::memory_order_relaxed);
        fRead.store((rd + std::min(n, available())) & (kSize - 1), std::memory_order_release);
    }

private:
    float fL[kSize], fR[kSize];
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

class TwirlPlugin : public Plugin
{
public:
    TwirlPlugin()
        : Plugin(0, 0, 0) // no parameters, no programs, no saved states (yet: the sheet comes later)
    {
        std::memset(fSounding, 0, sizeof(fSounding));
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
    * State: the page's messages (none saved with the project) */

    void initState(uint32_t, State&) override {}

    void setState(const char* const key, const char* const value) override
    {
        if (std::strcmp(key, "audio") == 0)
        {
            parseAudio(value);
            if (fDebug)
                report();
        }
        else if (std::strcmp(key, "notes") == 0)
            parseNotes(value);
        else if (std::strcmp(key, "panic") == 0)
            fPanic.store(true, std::memory_order_release);
    }

   /* --------------------------------------------------------------------------------------------------------
    * Process */

    void activate() override
    {
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
    }

    void run(const float**, float** const outputs, const uint32_t frames,
             const MidiEvent* const midiIn, const uint32_t midiInCount) override
    {
        renderAudio(outputs[0], outputs[1], frames);

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

    void renderAudio(float* const outL, float* const outR, const uint32_t frames) noexcept
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
                    fAudio.skip(fill - target);
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
            }
            if (fFade != 0)
            {
                fGain += fFade * kFadeStep;
                if (fFade < 0 && fGain <= 0.0f)
                {
                    const uint32_t now = fAudio.available();
                    if (now > target)
                        fAudio.skip(now - target);
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

    // "rate,base64": 16-bit little-endian stereo frames, interleaved.
    void parseAudio(const char* s) noexcept
    {
        char* end = nullptr;
        const long rate = std::strtol(s, &end, 10);
        if (end == s || *end != ',' || rate < 8000 || rate > 384000)
            return;
        fPageRate.store(static_cast<uint32_t>(rate), std::memory_order_release);
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
                fAudio.push(fDecodeL, fDecodeR, n);
                n = 0;
            }
        }
        fAudio.push(fDecodeL, fDecodeR, n);
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
