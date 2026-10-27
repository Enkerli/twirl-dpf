/*
 * Twirl's engine, offline, note by note, for comparing with the page's own
 * voices (music-suite's apps/twirl/tools/compare-engine.mjs renders both).
 * Dry (no delay, no Warmth), through Twirl's volume (0.8), mono ((L + R) / 2,
 * as the page's one-channel offline rendering mixes it), 48 kHz float32.
 *
 *   build/render_notes <out dir>   with lines on stdin: name pack voice midi vel y seconds shaker
 *
 * pack and voice as the page sends them (PACK_ORDER; 0 pluck … 4 shake), midi
 * the exact pitch. Writes <out dir>/<name>.f32.
 */

#include "../plugins/Twirl/TwirlEngine.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace twirl;

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: render_notes <out dir> < notes\n"); return 2; }
    const int sr = 48000, block = 256;
    static Engine engine; // large (its delay line): not on the stack
    char name[256];
    int pack = 0, voice = 0, shaker = 0;
    float midi = 60, vel = 0.8f, y = 0.5f, seconds = 2;
    while (std::scanf("%255s %d %d %f %f %f %f %d", name, &pack, &voice, &midi, &vel, &y, &seconds, &shaker) == 8)
    {
        engine.setSampleRate(sr); // and reset: nothing left from the note before
        engine.setTempo(96);
        engine.setWarmth(0.0f);
        engine.setDelayOn(false);
        engine.setVolume(0.8f);
        const NoteSpec n { static_cast<uint8_t>(voice), static_cast<uint8_t>(pack), midi, vel, 0.0f, y, 0.0f, 1.0f, static_cast<uint8_t>(shaker) };
        engine.play(n, 0);
        const int frames = static_cast<int>(seconds * sr);
        std::vector<float> L(block), R(block), mono;
        mono.reserve(frames);
        for (int done = 0; done < frames; done += block)
        {
            const int m = std::min(block, frames - done);
            std::fill(L.begin(), L.end(), 0.0f); std::fill(R.begin(), R.end(), 0.0f);   // it adds into them (the plugin clears them first)
            engine.render(L.data(), R.data(), static_cast<uint32_t>(m));
            for (int i = 0; i < m; ++i) mono.push_back(0.5f * (L[i] + R[i]));
        }
        const std::string path = std::string(argv[1]) + "/" + name + ".f32";
        FILE* const f = std::fopen(path.c_str(), "wb");
        if (f == nullptr) { std::fprintf(stderr, "can't write %s\n", path.c_str()); return 1; }
        std::fwrite(mono.data(), sizeof(float), mono.size(), f);
        std::fclose(f);
    }
    return 0;
}
