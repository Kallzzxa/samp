package com.sampmobile.game.ui.cef;

import android.util.Log;

import java.nio.charset.StandardCharsets;
import java.util.Locale;

/**
 * Injection source for the page-side window.cef implementation.
 *
 * Corresponds to the renderer-side JS API in omp-cef src/client/renderer/main.cpp:
 *
 *   cef.emit(eventName, ...args)   page -> server
 *   cef.on(eventName, callback)    server -> page (callback)
 *   cef.off([eventName[, callback]])
 *   cef.set_focus(hasFocus)
 *
 * The semantic difference is only in argument types: the original uses CefListValue (bool/int/double/string/null),
 * here JSON.stringify sends scalars across JNI, and CefJson restores the types from the literals
 * ("1" -> Integer, "1.5" -> Float, "true" -> Bool); the stage 4 device test
 * verifies byte-level consistency type by type.
 *
 * Injection method (key):
 *   1. We have full control over HTML under http://cef/, so CefResourceProvider inserts a <script> directly when
 *      returning the byte stream; window.cef already exists before the page's own scripts run,
 *      with no race — equivalent to CEF attaching the object when the V8 context is created.
 *   2. Remote URLs (server LoadUrl to an external site) cannot have their byte stream modified, so it falls back to injecting via onPageStarted /
 *      onPageFinished with two evaluateJavascript calls. In that case, if the page's own inline
 *      script calls cef.on before the late injection, it throws because window.cef does not exist yet — this is
 *      an inherent WebView limitation: scripts cannot run before document parsing, but the test page (path 1) is unaffected.
 *
 * The script must be pure ASCII: it may appear before <meta charset>, and non-ASCII bytes would be
 * parsed as the encoding the page declares, producing garbled text.
 */
final class CefJs {

    private static final String TAG = "CefJs";

    /** Bridge object name: window.cefNative (provided by CefWebView.addJavascriptInterface). */
    static final String BRIDGE_NAME = "cefNative";

    /** Already-injected flag, to avoid re-injection overwriting callbacks the page already registered. */
    private static final String MARKER = "__ompcef_v1";

