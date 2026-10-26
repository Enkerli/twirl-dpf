/*
 * Twirl as a plugin: Twirl's page (music-suite apps/twirl/index.html) in DPF's
 * web view. Its own sound comes out of the plugin's stereo output, its notes
 * out as MIDI, or both (the page's Out picker).
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
#define DISTRHO_PLUGIN_NUM_OUTPUTS      2
// MIDI in: DPF only gives the page sendNote() when the plugin takes MIDI in
// (the page's notes arrive as input events); host MIDI in is passed through.
#define DISTRHO_PLUGIN_WANT_MIDI_INPUT  1
#define DISTRHO_PLUGIN_WANT_MIDI_OUTPUT 1
// State: the page's (its sheet, its Out choice, Follow host transport), saved
// with the project. The page's notes and sound come to the plugin directly.
#define DISTRHO_PLUGIN_WANT_STATE       1
#define DISTRHO_PLUGIN_WANT_FULL_STATE  1
// The host's tempo and position, for "Follow host transport".
#define DISTRHO_PLUGIN_WANT_TIMEPOS     1
// The page belongs to the plugin (TwirlPage.hpp); the UI shows it in its
// window, through direct access. DPF's web view functions, without its web
// view UI.
#define DISTRHO_PLUGIN_WANT_DIRECT_ACCESS 1
#define DISTRHO_UI_WEB_VIEW             1
#define DISTRHO_UI_USE_WEB_VIEW         0
#define DISTRHO_UI_USER_RESIZABLE       1

#define DISTRHO_PLUGIN_LV2_CATEGORY    "lv2:InstrumentPlugin"
#define DISTRHO_PLUGIN_VST3_CATEGORIES "Instrument|Generator|Stereo"
#define DISTRHO_PLUGIN_CLAP_FEATURES   "instrument", "note-effect", "stereo"

#define DISTRHO_UI_DEFAULT_WIDTH  1100
#define DISTRHO_UI_DEFAULT_HEIGHT 720

#endif // DISTRHO_PLUGIN_INFO_H_INCLUDED
