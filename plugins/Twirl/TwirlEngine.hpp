/*
 * Twirl's sound, made in the plugin (route C, music-suite apps/twirl/PLAN.md).
 *
 * The page decides every note (it's still Twirl's brain) and sends it, timed;
 * this plays it, sample-accurate, in step with the MIDI and the host. The
 * voices follow the page's Web Audio recipes (index.html, section 3) with the
 * same building blocks, approximately: Alex, 2026-10-26, "Doesn't even need
 * to sound that close to the current one, as long as it sounds ok."
 *
 * Step 1: Paper's five voices (every pack plays them for now), the delay,
 * the volume and a compressor. Real-time safe: a fixed pool of voices, no
 * allocation, no locks.
 */

#ifndef TWIRL_ENGINE_HPP_INCLUDED
#define TWIRL_ENGINE_HPP_INCLUDED

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace twirl {

static constexpr double kPi = 3.14159265358979323846;

inline double mtof(const double midi) { return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0); }

/** A small, fast random source (xorshift), one per engine. */
struct Random {
    uint32_t s = 0x9e3779b9u;
    float next() noexcept // 0..1
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (s >> 8) * (1.0f / 16777216.0f);
    }
    float bipolar() noexcept { return next() * 2.0f - 1.0f; }
};

/** Web Audio's exponentialRampToValueAtTime, sample by sample: a value that
 *  multiplies toward its target over a number of samples, then holds it. */
struct ExpRamp {
    double value = 1.0, factor = 1.0;
    int left = 0;

    void set(const double v) noexcept { value = v; left = 0; }
    void rampTo(const double target, const double samples) noexcept
    {
        const int n = std::max(1, static_cast<int>(samples));
        factor = std::pow(target / value, 1.0 / n);
        left = n;
    }
    double next() noexcept
    {
        const double v = value;
        if (left > 0) { value *= factor; --left; }
        return v;
    }
};

/** Twirl's env(): from silence up to `peak` in `attack`, then down over `decay`. */
struct Env {
    ExpRamp ramp;
    int toDecay = 0;
    double decaySamples = 1.0;

    void start(const double sr, double peak, const double decay, const double attack) noexcept
    {
        peak = std::max(0.0002, peak);
        ramp.set(0.0001);
        ramp.rampTo(peak, attack * sr);
        toDecay = static_cast<int>(attack * sr);
        decaySamples = decay * sr;
    }
    double next() noexcept
    {
        if (toDecay > 0 && --toDecay == 0)
            ramp.rampTo(0.0001, decaySamples);
        return ramp.next();
    }
};

enum class Wave : uint8_t { Sine, Triangle, Saw, Square };

/** An oscillator. Saw and square band-limited with PolyBLEP (Web Audio's are band-limited too). */
struct Osc {
    double phase = 0.0;
    Wave wave = Wave::Sine;

    static double blep(double t, const double dt) noexcept
    {
        if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
        if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
        return 0.0;
    }

    double next(const double freq, const double sr) noexcept
    {
        const double dt = std::min(0.5, std::fabs(freq) / sr);
        double v;
        switch (wave)
        {
        case Wave::Sine:     v = std::sin(2.0 * kPi * phase); break;
        case Wave::Triangle: v = 1.0 - 4.0 * std::fabs(phase - 0.5); v = -v; break;
        case Wave::Saw:      v = 2.0 * phase - 1.0 - blep(phase, dt); break;
        case Wave::Square:
        default:
            v = (phase < 0.5 ? 1.0 : -1.0) + blep(phase, dt) - blep(std::fmod(phase + 0.5, 1.0), dt);
            break;
        }
        phase += freq / sr;
        phase -= std::floor(phase);
        return v;
    }
};

enum class Filter : uint8_t { Lowpass, Highpass, Bandpass };

/** Web Audio's BiquadFilterNode (the Audio EQ Cookbook). Its lowpass and
 *  highpass Q is in dB; its bandpass Q is plain. */
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    void set(const Filter type, double freq, const double q, const double sr) noexcept
    {
        freq = std::max(10.0, std::min(freq, sr * 0.49));
        const double w = 2.0 * kPi * freq / sr, cw = std::cos(w), sw = std::sin(w);
        const double Q = type == Filter::Bandpass ? std::max(1e-4, q) : std::pow(10.0, q / 20.0);
        const double alpha = sw / (2.0 * Q), a0 = 1.0 + alpha;
        switch (type)
        {
        case Filter::Lowpass:  b0 = (1 - cw) / 2; b1 = 1 - cw;    b2 = (1 - cw) / 2; break;
        case Filter::Highpass: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; break;
        case Filter::Bandpass: b0 = alpha;        b1 = 0;         b2 = -alpha;       break;
        }
        b0 /= a0; b1 /= a0; b2 /= a0;
        a1 = -2.0 * cw / a0; a2 = (1.0 - alpha) / a0;
    }
    double next(const double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() noexcept { z1 = z2 = 0; }
};

