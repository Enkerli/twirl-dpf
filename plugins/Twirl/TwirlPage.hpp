/*
 * Twirl's page, owned by the plugin rather than by its window.
 *
 * DPF's web view normally lives in the UI: close the window and the page goes,
 * and with it Twirl's clock, its loops and its sheet. Here the plugin starts
 * the page itself (DPF's web view, in a window of its own that is never
 * shown) and keeps it for its whole life; the UI, when it opens, moves the
 * page's window into its own, and moves it back before it closes. The page's
 * messages come straight here, not through the host's UI-to-plugin channel.
 *
 * One thread does everything that touches the web view (create, idle,
 * evaluate, resize, destroy). Moving the window in and out happens on the
 * caller's thread, with its own X connection, so that the UI can be sure the
 * page has left its window before the host destroys it.
 */

#ifndef TWIRL_PAGE_HPP_INCLUDED
#define TWIRL_PAGE_HPP_INCLUDED

#include "DistrhoUtils.hpp"
#include "extra/String.hpp"

#define DISTRHO_WEB_VIEW_HPP_INCLUDED
#define WEB_VIEW_NAMESPACE DISTRHO_NAMESPACE
#define WEB_VIEW_DISTRHO_NAMESPACE
START_NAMESPACE_DISTRHO
#include "extra/WebViewImpl.hpp"
END_NAMESPACE_DISTRHO

#include <X11/Xlib.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

START_NAMESPACE_DISTRHO

class TwirlPage
{
public:
    struct Listener {
        virtual ~Listener() {}
        /** A message from the page ("setstate key value", "sendnote c n v"), on the page's thread. */
        virtual void pageMessage(char* msg) = 0;
        /** Every 10 ms on the page's thread, once the page is up. */
        virtual void pageIdle() = 0;
    };

    static constexpr uint kWidth = 1100, kHeight = 720;

    TwirlPage(Listener& listener) : fListener(listener) {}

    ~TwirlPage()
    {
        fQuit = true;
        if (fThread.joinable())
            fThread.join();
    }

    /** Start the page (once): the web view gets 5 s to come up. `js` runs before the page's own. */
    void start(const char* const url, const std::string& js = std::string())
    {
        if (fThread.joinable() || std::getenv("DISPLAY") == nullptr)
            return;
        fUrl = url;
        fInitialJS = js;
        fThread = std::thread([this] { run(); });
    }

    bool ready() const noexcept { return fReady.load(); }

    /** Run some JavaScript in the page (any thread; run on the page's). */
    void evaluate(const std::string& js)
    {
        const std::lock_guard<std::mutex> lock(fJSMutex);
        fJS.push_back(js);
    }

    /** Show the page in `window` (the UI's, an X11 Window), at its size. */
    void attach(const uintptr_t window, const uint width, const uint height)
    {
        const std::lock_guard<std::mutex> lock(fXMutex);
        fTarget = window;
        fWidth = width;
        fHeight = height;
        fResize = true;
        if (const ::Window page = fPageWindow.load())
            moveTo(page, window);
    }

    /** Take the page back from the UI's window; done when this returns. */
    void detach()
    {
        const std::lock_guard<std::mutex> lock(fXMutex);
        if (const ::Window page = fPageWindow.load())
            if (fTarget != 0)
                moveTo(page, fHidden);
        fTarget = 0;
    }

    void resize(const uint width, const uint height)
    {
        fWidth = width;
        fHeight = height;
        fResize = true;
    }

private:
    Listener& fListener;
    String fUrl;
    std::string fInitialJS;      // before the page's own (the plugin's files, TwirlFiles.hpp)
    std::thread fThread;
    std::atomic<bool> fQuit { false }, fReady { false }, fResize { false };
    std::atomic<uint> fWidth { kWidth }, fHeight { kHeight };

    std::mutex fJSMutex;
    std::vector<std::string> fJS;

    std::mutex fXMutex;               // around moving the page's window
    ::Window fHidden = 0;             // the window it lives in when no UI is open
    std::atomic<::Window> fPageWindow { 0 };
    uintptr_t fTarget = 0;            // the UI's window, while one is open

    /** An environment variable set (or added to) while in scope, then put back. */
    struct EnvFor {
        const char* const name;
        bool had;
        std::string was;

