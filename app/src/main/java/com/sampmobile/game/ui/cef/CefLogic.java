package com.sampmobile.game.ui.cef;

/**
 * Pure logic on the CEF input path: "decisions only, no side effects".
 *
 * The sole reason for a separate class is **offline verification on PC**: no android classes are imported here,
 * so it can be compiled with javac and run assertions directly on a desktop JVM (see tools/cef_kat/logic_tests).
 *
 * On-device logcat is extremely unreliable on this ROM (logd keeps restarting); end-to-end, only uplink packets can
 * prove "a path was taken", not "every branch is correct". These two pieces of semantics are exactly the easiest to
 * silently break in later changes (key gating decides whether the server receives keys; the IME inset decides whether the page
 * gets covered by the keyboard), so they are extracted as pure functions + offline assertions.
 *
 * Formulas and thresholds are copied verbatim from the original inline implementation; behavior is unchanged.
 */
final class CefLogic {

    /** The soft keyboard takes at least 1/4 of the screen height; a smaller gap is treated as the nav bar / gesture bar. */
    private static final int IME_MIN_RATIO_DIVISOR = 4;

    private CefLogic() {
    }

    /**
     * Height (px) taken by the soft keyboard. 0 = keyboard not shown.
     *
     * @param decorHeight   Height of the window root View
     * @param visibleBottom bottom of getWindowVisibleDisplayFrame()
     */
    static int imeInsetOf(int decorHeight, int visibleBottom) {
        if (decorHeight <= 0) return 0;
        int inset = decorHeight - visibleBottom;
        if (inset < 0) inset = 0;
        // Permanent obstructions like the nav bar / gesture bar are only a few dozen px; the keyboard takes at least a quarter of the screen height.
        // A proportional threshold separates them, so the page bottom never carries a permanent blank strip.
        if (inset < decorHeight / IME_MIN_RATIO_DIVISOR) inset = 0;
        return inset;
    }

    /**
     * Keyboard height (px) that should become bottomMargin.
     *
     * {@link #imeInsetOf} only answers "how much of the screen the keyboard occupies", but in an ADJUST_RESIZE window
     * that space **has already been yielded by the window**; turning it into a margin again would be a double inset — the page would be squeezed to
     * only the sliver above the keyboard, and because the window itself got shorter, the next frame computes the same inset again,
     * forming a self-sustaining feedback loop (real-device symptom: after the keyboard hides, the viewport becomes 2298x372).
     *
     * The criterion is whether the window was resized by the system:
     *   - decorHeight <= 0            -> cannot measure; treat as 0 (wait for the next layout)
     *   - decorHeight < screenHeight  -> window already resized, space already yielded, return 0
     *   - otherwise                        -> still inset by deltas
     *
     * @param decorHeight    Height of the window root View
     * @param visibleBottom  bottom of getWindowVisibleDisplayFrame()
     * @param screenHeight   Height of the whole screen (WindowManager current display size)
     */
    static int imeMarginOf(int decorHeight, int visibleBottom, int screenHeight) {
        if (decorHeight <= 0 || screenHeight <= 0) return 0;
        // Window shorter than the screen: the system is already yielding space for it (adjustResize in effect); do not subtract again.
        if (decorHeight < screenHeight) return 0;
        return imeInsetOf(decorHeight, visibleBottom);
    }

    /**
     * Whether PressKey goes upstream. Aligned item by item with PC manager.cpp OnWndProcMessage:
     *
     *   if (!key_capture_enabled_) return;
     *   if (!key_allowed_[key]) return;
     *   if (!(down && repeat)) SendClientEmit(PressKey, ...)
     *
     * That is: no send when capture is off, no send outside the allowlist, no send for auto-repeat on key-down; on key-up (down=false)
     * send even with repeat, or the server would receive a "key down with no matching key up".
     *
     * @param captureEnabled Current state of server-side SetKeyCapture
     * @param allowed        EnableKey allowlist, indexed by Windows VK
     * @param vk             Windows virtual key code (result of {@link CefKeys#vkOf})
     */
    static boolean shouldSendKey(boolean captureEnabled, boolean[] allowed, int vk,
                                 boolean down, boolean repeat) {
        if (!captureEnabled) return false;
        if (vk <= 0 || vk > 255) return false;
        if (allowed == null || vk >= allowed.length || !allowed[vk]) return false;
        if (down && repeat) return false;
        return true;
    }

    /**
     * Whether the game chat box should yield to the page (PC FocusManager::ShouldBlockChat).
     *
     * PC original:
     *   if (!chat_input_enabled_) return true;
     *   if (input_focused_browser_id_ == -1) return false;
     *   return browser(input_focused_browser_id_)->controls_chat_input;
     *
     * @param chatInputEnabled   State of server-side ToggleChatInput, default true
     * @param inputFocusedId     Browser holding a page text input, -1 = none
     * @param controlsChatInput  controls_chat_input declared when this browser was created
     */
    static boolean shouldBlockChat(boolean chatInputEnabled, int inputFocusedId,
                                   boolean controlsChatInput) {
        if (!chatInputEnabled) return true;
        if (inputFocusedId < 0) return false;
        return controlsChatInput;
    }
}
