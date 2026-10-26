/*
 * Twirl as a plugin, the UI side: DPF's web view showing resources/index.html
 * (Twirl's page, copied from music-suite at build time). DPF injects
 * sendNote() and setState() into the page; Twirl finds them and sends its
 * notes here instead of to Web MIDI. Nothing to add in C++.
 */

#include "DistrhoUI.hpp"

START_NAMESPACE_DISTRHO

class TwirlUI : public UI
{
public:
    TwirlUI()
        : UI()
    {
    }

private:
    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TwirlUI)
};

UI* createUI()
{
    return new TwirlUI();
}

END_NAMESPACE_DISTRHO
