/*
 * What the UI asks of the plugin (DPF's direct access): show Twirl's page,
 * which the plugin owns, in the UI's window, and take it back; and what the
 * page changed among the host's parameters, for the UI to tell the host.
 */

#ifndef TWIRL_SHARED_HPP_INCLUDED
#define TWIRL_SHARED_HPP_INCLUDED

#include <cstdint>

class TwirlPageAccess
{
public:
    virtual ~TwirlPageAccess() {}
    virtual void uiAttach(uintptr_t window, unsigned width, unsigned height) = 0;
    virtual void uiDetach() = 0;
    virtual void uiResize(unsigned width, unsigned height) = 0;
    // The parameters the page changed since last asked (a bit each), and each
    // one's value, which the UI then tells the host.
    virtual uint32_t uiTakeParamChanges() = 0;
    virtual float uiParamValue(uint32_t index) = 0;
};

#endif // TWIRL_SHARED_HPP_INCLUDED
