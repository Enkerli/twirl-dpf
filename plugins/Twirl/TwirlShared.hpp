/*
 * What the UI asks of the plugin (DPF's direct access): show Twirl's page,
 * which the plugin owns, in the UI's window, and take it back.
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
};

#endif // TWIRL_SHARED_HPP_INCLUDED
