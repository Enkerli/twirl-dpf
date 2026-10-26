/*
 * Twirl as a plugin: Twirl's page (music-suite apps/twirl/index.html) in DPF's
 * web view, its notes sent to the host as MIDI. No audio: a MIDI generator.
 */

#ifndef DISTRHO_PLUGIN_INFO_H_INCLUDED
#define DISTRHO_PLUGIN_INFO_H_INCLUDED

#define DISTRHO_PLUGIN_BRAND   "Enkerli"
#define DISTRHO_PLUGIN_NAME    "Twirl"
#define DISTRHO_PLUGIN_URI     "https://github.com/Enkerli/twirl-dpf"
#define DISTRHO_PLUGIN_CLAP_ID "net.enkerli.twirl"

#define DISTRHO_PLUGIN_BRAND_ID  Enkr
#define DISTRHO_PLUGIN_UNIQUE_ID Twrl

#define DISTRHO_PLUGIN_HAS_UI           1
#define DISTRHO_PLUGIN_IS_RT_SAFE       1
#define DISTRHO_PLUGIN_NUM_INPUTS       0
#define DISTRHO_PLUGIN_NUM_OUTPUTS      0
// MIDI in: DPF only gives the page sendNote() when the plugin takes MIDI in
// (the page's notes arrive as input events); host MIDI in is passed through.
#define DISTRHO_PLUGIN_WANT_MIDI_INPUT  1
#define DISTRHO_PLUGIN_WANT_MIDI_OUTPUT 1
// State: the page's timed note batches arrive through setState("notes", …).
#define DISTRHO_PLUGIN_WANT_STATE       1
#define DISTRHO_UI_USER_RESIZABLE       1
#define DISTRHO_UI_USE_WEB_VIEW         1

#define DISTRHO_PLUGIN_LV2_CATEGORY    "lv2:MIDIPlugin"
#define DISTRHO_PLUGIN_VST3_CATEGORIES "Fx|Generator"
#define DISTRHO_PLUGIN_CLAP_FEATURES   "note-effect"

#define DISTRHO_UI_DEFAULT_WIDTH  1100
#define DISTRHO_UI_DEFAULT_HEIGHT 720

#endif // DISTRHO_PLUGIN_INFO_H_INCLUDED
