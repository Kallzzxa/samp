package com.sampmobile.game.ui.cef;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ApplicationInfo;
import android.graphics.Color;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.SparseArray;
import android.view.KeyEvent;
import android.view.View;
import android.webkit.WebSettings;
import android.webkit.WebView;

import java.io.File;
import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.List;

/**
 * omp-cef browser manager (Android implementation).
 *
 * Exposes static methods to the upper layer (native CefBrowser), callable from any thread — the native side calls in directly from the game
 * thread when it receives a server event, and internally everything switches to the main thread for View operations.
 *
 * Mirrors the responsibilities of the PC BrowserManager:
 *
 *   createBrowser        <-> CreateBrowser / CreateBrowserInternal
 *   setBrowserVisible    <-> SetBrowserVisible
 *   destroyBrowser       <-> DestroyBrowser
 *   loadUrl              <-> LoadUrl
 *   reloadBrowser        <-> ReloadBrowser
 *   focusBrowser         <-> FocusBrowser
 *   setBrowserLayer      <-> SetBrowserLayer
 *   nativeOnBrowserCreated -> SendBrowserCreateResult (back to the server)
 *
 * Unsupported parts (stages 6/7): World2D / WorldObject3D / audio spatialization / screen capture.
 * These events are currently logged and ignored by CefBrowser, not faked as implemented.
 */
public final class CefManager {

    private static final String TAG = "CefManager";

    /** BrowserCreateStatus (omp-cef manager.hpp). */
    public static final int CREATE_OK = 0;
    public static final int CREATE_ERROR_GENERIC = 1;
    public static final int CREATE_ERROR_ID_IN_USE = 2;

    private static final Object LOCK = new Object();

    /** Warm-up runs only once; set to true only after a successful handshake. */
    private static boolean sWarmUpStarted = false;
    private static final SparseArray<CefWebView> sBrowsers = new SparseArray<>();
    private static final List<Runnable> sPending = new ArrayList<>();

    private static WeakReference<Activity> sActivity = new WeakReference<>(null);
    private static Handler sMain;
    private static boolean sInstalled = false;

    /** Debug broadcast (registered only in debuggable builds): verifies the browser layer without needing a server. */
    private static final String ACTION_DEBUG = "com.sampmobile.CEFDEBUG";
    private static BroadcastReceiver sDebugReceiver;

    /**
     * Hidden WebView used for warm-up.
     *
     * Measured (vivo V2312A / Android 13): the first new WebView + loadUrl takes
     * 6-9 seconds (Chromium cold process start + first-frame composition) before anything is actually on screen. After the server sends CreateBrowser,
     * making the player wait that long for the UI is unacceptable, so an
     * invisible WebView is created early in install() and kept resident to warm up the engine and first-frame pipeline.
     */
    private static WebView sWarmupView;

    private CefManager() {
    }

    // ---------------------------------------------------------------- Lifecycle

    /** Called once by SAMP.onCreate (main thread). */
    public static void install(Activity activity) {
        if (activity == null) return;

        Handler main = new Handler(Looper.getMainLooper());
        List<Runnable> flush;
        synchronized (LOCK) {
            sActivity = new WeakReference<>(activity);
            sMain = main;
            sInstalled = true;
            flush = new ArrayList<>(sPending);
            sPending.clear();
        }
        Log.i(TAG, "installed, replaying " + flush.size() + " pending command(s)");
        for (Runnable r : flush) {
            main.post(r);
        }

        // The resource layer (stage 5) extracts .pak files to <filesDir>/cef/resources/, and the Java side
        // CefResourceProvider serves http://cef/ files from this directory; both sides must share the same root.
        File cefRoot = new File(activity.getFilesDir(), "cef");
        nativeSetStorageRoot(cefRoot.getAbsolutePath());

        registerDebugReceiver(activity, main);
        installImeWatcher(activity);
        // No warm-up here: ordinary (non-CEF) servers do not need Chromium at all,
        // unconditionally spinning up a hidden WebView would make every player pay memory and startup time for nothing.
        // Warm up only after a successful handshake confirms a CEF server; see onTransportConnected().
    }

    /**
     * Called after a successful handshake (confirming the peer is a CEF server); only then is starting Chromium worthwhile.
     *
     * Called back from the C++ side when CefNetwork enters Connected. Idempotent: repeated calls have no side effects.
     */
    public static void onTransportConnected() {
        final Activity activity;
        final Handler main;
        synchronized (LOCK) {
            if (sWarmUpStarted) return;
            sWarmUpStarted = true;
            activity = sActivity.get();
            main = sMain;
        }
        if (activity == null || main == null) return;
        Log.i(TAG, "transport connected -> warming up WebView");
        warmUp(activity, main);
    }

    /**
     * Creates a resident hidden WebView to warm up the Chromium process and first-frame pipeline ahead of time.
     * Does not affect layout (1x1, INVISIBLE, no touch input).
     */
    private static void warmUp(final Activity activity, Handler main) {
        main.post(() -> {
            try {
                WebView view = new WebView(activity);
                view.setBackgroundColor(Color.TRANSPARENT);
                view.setLayerType(View.LAYER_TYPE_HARDWARE, null);
                WebSettings s = view.getSettings();
                s.setJavaScriptEnabled(true);
                s.setDomStorageEnabled(true);
                view.loadUrl("about:blank");

                android.view.ViewGroup decor =
                        (android.view.ViewGroup) activity.getWindow().getDecorView();
                decor.addView(view, new android.widget.FrameLayout.LayoutParams(1, 1));
                view.setVisibility(View.INVISIBLE);

                synchronized (LOCK) {
                    sWarmupView = view;
                }
                Log.i(TAG, "warmup WebView attached");
            } catch (Throwable t) {
                Log.w(TAG, "warmup failed", t);
            }
        });
    }