    static final String BOOTSTRAP =
            "(function(){\n"
            + "  if (window.cef && window.cef.__mark === '" + MARKER + "') return;\n"
            + "  var handlers = Object.create(null);\n"
            + "  var pending  = Object.create(null);\n"
            + "  var MAX_PENDING = 256;\n"
            + "\n"
            + "  function invoke(list, args) {\n"
            + "    for (var i = 0; i < list.length; i++) {\n"
            + "      try { list[i].apply(null, args); }\n"
            + "      catch (e) { if (window.console) console.error('cef handler error', e); }\n"
            + "    }\n"
            + "  }\n"
            + "\n"
            + "  function dispatch(name, args) {\n"
            + "    var list = handlers[name];\n"
            + "    if (list && list.length) { invoke(list, args); return; }\n"
            + "    var q = pending[name] || (pending[name] = []);\n"
            + "    q.push(args);\n"
            + "    if (q.length > MAX_PENDING) q.shift();\n"
            + "  }\n"
            + "\n"
            + "  function flush(name) {\n"
            + "    var q = pending[name];\n"
            + "    if (!q || !q.length) return;\n"
            + "    delete pending[name];\n"
            + "    var list = handlers[name];\n"
            + "    if (!list) return;\n"
            + "    for (var i = 0; i < q.length; i++) invoke(list, q[i]);\n"
            + "  }\n"
            + "\n"
            + "  function bridge() {\n"
            + "    return (typeof window." + BRIDGE_NAME + " === 'object') ? window." + BRIDGE_NAME + " : null;\n"
            + "  }\n"
            + "\n"
            + "  var api = {\n"
            + "    __mark: '" + MARKER + "',\n"
            + "\n"
            + "    emit: function(name) {\n"
            + "      if (typeof name !== 'string')\n"
            + "        throw new TypeError('Invalid arguments to cef.emit(eventName, ...args)');\n"
            + "      var out = [];\n"
            + "      for (var i = 1; i < arguments.length; i++) {\n"
            + "        var v = arguments[i];\n"
            + "        var t = typeof v;\n"
            + "        // Same as the original: only bool / number / string cross the line; everything else is passed as null\n"
            + "        // and dropped on the browser-process side.\n"
            + "        if (t === 'boolean' || t === 'number' || t === 'string') out.push(v);\n"
            + "        else out.push(null);\n"
            + "      }\n"
            + "      var b = bridge();\n"
            + "      if (!b) { if (window.console) console.warn('cef: bridge unavailable'); return; }\n"
            + "      b.emit(name, JSON.stringify(out));\n"
            + "    },\n"
            + "\n"
            + "    on: function(name, callback) {\n"
            + "      if (typeof name !== 'string' || typeof callback !== 'function')\n"
            + "        throw new TypeError('Invalid arguments to cef.on(eventName, callback)');\n"
            + "      (handlers[name] || (handlers[name] = [])).push(callback);\n"
            + "      flush(name);\n"
            + "    },\n"
            + "\n"
            + "    off: function(name, callback) {\n"
            + "      if (arguments.length === 0) {\n"
            + "        handlers = Object.create(null);\n"
            + "        pending  = Object.create(null);\n"
            + "        return;\n"
            + "      }\n"
            + "      if (typeof name !== 'string')\n"
            + "        throw new TypeError('Invalid arguments to cef.off(eventName[, callback])');\n"
            + "      if (arguments.length === 1) { delete handlers[name]; delete pending[name]; return; }\n"
            + "      if (typeof callback !== 'function')\n"
            + "        throw new TypeError('Invalid arguments to cef.off(eventName[, callback])');\n"
            + "      var list = handlers[name];\n"
            + "      if (!list) return;\n"
            + "      for (var i = list.length - 1; i >= 0; i--) if (list[i] === callback) list.splice(i, 1);\n"
            + "      if (!list.length) delete handlers[name];\n"
            + "    },\n"
            + "\n"
            + "    set_focus: function(hasFocus) {\n"
            + "      if (typeof hasFocus !== 'boolean')\n"
            + "        throw new TypeError('Invalid arguments to cef.set_focus(hasFocus: boolean)');\n"
            + "      var b = bridge();\n"
            + "      if (b && b.setFocus) b.setFocus(hasFocus);\n"
            + "    }\n"
            + "  };\n"
            + "\n"
            + "  window.__cefDispatch = dispatch;\n"
            + "  try { Object.defineProperty(window, 'cef', { value: api, writable: false, configurable: true }); }\n"
            + "  catch (e) { window.cef = api; }\n"
            // Input-focus tracking: the original injects the same logic in OnAfterCreated (client.cpp),
            // so the client can tell apart "the page has the keyboard" and "the browser has focus".
            + "  try {\n"
            + "    var isFocused = false;\n"
            + "    var checkFocus = function (target) {\n"
            + "      var tag = (target && target.tagName) ? target.tagName : '';\n"
            + "      var isInput = (tag === 'INPUT' || tag === 'TEXTAREA' || (target && target.isContentEditable));\n"
            + "      if (isInput !== isFocused) {\n"
            + "        isFocused = isInput;\n"
            + "        api.emit('omp:cef:internal:inputFocusChanged', isFocused);\n"
            + "      }\n"
            + "    };\n"
            + "    document.addEventListener('focusin', function (e) { checkFocus(e.target); }, true);\n"
            + "    document.addEventListener('focusout', function () {\n"
            + "      if (isFocused) { isFocused = false; api.emit('omp:cef:internal:inputFocusChanged', false); }\n"
            + "    }, true);\n"
            + "    document.addEventListener('mousedown', function (e) { checkFocus(e.target); }, true);\n"
            + "  } catch (e) {}\n"
            + "  if (window.console) console.log('cef: window.cef ready (" + MARKER + ")');\n"
            + "})();\n";

