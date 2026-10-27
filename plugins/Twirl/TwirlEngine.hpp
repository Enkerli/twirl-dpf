/*
 * Twirl's sound, made in the plugin (route C, music-suite apps/twirl/PLAN.md).
 *
 * The page decides every note (it's still Twirl's brain) and sends it, timed;
 * this plays it, sample-accurate, in step with the MIDI and the host. The
 * voices follow the page's Web Audio recipes (index.html, section 3) with the
 * same building blocks, approximately: Alex, 2026-10-26, "Doesn't even need
 * to sound that close to the current one, as long as it sounds ok."
 *
 * Step 1: Paper's five voices, the delay, the volume and a compressor.
 * Step 2: the six other packs (Mallets, Felt, Steel, Kalimba, Chip,
 * Chimes); a voice a pack doesn't have is Paper's, as in the page. Step 3 (first, as Alex asked): the Drone and
 * Warmth's room (hiss, crackle, wow and flutter). Real-time safe: fixed
 * pools, no allocation, no locks.
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
/** How much longer the note being started rings: Twirl's accents (1 = as made, up to 1.5).
 *  Set by Engine::play around each note, on the audio thread; every envelope's decay reads it. */
static double& ringNow() noexcept { static thread_local double r = 1.0; return r; }

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
        decaySamples = decay * ringNow() * sr;
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
    float nuance;      // Vary's -1..1 (an accent brightens it)
    float ring;        // an accent's longer ring, 1..1.5 (0, from an older page: 1)
};

enum : uint8_t { kPluck = 0, kBell, kBlip, kDrum, kShake };
// The packs, in Twirl's PACK_ORDER (share codes store the index).
enum : uint8_t { kPaper = 0, kMallets, kFelt, kSteel, kKalimba, kChip, kChimes };
// What a sounding voice is, beyond Paper's own five.
enum : uint8_t { kGeneric = 10, kPartials, kVibe };

/** The Drone as the page has it now (its notes already through scale, root,
 *  Walk; its "space" as numbers: Twirl's DRONE_SPACES). */
struct DroneSpec {
    static constexpr int kMaxNotes = 8;
    bool on = false;
    float level = 0.3f;            // the slider (0..1); the gain is 0.1 × level²
    float side = 0.35f, rate = 0.031f, panSwing = 0.35f, trem = 0.0f, dop = 0.0f, split = 0.0f;
    int count = 0;
    float freq[kMaxNotes] = {};    // Hz
};

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

    // kGeneric: an oscillator (and a second at a ratio of it) and/or noise,
    // through a filter whose cutoff may move, under one envelope.
    bool useOsc = false, useNoise = false, useFilter = false;
    double osc2Ratio = 0, osc2Level = 0;

    // kPartials: up to five sines (rotating phasors: cheap, exact enough), each its own envelope.
    static constexpr int kMaxPartials = 5;
    int pn = 0;
    double pre[kMaxPartials] = {}, pim[kMaxPartials] = {}, pc[kMaxPartials] = {}, ps[kMaxPartials] = {};
    Env pe[kMaxPartials];
    int pleft[kMaxPartials] = {};

    // kVibe: a slow tremolo
    double trem = 0;

    void setPan(const float pan) noexcept // Web Audio's StereoPannerNode, mono in: equal power
    {
        const double x = (std::max(-1.0f, std::min(1.0f, pan)) + 1.0) * 0.5;
        gainL = static_cast<float>(std::cos(x * kPi / 2));
        gainR = static_cast<float>(std::sin(x * kPi / 2));
    }
};

class Engine {
public:
    static constexpr int kVoices = 384;

    void setSampleRate(const double sr) noexcept
    {
        fSr = sr;
        fDelay.assign(sr);
        fDelayLp.set(Filter::Lowpass, 2400.0, 1.0, sr); // Web Audio's default Q (1 dB)
        roomSetup();
        reset();
    }

    void reset() noexcept
    {
        for (Voice& v : fVoices) v.active = false;
        fDelay.clear();
        fDelayLp.reset();
        fEnvelope = 0.0;
        fDrone.sounding = false;
        fMidiDrone.sounding = false;
    }