        EnvFor(const char* const n, const char* const value, const bool append) : name(n)
        {
            const char* const old = std::getenv(name);
            had = old != nullptr;
            was = had ? old : "";
            if (append && had && std::strstr(old, value) != nullptr)
                return;
            const std::string now = append && had ? was + " " + value : std::string(value);
            setenv(name, now.c_str(), 1);
        }

        ~EnvFor()
        {
            if (had)
                setenv(name, was.c_str(), 1);
            else
                unsetenv(name);
        }
    };

    static void messageCallback(void* const arg, char* const msg)
    {
        static_cast<TwirlPage*>(arg)->fListener.pageMessage(msg);
    }

    // On the caller's thread, with a connection of its own (Xlib connections
    // aren't shared between threads here).
    void moveTo(const ::Window page, const uintptr_t parent)
    {
        if (::Display* const display = XOpenDisplay(nullptr))
        {
            XReparentWindow(display, page, static_cast<::Window>(parent), 0, 0);
            XMapWindow(display, page);
            XSync(display, False);
            XCloseDisplay(display);
        }
    }

    void run()
    {
        ::Display* const display = XOpenDisplay(nullptr);
        if (display == nullptr)
            return;

        fHidden = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, kWidth, kHeight, 0, 0, 0);
        XSync(display, False);

        // What DPF's own web view UI gives a page (sendNote, setState), plus a
        // press taking the keyboard (DPF patch 0003).
        WebViewOptions options(messageCallback, this);
        const std::string initialJS = std::string()
            + "window.setState = function(key, value){ postMessage('setstate ' + key + ' ' + value) };"
            + "window.sendNote = function(channel, note, velocity){ postMessage('sendnote ' + channel + ' ' + note + ' ' + velocity) };"
            + "addEventListener('pointerdown', function(){ postMessage('__dpf_focus') }, true);"
            + fInitialJS;
        options.initialJS = initialJS.c_str();

        // The web view's process inherits the environment, set for it alone and
        // put back as soon as it has started:
        // - the page starts its sound by itself (its window may never open),
        //   which Chromium allows only after a click unless told otherwise;
        // - Qt draws the view without GL. With GL (Mesa, DRI3), a view that
        //   isn't on screen (its window closed, here) waits forever in its
        //   buffer swap for a frame to be shown, and the page's messages, which
        //   go through that thread, stop. Measured: 60 fps without it.
        const EnvFor chromium("QTWEBENGINE_CHROMIUM_FLAGS", "--autoplay-policy=no-user-gesture-required", true);
        const EnvFor gl("QT_XCB_GL_INTEGRATION", "none", false);
        const WebViewHandle webview = webViewCreate(fUrl, fHidden, kWidth, kHeight, 1.0, options);
        if (webview == nullptr)
        {
            d_stderr("twirl: the page didn't start");
            XDestroyWindow(display, fHidden);
            XCloseDisplay(display);
            return;
        }

        // The web view's own window, inside the hidden one.
        for (int i = 0; i < 500 && fPageWindow.load() == 0 && ! fQuit; ++i)
        {
            ::Window root = 0, parent = 0, *children = nullptr;
            uint count = 0;
            if (XQueryTree(display, fHidden, &root, &parent, &children, &count) != 0 && count != 0)
            {
                const std::lock_guard<std::mutex> lock(fXMutex);
                fPageWindow = children[count - 1];
                if (fTarget != 0)
                    moveTo(fPageWindow.load(), fTarget); // a UI opened while it was starting
            }
            if (children != nullptr)
                XFree(children);
            if (fPageWindow.load() == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        fReady = true;

        while (! fQuit)
        {
            webViewIdle(webview);

            std::vector<std::string> js;
            {
                const std::lock_guard<std::mutex> lock(fJSMutex);
                js.swap(fJS);
            }
            for (const std::string& s : js)
                webViewEvaluateJS(webview, s.c_str());

            if (fResize.exchange(false))
                webViewResize(webview, fWidth, fHeight, 1.0);

            fListener.pageIdle();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        fReady = false;
        {
            const std::lock_guard<std::mutex> lock(fXMutex);
            fPageWindow = 0;
        }
        webViewDestroy(webview);
        XDestroyWindow(display, fHidden);
        XCloseDisplay(display);
    }
};

END_NAMESPACE_DISTRHO

#endif // TWIRL_PAGE_HPP_INCLUDED
