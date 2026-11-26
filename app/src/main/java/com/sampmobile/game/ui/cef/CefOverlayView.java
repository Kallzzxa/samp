package com.sampmobile.game.ui.cef;

import android.content.Context;
import android.os.SystemClock;
import android.util.Log;
import android.view.MotionEvent;
import android.view.ViewGroup;
import android.webkit.WebView;
import android.widget.FrameLayout;

/**
 * Touch gate hosting one omp-cef WebView.
 *
 * Why it is needed:
 *   Once visible, a WebView eats all touches, breaking the game's virtual joystick and camera drag. But omp-cef's
 *   semantics are "when a browser has focus, the game yields input" (PC: FocusManager::Update locks the mouse into CEF,
 *   and returns input to the game when IsAnyBrowserFocused is false). So whether touches go to the page must be decided by focus,
 *   not by "whether a page is visible".
 *
 * Implementation:
 *   This class is a child View of DecorView, and the WebView is its only child View.
 *   When passThrough = true, dispatchTouchEvent returns false directly — no child View consumes this
 *   ACTION_DOWN, so the ViewGroup dispatch loop continues to the next sibling (the game screen), and the event lands unchanged on
 *   Activity.onTouchEvent -> multiTouchEvent4Ex, exactly as with no overlay.
 *   Note this must not be done in onInterceptTouchEvent: interception only affects "whether the child View gets it",
 *   and returning false would still hand the event to the WebView.
 *
 * Gesture consistency:
 *   If the gate changes direction after a finger is down (e.g. the server calls FocusBrowser mid-gesture), one side receives a
 *   "half gesture". So when switching direction, send ACTION_CANCEL to the side losing the events; see
 *   {@link #setPassThrough(boolean)} and {@link #cancelChildGesture()}.
 */
final class CefOverlayView extends FrameLayout {

    private static final String TAG = "CefOverlay";

    private final int browserId;
    private final WebView content;

    /** true = pass through to the game; false = the page consumes it. */
    private boolean passThrough = false;

    CefOverlayView(Context context, int browserId, WebView content) {
        super(context);
        this.browserId = browserId;
        this.content = content;
        setClipChildren(false);
        setClipToPadding(false);
        addView(content, new LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    void setPassThrough(boolean passThrough) {
        if (this.passThrough == passThrough) return;
        this.passThrough = passThrough;
        if (passThrough) {
            // The page is holding this gesture: cancel it first, or the WebView will keep thinking the finger is still down.
            cancelChildGesture();
        }
        Log.i(TAG, "browser " + browserId + " touch " + (passThrough ? "-> game" : "-> page"));
    }

    boolean isPassThrough() {
        return passThrough;
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent ev) {
        if (passThrough) {
            // Not consuming: let DecorView keep dispatching to the game screen.
            return false;
        }
        return super.dispatchTouchEvent(ev);
    }

    /** Sends an ACTION_CANCEL to the WebView to end the gesture it is handling. */
    void cancelChildGesture() {
        if (content == null) return;
        final long now = SystemClock.uptimeMillis();
        MotionEvent cancel = MotionEvent.obtain(now, now, MotionEvent.ACTION_CANCEL, 0f, 0f, 0);
        try {
            super.dispatchTouchEvent(cancel);
        } catch (Throwable t) {
            Log.w(TAG, "cancel gesture failed", t);
        } finally {
            cancel.recycle();
        }
    }
}