    void setVolume(const float v) noexcept { fVolumeTarget = std::max(0.0f, std::min(2.0f, v)); }
    void setWarmth(const float w) noexcept { fWarmth = std::max(0.0f, std::min(1.0f, w)); }

    /** The Drone: on, off, its notes (gliding there), its level and space. */
    void setDrone(const DroneSpec& d) noexcept { setDroneOf(fDrone, d); }
    /** A second Drone, for one coming in as MIDI (synth mode: channel 4's held notes). */
    void setMidiDrone(const DroneSpec& d) noexcept { setDroneOf(fMidiDrone, d); }
    bool midiDroneSounding() const noexcept { return fMidiDrone.sounding; }

private:
    struct Drone;
    void setDroneOf(Drone& z, const DroneSpec& d) noexcept
    {
        const bool fresh = d.on && (! z.sounding || d.count != z.spec.count);
        z.spec = d;
        z.spec.count = std::max(0, std::min(d.count, DroneSpec::kMaxNotes));
        if (fresh)
        {
            // Two saws a note, 6 cents either side, and ±3 cents note to note (as the page's).
            for (int i = 0; i < z.spec.count; ++i)
                for (int j = 0; j < 2; ++j)
                {
                    z.saw[i][j].wave = Wave::Saw;
                    z.saw[i][j].phase = fRand.next();
                    z.cents[i][j] = (j ? 6.0f : -6.0f) + (i % 2 ? 3.0f : -3.0f);
                    z.freq[i][j] = d.freq[i] + (j ? 1 : -1) * d.split / 2;
                }
            z.gain.set(0.0001);
            z.gain.rampTo(droneGain(d.level), 2.0 * fSr);   // swells in over 2 s
            z.lp[0].reset(); z.lp[1].reset();
            z.sounding = true;
        }
        z.fading = ! d.on;
    }

public:
    void setTempo(const double bpm) noexcept { if (bpm > 0) fDelaySeconds = 0.75 * 60.0 / bpm; }

    /** Start a note `offset` samples into the next render. */
    /** Start a note `offset` samples into the next render, in its pack's sound. */
    void play(const NoteSpec& n, const int offset) noexcept
    {
        ringNow() = n.ring > 0.0f ? std::max(1.0f, std::min(2.0f, n.ring)) : 1.0f;
        if (! playPack(n, offset))
            playPaper(n, offset);
        ringNow() = 1.0;
    }

    /** Paper's five voices (and every pack's fallback). */
    void playPaper(const NoteSpec& n, const int offset) noexcept
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

