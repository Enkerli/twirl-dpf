/*
 * Twirl as a plugin, the UI side: a window for Twirl's page, which the plugin
 * owns (TwirlPage.hpp). Opening moves the page into this window; closing
 * moves it back out first, so Twirl goes on playing with its sheet intact.
 */

#include "DistrhoUI.hpp"
#include "Window.hpp"
#include "DistrhoPlugin.hpp"
#include "TwirlShared.hpp"

START_NAMESPACE_DISTRHO

class TwirlUI : public UI
{
public:
    TwirlUI()
        : UI(DISTRHO_UI_DEFAULT_WIDTH, DISTRHO_UI_DEFAULT_HEIGHT),
          fPlugin(dynamic_cast<TwirlPageAccess*>(static_cast<Plugin*>(getPluginInstancePointer())))
    {
        if (fPlugin != nullptr)
            fPlugin->uiAttach(getWindow().getNativeWindowHandle(), getWidth(), getHeight());
    }

    ~TwirlUI() override
    {
        if (fPlugin != nullptr)
            fPlugin->uiDetach();
    }

protected:
    void onDisplay() override {}

    void parameterChanged(uint32_t, float) override {}

    void stateChanged(const char*, const char*) override {}

    void onResize(const ResizeEvent& ev) override
    {
        UI::onResize(ev);
        if (fPlugin != nullptr)
            fPlugin->uiResize(ev.size.getWidth(), ev.size.getHeight());
    }

private:
    TwirlPageAccess* const fPlugin;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TwirlUI)
};

UI* createUI()
{
    return new TwirlUI();
}

END_NAMESPACE_DISTRHO
