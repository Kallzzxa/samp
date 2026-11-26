package com.sampmobile.game.ui.cef;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.InputMethodManager;
import android.webkit.ConsoleMessage;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;

import java.nio.ByteBuffer;

import java.util.ArrayDeque;
import java.util.Deque;
import java.util.Locale;

/**
 * An Android WebView for one omp-cef browser id (Overlay2D mode).
 *
 * Correspondence with the PC client:
 *   PC: CefBrowserHost::CreateBrowser(windowless) + ViewTexture upload to D3D9
 *   Android: WebView attached to the Activity DecorView, composited by SurfaceFlinger
 *
 * Overlay2D viewport semantics (see omp-cef manager.cpp CreateBrowserInternal): the page
 * viewport is the game screen size, and its position is decided by the page's own CSS. So MATCH_PARENT is used here,
 * with no extra coordinate conversion.
 *
 * Threading: all methods must be called on the main thread; CefManager handles the switch.
 */
final class CefWebView {

    private static final String TAG = "CefWebView";

    /** The Overlay2D page viewport equals the game screen, so layout params are fixed to fullscreen. */
    private static final FrameLayout.LayoutParams FULLSCREEN_LP = new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT,
            Gravity.TOP | Gravity.START);

    private final Activity activity;
    private final int browserId;
    private final WebView web;
    private final CefOverlayView overlay;
    private final boolean controlsChatInput;

    /** World2D: the viewport is the pixel size given by the server, and its position is decided by the world-coordinate projection. */
    private final boolean world2d;
    private final int pxWidth;
    private final int pxHeight;

    /**
     * WorldObject3D: the page is not shown to the player directly; it is captured as pixels and pasted onto a game object's surface.
     *
     * WebView has no GPU texture output, so it can only read pixels back via draw(Canvas); it must therefore really be
     * laid out and drawn by the View system. To keep it hidden from the player, it gets a very low alpha (not
     * INVISIBLE — an invisible View is skipped during drawing, so the read-back would be blank).
     */
    private final boolean offscreen;

    private boolean visible = true;
    private boolean focused = false;
    private boolean inputFocused = false;
    private int layer = 0;
    private boolean destroyed = false;

    // ---- WorldObject3D frame-capture reuse buffer (stage 8)------------------------------------
    // Capturing a frame with createBitmap + allocateDirect + new byte[] every time would allocate ~1MB per 512x256 frame,
    // and GC jitter would show up directly as frame-rate jitter. So the buffer is reused by size, rebuilt only when the page size changes.
    // frameBuf is direct: the C++ side reads it directly with GetDirectBufferAddress,
    // saving one 0.5MB copy via GetByteArrayElements.
    private Bitmap frameBmp;
    private Canvas frameCanvas;
    private ByteBuffer frameBuf;

    /** Last draw+copy time (ms); 0 = not measured yet. Used by native for adaptive throttling. */
    private int lastCaptureMs = 0;

    /** World2D: whether the latest projection falls in front of the camera (false = do not draw, same as PC skipping the draw). */
    private boolean onScreen = true;

    /** Height (px) currently taken by the soft keyboard; 0 = keyboard not shown. See {@link #setImeInset(int)}. */
    private int imeInset = 0;

    /**
     * Whether the page has finished loading (onPageFinished).
     *
     * EmitBrowserEvent may arrive a few hundred ms after CreateBrowser, while a cold WebView start
     * takes 1-3 seconds before it runs page scripts. During that window __cefDispatch does not exist yet, so a direct evaluate would
     * silently lose the event, so events are queued first and replayed in order after load completes (the PC pending_emits_ equivalent).
     */
    private boolean pageReady = false;
    private final Deque<String[]> pendingEvents = new ArrayDeque<>();

    private static final int MAX_PENDING_EVENTS = 128;

    @SuppressLint("SetJavaScriptEnabled")
    CefWebView(Activity activity, int browserId, String url, boolean focused, boolean controlsChat) {
        this(activity, browserId, url, focused, controlsChat, 0, 0);
    }

    /**
     * @param pxWidth  World2D viewport width (pixels, 1..1024); 0 = fullscreen Overlay2D
     * @param pxHeight World2D viewport height (pixels, 1..1024); 0 = fullscreen Overlay2D
     */
    @SuppressLint("SetJavaScriptEnabled")
    CefWebView(Activity activity, int browserId, String url, boolean focused, boolean controlsChat,
               int pxWidth, int pxHeight) {
        this(activity, browserId, url, focused, controlsChat, pxWidth, pxHeight, false);
    }

    /**
     * @param pxWidth  World2D viewport width (pixels, 1..1024); 0 = fullscreen Overlay2D
     * @param pxHeight World2D viewport height (pixels, 1..1024); 0 = fullscreen Overlay2D
     * @param offscreen WorldObject3D: only used for frame capture, no input, nearly transparent
     */
    @SuppressLint("SetJavaScriptEnabled")
    CefWebView(Activity activity, int browserId, String url, boolean focused, boolean controlsChat,
               int pxWidth, int pxHeight, boolean offscreen) {
        this.activity = activity;
        this.browserId = browserId;
        this.controlsChatInput = controlsChat;
        this.pxWidth = pxWidth;
        this.pxHeight = pxHeight;
        this.world2d = pxWidth > 0 && pxHeight > 0;
        this.offscreen = offscreen;

        this.web = new WebView(activity);
        this.web.setBackgroundColor(Color.TRANSPARENT);
        // Hardware layer: with a transparent background, floating above the game Surface, the software layer shows a black background.
        //
        // Exception is offscreen (WorldObject3D): that page needs draw(Canvas) to read pixels back,
        // but under a hardware layer Chromium's content lives in the compositor, not on the View's Canvas, so draw()
        // would keep waiting for composition (measured: it deadlocks the game main thread, after which no broadcast responds). The software layer
        // makes it use CPU rasterization, so draw() returns real content immediately. At 512x256 that cost is
        // acceptable.
        this.web.setLayerType(offscreen ? View.LAYER_TYPE_SOFTWARE : View.LAYER_TYPE_HARDWARE, null);
        this.web.setFocusable(true);
        this.web.setFocusableInTouchMode(true);
        this.web.setVerticalScrollBarEnabled(false);
        this.web.setHorizontalScrollBarEnabled(false);
        this.web.setOverScrollMode(View.OVER_SCROLL_NEVER);

        configureSettings(web.getSettings());

        web.setWebViewClient(new CefClient());
        web.setWebChromeClient(new CefChromeClient());

        // The interface must be attached before loadUrl, or the first page will not get window.cefNative.
        web.addJavascriptInterface(new CefJsBridge(browserId), CefJs.BRIDGE_NAME);

        // Touch gate: when a browser has focus the page consumes touches; otherwise they pass through to the game unchanged.
        this.overlay = new CefOverlayView(activity, browserId, web);

        if (url != null && url.length() > 0) {
            web.loadUrl(url);
        }
        this.focused = focused;
        applyTouchMode();
    }

    private void configureSettings(WebSettings settings) {
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(true);
        settings.setDatabaseEnabled(true);
        // http://cef/<resource>/<path> is handled by CefResourceProvider, not the filesystem,
        // so file:// access is disabled to stop pages reading device files via file://.
        settings.setAllowFileAccess(false);
        settings.setAllowContentAccess(false);
        settings.setSupportZoom(false);
        settings.setBuiltInZoomControls(false);
        settings.setDisplayZoomControls(false);
        settings.setLoadsImagesAutomatically(true);
        settings.setJavaScriptCanOpenWindowsAutomatically(true);
        // The game UI needs sound/voice prompts, so CEF allows autoplay by default.
        settings.setMediaPlaybackRequiresUserGesture(false);
        // Resources come from local 127.0.0.1 and http://cef/; http resources embedded in https pages need to be allowed.
        settings.setMixedContentMode(WebSettings.MIXED_CONTENT_ALWAYS_ALLOW);
        settings.setCacheMode(WebSettings.LOAD_DEFAULT);
        settings.setTextZoom(100);
    }

    /** Attaches to the Activity's DecorView (same layering strategy as the existing WebOverlay). */
    void attach() {
        ViewGroup decor = (ViewGroup) activity.getWindow().getDecorView();
        // Copies the LayoutParams: sharing a static instance would make multiple WebViews fight over the same params.
        // World2D uses the pixel size given by the server (PC: View::Create(width, height)),
        // Overlay2D stays fullscreen.
        final FrameLayout.LayoutParams lp = world2d
                ? new FrameLayout.LayoutParams(pxWidth, pxHeight, Gravity.TOP | Gravity.START)
                : new FrameLayout.LayoutParams(FULLSCREEN_LP);
        decor.addView(overlay, lp);
        applyLayer();
        if (offscreen) {
            // Frame capture requires the View to actually be drawn, so it cannot be INVISIBLE; alpha is pressed to almost 0 instead.
            overlay.setAlpha(0.02f);
        }

        // INVISIBLE first, then VISIBLE on the next frame: same as WebOverlay's warm-up path,
        // to avoid the SurfaceView z-order not being settled on the first attach.
        overlay.setVisibility(View.INVISIBLE);
        overlay.post(() -> {
            overlay.setVisibility(visible ? View.VISIBLE : View.INVISIBLE);
            forceRedraw();
            Log.i(TAG, "attach id=" + browserId
                    + " vis=" + (overlay.getVisibility() == View.VISIBLE ? "V" : String.valueOf(overlay.getVisibility()))
                    + " shown=" + overlay.isShown()
                    + " size=" + web.getWidth() + "x" + web.getHeight()
                    + " parent=" + (overlay.getParent() != null)
                    + " z=" + overlay.getTranslationZ()
                    + " touch=" + (overlay.isPassThrough() ? "game" : "page"));
        });
    }

    /**
     * After a WebView's visibility switches back from INVISIBLE, Chromium does not necessarily commit a new frame right away
     * (especially noticeable while the game SurfaceView keeps compositing). Force one draw invalidation.
     */
    private void forceRedraw() {
        web.requestLayout();
        web.invalidate();
        web.postInvalidateOnAnimation();
    }

    void setVisible(boolean visible) {
        this.visible = visible;
        if (destroyed) return;
        // Uses INVISIBLE instead of GONE: GONE would make the WebView lose its viewport size,
        // so the next show would need a re-layout and HUD-style pages would flicker.
        applyVisibility();
        Log.i(TAG, "setVisible id=" + browserId + " want=" + visible
                + " now=" + (overlay.getVisibility() == View.VISIBLE ? "V" : String.valueOf(overlay.getVisibility()))
                + " shown=" + overlay.isShown()
                + " touch=" + (overlay.isPassThrough() ? "game" : "page"));
    }

    /**
     * Effective visibility = the server wants it visible AND (not World2D, or the projection falls in front of the camera).
     *
     * On PC, World2D simply `continue`s without drawing when projection fails / the point is behind the camera, which is semantically equivalent to
     * hiding here; the server's SetBrowserVisible still applies, and the two are ANDed.
     */
    private void applyVisibility() {
        if (destroyed) return;
        final boolean show = visible && onScreen;
        overlay.setVisibility(show ? View.VISIBLE : View.INVISIBLE);
        if (show) {
            forceRedraw();
        } else {
            web.clearFocus();
            hideIme();
        }
        applyTouchMode();
    }

    /**
     * World2D: the projected screen top-left corner (pixels). PC equivalent is `view.SetPosition(x, y)`.
     *
     * Uses translation instead of changing LayoutParams: the viewport size must stay unchanged (the page lays out at
     * width x height), only its position on screen moves.
     */
    void setScreenPosition(int x, int y, boolean onScreen) {
        if (destroyed || !world2d) return;
        final boolean changed = this.onScreen != onScreen;
        this.onScreen = onScreen;
        if (onScreen) {
            overlay.setX(x);
            overlay.setY(y);
        }
        if (changed) {
            applyVisibility();
        }
    }

    boolean isWorld2D() {
        return world2d;
    }

    /**
     * Browser focus (server FocusBrowser / page cef.set_focus).
     *
     * Same as PC BrowserManager::FocusBrowser: the focused browser owns input exclusively, and the old focus is taken away
     * (the focus-grab logic lives in CefManager, guaranteeing only one focused browser among many).
     */
    void setFocus(boolean focused) {
        this.focused = focused;
        if (destroyed) return;
        if (focused) {
            web.requestFocus();
        } else {
            web.clearFocus();
            hideIme();
        }
        applyTouchMode();
        Log.i(TAG, "setFocus id=" + browserId + " focus=" + focused
                + " touch=" + (overlay.isPassThrough() ? "game" : "page"));
    }

    /**
     * Touch ownership: only a "visible + focused" browser consumes touches; all others pass through to the game.
     *
     * This way a fullscreen HUD (CreateBrowser(focused=false)) does not kill the virtual joystick, while the server/page can explicitly
     * request focus and take over the whole screen — exactly matching when PC FocusManager locks the mouse into CEF.
     */
    private void applyTouchMode() {
        if (destroyed) return;
        // Offscreen (WorldObject3D) pages are only used for frame capture and never receive touches.
        final boolean consume = visible && focused && !offscreen;
        overlay.setPassThrough(!consume);
    }

    /**
     * Reads the current WebView content as RGBA8888 pixels (must be called on the main thread).
     *
     * WebView has no GPU texture output; this is the only channel for getting a page into the game renderer:
     * draw(Canvas) takes the View's software drawing path, which can retrieve Chromium's composited content.
     *
     * Returns a **reused** direct ByteBuffer; do not keep a reference: the next capture will overwrite it.
     * The native side uploads it to the texture immediately after reading, and only then is the next capture allowed (guaranteed by CefManager's
     * in-flight flag), so reuse is safe.
     *
     * @return w*h*4 bytes, rows top to bottom; null = layout not finished yet or read failed
     */
    ByteBuffer captureFrame() {
        if (destroyed) return null;
        final int w = web.getWidth();
        final int h = web.getHeight();
        if (w <= 0 || h <= 0) return null;

        try {
            if (frameBmp == null || frameBmp.getWidth() != w || frameBmp.getHeight() != h) {
                if (frameBmp != null) frameBmp.recycle();
                frameBmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
                frameCanvas = new Canvas(frameBmp);
                frameBuf = ByteBuffer.allocateDirect(w * h * 4);
            }

            final long t0 = System.nanoTime();

            // The page background may be transparent; lay down a base color first to avoid reading back uninitialized video memory.
            frameCanvas.drawColor(Color.rgb(16, 16, 16));
            web.draw(frameCanvas);

            frameBuf.clear();
            frameBmp.copyPixelsToBuffer(frameBuf);
            frameBuf.position(0);

            lastCaptureMs = (int) Math.max(0, (System.nanoTime() - t0) / 1000000L);
            return frameBuf;
        } catch (Throwable t) {
            Log.w(TAG, "captureFrame id=" + browserId + " failed", t);
            return null;
        }
    }

    /** Last frame-capture time (ms). 0 = not measured yet. */
    int lastCaptureMs() {
        return lastCaptureMs;
    }

    /** Keyboard (IME) hide: must be done when handing focus back, or the IME will stay stuck above the game screen. */
    private void hideIme() {
        try {
            InputMethodManager imm = (InputMethodManager)
                    activity.getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm != null) {
                imm.hideSoftInputFromWindow(web.getWindowToken(), 0);
            }
        } catch (Throwable ignored) {
        }
    }

    /** Page text input gaining/losing focus (omp:cef:internal:inputFocusChanged, PC SetInputFocus). */
    void setInputFocus(boolean hasFocus) {
        this.inputFocused = hasFocus;
        Log.i(TAG, "setInputFocus id=" + browserId + " hasFocus=" + hasFocus);
    }

    boolean hasInputFocus() {
        return inputFocused;
    }

    /**
     * Soft-keyboard inset: raises the overlay's bottom by {@code px} so the WebView *viewport* actually shrinks.
     *
     * Why it must shrink the viewport instead of panning the whole window:
     *   adjustPan only pushes the whole window up; the page viewport (innerHeight) stays the same, so Chromium
     *   thinks "nothing needs doing", and inputs/buttons in the lower half of the page still land under the keyboard — measured
     *   in the 1b-ime screenshot on the V2312A, the input sits exactly under the keyboard's bottom edge. Once the viewport gets shorter, Chromium scrolls the
     *   focused element into view, and the page CSS 100vh is also recalculated correctly — that is the real browser semantics.
     *
     * The implementation uses FrameLayout's bottomMargin instead of setPadding: both measure MATCH_PARENT
     * child Views the same way, but margin does not change the overlay's own content coordinates, so later
     * World2D coordinate conversion does not need to subtract padding again.
     */
    void setImeInset(int px) {
        if (destroyed) return;
        if (px < 0) px = 0;
        if (px == imeInset) return;
        imeInset = px;

        ViewGroup.LayoutParams params = overlay.getLayoutParams();
        if (params instanceof FrameLayout.LayoutParams) {
            FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) params;
            lp.bottomMargin = px;
            overlay.setLayoutParams(lp);
        }
        // After a viewport change, Chromium needs a new layout before it re-scrolls; force one frame.
        web.requestLayout();
        Log.i(TAG, "setImeInset id=" + browserId + " px=" + px
                + " viewport=" + web.getWidth() + "x" + web.getHeight());
    }

    int imeInset() {
        return imeInset;
    }

    /** On PC, layer is draw order; on Android it maps to Z-axis height. */
    void setLayer(int layer) {
        this.layer = layer;
        applyLayer();
    }

    private void applyLayer() {
        if (destroyed) return;
        // Z is only raised when the server explicitly specifies a layer; setTranslationZ(0) on some ROMs
        // pushes the View into a standalone RenderNode, which then gets covered by the game Surface instead.
        if (layer != 0) {
            overlay.setTranslationZ(layer);
        }
    }

    void loadUrl(String url) {
        if (destroyed || url == null || url.length() == 0) return;
        web.loadUrl(url);
    }

    void reload(boolean ignoreCache) {
        if (destroyed) return;
        if (ignoreCache) {
            web.clearCache(true);
        }
        web.reload();
    }

    // ------------------------------------------------------------ Page event channel

    /**
     * Last hop of EmitBrowserEvent: server event -> window.__cefDispatch(name, args).
     *
     * Pages not yet loaded are queued (measured WebView cold start: 1-3 seconds), then replayed in arrival order after loading completes;
     * over the limit, the oldest are dropped with a warning, never growing without bound.
     */
    void dispatchEvent(String name, String argsJson) {
        if (destroyed || name == null || name.length() == 0) return;

        if (!pageReady) {
            if (pendingEvents.size() >= MAX_PENDING_EVENTS) {
                String[] dropped = pendingEvents.pollFirst();
                Log.w(TAG, "pending queue full, dropping '" + (dropped != null ? dropped[0] : "?") + "'");
            }
            pendingEvents.addLast(new String[]{name, argsJson != null ? argsJson : "[]"});
            Log.i(TAG, "queue event '" + name + "' for browser " + browserId
                    + " (" + pendingEvents.size() + " pending, page not ready)");
            return;
        }
        evaluateDispatch(name, argsJson);
    }

    private void evaluateDispatch(String name, String argsJson) {
        web.evaluateJavascript(CefJs.dispatchScript(name, argsJson), null);
        Log.i(TAG, "dispatch '" + name + "' -> browser " + browserId
                + " " + (argsJson != null ? argsJson : "[]"));
    }

    /**
     * Client internal event (local branch of PC ChatHook::SetChatInputState): sent only to the page,
     * never upstream. Takes the same queue path, so it is not lost when the page has not finished loading.
     */
    void dispatchInternalEvent(String name, String argsJson) {
        dispatchEvent(name, argsJson);
    }

    /** When a remote URL's byte stream cannot be modified, evaluateJavascript injects it afterwards (local http://cef/ pages already have it inlined). */
    private void injectBootstrap(String why) {
        web.evaluateJavascript(CefJs.BOOTSTRAP, null);
        Log.i(TAG, "bootstrap injected (" + why + ") browser " + browserId);
    }

    private void flushPendingEvents() {
        if (pendingEvents.isEmpty()) return;
        Log.i(TAG, "flushing " + pendingEvents.size() + " queued event(s) for browser " + browserId);
        while (!pendingEvents.isEmpty()) {
            String[] ev = pendingEvents.pollFirst();
            if (ev != null && ev.length >= 2) {
                evaluateDispatch(ev[0], ev[1]);
            }
        }
    }

    /** Keeps the same routing check as CefResourceProvider: http://cef/ or cef://. */
    private static boolean isCefUrl(String url) {
        if (url == null) return false;
        String lower = url.toLowerCase(Locale.US);
        return lower.startsWith("http://cef/") || lower.startsWith("https://cef/")
                || lower.startsWith("cef://");
    }

    boolean isDestroyed() {
        return destroyed;
    }

    int browserId() {
        return browserId;
    }

    boolean isVisible() {
        return visible;
    }

    boolean isFocused() {
        return focused;
    }

    boolean controlsChatInput() {
        return controlsChatInput;
    }

    void destroy() {
        if (destroyed) return;
        destroyed = true;
        pageReady = false;
        pendingEvents.clear();
        hideIme();
        ViewGroup parent = (ViewGroup) overlay.getParent();
        if (parent != null) {
            parent.removeView(overlay);
        }
        web.stopLoading();
        web.loadUrl("about:blank");
        web.removeAllViews();
        web.removeJavascriptInterface(CefJs.BRIDGE_NAME);
        web.destroy();
        // The frame-capture reuse buffer is released along with it, or an offscreen page would still hold a 0.5MB bitmap after destroy.
        if (frameBmp != null) {
            frameBmp.recycle();
            frameBmp = null;
        }
        frameCanvas = null;
        frameBuf = null;
        Log.i(TAG, "browser " + browserId + " destroyed");
    }

    private final class CefClient extends WebViewClient {

        @Override
        public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
            // Keeps in-page navigation in the same WebView; external links are never handed to the system browser,
            // since a system browser popping up in-game would cover the game screen, unlike the original client.
            return false;
        }

        @Override
        public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
            String url = request.getUrl() != null ? request.getUrl().toString() : "";
            return CefResourceProvider.open(activity, url);
        }

        @Override
        public void onPageStarted(WebView view, String url, android.graphics.Bitmap favicon) {
            // New document: the previous __cefDispatch disappears with the old JS context.
            pageReady = false;
            if (!isCefUrl(url)) {
                // Remote pages cannot get our byte stream, so injection can only be added here.
                injectBootstrap("onPageStarted remote " + url);
            }
        }

        @Override
        public void onPageFinished(WebView view, String url) {
            pageReady = true;
            // Local pages already have the bootstrap inlined by CefResourceProvider (no race); injecting once more here
            // is an idempotent safeguard, covering 302 / direct data: input and other paths that never went through the interceptor.
            injectBootstrap("onPageFinished");
            flushPendingEvents();
            Log.i(TAG, "browser " + browserId + " onPageFinished " + url);
        }

        @Override
        public void onReceivedError(WebView view, WebResourceRequest request,
                                    android.webkit.WebResourceError error) {
            Log.w(TAG, "browser " + browserId + " onReceivedError "
                    + request.getUrl() + " : " + error.getErrorCode()
                    + " " + error.getDescription());
        }

        @Override
        public void onReceivedHttpError(WebView view, WebResourceRequest request,
                                        WebResourceResponse errorResponse) {
            Log.w(TAG, "browser " + browserId + " onReceivedHttpError "
                    + request.getUrl() + " : " + errorResponse.getStatusCode()
                    + " " + errorResponse.getReasonPhrase());
        }
    }

    private final class CefChromeClient extends WebChromeClient {

        @Override
        public boolean onConsoleMessage(ConsoleMessage message) {
            Log.i(TAG, "console[" + browserId + "] " + message.message()
                    + " @" + message.sourceId() + ":" + message.lineNumber());
            return true;
        }
    }
}