/** What a note asks for: the page's sound() call, as it reaches the plugin. */
struct NoteSpec {
    uint8_t voice;     // 0 pluck, 1 bell, 2 blip, 3 drum, 4 shake
    uint8_t pack;      // Twirl's PACK_ORDER index (step 1: all play Paper)
    float midi;        // exact pitch, microtonal included
    float vel;         // 0..1.5
    float pan;         // -1..1
    float y;           // height on the sheet, 0 (top) .. 1
    float nuance;      // Vary's -1..1
};

enum : uint8_t { kPluck = 0, kBell, kBlip, kDrum, kShake };

/** One sounding note. Its parts depend on what it is; unused ones stay idle. */
struct Voice {
    bool active = false;
    int wait = 0;               // samples until it starts (its offset in the block)
    int left = 0;               // samples until it stops (Web Audio's stop())
    uint8_t kind = 0, drumKind = 0;
    float gainL = 0, gainR = 0, send = 0;
    double sr = 48000.0;

    Osc osc1, osc2;
    ExpRamp freq1, freq2, mod, cutoff;
    Biquad f1, f2;
    Env env1, env2;
    Filter cutType = Filter::Lowpass;
    double cutQ = 1.0;
    int noiseLeft = 0, osc2Left = 0, grainWait = 0;
    float noiseAmp = 1.0f;
    int cutEvery = 0;

    void setPan(const float pan) noexcept // Web Audio's StereoPannerNode, mono in: equal power
    {
        const double x = (std::max(-1.0f, std::min(1.0f, pan)) + 1.0) * 0.5;
        gainL = static_cast<float>(std::cos(x * kPi / 2));
        gainR = static_cast<float>(std::sin(x * kPi / 2));
    }
};

class Engine {
public:
    static constexpr int kVoices = 160;

    void setSampleRate(const double sr) noexcept
    {
        fSr = sr;
        fDelay.assign(sr);
        fDelayLp.set(Filter::Lowpass, 2400.0, 1.0, sr); // Web Audio's default Q (1 dB)
        reset();
    }

    void reset() noexcept
    {
        for (Voice& v : fVoices) v.active = false;
        fDelay.clear();
        fDelayLp.reset();
        fEnvelope = 0.0;
    }

    void setVolume(const float v) noexcept { fVolumeTarget = std::max(0.0f, std::min(2.0f, v)); }
    void setTempo(const double bpm) noexcept { if (bpm > 0) fDelaySeconds = 0.75 * 60.0 / bpm; }