    private CefJs() {
    }

    /** Server event -> JS dispatch call. name is JSON-escaped; argsJson is already an array literal. */
    static String dispatchScript(String name, String argsJson) {
        String safeArgs = (argsJson == null || argsJson.length() == 0) ? "[]" : argsJson;
        return "if(typeof window.__cefDispatch==='function'){window.__cefDispatch("
                + quote(name) + "," + safeArgs + ");}";
    }

    /**
     * Inserts the bootstrap into the HTML byte stream after <head>.
     *
     * Handled as bytes throughout: avoids UTF-8 decode/re-encode corrupting non-UTF-8 pages, and avoids
     * encoding ambiguity from the charset declaration's position (the script itself is ASCII).
     */
    static byte[] inject(byte[] html) {
        if (html == null || html.length == 0)
            return html;

        if (indexOf(html, MARKER.getBytes(StandardCharsets.US_ASCII), 0) >= 0) {
            return html; // Page already carries its own bootstrap
        }

        byte[] script = ("<script>" + BOOTSTRAP + "</script>")
                .getBytes(StandardCharsets.US_ASCII);

        int insertAt = afterTag(html, "head");
        if (insertAt < 0) insertAt = afterTag(html, "html");
        if (insertAt < 0) insertAt = 0;

        byte[] out = new byte[html.length + script.length];
        System.arraycopy(html, 0, out, 0, insertAt);
        System.arraycopy(script, 0, out, insertAt, script.length);
        System.arraycopy(html, insertAt, out, insertAt + script.length, html.length - insertAt);
        Log.i(TAG, "bootstrap injected at byte " + insertAt + " (page " + html.length + " B)");
        return out;
    }

    /** Returns the index after "<head ...>"; -1 if not found. ASCII case-insensitive. */
    private static int afterTag(byte[] data, String tag) {
        int i = 0;
        while (i < data.length) {
            if (data[i] == '<') {
                int j = i + 1;
                if (j < data.length && data[j] == '/') { i = j + 1; continue; }
                int k = 0;
                while (k < tag.length() && j + k < data.length) {
                    int c = data[j + k] & 0xFF;
                    if (c >= 'A' && c <= 'Z') c += 32;
                    if (c != tag.charAt(k)) break;
                    k++;
                }
                if (k == tag.length()) {
                    // Must be <head, not <header ...
                    int after = j + k;
                    if (after >= data.length) return data.length;
                    int c = data[after] & 0xFF;
                    if (c == '>' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/') {
                        for (int p = after; p < data.length; p++) {
                            if (data[p] == '>') return p + 1;
                        }
                        return data.length;
                    }
                }
            }
            i++;
        }
        return -1;
    }

    private static int indexOf(byte[] haystack, byte[] needle, int from) {
        if (needle.length == 0 || haystack.length < needle.length) return -1;
        outer:
        for (int i = Math.max(0, from); i + needle.length <= haystack.length; i++) {
            for (int j = 0; j < needle.length; j++) {
                if (haystack[i + j] != needle[j]) continue outer;
            }
            return i;
        }
        return -1;
    }

    private static String quote(String s) {
        StringBuilder sb = new StringBuilder(s == null ? 2 : s.length() + 2);
        sb.append('"');
        for (int i = 0; s != null && i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"':  sb.append("\\\""); break;
                case '\\': sb.append("\\\\"); break;
                case '\n': sb.append("\\n"); break;
                case '\r': sb.append("\\r"); break;
                case '\t': sb.append("\\t"); break;
                default:
                    if (c < 0x20 || c == 0x2028 || c == 0x2029) {
                        sb.append(String.format(Locale.US, "\\u%04x", (int) c));
                    } else {
                        sb.append(c);
                    }
                    break;
            }
        }
        sb.append('"');
        return sb.toString();
    }
}
