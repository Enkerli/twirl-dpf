/*
 * Twirl as a plugin, the DSP side: a MIDI generator with no audio.
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
        if (std::strcmp(key, "notes") == 0)
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
    }

    void run(const float**, float**, const uint32_t frames,
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