    /** Start a note `offset` samples into the next render. */
    void play(const NoteSpec& n, const int offset) noexcept
    {
        if (n.voice == kShake)
        {
            // Beads on a shell: a grain and a smaller rattle 11–17 ms later (and ±3 ms of jitter).
            const int jitter = static_cast<int>(fRand.bipolar() * 0.003 * fSr);
            startShakeGrain(n, std::max(0, offset + jitter), 1.0f);
            startShakeGrain(n, std::max(0, offset + jitter + static_cast<int>((0.011 + fRand.next() * 0.006) * fSr)), 0.35f);
            return;
        }
        Voice& v = take();
        v.wait = offset;
        v.kind = n.voice;
        v.sr = fSr;
        v.f1.reset(); v.f2.reset();
        v.osc1.phase = v.osc2.phase = 0.0;
        v.noiseLeft = v.osc2Left = 0;
        v.cutEvery = 0;
        const double sr = fSr, vel = n.vel, nu = n.nuance;
        v.setPan(n.pan);

        switch (n.voice)
        {
        case kPluck: // a saw through a closing lowpass
            v.osc1.wave = Wave::Saw;
            v.freq1.set(mtof(n.midi));
            v.cutType = Filter::Lowpass; v.cutQ = 3.0;
            v.cutoff.set((600.0 + 4200.0 * vel) * (1.0 + 0.25 * nu));
            v.cutoff.rampTo(320.0, 0.35 * sr);
            v.env1.start(sr, 0.22 * vel, 0.55 * (1.0 + 0.15 * nu), 0.004);
            v.send = 0.6f; v.left = static_cast<int>(0.7 * sr);
            break;
        case kBell: { // two-operator FM, an inharmonic ratio
            const double f0 = mtof(n.midi + 12.0);
            v.osc1.wave = v.osc2.wave = Wave::Sine;
            v.freq1.set(f0);
            v.freq2.set(f0 * (3.5 + 0.08 * nu));
            v.mod.set(std::max(1.0, f0 * 2.2 * vel));
            v.mod.rampTo(1.0, 1.2 * sr);
            v.env1.start(sr, 0.16 * vel, 1.8, 0.002);
            v.send = 0.8f; v.left = static_cast<int>(2.0 * sr);
            break;
        }
        case kBlip: { // a short square chirp, an octave up
            const double f = mtof(n.midi + 12.0);
            v.osc1.wave = Wave::Square;
            v.freq1.set(f * 1.5);
            v.freq1.rampTo(f, 0.04 * sr);
            v.f1.set(Filter::Lowpass, 3200.0, 1.0, sr);
            v.env1.start(sr, 0.1 * vel, 0.16 * (1.0 + 0.3 * nu), 0.002);
            v.send = 0.9f; v.left = static_cast<int>(0.3 * sr);
            break;
        }
        case kDrum:
        default: {
            const double y = n.y;
            v.drumKind = y < 0.28 ? 0 : y < 0.62 ? 1 : 2; // hat, snare, kick (Twirl's drumKind)
            if (v.drumKind == 0) // hat: noise through a highpass, brighter as it goes up
            {
                v.f1.set(Filter::Highpass, (5000.0 + 6000.0 * (0.28 - y) / 0.28) * (1.0 + 0.1 * nu), 1.0, sr);
                v.env1.start(sr, 0.28 * vel, 0.05, 0.001);
                v.noiseLeft = static_cast<int>(0.08 * sr);
                v.send = 0.3f; v.left = v.noiseLeft;
            }
            else if (v.drumKind == 1) // snare: banded noise, and a body tuned to the note
            {
                v.f1.set(Filter::Bandpass, 1900.0, 0.8, sr);
                v.env1.start(sr, 0.45 * vel, 0.17, 0.001);
                v.noiseLeft = static_cast<int>(0.22 * sr);
                v.osc2.wave = Wave::Triangle;
                v.freq2.set(mtof(n.midi - 12.0));
                v.env2.start(sr, 0.32 * vel, 0.1, 0.001);
                v.osc2Left = static_cast<int>(0.14 * sr);
                v.send = 0.35f; v.left = v.noiseLeft;
            }
            else // kick: a sine dropping to its pitch
            {
                const double f0 = mtof(n.midi - 24.0);
                v.osc1.wave = Wave::Sine;
                v.freq1.set(f0 * 3.0);
                v.freq1.rampTo(f0, 0.12 * sr);
                v.env1.start(sr, 0.9 * vel, 0.38, 0.002);
                v.setPan(n.pan * 0.3f);
                v.send = 0.1f; v.left = static_cast<int>(0.45 * sr);
            }
            break;
        }
        }
        v.active = true;
    }

    /** Add `frames` samples of Twirl into the outputs (which the caller has zeroed or filled). */
    void render(float* const outL, float* const outR, const uint32_t frames) noexcept
    {
        const double sr = fSr;
        const int delaySamples = std::max(1, std::min(static_cast<int>(fDelaySeconds * sr), fDelay.size - 1));

        for (uint32_t i = 0; i < frames; ++i)
        {
            double dryL = 0, dryR = 0, wet = 0;
            for (Voice& v : fVoices)
            {
                if (! v.active)
                    continue;
                if (v.wait > 0) { --v.wait; continue; }
                const double s = tick(v);
                dryL += s * v.gainL;
                dryR += s * v.gainR;
                wet += s * (v.gainL + v.gainR) * 0.5 * v.send;
                if (--v.left <= 0)
                    v.active = false;
            }

            // The delay: a dotted eighth, darkened each time round (Twirl's delaySend 0.28, feedback 0.38).
            const double echoed = fDelayLp.next(fDelay.read(delaySamples));
            fDelay.write(wet * 0.28 + echoed * 0.38);

            // Volume (smoothed), then the compressor.
            fVolume += (fVolumeTarget - fVolume) * 0.001;
            double l = (dryL + echoed) * fVolume, r = (dryR + echoed) * fVolume;
            const double g = compress(std::max(std::fabs(l), std::fabs(r)));
            outL[i] += static_cast<float>(l * g);
            outR[i] += static_cast<float>(r * g);
        }
    }