    /** Called by SAMP.onDestroy; tears down all WebViews to avoid leaking across the Activity lifecycle. */
    public static void shutdown() {
        Activity activity = sActivity.get();
        BroadcastReceiver receiver;
        synchronized (LOCK) {
            receiver = sDebugReceiver;
            sDebugReceiver = null;
        }
        if (receiver != null && activity != null) {
            try {
                activity.unregisterReceiver(receiver);
            } catch (Throwable ignored) {
            }
        }

        List<CefWebView> victims;
        WebView warmup;
        synchronized (LOCK) {
            sInstalled = false;
            victims = new ArrayList<>();
            for (int i = 0; i < sBrowsers.size(); i++) {
                victims.add(sBrowsers.valueAt(i));
            }
            sBrowsers.clear();
            sPending.clear();
            warmup = sWarmupView;
            sWarmupView = null;
            sActivity = new WeakReference<>(null);
        }
        Handler main = sMain;
        Runnable task = () -> {
            for (CefWebView view : victims) {
                try {
                    view.destroy();
                } catch (Throwable t) {
                    Log.w(TAG, "destroy failed", t);
                }
            }
            if (warmup != null) {
                try {
                    android.view.ViewGroup parent =
                            (android.view.ViewGroup) warmup.getParent();
                    if (parent != null) parent.removeView(warmup);
                    warmup.loadUrl("about:blank");
                    warmup.destroy();
                } catch (Throwable t) {
                    Log.w(TAG, "warmup destroy failed", t);
                }
            }
        };
        if (main != null) {
            main.post(task);
        } else {
            task.run();
        }
        Log.i(TAG, "shutdown, " + victims.size() + " browser(s) destroyed");
    }

    public static int browserCount() {
        synchronized (LOCK) {
            return sBrowsers.size();
        }
    }

    // ------------------------------------------------------- Native-side calls (any thread)

    public static void createBrowser(int id, String url, boolean focused, boolean controlsChat) {
        post(() -> doCreateBrowser(id, url, focused, controlsChat));
    }

    /**
     * World2D (stage 7): server-side `CreateWorld2DBrowser`.
     *
     * PC: `BrowserManager::CreateWorld2DBrowser` -> windowless browser + `View::Create(width, height)`,
     * then projects world coordinates to the screen every frame and positions it with `view.SetPosition(x, y)` (manager.cpp:1569).
     * Android: the WebView viewport is exactly the width x height pixels given by the server, and its position comes from the native projection each frame,
     * pushed over via {@link #setWorld2DScreen}. So page content, size, and anchor semantics match the PC,
     * the only difference is that "the page is overlaid on the game screen rather than drawn into a game texture" (no world occlusion).
     */
    public static void createWorld2DBrowser(int id, String url,
                                            float worldX, float worldY, float worldZ,
                                            float width, float height,
                                            float offsetZ, float pivotX, float pivotY) {
        post(() -> doCreateWorld2D(id, url, worldX, worldY, worldZ,
                width, height, offsetZ, pivotX, pivotY));
    }

    /**
     * Native per-frame projection result (game thread) -> main-thread placement.
     *
     * Every camera move produces one per frame; posting a Runnable each time would flood the main-thread queue with
     * position updates, leaving no room for offscreen frame capture (WorldObject3D). So only the "latest
     * frame" is kept: if a Runnable is already queued, only its parameters are updated, with no duplicate posting. Semantically equivalent,
     * because the final placement is just the result of the last projection.
     */
    public static void setWorld2DScreen(int id, int x, int y, boolean onScreen) {
        synchronized (sWorld2DPending) {
            sWorld2DPending.put(id, new int[]{x, y, onScreen ? 1 : 0});
            if (!sWorld2DQueued.add(id)) return;
        }
        post(() -> {
            final int[] p;
            synchronized (sWorld2DPending) {
                p = sWorld2DPending.remove(id);
                sWorld2DQueued.remove(id);
            }
            if (p == null) return;
            CefWebView view = find(id);
            if (view == null) return;
            view.setScreenPosition(p[0], p[1], p[2] != 0);
        });
    }

    /** Latest World2D position waiting to be placed (see {@link #setWorld2DScreen}). */
    private static final java.util.HashMap<Integer, int[]> sWorld2DPending =
            new java.util.HashMap<>();
    private static final java.util.HashSet<Integer> sWorld2DQueued = new java.util.HashSet<>();

    // ------------------------------------------------- WorldObject3D (stage 7, second batch)

    /** Most recently captured page frame, stored by browserId. Taken by the game thread, filled in by the main thread. */
    private static final java.util.HashMap<Integer, java.nio.ByteBuffer> sWorldFrames =
            new java.util.HashMap<>();

    /**
     * Last rasterization time (ms) of each offscreen browser; native uses it to adapt the frame-capture interval.
     * Independent of sWorldFrames: even when there is no new frame this time, native still needs the old data.
     */
    private static final java.util.HashMap<Integer, Integer> sWorldFrameCost =
            new java.util.HashMap<>();

    /**
     * The browserId currently being captured. draw(Canvas) is occasionally slow; this guarantees only one
     * capture task is queued on the main thread at a time, so the every-100ms requests cannot pile up.
     */
    private static final java.util.HashSet<Integer> sCaptureInFlight = new java.util.HashSet<>();

    /**
     * Offscreen variant of CreateBrowser (called by native, game thread).
     *
     * The page is not shown to the player, only used for frame capture; its size is the texture size given by the server.
     */
    public static void createWorldObjectBrowser(int id, String url, int w, int h) {
        post(() -> doCreateWorldObject(id, url, w, h));
    }

    /**
     * Takes the previous frame and requests the next one (called by native at an adaptive interval, game thread).
     *
     * Cannot draw directly on the calling thread: Canvas drawing must happen on the main thread, but this is the game
     * thread. So it becomes "take the cached frame + post the next capture", blocking neither side.
     *
     * Returns a reference to the reused buffer; native must upload it to the texture immediately and must not hold it long-term.
     */
    public static java.nio.ByteBuffer captureWorldObject(int id) {
        java.nio.ByteBuffer ready;
        synchronized (sWorldFrames) {
            ready = sWorldFrames.remove(id);
        }
        synchronized (sCaptureInFlight) {
            if (!sCaptureInFlight.add(id)) {
                // The previous frame is not drawn yet; hand back the existing one (or null) directly.
                return ready;
            }
        }
        post(() -> {
            try {
                CefWebView view = find(id);
                if (view != null) {
                    java.nio.ByteBuffer px = view.captureFrame();
                    if (px != null) {
                        synchronized (sWorldFrames) {
                            sWorldFrames.put(id, px);
                        }
                        synchronized (sWorldFrameCost) {
                            sWorldFrameCost.put(id, view.lastCaptureMs());
                        }
                    }
                }
            } finally {
                synchronized (sCaptureInFlight) {
                    sCaptureInFlight.remove(id);
                }
            }
        });
        return ready;
    }

