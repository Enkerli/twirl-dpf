/*
 * Twirl's host parameters (route C step 5): automatable, shown by the host,
 * saved with its project. The page holds the same list in the same order
 * (HOST_PARAMS in Twirl's index.html): append only, hosts keep them by
 * number. A choice is its place in the page's list, a switch 0 or 1.
 *
 * Renumbered once, 2026-10-31 (31 → 20): a core of twelve, then eight knobs
 * the page points at any other control, so the list needn't grow with Twirl.
 * Follow host transport and Incoming MIDI only are settings saved with the
 * page now, not parameters. Projects saved before keep their values by
 * number (VST3, CLAP) or symbol (LV2), so their automation needs redoing.
 */

#ifndef TWIRL_PARAMS_HPP_INCLUDED
#define TWIRL_PARAMS_HPP_INCLUDED

#include <cstdint>

namespace twirl {

enum ParamKind : uint8_t { kFloat, kInt, kSwitch, kChoice };

struct ParamSpec {
    const char* symbol;
    const char* name;
    ParamKind kind;
    float min, max, def;
    const char* const* choices;   // kChoice: max + 1 names
    const char* unit;
};

static const char* const kWalks[] = { "Off", "A progression", "Sway", "By fifths", "Climb", "Wander" };
static const char* const kPacks[] = { "Paper", "Mallets", "Felt", "Steel", "Kalimba", "Chip", "Chimes", "Music box", "Foley" };
static const char* const kScales[] = {
    "Major pentatonic", "Minor pentatonic", "Suspended", "Hirajoshi",
    "Ionian", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Aeolian",
    "Harmonic minor", "Jazz minor", "Blues", "Whole tone", "Diminished HW", "Chromatic",
    "Maj 7 arp", "Min 7 arp",
    "5-EDO", "Slendro (approx.)", "Pelog (approx.)", "Maqam Rast (approx.)", "7-EDO", "19-EDO",
    "24-EDO (quarter tones)", "Harmonics 8-15", "Bohlen-Pierce",
};
static const char* const kRoots[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };

static const ParamSpec kParams[] = {
    // The core: what a track most often moves.
    { "volume",        "Volume",                kFloat,  0, 1,    0.8f, nullptr,    "" },
    { "tempo",         "Tempo",                 kInt,    60, 160, 96,   nullptr,    "BPM" },
    { "swing",         "Swing",                 kFloat,  0, 0.5f, 0,    nullptr,    "" },
    { "warmth",        "Warmth",                kFloat,  0, 1,    0.5f, nullptr,    "" },
    { "air",           "Air",                   kFloat,  0, 1,    0,    nullptr,    "" },
    // How much a note's place in its loop's cycle shapes it (louder, brighter, longer); as a new sheet's
    { "accents",       "Accents",               kFloat,  0, 1,    0.3f, nullptr,    "" },
    // The ground (layer 1): how much it colours each note where it sounds; none on a new sheet
    { "ground",        "Ground",                kFloat,  0, 1,    0,    nullptr,    "" },
    // The Drone: its level, 0 for off
    { "drone",         "Drone",                 kFloat,  0, 1,    0,    nullptr,    "" },
    // Walk: off, or how it moves
    { "walk",          "Walk",                  kChoice, 0, 5,    0,    kWalks,     "" },
    { "pack",          "Sounds",                kChoice, 0, 8,    0,    kPacks,     "" },
    { "scale",         "Scale",                 kChoice, 0, 26,   0,    kScales,    "" },
    { "root",          "Root",                  kChoice, 0, 11,   0,    kRoots,     "" },
    // Eight knobs, each pointed in Twirl (Everything → Sound and room) at any other control
    // (Lock, Vary, a group's level…), 0..1 across its range; saved with the page
    { "knob_1",        "Knob 1",                kFloat,  0, 1,    0,    nullptr,    "" },
    { "knob_2",        "Knob 2",                kFloat,  0, 1,    0,    nullptr,    "" },
    { "knob_3",        "Knob 3",                kFloat,  0, 1,    0,    nullptr,    "" },
    { "knob_4",        "Knob 4",                kFloat,  0, 1,    0,    nullptr,    "" },
    { "knob_5",        "Knob 5",                kFloat,  0, 1,    0,    nullptr,    "" },
    { "knob_6",        "Knob 6",                kFloat,  0, 1,    0,    nullptr,    "" },
    { "knob_7",        "Knob 7",                kFloat,  0, 1,    0,    nullptr,    "" },
    { "knob_8",        "Knob 8",                kFloat,  0, 1,    0,    nullptr,    "" },
};

static constexpr uint32_t kParamCount = sizeof(kParams) / sizeof(kParams[0]);
static_assert(kParamCount <= 32, "the changed-parameter masks are 32 bits");

}

#endif // TWIRL_PARAMS_HPP_INCLUDED