    int sounding() const noexcept
    {
        int n = 0;
        for (const Voice& v : fVoices) n += v.active ? 1 : 0;
        return n;
    }

private:
    double fSr = 48000.0;
    Voice fVoices[kVoices];
    Random fRand;
    float fVolumeTarget = 0.8f, fVolume = 0.8f;
    double fDelaySeconds = 0.75 * 60.0 / 96.0;
    double fEnvelope = 0.0;

    struct DelayLine {
        float buf[2 * 192000 + 4];
        int size = 2 * 48000, pos = 0;
        void assign(const double sr) noexcept { size = std::min(static_cast<int>(2 * sr) + 2, static_cast<int>(sizeof(buf) / sizeof(buf[0]))); clear(); }
        void clear() noexcept { std::memset(buf, 0, sizeof(buf)); pos = 0; }
        double read(const int d) const noexcept { int p = pos - d; if (p < 0) p += size; return buf[p]; }
        void write(const double x) noexcept { buf[pos] = static_cast<float>(x); if (++pos >= size) pos = 0; }
    } fDelay;
    Biquad fDelayLp;

    /** A free voice, or the oldest-sounding one. */
    Voice& take() noexcept
    {
        Voice* best = &fVoices[0];
        for (Voice& v : fVoices)
        {
            if (! v.active)
                return v;
            if (v.left < best->left)
                best = &v;
        }
        return *best;
    }

    void startShakeGrain(const NoteSpec& n, const int offset, const float amp) noexcept
    {
        Voice& v = take();
        const double sr = fSr, bright = 1.0 - n.y, vel = n.vel;
        v.wait = offset;
        v.kind = kShake;
        v.sr = sr;
        v.f1.reset(); v.f2.reset();
        v.f1.set(Filter::Bandpass, (3200.0 + 5200.0 * bright) * (0.92 + fRand.next() * 0.16), 1.4, sr);
        v.f2.set(Filter::Highpass, 1800.0, 1.0, sr);
        v.env1.start(sr, 0.5 * vel * amp, 0.045 + 0.07 * vel, 0.007);
        v.noiseLeft = static_cast<int>(0.16 * sr);
        v.osc2Left = 0;
        v.setPan(n.pan);
        v.send = 0.25f;
        v.left = v.noiseLeft;
        v.active = true;
    }

    double tick(Voice& v) noexcept
    {
        const double sr = v.sr;
        switch (v.kind)
        {
        case kPluck: {
            if (v.cutEvery-- <= 0) { v.f1.set(v.cutType, v.cutoff.value, v.cutQ, sr); v.cutEvery = 15; }
            v.cutoff.next();
            return v.f1.next(v.osc1.next(v.freq1.next(), sr)) * v.env1.next();
        }
        case kBell: {
            const double m = v.osc2.next(v.freq2.value, sr) * v.mod.next();
            return v.osc1.next(v.freq1.value + m, sr) * v.env1.next();
        }
        case kBlip:
            return v.f1.next(v.osc1.next(v.freq1.next(), sr)) * v.env1.next();
        case kShake: {
            const double x = v.noiseLeft-- > 0 ? fRand.bipolar() : 0.0;
            return v.f2.next(v.f1.next(x)) * v.env1.next();
        }
        case kDrum:
        default:
            if (v.drumKind == 0)
            {
                const double x = v.noiseLeft-- > 0 ? fRand.bipolar() : 0.0;
                return v.f1.next(x) * v.env1.next();
            }
            if (v.drumKind == 1)
            {
                const double x = v.noiseLeft-- > 0 ? fRand.bipolar() : 0.0;
                double s = v.f1.next(x) * v.env1.next();
                if (v.osc2Left-- > 0)
                    s += v.osc2.next(v.freq2.value, sr) * v.env2.next();
                return s;
            }
            return v.osc1.next(v.freq1.next(), sr) * v.env1.next();
        }
    }

    /** A plain peak compressor near Twirl's (threshold -14 dB, ratio 4, attack 3 ms, release 250 ms). */
    double compress(const double level) noexcept
    {
        const double a = level > fEnvelope ? 1.0 - std::exp(-1.0 / (0.003 * fSr)) : 1.0 - std::exp(-1.0 / (0.25 * fSr));
        fEnvelope += (level - fEnvelope) * a;
        const double threshold = 0.2; // -14 dB
        if (fEnvelope <= threshold)
            return 1.0;
        const double over = fEnvelope / threshold;
        return std::pow(over, 1.0 / 4.0 - 1.0); // what's over the threshold, at a quarter
    }
};

} // namespace twirl

#endif // TWIRL_ENGINE_HPP_INCLUDED