        // The voices, one at a time over the block (each voice's state stays in
        // cache), into dry and delay-send buses; then the rest, sample by sample.
        static constexpr uint32_t kChunk = 256;
        for (uint32_t c0 = 0; c0 < frames; c0 += kChunk)
        {
        const uint32_t cn = std::min(kChunk, frames - c0);
        std::memset(fBusL, 0, sizeof(double) * cn);
        std::memset(fBusR, 0, sizeof(double) * cn);
        std::memset(fBusWet, 0, sizeof(double) * cn);
        for (Voice& v : fVoices)
        {
            if (! v.active)
                continue;
            uint32_t i = 0;
            if (v.wait > 0)
            {
                const uint32_t skip = std::min<uint32_t>(cn, static_cast<uint32_t>(v.wait));
                v.wait -= static_cast<int>(skip);
                i = skip;
            }
            const double gl = v.gainL, gr = v.gainR, gw = (v.gainL + v.gainR) * 0.5 * v.send;
            if (v.kind == kPartials)
            {
                // All its partials together, sample by sample: each partial is a chain
                // (every sample needs the one before), so stepping them side by side
                // lets the processor overlap them. The heaviest voices' fast path.
                const uint32_t m = std::min<uint32_t>(cn - i, static_cast<uint32_t>(std::max(0, v.left)));
                const int pn = v.pn;
                for (uint32_t k = 0; k < m; ++k)
                {
                    double x = 0;
                    for (int p = 0; p < pn; ++p)
                    {
                        if (v.pleft[p] <= 0) continue;
                        --v.pleft[p];
                        const double re = v.pre[p] * v.pc[p] - v.pim[p] * v.ps[p];
                        v.pim[p] = v.pre[p] * v.ps[p] + v.pim[p] * v.pc[p];
                        v.pre[p] = re;
                        x += v.pim[p] * v.pe[p].next();
                    }
                    fTmp[k] = x;
                }
                for (uint32_t k = 0; k < m; ++k)
                {
                    const double x = fTmp[k];
                    fBusL[i + k] += x * gl; fBusR[i + k] += x * gr; fBusWet[i + k] += x * gw;
                }
                v.left -= static_cast<int>(m);
                if (v.left <= 0) v.active = false;
                continue;
            }
            for (; i < cn; ++i)
            {
                const double s = tick(v);
                fBusL[i] += s * gl;
                fBusR[i] += s * gr;
                fBusWet[i] += s * gw;
                if (--v.left <= 0) { v.active = false; break; }
            }
        }

        for (uint32_t k = 0; k < cn; ++k)
        {
            const uint32_t i = c0 + k;
            double dryL = fBusL[k], dryR = fBusR[k], wet = fBusWet[k];

            double droneL = 0, droneR = 0;
            for (Drone* const z : { &fDrone, &fMidiDrone })
                if (z->sounding)
                {
                    double l = 0, r = 0;
                    droneTick(*z, l, r);
                    droneL += l; droneR += r;
                    wet += (l + r) * 0.5 * 0.35;
                }

            // The delay: a dotted eighth, darkened each time round (Twirl's delaySend 0.28, feedback 0.38).
            const double echoed = fDelayLp.next(fDelay.read(delaySamples));
            fDelay.write(wet * 0.28 + echoed * 0.38);

            // The room (Warmth): hiss and crackle into the master, then the whole
            // mix through a drifting delay (wow and flutter), as the page's.
            double hissL = 0, hissR = 0;
            if (fWarmth > 0.0f)
                roomTick(hissL, hissR);

            // Volume (smoothed), the wobble, then the compressor.
            fVolume += (fVolumeTarget - fVolume) * 0.001;
            double l = (dryL + droneL + echoed + hissL) * fVolume, r = (dryR + droneR + echoed + hissR) * fVolume;
            wobble(l, r);
            const double g = compress(std::max(std::fabs(l), std::fabs(r)));
            outL[i] += static_cast<float>(l * g);
            outR[i] += static_cast<float>(r * g);
        }
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

    float fWarmth = 0.5f;
    double fBusL[256], fBusR[256], fBusWet[256], fTmp[256];

    // ---- the Drone --------------------------------------------------------
    struct Drone {
        DroneSpec spec;
        bool sounding = false, fading = false;
        Osc saw[DroneSpec::kMaxNotes][2];
        float cents[DroneSpec::kMaxNotes][2] = {};
        double freq[DroneSpec::kMaxNotes][2] = {};    // gliding toward the spec's
        ExpRamp gain;
        Biquad lp[2];
        double lfo = 0, rotor = 0, wob = 0, wobRate = 0.19;
        int every = 0;
        float rd[512] = {}; int rdPos = 0;          // the rotor's moving delay (Doppler), ~10 ms
    } fDrone, fMidiDrone;

    static double droneGain(const float level) noexcept { return std::max(0.0002, 0.1 * level * level); }

    void droneTick(Drone& z, double& outL, double& outR) noexcept
    {
        const DroneSpec& d = z.spec;
        const double sr = fSr;
        // Level: toward the slider's (or toward silence, fading out over ~1 s).
        const double target = z.fading ? 0.0001 : droneGain(d.level);
        if (z.gain.left == 0)
            z.gain.value += (target - z.gain.value) * (1.0 / (0.3 * sr));
        const double g = z.gain.next();
        if (z.fading && g < 0.0003) { z.sounding = false; return; }

        // Slow wheels: the cutoff's breath (0.06 Hz, ±350 Hz), the rotor, the tape wobble.
        z.lfo += 0.06 / sr; z.lfo -= std::floor(z.lfo);
        z.rotor += d.rate / sr; z.rotor -= std::floor(z.rotor);
        z.wob += z.wobRate / sr; z.wob -= std::floor(z.wob);
        if (z.every-- <= 0)
        {
            const double cut = 800.0 + 350.0 * std::sin(2 * kPi * z.lfo);
            z.lp[0].set(Filter::Lowpass, cut, -3.1, sr); // Q 0.7 (as the page's), in dB
            z.lp[1].set(Filter::Lowpass, cut, -3.1, sr);
            z.every = 31;
        }
        const double wobCents = 5.0 * fWarmth * std::sin(2 * kPi * z.wob);

        double side[2] = { 0, 0 };
        for (int i = 0; i < d.count; ++i)
            for (int j = 0; j < 2; ++j)
            {
                const double want = d.freq[i] + (j ? 1 : -1) * d.split / 2;
                z.freq[i][j] += (want - z.freq[i][j]) * (1.0 / (0.4 * sr)); // glide (setTargetAtTime, 0.4 s)
                const double f = z.freq[i][j] * std::pow(2.0, (z.cents[i][j] + wobCents) / 1200.0);
                side[j] += z.saw[i][j].next(f, sr);
            }
        // Each side through its lowpass, panned to its side.
        double l = 0, r = 0;
        for (int j = 0; j < 2; ++j)
        {
            const double x = z.lp[j].next(side[j]) * g;
            const double p = ((j ? 1 : -1) * d.side + 1.0) * 0.5;
            l += x * std::cos(p * kPi / 2);
            r += x * std::sin(p * kPi / 2);
        }
        // The rotor: a moving delay (Doppler), a swinging pan, a swelling level.
        const double wheel = std::sin(2 * kPi * z.rotor);
        double mono = (l + r) * 0.5;
        z.rd[z.rdPos] = static_cast<float>(mono);
        const double back = (0.002 + d.dop * wheel) * sr;
        const double pos = z.rdPos - std::max(1.0, std::min(500.0, back));
        const int p0 = static_cast<int>(std::floor(pos)), q0 = (p0 + 512) % 512, q1 = (q0 + 1) % 512;
        const double fr = pos - std::floor(pos);
        mono = z.rd[q0] + (z.rd[q1] - z.rd[q0]) * fr;
        z.rdPos = (z.rdPos + 1) % 512;
        const double swing = d.panSwing * wheel, tremGain = 1.0 - d.trem + d.trem * wheel;
        const double width = 0.5 * (l - r);
        l = (mono + width) * tremGain; r = (mono - width) * tremGain;
        // Pan as a balance (the page pans the stereo drone with a StereoPanner).
        const double pp = (swing + 1.0) * 0.5;
        outL = l * std::cos(pp * kPi / 2) * 1.41421356;
        outR = r * std::sin(pp * kPi / 2) * 1.41421356;
    }

    // ---- the room ---------------------------------------------------------
    struct Room {
        double b[2][7] = {};            // pink noise (Paul Kellet's), per channel
        double pinkScale = 1.0;         // to an RMS of 0.2, as the page's
        Biquad hissHp[2], hissLp[2], crackleHp[2];
        int tick = 0, tickLen = 0; double tickAmp = 0; float where = 0.5f;
        float line[2][4096] = {}; int pos = 0; // the wobble's delay, ~85 ms at 48 kHz
        double wow = 0, flutter = 0;
    } fRoom;

    void roomSetup() noexcept
    {
        Room& m = fRoom;
        for (int c = 0; c < 2; ++c)
        {
            m.hissHp[c].set(Filter::Highpass, 60.0, 1.0, fSr);
            m.hissLp[c].set(Filter::Lowpass, 7000.0, 1.0, fSr);
            m.crackleHp[c].set(Filter::Highpass, 1200.0, 1.0, fSr);
        }
        // Measure the pink filter's level once, to scale it to an RMS of 0.2.
        double sq = 0;
        for (int i = 0; i < 48000; ++i) { const double x = pink(0); sq += x * x; }
        m.pinkScale = 0.2 / std::sqrt(sq / 48000.0);
        std::memset(m.b, 0, sizeof(m.b));
    }

    double pink(const int c) noexcept
    {
        double* const b = fRoom.b[c];
        const double w = fRand.bipolar();
        b[0] = 0.99886 * b[0] + w * 0.0555179; b[1] = 0.99332 * b[1] + w * 0.0750759; b[2] = 0.969 * b[2] + w * 0.153852;
        b[3] = 0.8665 * b[3] + w * 0.3104856; b[4] = 0.55 * b[4] + w * 0.5329522; b[5] = -0.7616 * b[5] - w * 0.016898;
        const double y = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362;
        b[6] = w * 0.115926;
        return y;
    }

    void roomTick(double& outL, double& outR) noexcept
    {
        Room& m = fRoom;
        const double w = fWarmth;
        // Hiss: pink noise, 60 Hz to 7 kHz, near -58 dB at Warmth 0.5.
        for (int c = 0; c < 2; ++c)
        {
            const double h = m.hissLp[c].next(m.hissHp[c].next(pink(c) * m.pinkScale)) * 0.012 * w;
            (c ? outR : outL) += h;
        }
        // Crackle: about nine ticks in 17 s, one in five a pop, some in one ear.
        if (m.tickLen == 0 && fRand.next() < 9.0 / (17.0 * fSr))
        {
            const bool pop = fRand.next() < 0.2f;
            m.tickLen = static_cast<int>(pop ? 60 + fRand.next() * 120 : 6 + fRand.next() * 24);
            m.tickAmp = pop ? 1.0 : 0.3 + fRand.next() * 0.5;
            m.tick = 0;
            m.where = fRand.next();
        }
        double cl = 0, cr = 0;
        if (m.tickLen > 0)
        {
            const double v = m.tickAmp * std::exp(-m.tick / (m.tickLen / 4.0)) * (m.tick % 2 ? -1 : 1) * (0.6 + fRand.next() * 0.4);
            if (m.where < 0.7f) cl = v;
            if (m.where > 0.3f) cr = v;
            if (++m.tick >= m.tickLen) m.tickLen = 0;
        }
        outL += m.crackleHp[0].next(cl) * 0.03 * w;
        outR += m.crackleHp[1].next(cr) * 0.03 * w;
    }

    /** Wow (0.45 Hz) and flutter (6.3 Hz): the mix through a delay of 3 ms that drifts, scaled by Warmth. */
    void wobble(double& l, double& r) noexcept
    {
        Room& m = fRoom;
        m.line[0][m.pos] = static_cast<float>(l);
        m.line[1][m.pos] = static_cast<float>(r);
        m.wow += 0.45 / fSr; m.wow -= std::floor(m.wow);
        m.flutter += 6.3 / fSr; m.flutter -= std::floor(m.flutter);
        const double d = (0.003 + fWarmth * (0.0003 * std::sin(2 * kPi * m.wow) + 0.00001 * std::sin(2 * kPi * m.flutter))) * fSr;
        const double at = m.pos - d;
        const int i0 = static_cast<int>(std::floor(at)), a = (i0 % 4096 + 4096) % 4096, b = (a + 1) % 4096;
        const double fr = at - std::floor(at);
        l = m.line[0][a] + (m.line[0][b] - m.line[0][a]) * fr;
        r = m.line[1][a] + (m.line[1][b] - m.line[1][a]) * fr;
        m.pos = (m.pos + 1) % 4096;
    }

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

    // ---- the other packs (index.html, section 3a) ----------------------------

    /** A fresh voice, waiting `offset` samples, panned. */
    Voice& fresh(const uint8_t kind, const int offset, const float pan, const float send, const double seconds) noexcept
    {
        Voice& v = take();
        v.kind = kind;
        v.wait = std::max(0, offset);
        v.sr = fSr;
        v.f1.reset(); v.f2.reset();
        v.osc1.phase = v.osc2.phase = 0.0;
        v.noiseLeft = v.osc2Left = 0;
        v.cutEvery = 0;
        v.useOsc = v.useNoise = v.useFilter = false;
        v.osc2Ratio = v.osc2Level = 0;
        v.pn = 0;
        v.trem = 0;
        v.setPan(pan);
        v.send = send;
        v.left = static_cast<int>(seconds * fSr);
        v.active = true;
        return v;
    }

    /** One oscillator, its pitch from `f0` (gliding to `f1` over `glide` s if given), under an envelope. */
    void tone(const int offset, const float pan, const float send, const Wave wave, const double f0, const double f1, const double glide,
              const double peak, const double decay, const double attack, const double stop) noexcept
    {
        Voice& v = fresh(kGeneric, offset, pan, send, stop);
        v.useOsc = true;
        v.osc1.wave = wave;
        v.freq1.set(f0);
        if (glide > 0) v.freq1.rampTo(f1, glide * fSr);
        v.env1.start(fSr, peak, decay, attack);
    }

    /** Noise through a filter, under an envelope. */
    void noiseHit(const int offset, const float pan, const float send, const Filter type, const double freq, const double q,
                  const double peak, const double decay, const double attack, const double dur) noexcept
    {
        Voice& v = fresh(kGeneric, offset, pan, send, dur);
        v.useNoise = v.useFilter = true;
        v.noiseLeft = static_cast<int>(dur * fSr);
        v.f1.set(type, freq, q, fSr);
        v.env1.start(fSr, peak, decay, attack);
    }

    struct Partial { double mul, amp, decay; };

    /** Twirl's partials(): a struck tone from sines, each with its level and decay. */
    void partials(const int offset, const float pan, const double f0, const double vel, const Partial* const list, const int count,
                  const double attack = 0.003, const float send = 0.5f) noexcept
    {
        double longest = 0;
        for (int i = 0; i < count; ++i) longest = std::max(longest, list[i].decay);
        Voice& v = fresh(kPartials, offset, pan, send, attack + longest + 0.05);
        v.pn = std::min(count, Voice::kMaxPartials);
        for (int i = 0; i < v.pn; ++i)
        {
            const double w = 2.0 * kPi * std::min(f0 * list[i].mul, fSr * 0.45) / fSr;
            v.pre[i] = 1.0; v.pim[i] = 0.0;
            v.pc[i] = std::cos(w); v.ps[i] = std::sin(w);
            v.pe[i].start(fSr, list[i].amp * vel, list[i].decay, attack);
            v.pleft[i] = static_cast<int>((attack + list[i].decay + 0.05) * fSr);
        }
    }

    /** Mallets' drums: log drum, woodblock, claves (Steel and Kalimba use them too). */
    void malletDrum(const NoteSpec& n, const int offset) noexcept
    {
        const double y = n.y;
        const int kind = y < 0.28 ? 0 : y < 0.62 ? 1 : 2;
        const double f = kind == 2 ? mtof(n.midi - 12.0) : kind == 1 ? 750.0 + (0.62 - y) * 600.0 : 2400.0;
        tone(offset, n.pan, 0.25f, Wave::Sine, f * (kind == 2 ? 1.6 : 1.0), f, 0.03,
             (kind == 2 ? 0.7 : 0.35) * n.vel, kind == 2 ? 0.3 : 0.05, 0.001, 0.4);
    }

    /** Felt's pluck: a triangle and a sine an octave up, through a lowpass that closes over 1.2 s. */
    void feltPluck(const int offset, const float pan, const double midi, const double vel) noexcept
    {
        Voice& v = fresh(kGeneric, offset, pan, 0.45f, 1.4);
        v.useOsc = v.useFilter = true;
        v.osc1.wave = Wave::Triangle;
        v.osc2.wave = Wave::Sine;
        v.freq1.set(mtof(midi));
        v.osc2Ratio = 2.003; v.osc2Level = 1.0;
        v.cutType = Filter::Lowpass; v.cutQ = 1.0;
        v.cutoff.set(900.0 + 1600.0 * vel);
        v.cutoff.rampTo(400.0, 1.2 * fSr);
        v.env1.start(fSr, 0.24 * vel, 1.3, 0.012);
    }

    /** Kalimba's pluck: a clean tine, a high inharmonic ping, a tiny click. */
    void kalimbaPluck(const int offset, const float pan, const double midi, const double vel) noexcept
    {
        static const Partial tine[] = { { 1, 0.26, 1.1 }, { 5.95, 0.05, 0.06 } };
        partials(offset, pan, mtof(midi), vel, tine, 2, 0.002, 0.45f);
        noiseHit(offset, pan, 0.2f, Filter::Bandpass, 3500.0, 1.0, 0.05 * vel, 0.01, 0.001, 0.03);
    }

    /** The pack's own voice, if it has one (false: Paper's). */
    bool playPack(const NoteSpec& n, const int o) noexcept
    {
        const double vel = n.vel, nu = n.nuance, m = n.midi;
        const float pan = n.pan;
        switch (n.pack)
        {
        case kMallets:
            switch (n.voice)
            {
            case kPluck: { // marimba: a fundamental and a quick 4th partial
                static const Partial p[] = { { 1, 0.3, 0.5 }, { 3.93, 0.08, 0.08 } };
                partials(o, pan, mtof(m), vel, p, 2, 0.002, 0.4f);
                return true;
            }
            case kBell: { // vibraphone-ish: a sine with a slow shimmer
                Voice& v = fresh(kVibe, o, pan, 0.7f, 1.8);
                v.useOsc = true;
                v.osc1.wave = Wave::Sine;
                v.freq1.set(mtof(m + 12));
                v.env1.start(fSr, 0.18 * vel, 1.6, 0.003);
                return true;
            }
            case kBlip: // xylophone: bright and short
                tone(o, pan, 0.6f, Wave::Triangle, mtof(m + 24), 0, 0, 0.16 * vel, 0.12, 0.001, 0.2);
                return true;
            case kDrum:
                malletDrum(n, o);
                return true;
            }
            return false;
        case kChip:
            switch (n.voice)
            {
            case kPluck: // a square, no frills
                tone(o, pan, 0.4f, Wave::Square, mtof(m), 0, 0, 0.09 * vel, 0.22, 0.002, 0.3);
                return true;
            case kBell: { // a fast up-arpeggio, the chiptune chord
                const int iv[] = { 0, 4, 7, 12 };
                for (int i = 0; i < 4; ++i)
                    tone(o + static_cast<int>(i * 0.045 * fSr), pan, 0.5f, Wave::Square, mtof(m + 12 + iv[i]), 0, 0, 0.06 * vel, 0.06, 0.001, 0.1);
                return true;
            }
            case kBlip: // the classic pitch-drop blip
                tone(o, pan, 0.5f, Wave::Square, mtof(m + 24), mtof(m + 12), 0.08, 0.07 * vel, 0.1, 0.001, 0.15);
                return true;
            case kDrum: {
                const double y = n.y;
                const int kind = y < 0.28 ? 0 : y < 0.62 ? 1 : 2;
                if (kind == 2) // a square kick
                    tone(o, pan * 0.3f, 0.1f, Wave::Square, 220.0, 45.0, 0.1, 0.35 * vel, 0.14, 0.001, 0.2);
                else           // noise: a hat (highpass) or a snare (lowpass)
                    noiseHit(o, pan, 0.2f, kind == 0 ? Filter::Highpass : Filter::Lowpass, kind == 0 ? 8000.0 : 5000.0, 1.0,
                             (kind == 0 ? 0.2 : 0.35) * vel, kind == 0 ? 0.03 : 0.12, 0.001, 0.16);
                return true;
            }
            case kShake:
                noiseHit(o, pan, 0.2f, Filter::Highpass, 9000.0, 1.0, 0.18 * vel, 0.025, 0.001, 0.05);
                return true;
            }
            return false;
        case kFelt: // a soft, felted upright: round attack, warm tail
            switch (n.voice)
            {
            case kPluck: feltPluck(o, pan, m, vel); return true;
            case kBell:  feltPluck(o, pan, m + 12, vel * 0.8); return true;
            case kBlip: {
                static const Partial p[] = { { 1, 0.12, 0.18 }, { 2, 0.03, 0.08 } };
                partials(o, pan, mtof(m + 24), vel, p, 2, 0.008);
                return true;
            }
            }
            return false;
        case kSteel: // steelpan: bright partials at 1, 2 and 3
            switch (n.voice)
            {
            case kPluck: { static const Partial p[] = { { 1, 0.22, 0.9 }, { 2, 0.1, 0.5 }, { 3.01, 0.05, 0.25 } }; partials(o, pan, mtof(m), vel, p, 3, 0.006, 0.6f); return true; }
            case kBell:  { static const Partial p[] = { { 1, 0.18, 1.2 }, { 2, 0.08, 0.6 }, { 3.01, 0.04, 0.3 } }; partials(o, pan, mtof(m + 12), vel, p, 3, 0.006, 0.7f); return true; }
            case kBlip:  { static const Partial p[] = { { 1, 0.12, 0.25 }, { 2, 0.04, 0.12 } }; partials(o, pan, mtof(m + 24), vel, p, 2, 0.003, 0.6f); return true; }
            case kDrum:  malletDrum(n, o); return true;
            }
            return false;
        case kKalimba: // thumb piano
            switch (n.voice)
            {
            case kPluck: kalimbaPluck(o, pan, m, vel); return true;
            case kBell:  kalimbaPluck(o, pan, m + 12, vel * 0.85); return true;
            case kBlip:  { static const Partial p[] = { { 1, 0.13, 0.3 }, { 5.95, 0.03, 0.03 } }; partials(o, pan, mtof(m + 24), vel, p, 2, 0.002, 0.45f); return true; }
            case kDrum:  malletDrum(n, o); return true;
            }
            return false;
        case kChimes: { // wind chimes: tuned bars, inharmonic partials, long quiet tails
            const double k = 1.0 + 0.006 * nu; // Vary: some strikes clang more
            switch (n.voice)
            {
            case kBell: {
                const Partial p[] = { { 1, 0.09, 4.5 }, { 1.0016, 0.04, 4 }, { 2.756 * k, 0.05, 2.4 }, { 5.404 * k, 0.025, 1.1 }, { 8.933 * k, 0.012, 0.5 } };
                partials(o, pan, mtof(m + 12), vel, p, 5, 0.002, 0.9f);
                return true;
            }
            case kPluck: { static const Partial p[] = { { 1, 0.14, 1.4 }, { 2.756, 0.05, 0.6 }, { 5.404, 0.02, 0.25 } }; partials(o, pan, mtof(m), vel, p, 3, 0.003, 0.6f); return true; }
            case kBlip:  { static const Partial p[] = { { 1, 0.07, 1.2 }, { 2.756, 0.025, 0.5 } }; partials(o, pan, mtof(m + 24), vel, p, 2, 0.001, 0.9f); return true; }
            }
            return false;
        }
        }
        return false;
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
        case kGeneric: {
            double x = 0;
            if (v.useOsc)
            {
                const double f = v.freq1.next();
                x = v.osc1.next(f, sr);
                if (v.osc2Level > 0) x += v.osc2.next(f * v.osc2Ratio, sr) * v.osc2Level;
            }
            if (v.useNoise && v.noiseLeft-- > 0) x += fRand.bipolar();
            if (v.useFilter)
            {
                if (v.cutoff.left > 0 && v.cutEvery-- <= 0) { v.f1.set(v.cutType, v.cutoff.value, v.cutQ, sr); v.cutEvery = 15; }
                if (v.cutoff.left > 0) v.cutoff.next();
                x = v.f1.next(x);
            }
            return x * v.env1.next();
        }
        case kPartials: {
            double x = 0;
            for (int i = 0; i < v.pn; ++i)
            {
                if (v.pleft[i]-- <= 0) continue;
                const double re = v.pre[i] * v.pc[i] - v.pim[i] * v.ps[i];
                v.pim[i] = v.pre[i] * v.ps[i] + v.pim[i] * v.pc[i];
                v.pre[i] = re;
                x += v.pim[i] * v.pe[i].next();
            }
            // Kalimba's click and the like ride along as their own voices.
            return x;
        }
        case kVibe: {
            v.trem += 5.5 / sr; v.trem -= std::floor(v.trem);
            const double tremGain = 0.75 + 0.25 * std::sin(2 * kPi * v.trem);
            return v.osc1.next(v.freq1.value, sr) * v.env1.next() * tremGain;
        }
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
