/*
 * Twirl's engine, offline: every voice at a few pitches and loudnesses.
 * Fails if a note is silent, clips, goes non-finite, or doesn't die away
 * (route C's bar is "sounds ok", so this guards against breakage, not
 * likeness). Writes each voice to build/render/<voice>.wav for listening.
 *
 *   make test
 */

#include "../plugins/Twirl/TwirlEngine.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace twirl;

static void writeWav(const std::string& path, const std::vector<float>& l, const std::vector<float>& r, const int sr)
{
    FILE* const f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
        return;
    const uint32_t n = static_cast<uint32_t>(l.size()), bytes = n * 4;
    auto u32 = [f](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [f](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(sr); u32(sr * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(bytes);
    for (uint32_t i = 0; i < n; ++i)
    {
        const int16_t a = static_cast<int16_t>(std::max(-1.0f, std::min(1.0f, l[i])) * 32767);
        const int16_t b = static_cast<int16_t>(std::max(-1.0f, std::min(1.0f, r[i])) * 32767);
        std::fwrite(&a, 2, 1, f); std::fwrite(&b, 2, 1, f);
    }
    std::fclose(f);
}

int main()
{
    const int sr = 48000, block = 256;
    const char* const names[] = { "pluck", "bell", "blip", "drum", "shake" };
    int failures = 0;
    static Engine engine; // large (its delay line): not on the stack

    for (int voice = 0; voice < 5; ++voice)
    {
        engine.setSampleRate(sr);
        engine.setTempo(96);
        engine.setWarmth(0.0f); // the voices alone: Warmth's hiss never dies away (that's its point)
        std::vector<float> L, R;
        // A phrase: pitches and loudnesses across the range, one every 0.4 s;
        // drums go through hat, snare and kick by height.
        const float pitches[] = { 36, 48, 55, 60, 67, 72, 79, 84 };
        const float vels[] = { 0.2f, 0.6f, 1.0f, 1.5f };
        int n = 0;
        for (const float vel : vels)
            for (const float midi : pitches)
            {
                const float y = (n % 3) * 0.33f + 0.1f;
                NoteSpec spec { static_cast<uint8_t>(voice), 0, midi, vel, (n % 5 - 2) * 0.4f, y, (n % 3 - 1) * 0.5f };
                const int at = n * static_cast<int>(0.4 * sr);
                // render up to this note, then start it
                while (static_cast<int>(L.size()) + block <= at)
                {
                    float l[block] = {}, r[block] = {};
                    engine.render(l, r, block);
                    L.insert(L.end(), l, l + block); R.insert(R.end(), r, r + block);
                }
                engine.play(spec, at - static_cast<int>(L.size()));
                ++n;
            }
        // and 4 s of tail
        for (int k = 0; k < 4 * sr / block; ++k)
        {
            float l[block] = {}, r[block] = {};
            engine.render(l, r, block);
            L.insert(L.end(), l, l + block); R.insert(R.end(), r, r + block);
        }

        double peak = 0, sum = 0, tail = 0;
        bool finite = true;
        for (size_t i = 0; i < L.size(); ++i)
        {
            const double a = std::max(std::fabs(L[i]), std::fabs(R[i]));
            finite = finite && std::isfinite(L[i]) && std::isfinite(R[i]);
            peak = std::max(peak, a);
            sum += L[i] * L[i] + R[i] * R[i];
            if (i + sr / 2 >= L.size()) tail = std::max(tail, a);  // the last half second
        }
        const double rms = std::sqrt(sum / (2.0 * L.size()));
        const bool ok = finite && peak > 0.01 && peak < 1.0 && tail < 0.001 && engine.sounding() == 0;
        std::printf("%-6s peak %.3f  rms %.4f  last 0.5 s %.6f  %s\n", names[voice], peak, rms, tail, ok ? "ok" : "FAIL");
        failures += ok ? 0 : 1;
        writeWav(std::string("build/render/") + names[voice] + ".wav", L, R, sr);
    }

    // Render `seconds` into L/R (appending).
    auto run = [&](std::vector<float>& L, std::vector<float>& R, const double seconds) {
        for (int k = 0; k < static_cast<int>(seconds * sr / block); ++k)
        {
            float l[block] = {}, r[block] = {};
            engine.render(l, r, block);
            L.insert(L.end(), l, l + block); R.insert(R.end(), r, r + block);
        }
    };
    auto stats = [&](const std::vector<float>& L, const std::vector<float>& R, const size_t from, const size_t to, double& peak, double& rms, bool& finite) {
        double sum = 0; peak = 0; finite = true;
        for (size_t i = from; i < to; ++i)
        {
            finite = finite && std::isfinite(L[i]) && std::isfinite(R[i]);
            peak = std::max(peak, static_cast<double>(std::max(std::fabs(L[i]), std::fabs(R[i]))));
            sum += L[i] * L[i] + R[i] * R[i];
        }
        rms = std::sqrt(sum / (2.0 * (to - from)));
    };

    // The Drone: a fifth (C2, G2) at its default level, swelling in, playing,
    // gliding to a new chord (as Walk moves it), then off: it fades away.
    {
        engine.setSampleRate(sr);
        engine.setWarmth(0.0f);
        std::vector<float> L, R;
        DroneSpec d;
        d.on = true; d.level = 0.3f; d.count = 2; d.freq[0] = 65.41f; d.freq[1] = 98.0f;
        engine.setDrone(d);
        run(L, R, 4.0);
        d.freq[0] = 73.42f; d.freq[1] = 110.0f;  // Walk: up a tone
        engine.setDrone(d);
        run(L, R, 3.0);
        double peak, rms, endPeak, endRms; bool finite, f2;
        stats(L, R, 2 * sr, L.size(), peak, rms, finite);
        d.on = false;
        engine.setDrone(d);
        const size_t off = L.size();
        run(L, R, 4.0);
        stats(L, R, L.size() - sr / 2, L.size(), endPeak, endRms, f2);
        const bool ok = finite && f2 && rms > 0.003 && peak < 0.5 && endPeak < 0.0005;
        std::printf("drone  peak %.3f  rms %.4f  after off %.6f  %s\n", peak, rms, endPeak, ok ? "ok" : "FAIL");
        failures += ok ? 0 : 1;
        (void)off;
        writeWav("build/render/drone.wav", L, R, sr);
    }

    // Warmth, alone: the hiss and the odd crackle, felt more than heard
    // (the page's: near -58 dB at 0.5), and nothing at 0.
    {
        engine.setSampleRate(sr);
        std::vector<float> L, R;
        engine.setWarmth(0.5f);
        run(L, R, 17.0);
        double peak, rms; bool finite;
        stats(L, R, sr, L.size(), peak, rms, finite);
        const double db = 20 * std::log10(std::max(1e-9, rms));
        engine.setWarmth(0.0f);
        std::vector<float> L0, R0;
        run(L0, R0, 1.0);
        double p0, r0; bool f0;
        stats(L0, R0, sr / 10, L0.size(), p0, r0, f0);   // past the wobble's few ms
        const bool ok = finite && db > -70 && db < -50 && peak < 0.1 && p0 < 1e-6;
        std::printf("warmth rms %.1f dB  peak %.4f  at 0: %.1e  %s\n", db, peak, p0, ok ? "ok" : "FAIL");
        failures += ok ? 0 : 1;
        writeWav("build/render/warmth.wav", L, R, sr);
    }
    return failures;
}