    /**
     * Last offscreen frame-capture time (ms); 0 = no data yet.
     *
     * Native uses it to decide the next capture interval: the more complex the page, the slower draw is, so captures get sparser,
     * instead of letting requests pile up in the main-thread queue.
     */
    public static int worldObjectCaptureCost(int id) {
        synchronized (sWorldFrameCost) {
            Integer v = sWorldFrameCost.get(id);
            return v != null ? v : 0;
        }
    }

    private static void doCreateWorldObject(int id, String url, int w, int h) {
        Activity activity = sActivity.get();
        if (activity == null) {
            Log.e(TAG, "createWorldObject(" + id + "): activity gone");
            reportCreateResult(id, false, CREATE_ERROR_GENERIC, "Activity unavailable");
            return;
        }
        synchronized (LOCK) {
            if (sBrowsers.get(id) != null) {
                Log.e(TAG, "createWorldObject(" + id + "): id already in use");
                reportCreateResult(id, false, CREATE_ERROR_ID_IN_USE, "Browser id already in use");
                return;
            }
        }

        final int pw = clamp(w, 1, 1024);
        final int ph = clamp(h, 1, 1024);

        CefWebView view;
        try {
            // offscreen=true: nearly transparent, no touch input, only produces frames.
            view = new CefWebView(activity, id, url, false, false, pw, ph, true);
        } catch (Throwable t) {
            Log.e(TAG, "createWorldObject(" + id + "): WebView creation failed", t);
            reportCreateResult(id, false, CREATE_ERROR_GENERIC, String.valueOf(t.getMessage()));
            return;
        }

        try {
            view.attach();
        } catch (Throwable t) {
            Log.e(TAG, "createWorldObject(" + id + "): attach failed", t);
            view.destroy();
            reportCreateResult(id, false, CREATE_ERROR_GENERIC, String.valueOf(t.getMessage()));
            return;
        }

        synchronized (LOCK) {
            sBrowsers.put(id, view);
        }
        view.setVisible(true);
        Log.i(TAG, "createWorldObject " + id + " " + pw + "x" + ph + " url=" + url);
        reportCreateResult(id, true, 0, "");
    }

    public static void destroyBrowser(int id) {
        post(() -> {
            CefWebView view;
            synchronized (LOCK) {
                view = sBrowsers.get(id);
                sBrowsers.remove(id);
            }
            if (view == null) {
                Log.w(TAG, "destroyBrowser: no browser " + id);
                // Native may still hold a WorldObject3D entry for this id (only
                // the Java side was destroyed); drop it anyway.
                forgetWorldObject(id);
                return;
            }
            view.destroy();
            forgetWorldObject(id);
            synchronized (sWorldFrames) {
                sWorldFrames.remove(id);
            }
            synchronized (sWorldFrameCost) {
                sWorldFrameCost.remove(id);
            }
            synchronized (sWorld2DPending) {
                sWorld2DPending.remove(id);
                sWorld2DQueued.remove(id);
            }
            applyImeResize(isAnyBrowserFocused());
        });
    }

    /**
     * Tells native to drop this id's WorldObject3D entry (texture + capture scheduling).
     *
     * On the normal path the server sends DestroyBrowser and native cleans up itself; this covers the case where "only
     * the Java side is destroyed" (debug broadcasts, destroys the server never confirmed). Without cleanup that texture
     * stays around, and every 3 seconds there is another capture attempt doomed to get no frame — in practice this shows up as
     * worldobjperf repeatedly reporting frames=0 without ever exiting.
     */
    private static void forgetWorldObject(int id) {
        try {
            nativeForgetWorldObject(id);
        } catch (Throwable t) {
            // The only possibility is that the symbol was not exported (missing extern "C", so the name was C++-mangled).
            // The catch here used to be empty, so a change could "take effect nowhere" with no trace at all;
            // now at least a log line is left, and verify_all.sh checks the symbols directly with nm.
            Log.w(TAG, "nativeForgetWorldObject(" + id + ") unavailable", t);
        }
    }

    private static native void nativeForgetWorldObject(int id);

    public static void setBrowserVisible(int id, boolean visible) {
        post(() -> {
            CefWebView view = find(id);
            if (view == null) {
                Log.w(TAG, "setBrowserVisible: no browser " + id);
                return;
            }
            view.setVisible(visible);
            // A hidden browser does not keep its inset; otherwise it would show again with a blank block on top.
            pushImeInset();
            Log.i(TAG, "browser " + id + " visible=" + visible);
        });
    }

    public static void loadUrl(int id, String url) {
        post(() -> {
            CefWebView view = find(id);
            if (view == null) {
                Log.w(TAG, "loadUrl: no browser " + id);
                return;
            }
            view.loadUrl(url);
            Log.i(TAG, "browser " + id + " loadUrl " + url);
        });
    }

    public static void reloadBrowser(int id, boolean ignoreCache) {
        post(() -> {
            CefWebView view = find(id);
            if (view == null) {
                Log.w(TAG, "reloadBrowser: no browser " + id);
                return;
            }
            view.reload(ignoreCache);
            Log.i(TAG, "browser " + id + " reload ignoreCache=" + ignoreCache);
        });
    }

    public static void focusBrowser(int id, boolean focus) {
        post(() -> {
            CefWebView view = find(id);
            if (view == null) {
                Log.w(TAG, "focusBrowser: no browser " + id);
                return;
            }
            if (focus) {
                // Single-focus semantics, same as PC: a newly focused browser takes focus from the old one.
                for (int i = 0; i < sBrowsers.size(); i++) {
                    CefWebView other = sBrowsers.valueAt(i);
                    if (other != view && other.isFocused()) {
                        other.setFocus(false);
                    }
                }
                // The page is about to take over fullscreen input: first clear any game-side fingers still down, or the joystick will stick.
                cancelGameTouches();
            }
            view.setFocus(focus);
            applyImeResize(isAnyBrowserFocused());
            Log.i(TAG, "browser " + id + " focus=" + focus
                    + " (any focused: " + isAnyBrowserFocused() + ")");
        });
    }

