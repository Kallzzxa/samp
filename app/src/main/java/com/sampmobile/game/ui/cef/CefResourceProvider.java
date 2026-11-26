package com.sampmobile.game.ui.cef;

import android.content.Context;
import android.util.Log;
import android.webkit.WebResourceResponse;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * Replicates the URL semantics of the PC client's CEF custom scheme handler:
 *
 *     http://cef/[resourceName]/[relativePath]
 *     cef://[resourceName]/[relativePath]        (the PC side also recognizes this form)
 *
 * PC-side data flow: server sends FileData in chunks -> client assembles/decrypts into a local resource pack ->
 * scheme handler reads from the resource pack. Android keeps the same split:
 *
 *     Server delivers  ->  CefSession/CefResourceStore writes to disk (stage 5)
 *                          |
 *                          v
 *                 <filesDir>/cef/resources/<resource>/<path>
 *                          |
 *                          v
 *                   CefResourceProvider (this class)
 *
 * This round (stage 3) only wires up URL routing and error feedback: existing files are returned as-is; missing files return a
 * diagnostic page clearly marked "resource layer not synced", so an empty page is not mistaken for a successful page load.
 * Stage 5 adds OCEF2 decryption and manifest version checks here, without changing callers.
 */
final class CefResourceProvider {

    private static final String TAG = "CefResourceProvider";

    private CefResourceProvider() {
    }

    static String resourceRootPath(Context context) {
        return new File(context.getFilesDir(), "cef/resources").getAbsolutePath();
    }

    /**
     * @return null means do not intercept (leave to WebView default handling); non-null means a local response.
     */
    static WebResourceResponse open(Context context, String url) {
        if (url == null || url.length() == 0) return null;

        int schemeEnd = url.indexOf("://");
        if (schemeEnd <= 0) return null;
        String scheme = url.substring(0, schemeEnd).toLowerCase(Locale.US);

        // Strips query / fragment: resource paths never contain them, but they would pollute the filename.
        String rest = url.substring(schemeEnd + 3);
        int cut = rest.length();
        int q = rest.indexOf('?');
        if (q >= 0 && q < cut) cut = q;
        int f = rest.indexOf('#');
        if (f >= 0 && f < cut) cut = f;
        rest = rest.substring(0, cut);

        int slash = rest.indexOf('/');
        String authority = slash >= 0 ? rest.substring(0, slash) : rest;
        String path = slash >= 0 ? rest.substring(slash + 1) : "";

        // The authority segment may carry userinfo / port; resource routing only cares about the hostname.
        int at = authority.lastIndexOf('@');
        if (at >= 0) authority = authority.substring(at + 1);
        int colon = authority.indexOf(':');
        if (colon >= 0) authority = authority.substring(0, colon);

        // PC client registration (manager.cpp:314):
        //   CefRegisterSchemeHandlerFactory("http", "cef", ...)
        // So the real URL is http://cef/<resource>/<path>; cef:// is also accepted.
        final boolean isCefUrl =
                "cef".equals(scheme)
                        || (("http".equals(scheme) || "https".equals(scheme))
                        && "cef".equalsIgnoreCase(authority));
        if (!isCefUrl) return null;

        int pathSlash = path.indexOf('/');
        String resourceName = pathSlash >= 0 ? path.substring(0, pathSlash) : path;
        String relativePath = pathSlash >= 0 ? path.substring(pathSlash + 1) : "";
        if (relativePath.length() == 0) {
            // In requests like http://cef/favicon.ico, "favicon.ico" is the first segment after the host,
            // but it is clearly a file, not a resource name. The CEF side only handles <resource>/<path>,
            // so return 404 directly here; do not wrap it in index.html to generate a diagnostic page (that would create a fake
            // CEF_RESOURCE_MISS in the logs and inject a bootstrap for nothing).
            if (resourceName.indexOf('.') >= 0) {
                return response("text/plain", 404, "Not Found", new byte[0]);
            }
            relativePath = "index.html";
        }

        if (!isSafeSegment(resourceName) || !isSafeRelativePath(relativePath)) {
            Log.w(TAG, "rejected unsafe resource path: " + url);
            return errorPage(400, "Bad Request", "Invalid resource path");
        }

        // Segments starting with '.' are internal markers (e.g. .cefhash under the resource root) and are not exposed to pages.
        for (String segment : relativePath.split("/")) {
            if (segment.startsWith(".")) {
                return response("text/plain", 404, "Not Found", new byte[0]);
            }
        }

        File root = new File(context.getFilesDir(), "cef/resources");
        File target = new File(new File(root, resourceName), relativePath);

        // Defensive check: the resolved path must stay inside the resource root.
        try {
            String rootCanonical = root.getCanonicalPath();
            String targetCanonical = target.getCanonicalPath();
            if (!targetCanonical.startsWith(rootCanonical + File.separator)) {
                Log.w(TAG, "rejected traversal: " + url);
                return errorPage(403, "Forbidden", "Resource path out of bounds");
            }
        } catch (IOException e) {
            return errorPage(500, "Internal Error", "Failed to parse resource path");
        }

        if (!target.isFile()) {
            Log.i(TAG, "CEF_RESOURCE_MISS " + resourceName + "/" + relativePath);
            return missingPage(resourceName, relativePath);
        }

        try {
            byte[] data = readAll(target);
            // On the normal path, native (CefResources) has already unpacked and decrypted, so the disk copy is plaintext;
            // if an OCEF2 header is still seen, the unpack step went wrong (e.g. a manually inserted pak entry),
            // do not serve it to the WebView as a page.
            if (data.length >= 5 && data[0] == 'O' && data[1] == 'C' && data[2] == 'E'
                    && data[3] == 'F' && data[4] == '2') {
                Log.e(TAG, "CEF_RESOURCE_ENCRYPTED " + resourceName + "/" + relativePath
                        + " (" + data.length + " bytes) - still wrapped, extraction failed");
                return errorPage(503, "Resource Encrypted",
                        "This file is still in its encrypted package (the resource layer should decrypt it before writing it to disk)");
            }
            String mime = mimeOf(relativePath);
            if ("text/html".equals(mime)) {
                // window.cef is injected by CEF when the V8 context is created; WebView has no equivalent
                // pre-document script hook, so it is inlined into the byte stream — the object is ready before page scripts run,
                // and there is no race where cef.on runs before injection.
                data = CefJs.inject(data);
            }
            Log.i(TAG, "CEF_RESOURCE_HIT " + resourceName + "/" + relativePath
                    + " " + data.length + " bytes, mime=" + mime);
            return response(mime, 200, "OK", data);
        } catch (IOException e) {
            Log.w(TAG, "read failed " + target, e);
            return errorPage(500, "Internal Error", "Failed to read resource");
        }
    }

