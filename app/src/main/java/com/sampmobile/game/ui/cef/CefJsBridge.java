package com.sampmobile.game.ui.cef;

import android.webkit.JavascriptInterface;

/**
 * window.cefNative — the channel between the page-side window.cef implementation and native.
 *
 * Corresponds to CEF's process boundary:
 *   PC: renderer process CefV8Handler -> SendProcessMessage(PID_BROWSER) -> client.cpp
 *   Android: same process; the WebView JavaBridge thread calls back here directly
 *
 * Threading: methods run on the WebView JavaBridge thread, not the main thread. No UI work is done here;
 * events are only handed to CefManager -> JNI -> CefNetwork, so there is no need to switch to the main thread (switching would
 * stretch the delay between a page click and the server receipt by a frame).
 *
 * Security boundary: the interface is attached to a single WebView instance only (addJavascriptInterface is per-view),
 * and only pages loaded by that view can access it. Content comes from the same origin as the server (omp-cef UI
 * resources are delivered by the server), matching the PC trust model where any page can call window.cef.emit.
 */
final class CefJsBridge {

    private final int browserId;

    CefJsBridge(int browserId) {
        this.browserId = browserId;
    }

    /** cef.emit(eventName, ...args); argsJson is the JSON.stringify'd scalar array. */
    @JavascriptInterface
    public void emit(String name, String argsJson) {
        if (name == null || name.length() == 0) return;
        CefManager.onJsEmit(browserId, name, argsJson != null ? argsJson : "[]");
    }

    /** cef.set_focus(hasFocus) -> FocusBrowser semantics (same as PC manager.FocusBrowser). */
    @JavascriptInterface
    public void setFocus(boolean hasFocus) {
        CefManager.focusBrowser(browserId, hasFocus);
    }
}