    public static void setBrowserLayer(int id, int layer) {
        post(() -> {
            CefWebView view = find(id);
            if (view == null) {
                Log.w(TAG, "setBrowserLayer: no browser " + id);
                return;
            }
            view.setLayer(layer);
        });
    }

    /**
     * EmitBrowserEvent: a custom event the server sends to a specific browser page.
     *
     * PC: app.cpp SendEmitToBrowser -> renderer `emit_event` -> events_[name] callback.
     * Android: CefWebView.dispatchEvent -> window.__cefDispatch(name, args).
     * While the page has not finished loading, events queue inside the view and are delivered in order after onPageFinished, matching the PC
     * pending_emits_ behavior.
     */
    public static void emitBrowserEvent(int id, String name, String argsJson) {
        post(() -> {
            CefWebView view = find(id);
            if (view == null) {
                Log.w(TAG, "emitBrowserEvent '" + name + "': no browser " + id);
                return;
            }
            view.dispatchEvent(name, argsJson);
        });
    }

    /**
     * Page-side cef.emit(...) uplink. Runs on the WebView JavaBridge thread and goes straight into native
     * (CefNetwork's send path is thread-safe), avoiding the extra delay of main-thread queueing.
     */
    static void onJsEmit(int browserId, String name, String argsJson) {
        try {
            nativeEmitFromJs(browserId, name, argsJson);
        } catch (Throwable t) {
            Log.w(TAG, "nativeEmitFromJs failed (" + name + ")", t);
        }
    }

    /** Session disconnected: equivalent to the PC app.cpp disconnect cleanup; destroys all pages to avoid zombie UI. */
    public static void destroyAllBrowsers() {
        post(() -> {
            List<CefWebView> victims;
            synchronized (LOCK) {
                victims = new ArrayList<>();
                for (int i = 0; i < sBrowsers.size(); i++) {
                    victims.add(sBrowsers.valueAt(i));
                }
                sBrowsers.clear();
            }
            for (CefWebView view : victims) {
                try {
                    view.destroy();
                } catch (Throwable t) {
                    Log.w(TAG, "destroyAll: destroy failed", t);
                }
            }
            if (!victims.isEmpty()) {
                Log.i(TAG, "destroyAllBrowsers: " + victims.size() + " closed");
            }
            applyImeResize(isAnyBrowserFocused());
        });
    }

    // ------------------------------------------------- Input (stage 6: keyboard/mouse / IME / back key)

    /** Server-side SetKeyCapture: PressKey is only sent upstream once enabled. Off by default (same as PC key_capture_enabled_). */
    private static volatile boolean sKeyCaptureEnabled = false;

    /** Server-side EnableKey: allowlist indexed by Windows VK, all off by default (PC key_allowed_{} all zero). */
    private static final boolean[] sKeyAllowed = new boolean[256];

    /** Server-side ToggleChatInput: when false, the game chat box no longer opens (PC FocusManager::ShouldBlockChat). */
    private static volatile boolean sChatInputEnabled = true;

    /** The browser currently holding a page text input, -1 = none (PC input_focused_browser_id_). */
    private static volatile int sInputFocusedId = -1;

    /** The window's softInputMode before switching to adjustResize; -1 = never switched. */
    private static int sOriginalSoftInputMode = -1;

    /** Height (px) currently taken by the soft keyboard; 0 = keyboard not shown. Maintained by {@link #sampleImeInset()}. */
    private static int sImeInset = 0;

    /** Sample several times in a row right after input focus changes, waiting for the IME animation to finish. */
    private static final int[] IME_SAMPLE_DELAYS = {80, 200, 400, 700, 1200};

    public static void setKeyCaptureEnabled(boolean enabled) {
        sKeyCaptureEnabled = enabled;
        Log.i(TAG, "key capture " + (enabled ? "enabled" : "disabled"));
    }

    public static void enableKey(int key, boolean enabled) {
        if (key < 0 || key > 255) return;
        sKeyAllowed[key] = enabled;
        Log.i(TAG, "key VK " + key + " (0x" + Integer.toHexString(key) + ") "
                + (enabled ? "enabled" : "disabled"));
    }

    public static void setChatInputEnabled(boolean enabled) {
        sChatInputEnabled = enabled;
        Log.i(TAG, "game chat input " + (enabled ? "enabled" : "disabled"));
    }

    /**
     * Page text-input focus (omp:cef:internal:inputFocusChanged).
     *
     * PC stores it separately from "browser focus": browser focus decides who gets mouse/keyboard, input focus is only used to
     * decide whether the chat box should yield (ShouldBlockChat). Android keeps the same two states.
     */
    public static void setInputFocus(int browserId, boolean hasFocus) {
        if (hasFocus) {
            sInputFocusedId = browserId;
        } else if (sInputFocusedId == browserId) {
            sInputFocusedId = -1;
        }
        CefWebView view = find(browserId);
        if (view != null) view.setInputFocus(hasFocus);
        Log.i(TAG, "input focus browser=" + browserId + " hasFocus=" + hasFocus
                + " (current=" + sInputFocusedId + ")");

        // An input gaining/losing focus is the only reliable signal that the keyboard is about to show/hide; sample a few extra times here:
        // The IME window animation takes a few hundred milliseconds, so a single sample easily measures 0 or a half height.
        if (hasFocus) {
            Handler main;
            synchronized (LOCK) {
                main = sMain;
            }
            if (main != null) {
                for (final int delay : IME_SAMPLE_DELAYS) {
                    main.postDelayed(new Runnable() {
                        @Override
                        public void run() {
                            sampleImeInset();
                        }
                    }, delay);
                }
            }
        }
    }