    private static boolean isSafeSegment(String s) {
        return s.length() > 0 && s.indexOf('\\') < 0 && s.indexOf(':') < 0
                && !s.equals(".") && !s.equals("..");
    }

    private static boolean isSafeRelativePath(String path) {
        if (path.length() == 0) return false;
        if (path.indexOf('\\') >= 0 || path.indexOf(':') >= 0) return false;
        String[] parts = path.split("/");
        for (String part : parts) {
            if (part.equals("..")) return false;
        }
        return true;
    }

    private static byte[] readAll(File file) throws IOException {
        long length = file.length();
        if (length > 64L * 1024 * 1024) {
            throw new IOException("resource too large: " + length);
        }
        byte[] out = new byte[(int) length];
        try (InputStream in = new FileInputStream(file)) {
            int off = 0;
            while (off < out.length) {
                int n = in.read(out, off, out.length - off);
                if (n < 0) break;
                off += n;
            }
            if (off != out.length) {
                byte[] trimmed = new byte[off];
                System.arraycopy(out, 0, trimmed, 0, off);
                return trimmed;
            }
        }
        return out;
    }

    private static WebResourceResponse response(String mime, int status, String reason, byte[] body) {
        Map<String, String> headers = new HashMap<>();
        headers.put("Cache-Control", "no-store");
        if (body != null) {
            headers.put("Content-Length", Integer.toString(body.length));
        }
        InputStream in = new ByteArrayInputStream(body != null ? body : new byte[0]);
        return new WebResourceResponse(mime, "utf-8", status, reason, headers, in);
    }

    private static WebResourceResponse errorPage(int status, String reason, String detail) {
        String html = page(status + " " + reason, detail);
        return response("text/html", status, reason,
                CefJs.inject(html.getBytes(StandardCharsets.UTF_8)));
    }

    private static WebResourceResponse missingPage(String resourceName, String relativePath) {
        String body = page("Resource not synced",
                "Resource <code>" + escape(resourceName) + "/" + escape(relativePath) + "</code>"
                        + " is not in the local resource directory.<br>"
                        + "This means the server manifest did not declare this dead link, or the entry was missing when the resource pack was unpacked — "
                        + "Check the <code>CefResources</code> log for <code>extracted: N file(s) written</code>.");
        return response("text/html", 503, "Resource Not Downloaded",
                CefJs.inject(body.getBytes(StandardCharsets.UTF_8)));
    }

    private static String page(String title, String detailHtml) {
        return "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                + "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                + "<style>html,body{margin:0;height:100%;background:#0b0f14;color:#e6edf3;"
                + "font:14px/1.6 -apple-system,Roboto,sans-serif}"
                + ".box{position:absolute;left:50%;top:50%;transform:translate(-50%,-50%);"
                + "max-width:70%;padding:18px 22px;border:1px solid #30404d;border-radius:10px;"
                + "background:#111820}"
                + "h1{font-size:15px;margin:0 0 8px;color:#ffb454;font-weight:600}"
                + "code{background:#1c2632;padding:1px 5px;border-radius:4px;color:#8fd3ff}"
                + "p{margin:6px 0;color:#a8b6c4}</style></head><body><div class=\"box\">"
                + "<h1>" + escape(title) + "</h1><p>" + detailHtml + "</p></div></body></html>";
    }

    private static String escape(String s) {
        if (s == null) return "";
        return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
                .replace("\"", "&quot;");
    }

    private static String mimeOf(String path) {
        String lower = path.toLowerCase(Locale.US);
        int dot = lower.lastIndexOf('.');
        String ext = dot >= 0 ? lower.substring(dot + 1) : "";
        switch (ext) {
            case "html":
            case "htm":
                return "text/html";
            case "js":
            case "mjs":
                return "application/javascript";
            case "css":
                return "text/css";
            case "json":
                return "application/json";
            case "png":
                return "image/png";
            case "jpg":
            case "jpeg":
                return "image/jpeg";
            case "gif":
                return "image/gif";
            case "webp":
                return "image/webp";
            case "svg":
                return "image/svg+xml";
            case "ico":
                return "image/x-icon";
            case "woff":
                return "font/woff";
            case "woff2":
                return "font/woff2";
            case "ttf":
                return "font/ttf";
            case "otf":
                return "font/otf";
            case "mp3":
                return "audio/mpeg";
            case "ogg":
                return "audio/ogg";
            case "wav":
                return "audio/wav";
            case "mp4":
                return "video/mp4";
            case "webm":
                return "video/webm";
            case "wasm":
                return "application/wasm";
            default:
                return "application/octet-stream";
        }
    }
}
