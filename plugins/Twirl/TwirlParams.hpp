/*
 * Twirl's host parameters (route C step 5): automatable, shown by the host,
 * saved with its project. The page holds the same list in the same order
 * (HOST_PARAMS in Twirl's index.html): append only, hosts keep them by
 * number. A choice is its place in the page's list, a switch 0 or 1.
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

static const char* const kVoicings[] = { "Low fifth", "Fifth", "Triad", "Open" };
static const char* const kSpaces[] = { "Still", "Drifting", "Binaural", "Rotating" };
static const char* const kWalkWays[] = { "A progression", "Sway", "By fifths", "Climb", "Wander" };
static const char* const kWalkPaces[] = { "Every bar", "Every 2 bars", "Every 4 bars", "Every 8 bars" };
static const char* const kPacks[] = { "Paper", "Mallets", "Felt", "Steel", "Kalimba", "Chip", "Chimes" };
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
    { "volume",        "Volume",                kFloat,  0, 1,    0.8f, nullptr,    "" },
    { "tempo",         "Tempo",                 kInt,    60, 160, 96,   nullptr,    "BPM" },
    { "follow_host",   "Follow host transport", kSwitch, 0, 1,    0,    nullptr,    "" },
    { "swing",         "Swing",                 kFloat,  0, 0.5f, 0,    nullptr,    "" },
    { "warmth",        "Warmth",                kFloat,  0, 1,    0.5f, nullptr,    "" },
    { "air",           "Air",                   kFloat,  0, 1,    0,    nullptr,    "" },
    { "drone",         "Drone",                 kSwitch, 0, 1,    0,    nullptr,    "" },
    { "drone_level",   "Drone level",           kFloat,  0, 1,    0.3f, nullptr,    "" },
    { "drone_voicing", "Drone voicing",         kChoice, 0, 3,    1,    kVoicings,  "" },
    { "drone_space",   "Drone space",           kChoice, 0, 3,    1,    kSpaces,    "" },
    { "lock",          "Lock",                  kSwitch, 0, 1,    0,    nullptr,    "" },
    { "walk",          "Walk",                  kSwitch, 0, 1,    0,    nullptr,    "" },
    { "walk_way",      "Walk way",              kChoice, 0, 4,    0,    kWalkWays,  "" },
    { "walk_pace",     "Walk pace",             kChoice, 0, 3,    1,    kWalkPaces, "" },
    { "pack",          "Sounds",                kChoice, 0, 6,    0,    kPacks,     "" },
    { "scale",         "Scale",                 kChoice, 0, 26,   0,    kScales,    "" },
    { "root",          "Root",                  kChoice, 0, 11,   0,    kRoots,     "" },
    { "vary",          "Vary",                  kSwitch, 0, 1,    1,    nullptr,    "" },
    { "evolve",        "Evolve",                kSwitch, 0, 1,    1,    nullptr,    "" },
    { "morph",         "Morph",                 kSwitch, 0, 1,    1,    nullptr,    "" },
    { "midi_in_only",  "Incoming MIDI only",    kSwitch, 0, 1,    0,    nullptr,    "" },   // the sheet silent: an instrument
    // Groups A to D: the level (times their loops' own volume) and the mute of the loops in each
    { "group_a_level", "Group A level",         kFloat,  0, 1.5f, 1,    nullptr,    "" },
    { "group_a_mute",  "Group A mute",          kSwitch, 0, 1,    0,    nullptr,    "" },
    { "group_b_level", "Group B level",         kFloat,  0, 1.5f, 1,    nullptr,    "" },
    { "group_b_mute",  "Group B mute",          kSwitch, 0, 1,    0,    nullptr,    "" },
    { "group_c_level", "Group C level",         kFloat,  0, 1.5f, 1,    nullptr,    "" },
    { "group_c_mute",  "Group C mute",          kSwitch, 0, 1,    0,    nullptr,    "" },
    { "group_d_level", "Group D level",         kFloat,  0, 1.5f, 1,    nullptr,    "" },
    { "group_d_mute",  "Group D mute",          kSwitch, 0, 1,    0,    nullptr,    "" },
};

static constexpr uint32_t kParamCount = sizeof(kParams) / sizeof(kParams[0]);
static_assert(kParamCount <= 32, "the changed-parameter masks are 32 bits");

}

#endif // TWIRL_PARAMS_HPP_INCLUDED