    /**
     * Whether the game chat box should be taken over by the page (equivalent of PC FocusManager::ShouldBlockChat).
     *
     * true = the player is typing in a page input, so the game's own chat input must not grab the keyboard.
     */
    public static boolean shouldBlockChat() {
        final int id = sInputFocusedId;
        // The three-level check (chat disabled / no input focus / this page's controls_chat_input) lives in CefLogic
        // and is covered by offline unit tests; this only fetches the page state.
        CefWebView view = id < 0 ? null : find(id);
        return CefLogic.shouldBlockChat(sChatInputEnabled, id,
                view != null && view.controlsChatInput());
    }

    /**
     * Game chat input on/off (UI._onKeyboardVisibilityChanged -> here).
     *
     * PC ChatHook::SetChatInputState does two things: sends a local event to every page,
     * omp:cef:internal:chatInputState, then sends ClientEmitEvent(ChatInputState, browserId=-1).
     * Both are done here too.
     */
    public static void onGameChatStateChanged(final boolean open) {
        post(() -> {
            final String argsJson = open ? "[true]" : "[false]";
            List<CefWebView> all;
            synchronized (LOCK) {
                all = new ArrayList<>();
                for (int i = 0; i < sBrowsers.size(); i++) {
                    all.add(sBrowsers.valueAt(i));
                }
            }
            for (CefWebView view : all) {
                view.dispatchInternalEvent("omp:cef:internal:chatInputState", argsJson);
            }
            try {
                nativeSendChatInputState(open);
            } catch (Throwable t) {
                Log.w(TAG, "nativeSendChatInputState failed", t);
            }
            Log.i(TAG, "chat input " + open + " -> " + all.size() + " page(s) + server");
        });
    }

    /**
     * Key reporting (called by SAMP.dispatchKeyEvent, main thread).
     *
     * Aligned with PC item by item: only sent when key_capture_enabled is on and the VK is allowlisted; auto-repeat on key-down
     * is always dropped (PC: if (!(down && repeat))), so the repeat field is always false in production.
     * Does no UI work, so the event can be sent into KCP directly on the current thread.
     */
    public static void handleKeyEvent(KeyEvent event) {
        if (event == null || !sKeyCaptureEnabled) return;

        final int action = event.getAction();
        final boolean down;
        if (action == KeyEvent.ACTION_DOWN) {
            down = true;
        } else if (action == KeyEvent.ACTION_UP) {
            down = false;
        } else {
            return;   // ACTION_MULTIPLE etc.: PC only handles WM_KEYDOWN/WM_KEYUP
        }

        final boolean repeat = down && event.getRepeatCount() > 0;
        final int vk = CefKeys.vkOf(event.getKeyCode());
        // Gating conditions (capture switch / allowlist / drop auto-repeat) are centralized in CefLogic and covered by offline unit tests.
        if (!CefLogic.shouldSendKey(sKeyCaptureEnabled, sKeyAllowed, vk, down, repeat)) return;

        final int scan = CefKeys.scanOf(event.getKeyCode());
        final int modifiers = CefKeys.modifiersOfMetaState(event.getMetaState());
        try {
            nativeSendPressKey(vk, scan, modifiers, down, repeat);
            Log.i(TAG, "PressKey vk=" + vk + " scan=" + scan + " mod=" + modifiers
                    + " down=" + down + " repeat=" + repeat);
        } catch (Throwable t) {
            Log.w(TAG, "nativeSendPressKey failed", t);
        }
    }

    public static boolean isAnyBrowserFocused() {
        synchronized (LOCK) {
            for (int i = 0; i < sBrowsers.size(); i++) {
                if (sBrowsers.valueAt(i).isFocused()) return true;
            }
        }
        return false;
    }

    /**
     * Back key: when a browser holds focus, the first press only returns focus to the game, without destroying the page
     * (the page lifecycle belongs to the server; the client must not DestroyBrowser on its behalf).
     */
    public static boolean onBackPressed() {
        int focusedId = -1;
        synchronized (LOCK) {
            for (int i = 0; i < sBrowsers.size(); i++) {
                CefWebView view = sBrowsers.valueAt(i);
                if (view.isFocused()) {
                    focusedId = view.browserId();
                    break;
                }
            }
        }
        if (focusedId < 0) return false;
        focusBrowser(focusedId, false);
        Log.i(TAG, "back pressed -> focus released (browser " + focusedId + ")");
        return true;
    }

    /** Hides the IME and yields game input when the app goes to the background / is recreated. */
    public static void releaseAllFocus() {
        List<CefWebView> all;
        synchronized (LOCK) {
            all = new ArrayList<>();
            for (int i = 0; i < sBrowsers.size(); i++) {
                all.add(sBrowsers.valueAt(i));
            }
        }
        for (CefWebView view : all) {
            if (view.isFocused()) {
                view.setFocus(false);
            }
        }
        applyImeResize(false);
    }

