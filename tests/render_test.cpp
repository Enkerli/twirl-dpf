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
    return failures;
}