    /**
     * When a page needs the keyboard, switches the window to adjustResize and yields the soft-keyboard height to the page viewport.
     *
     * Why both steps are needed:
     *
     *   1) Window mode. The Activity manifest uses adjustPan (panning is enough for the game's own chat box), but
     *      adjustPan only pushes the whole window up; Chromium never gets a "visible area shrank" signal, so an input near the bottom of the page
     *      gets covered by the keyboard and never auto-scrolls — measured (V2312A) with the input sitting exactly under the
     *      keyboard's bottom edge. Switching to adjustResize shrinks the window itself.
     *
     *   2) Viewport inset. On this ROM, setSoftInputMode(adjustResize) only changes the requested value; DecorView's
     *      measurement does not follow (dumpsys window still shows sim={adjust=pan}). And the CEF overlay
     *      is a MATCH_PARENT child view attached directly to DecorView, so even if android.R.id.content
     *      shrinks, it does not. So in the end {@link #sampleImeInset()} measures the keyboard's real height, and
     *      the overlay's bottom is raised by the same height, so the WebView innerHeight actually shrinks and Chromium
     *      scrolls the focused element into view.
     *
     * The two steps are self-consistent: if the window is really resized, DecorView's height equals the visible height, so the measured inset
     * is naturally 0, with no double inset. It only applies while "a browser holds focus" — when game input has already yielded.
     */
    private static void applyImeResize(boolean enable) {
        Activity activity = sActivity.get();
        if (activity == null) return;
        try {
            android.view.Window window = activity.getWindow();
            final int current = window.getAttributes().softInputMode;
            if (enable) {
                if (sOriginalSoftInputMode < 0) {
                    sOriginalSoftInputMode = current;
                }
                final int state = current & android.view.WindowManager.LayoutParams.SOFT_INPUT_MASK_STATE;
                window.setSoftInputMode(state | android.view.WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
                Log.i(TAG, "softInputMode -> adjustResize (was 0x" + Integer.toHexString(current) + ")");
            } else if (sOriginalSoftInputMode >= 0) {
                window.setSoftInputMode(sOriginalSoftInputMode);
                Log.i(TAG, "softInputMode restored to 0x" + Integer.toHexString(sOriginalSoftInputMode));
                sOriginalSoftInputMode = -1;
            }
        } catch (Throwable t) {
            Log.w(TAG, "setSoftInputMode failed", t);
        }
        // Focus/visibility changed: immediately re-assign by the current inset (browsers that do not yield are zeroed).
        pushImeInset();
        sampleImeInset();
    }

    /**
     * Installs a sampler that runs once only on layout changes (no polling).
     *
     * A keyboard showing triggers a traversal on most ROMs, so the global layout callback is enough. But
     * under the adjustPan path the window size does not change and some ROMs never re-layout, so when a page reports
     * inputFocusChanged, {@link #IME_SAMPLE_DELAYS} samples a few extra times to avoid
     * "the keyboard clearly showed but nothing was measured". Sampling is capped, so it never drains battery in the background.
     */
    private static void installImeWatcher(final Activity activity) {
        try {
            View decor = activity.getWindow().getDecorView();
            decor.getViewTreeObserver().addOnGlobalLayoutListener(
                    new android.view.ViewTreeObserver.OnGlobalLayoutListener() {
                        @Override
                        public void onGlobalLayout() {
                            sampleImeInset();
                        }
                    });
        } catch (Throwable t) {
            Log.w(TAG, "installImeWatcher failed", t);
        }
    }

    /** Measures the gap between the bottom of the "visible area" and the bottom of DecorView once, as the soft-keyboard height. */
    private static void sampleImeInset() {
        Activity activity = sActivity.get();
        if (activity == null) return;
        final View decor = activity.getWindow().getDecorView();
        if (decor == null) return;
        final int height = decor.getHeight();
        if (height <= 0) return;
        try {
            final android.graphics.Rect frame = new android.graphics.Rect();
            decor.getWindowVisibleDisplayFrame(frame);
            // Uses imeMarginOf instead of imeInsetOf: when adjustResize takes effect the window is already shorter,
            // subtracting bottomMargin again by "how much of the screen the keyboard occupies" would be a double inset — the page would be
            // permanently pressed against the top edge of the keyboard (real-device symptom: after the keyboard hides, the viewport is left at only 2298x372).
            final android.util.DisplayMetrics dm = new android.util.DisplayMetrics();
            try {
                activity.getWindowManager().getDefaultDisplay().getRealMetrics(dm);
            } catch (Throwable ignored) {
                dm.heightPixels = height;
            }
            final int inset = CefLogic.imeMarginOf(height, frame.bottom, dm.heightPixels);
            if (inset != sImeInset) {
                sImeInset = inset;
                Log.i(TAG, "ime margin=" + inset + "px (decor " + height
                        + ", visible bottom " + frame.bottom + ", screen " + dm.heightPixels + ")");
                pushImeInset();
            }
        } catch (Throwable t) {
            Log.w(TAG, "sampleImeInset failed", t);
        }
    }

    /** Writes the current soft-keyboard height to "visible and focused" browsers; zeroes the rest. */
    private static void pushImeInset() {
        for (int i = 0; i < sBrowsers.size(); i++) {
            CefWebView view = sBrowsers.valueAt(i);
            final int want = (view.isVisible() && view.isFocused()) ? sImeInset : 0;
            if (view.imeInset() != want) {
                view.setImeInset(want);
            }
        }
    }

    /** Clears game-side gestures (called when a page starts taking over touch, to avoid a stuck joystick). */
    private static void cancelGameTouches() {
        Activity activity = sActivity.get();
        if (activity instanceof com.sampmobile.game.SAMP) {
            try {
                ((com.sampmobile.game.SAMP) activity).cancelAllNativeTouches();
            } catch (Throwable t) {
                Log.w(TAG, "cancelAllNativeTouches failed", t);
            }
        }
    }

    // ------------------------------------------------------------- Main-thread implementation

    private static void doCreateBrowser(int id, String url, boolean focused, boolean controlsChat) {
        Activity activity = sActivity.get();
        if (activity == null) {
            Log.e(TAG, "createBrowser(" + id + "): activity gone");
            reportCreateResult(id, false, CREATE_ERROR_GENERIC, "Activity unavailable");
            return;
        }

        synchronized (LOCK) {
            if (sBrowsers.get(id) != null) {
                Log.e(TAG, "createBrowser(" + id + "): id already in use");
                reportCreateResult(id, false, CREATE_ERROR_ID_IN_USE, "Browser id already in use");
                return;
            }
        }

        // CreateBrowser(focused=true): the new page takes over input directly; the old focus yields (single-focus semantics).
        if (focused) {
            for (int i = 0; i < sBrowsers.size(); i++) {
                CefWebView other = sBrowsers.valueAt(i);
                if (other.isFocused()) {
                    other.setFocus(false);
                }
            }
            cancelGameTouches();
        }

        CefWebView view;
        try {
            view = new CefWebView(activity, id, url, focused, controlsChat);
        } catch (Throwable t) {
            Log.e(TAG, "createBrowser(" + id + "): WebView creation failed", t);
            reportCreateResult(id, false, CREATE_ERROR_GENERIC, String.valueOf(t.getMessage()));
            return;
        }

        try {
            view.attach();
        } catch (Throwable t) {
            Log.e(TAG, "createBrowser(" + id + "): attach failed", t);
            view.destroy();
            reportCreateResult(id, false, CREATE_ERROR_GENERIC, String.valueOf(t.getMessage()));
            return;
        }

        synchronized (LOCK) {
            sBrowsers.put(id, view);
        }

        // On PC, success is reported as soon as the CefBrowser object is created (without waiting for the page to load);
        // here the WebView is already attached, so the semantics are equivalent.
        applyImeResize(isAnyBrowserFocused());
        Log.i(TAG, "browser " + id + " created url=" + url
                + " focused=" + focused + " controlsChat=" + controlsChat);
        reportCreateResult(id, true, CREATE_OK, "Successfully created");
    }

    /** World2D creation: fixed pixel viewport, no focus grab, no game-chat takeover (PC sets false too). */
    private static void doCreateWorld2D(int id, String url,
                                        float worldX, float worldY, float worldZ,
                                        float width, float height,
                                        float offsetZ, float pivotX, float pivotY) {
        Activity activity = sActivity.get();
        if (activity == null) {
            Log.e(TAG, "createWorld2D(" + id + "): activity gone");
            reportCreateResult(id, false, CREATE_ERROR_GENERIC, "Activity unavailable");
            return;
        }

        synchronized (LOCK) {
            if (sBrowsers.get(id) != null) {
                Log.e(TAG, "createWorld2D(" + id + "): id already in use");
                reportCreateResult(id, false, CREATE_ERROR_ID_IN_USE, "Browser id already in use");
                return;
            }
        }

        // manager.cpp: int bw = clamp((int)width, 1, 1024); same for height.
        final int w = clamp((int) width, 1, 1024);
        final int h = clamp((int) height, 1, 1024);
        if (width < 1.0f || height < 1.0f) {
            // PC clamps 0 to a 1x1 view, which renders a page nobody can see.
            // Keep the same behaviour but say so loudly, it is almost always a
            // server-side mistake (the Pawn defaults are 0.0).
            Log.w(TAG, "createWorld2D(" + id + "): width/height " + width + "x" + height
                    + " clamped to " + w + "x" + h + " like the PC client");
        }

        CefWebView view;
        try {
            view = new CefWebView(activity, id, url, false, false, w, h);
        } catch (Throwable t) {
            Log.e(TAG, "createWorld2D(" + id + "): WebView creation failed", t);
            reportCreateResult(id, false, CREATE_ERROR_GENERIC, String.valueOf(t.getMessage()));
            return;
        }

        try {
            view.attach();
        } catch (Throwable t) {
            Log.e(TAG, "createWorld2D(" + id + "): attach failed", t);
            view.destroy();
            reportCreateResult(id, false, CREATE_ERROR_GENERIC, String.valueOf(t.getMessage()));
            return;
        }

        synchronized (LOCK) {
            sBrowsers.put(id, view);
        }

        Log.i(TAG, "world2d browser " + id + " created url=" + url
                + " viewport=" + w + "x" + h
                + " world=(" + worldX + "," + worldY + "," + worldZ + ")"
                + " offsetZ=" + offsetZ + " pivot=(" + pivotX + "," + pivotY + ")");
        // PC's OnBrowserCreated also reports BrowserCreateResult for World2D.
        reportCreateResult(id, true, CREATE_OK, "Successfully created");
    }

    private static int clamp(int value, int min, int max) {
        return value < min ? min : (value > max ? max : value);
    }

    private static void reportCreateResult(int id, boolean ok, int code, String reason) {        try {
            nativeOnBrowserCreated(id, ok, code, reason != null ? reason : "");
        } catch (Throwable t) {
            Log.w(TAG, "nativeOnBrowserCreated failed", t);
        }
    }

    private static CefWebView find(int id) {
        synchronized (LOCK) {
            return sBrowsers.get(id);
        }
    }

    private static void post(Runnable task) {
        Handler main;
        synchronized (LOCK) {
            main = sMain;
            if (!sInstalled || main == null) {
                // Manager not installed yet (the game thread received an event before onCreate):
                // Queue it first; replay in order during install(), losing no events.
                if (sPending.size() < 64) {
                    sPending.add(task);
                    Log.i(TAG, "queued command until install (" + sPending.size() + ")");
                } else {
                    Log.e(TAG, "pending queue overflow, dropping command");
                }
                return;
            }
        }
        main.post(task);
    }

    /** Implemented by native CefBrowser: returns the creation result to the server as a BrowserCreateResult event. */
    private static native void nativeOnBrowserCreated(int id, boolean ok, int code, String reason);

    /** window.cef.emit uplink: name + JSON scalar array; native restores the types and sends ClientEmitEvent. */
    private static native void nativeEmitFromJs(int browserId, String name, String argsJson);

    /** Key uplink: ClientEmitEvent(PressKey, [vk, scancode, modifiers, down, repeat]). */
    private static native void nativeSendPressKey(int vk, int scancode, int modifiers,
                                                 boolean down, boolean repeat);

    /** Game chat input on/off: ClientEmitEvent(ChatInputState, [open]), browserId = -1. */
    private static native void nativeSendChatInputState(boolean open);

    private static native void nativeDebugConnect(String host, int port, int playerId);

    /** Resource cache root (<filesDir>/cef); pages extracted before DownloadComplete all live under it. */
    private static native void nativeSetStorageRoot(String root);

    private static native void nativeDebugDisconnect();

    /** Stage 7 debug entry: places a World2D page above the local player's head (debuggable builds only). */
    private static native void nativeDebugWorld2D(int id, float heightAbovePed, float width, float height);

    /** Makes all World2D pages (including server-driven ones) report their projection rectangles, for acceptance testing only. */
    private static native void nativeSetWorld2DProbe(boolean on);
    private static native void nativeDebugWorldObject(int id, int model, float distance);

    // ------------------------------------------------------------- Debug entries

    /**
     * Registered in debuggable builds only. Purpose: even when the phone is not connected to a game server, it can still verify
     * CreateBrowser / visibility / destroy and the protocol receipt chain.
     *
     *   adb shell am broadcast -a com.sampmobile.CEFDEBUG -p <pkg> \
     *       --es action connect --es host 127.0.0.1 --ei port 7900 --ei playerid 1
     *   adb shell am broadcast -a com.sampmobile.CEFDEBUG -p <pkg> \
     *       --es action create --ei id 1 --es url http://cef/test/index.html
     *   adb shell am broadcast -a com.sampmobile.CEFDEBUG -p <pkg> \
     *       --es action visible --ei id 1 --ei on 1
     *   adb shell am broadcast -a com.sampmobile.CEFDEBUG -p <pkg> \
     *       --es action destroy --ei id 1
     */
    private static void registerDebugReceiver(Activity activity, Handler main) {
        final boolean debuggable =
                (activity.getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
        if (!debuggable) {
            Log.i(TAG, "release build: debug receiver not registered");
            return;
        }

        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                String action = intent.getStringExtra("action");
                if (action == null) action = "";
                Log.i(TAG, "debug action=" + action);
                switch (action) {
                    case "connect": {
                        String host = intent.getStringExtra("host");
                        int port = intent.getIntExtra("port", 0);
                        int playerId = intent.getIntExtra("playerid", 1);
                        if (host == null || host.length() == 0 || port <= 0) {
                            Log.w(TAG, "debug connect: need host + port");
                            return;
                        }
                        nativeDebugConnect(host, port, playerId);
                        break;
                    }
                    case "disconnect":
                        nativeDebugDisconnect();
                        break;
                    case "create": {
                        int id = intent.getIntExtra("id", 1);
                        String url = intent.getStringExtra("url");
                        if (url == null) url = "";
                        createBrowser(id, url,
                                intent.getIntExtra("focused", 0) != 0,
                                intent.getIntExtra("controls", 0) != 0);
                        break;
                    }
                    case "visible": {
                        int id = intent.getIntExtra("id", 1);
                        setBrowserVisible(id, intent.getIntExtra("on", 1) != 0);
                        break;
                    }
                    case "destroy":
                        destroyBrowser(intent.getIntExtra("id", 1));
                        break;
                    case "loadurl": {
                        int id = intent.getIntExtra("id", 1);
                        loadUrl(id, intent.getStringExtra("url"));
                        break;
                    }
                    case "layer": {
                        int id = intent.getIntExtra("id", 1);
                        setBrowserLayer(id, intent.getIntExtra("layer", 0));
                        break;
                    }
                    case "emit": {
                        // Local injection of an EmitBrowserEvent: verifies the page
                        // channel without a server round trip.
                        int id = intent.getIntExtra("id", 1);
                        String name = intent.getStringExtra("name");
                        if (name == null) name = "";
                        String args = intent.getStringExtra("args");
                        emitBrowserEvent(id, name, args != null ? args : "[]");
                        break;
                    }
                    case "destroyall":
                        destroyAllBrowsers();
                        break;
                    case "keycapture": {
                        setKeyCaptureEnabled(intent.getIntExtra("on", 1) != 0);
                        break;
                    }
                    case "enablekey": {
                        enableKey(intent.getIntExtra("key", 0), intent.getIntExtra("on", 1) != 0);
                        break;
                    }
                    case "chatstate": {
                        // Goes through the game's own entry point (UI.showKeyboard), the same chain as when native code opens the chat box,
                        // so it also verifies the shouldBlockChat gate.
                        boolean open = intent.getIntExtra("on", 1) != 0;
                        Activity activity = sActivity.get();
                        if (activity instanceof com.sampmobile.game.SAMP) {
                            com.sampmobile.game.SAMP samp = (com.sampmobile.game.SAMP) activity;
                            if (samp.ui != null) {
                                samp.ui.showKeyboard(open);
                                Log.i(TAG, "debug chatstate -> ui.showKeyboard(" + open + ")");
                            }
                        }
                        break;
                    }
                    case "focus": {
                        focusBrowser(intent.getIntExtra("id", 1), intent.getIntExtra("on", 1) != 0);
                        break;
                    }
                    case "world2d": {
                        // Stage 7 debug entry: puts a World2D page above the player's head to verify projection and hit-testing.
                        // World coordinates are computed by native from the local ped position; only size/height are passed here.
                        int id = intent.getIntExtra("id", 12);
                        float above = intent.getIntExtra("above", 150) / 100.0f;
                        float w = intent.getIntExtra("w", 512);
                        float h = intent.getIntExtra("h", 256);
                        nativeDebugWorld2D(id, above, w, h);
                        break;
                    }
                    case "world2dprobe": {
                        // For acceptance: World2D pages built by the stock plugin carry no debugProbe; turning this
                        // switch on makes the client report debug:world2d just like the debug entry.
                        nativeSetWorld2DProbe(intent.getIntExtra("on", 1) != 0);
                        break;
                    }
                    case "worldobj": {
                        // Stage 7 second-batch debug entry: spawns a real object in front of the player and sticks the page
                        // onto its surface. In single-player mode there is no SA-MP object pool, so the protocol path's
                        // objectId cannot be resolved, so the texture path can only be verified this way.
                        int id = intent.getIntExtra("id", 12);
                        int model = intent.getIntExtra("model", 19377);
                        float dist = intent.getIntExtra("dist", 6);
                        nativeDebugWorldObject(id, model, dist);
                        break;
                    }
                    case "worldobjsize": {
                        // Only builds the offscreen page (no object spawned), to verify frame capture on its own.
                        int id = intent.getIntExtra("id", 12);
                        int w = intent.getIntExtra("w", 512);
                        int h = intent.getIntExtra("h", 256);
                        String url = intent.getStringExtra("url");
                        if (url == null) url = "http://cef/test/index.html";
                        createWorldObjectBrowser(id, url, w, h);
                        break;
                    }
                    case "status":
                        Log.i(TAG, "status: browsers=" + browserCount()
                                + " focused=" + isAnyBrowserFocused()
                                + " keyCapture=" + sKeyCaptureEnabled
                                + " inputFocus=" + sInputFocusedId);
                        break;
                    default:
                        Log.w(TAG, "unknown debug action " + action);
                        break;
                }
            }
        };

        try {
            IntentFilter filter = new IntentFilter(ACTION_DEBUG);
            if (Build.VERSION.SDK_INT >= 33) {
                activity.registerReceiver(receiver, filter, Context.RECEIVER_EXPORTED);
            } else {
                activity.registerReceiver(receiver, filter);
            }
            synchronized (LOCK) {
                sDebugReceiver = receiver;
            }
            Log.i(TAG, "debug receiver ready: " + ACTION_DEBUG);
        } catch (Throwable t) {
            Log.w(TAG, "register debug receiver failed", t);
        }
    }
}
