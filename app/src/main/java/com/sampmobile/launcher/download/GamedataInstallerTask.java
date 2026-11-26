package com.sampmobile.launcher.download;

import android.content.Context;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * Overall orchestration of the data-package preparation flow.
 *
 * One complete flow:
 *   1. Fetch manifest.json
 *   2. Compare with the locally installed version — if identical, return "ready" directly
 *   3. Download each package's chunks (resumable)
 *   4. Assemble into the whole package and verify its SHA-256
 *   5. Extract to staging
 *   6. Promote staging to the live directory
 *   7. Record the installed version
 *
 * Every step can be interrupted; re-entering resumes from the last breakpoint (chunk-level resume).
 */
public final class GamedataInstallerTask {

    private static final String TAG = "GamedataTask";

    /** Max bytes read for a manifest in one go, to prevent a huge JSON from blowing up memory. */
    private static final int MAX_MANIFEST_BYTES = 8 * 1024 * 1024;

    public interface Listener {
        void onStage(Stage stage);

        /** @param bytesDone / bytesTotal Cumulative progress across all packages */
        void onProgress(long bytesDone, long bytesTotal);

        void onStatus(String message);

        void onError(String message, Throwable cause);

        void onDone(boolean installedNow);
    }

    public enum Stage {
        IDLE,
        FETCHING_MANIFEST,
        CHECKING,
        DOWNLOADING,
        VERIFYING,
        EXTRACTING,
        ACTIVATING,
        READY
    }

    private final Context context;
    private final String manifestUrl;
    private final boolean forceRedownload;
    private final GamedataInstaller installer;
    private final ChunkDownloader downloader;

    private volatile boolean cancelled = false;
    private volatile Listener listener;

    public GamedataInstallerTask(Context context, String manifestUrl) {
        this(context, manifestUrl, false);
    }

    public GamedataInstallerTask(Context context, String manifestUrl, boolean forceRedownload) {
        this.context = context.getApplicationContext();
        this.manifestUrl = manifestUrl;
        this.forceRedownload = forceRedownload;
        this.installer = new GamedataInstaller(this.context);
        this.downloader = new ChunkDownloader(cacheDir());
    }

    public GamedataInstaller installer() {
        return installer;
    }

    public void cancel() {
        cancelled = true;
        downloader.cancel();
    }

    private File cacheDir() {
        File d = new File(installer.filesRoot(), ".dl");
        //noinspection ResultOfMethodCallIgnored
        d.mkdirs();
        return d;
    }

    private File stateFile() {
        return new File(cacheDir(), "installed.json");
    }

    /**
     * Whether the data is usable. The rules live in {@link GamedataState}; this only forwards the call,
     * so the check is not written twice (here and in Splash).
     */
    public boolean isReady() {
        return GamedataState.isReady(context);
    }

    /** Installed version; null if never installed. */
    public String installedVersion() {
        return GamedataState.installedVersion(context);
    }

    /**
     * Run one complete flow. **Must be called on a background thread.**
     */
    public void run(Listener l) {
        this.listener = l;
        try {
            if (!installer.isInstalled()) {
                l.onStage(Stage.FETCHING_MANIFEST);
                l.onStatus("Fetching data package info...");
            } else {
                l.onStage(Stage.CHECKING);
                l.onStatus("Checking for data updates...");
            }

            String json = fetchManifest();
            DataManifest manifest;
            try {
                manifest = DataManifest.parse(json, clientVersionCode());
            } catch (DataManifest.ManifestException e) {
                fail(l, e.getMessage(), e);
                return;
            }

            if (cancelled) {
                fail(l, "Cancelled", null);
                return;
            }

            // When not forcing a re-download: skip if the version matches or local data is already complete
            String installed = installedVersion();
            if (!forceRedownload && installed != null && installed.equals(manifest.version) && installer.isInstalled()) {
                l.onStage(Stage.READY);
                l.onStatus("Data is already up to date (" + manifest.version + ")");
                l.onDone(false);
                return;
            }

            // When not forcing a re-download and local core files are already complete (e.g. the player imported manually): only write the version record, do not download again
            if (!forceRedownload && installed == null && installer.isInstalled()) {
                writeState(manifest.version);
                l.onStage(Stage.READY);
                l.onStatus("Local data detected, no download needed");
                l.onDone(false);
                return;
            }

            long total = manifest.totalBytes();
            long doneBase = 0;

            List<DataManifest.Package> allPackages = new ArrayList<>();
            for (DataManifest.Group g : manifest.groups) {
                allPackages.addAll(g.packages);
            }

            l.onStage(Stage.DOWNLOADING);

            for (DataManifest.Package pkg : allPackages) {
                if (cancelled) {
                    fail(l, "Cancelled", null);
                    return;
                }
                final long base = doneBase;
                l.onStatus("Downloading " + pkg.label);
                boolean ok = downloader.downloadPackage(
                        baseUrlOf(manifestUrl), pkg,
                        new ChunkDownloader.Listener() {
                            @Override
                            public void onProgress(long bytesDone, long bytesTotal) {
                                if (listener != null) {
                                    listener.onProgress(base + bytesDone, total);
                                }
                            }

                            @Override
                            public void onChunkDone(int chunkIndex, int chunkCount) {
                                // A finished chunk is not announced separately; the progress bar already reflects it
                            }

                            @Override
                            public void onStatus(String message) {
                                if (listener != null) listener.onStatus(message);
                            }
                        });
                if (!ok) {
                    fail(l, "Download failed: " + pkg.label, null);
                    return;
                }
                doneBase += pkg.bytes;
            }

            if (cancelled) {
                fail(l, "Cancelled", null);
                return;
            }

            // ---- Verify + extract ----
            l.onStage(Stage.VERIFYING);
            l.onProgress(total, total);
            // Clear staging only once, before the earliest package; later packages extract into the same directory,
            // so the online package can overwrite same-named SAMP/ resources from the game package.
            installer.discardStaging();

            // Process package by package: assemble → verify → extract → delete, without holding all zips at once.
            // A 2GB package must be handled this way on a phone, otherwise "2G of chunks + 2G zip + 2G extracted" existing side by side would blow up storage.
            for (int i = 0; i < allPackages.size(); i++) {
                if (cancelled) {
                    fail(l, "Cancelled", null);
                    return;
                }
                DataManifest.Package pkg = allPackages.get(i);
                File dir = new File(cacheDir(), pkg.id);
                File zip = new File(cacheDir(), pkg.id + ".zip");

                l.onStage(Stage.VERIFYING);
                l.onStatus("Verifying " + pkg.label);

                // If a finished zip of the same size already exists, skip assembly (a previous run already got this far)
                if (!(zip.isFile() && zip.length() == pkg.bytes)) {
                    // Assembly deletes chunks as it goes, so peak usage is only one chunk more than the whole package
                    ChunkDownloader.concatChunks(dir, pkg, zip, false);
                }
                if (!ChunkDownloader.verify(zip, pkg.sha256)) {
                    //noinspection ResultOfMethodCallIgnored
                    zip.delete();
                    fail(l, "Data package verification failed (possibly damaged in transit), please retry: " + pkg.label, null);
                    return;
                }

                l.onStage(Stage.EXTRACTING);
                l.onStatus("Installing " + (i + 1) + "/" + allPackages.size() + ":" + pkg.label);
                // The first package clears staging; later packages append (overwriting same-named resources)
                installer.extractToStaging(zip, zip.length(), new GamedataInstaller.Listener() {
                    @Override
                    public void onProgress(String entryName, long bytesWritten, long bytesTotal) {
                        if (listener != null) listener.onStatus("Extracting " + entryName);
                    }

                    @Override
                    public void onStatus(String message) {
                        if (listener != null) listener.onStatus(message);
                    }
                }, i == 0);

                // Delete the zip right after extracting, to free space for the next package
                //noinspection ResultOfMethodCallIgnored
                zip.delete();
                GamedataInstaller.deleteRecursively(dir);
            }

            l.onStage(Stage.ACTIVATING);
            l.onStatus("Activating data...");
            installer.activateStaging();

            // Record the installed version
            writeState(manifest.version);

            // After success, clean up the chunk cache (saves hundreds of MB)
            for (DataManifest.Package pkg : allPackages) {
                GamedataInstaller.deleteRecursively(new File(cacheDir(), pkg.id));
                //noinspection ResultOfMethodCallIgnored
                new File(cacheDir(), pkg.id + ".zip").delete();
            }

            l.onStage(Stage.READY);
            l.onStatus("Data preparation complete");
            l.onDone(true);

        } catch (InterruptedException e) {
            fail(l, "Cancelled", e);
        } catch (Exception e) {
            Log.e(TAG, "Failed to prepare data", e);
            fail(l, e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage(), e);
        }
    }

    private void fail(Listener l, String msg, Throwable cause) {
        if (l == null) return;
        l.onError(msg, cause);
    }

    // ------------------------------------------------------------------ Network

    private String fetchManifest() throws IOException {
        HttpURLConnection conn = null;
        try {
            URL url = new URL(manifestUrl);
            conn = (HttpURLConnection) url.openConnection();
            conn.setConnectTimeout(15000);
            conn.setReadTimeout(20000);
            conn.setInstanceFollowRedirects(true);
            // The manifest must be fresh; do not let an intermediate layer cache it
            conn.setRequestProperty("Cache-Control", "no-cache");

            int code = conn.getResponseCode();
            if (code != HttpURLConnection.HTTP_OK) {
                throw new IOException("Failed to fetch manifest: HTTP " + code);
            }

            try (InputStream in = conn.getInputStream()) {
                java.io.ByteArrayOutputStream bos = new java.io.ByteArrayOutputStream();
                byte[] buf = new byte[16 * 1024];
                int n;
                int total = 0;
                while ((n = in.read(buf)) > 0) {
                    total += n;
                    if (total > MAX_MANIFEST_BYTES) {
                        throw new IOException("Manifest is abnormally large (>" + MAX_MANIFEST_BYTES + " bytes)");
                    }
                    bos.write(buf, 0, n);
                }
                return new String(bos.toByteArray(), StandardCharsets.UTF_8);
            }
        } finally {
            if (conn != null) conn.disconnect();
        }
    }

    /** The manifest's directory (dropping the final filename segment). */
    static String baseUrlOf(String manifestUrl) {
        int idx = manifestUrl.lastIndexOf('/');
        return idx > 0 ? manifestUrl.substring(0, idx + 1) : manifestUrl;
    }

    private int clientVersionCode() {
        try {
            return context.getPackageManager()
                    .getPackageInfo(context.getPackageName(), 0).versionCode;
        } catch (Exception e) {
            return 0;
        }
    }

    // ------------------------------------------------------------------ State

    private void writeState(String version) {
        JSONObject o = new JSONObject();
        try {
            o.put("version", version);
            o.put("installedAt", System.currentTimeMillis());
            try (FileOutputStream fos = new FileOutputStream(stateFile())) {
                fos.write(o.toString().getBytes(StandardCharsets.UTF_8));
            }
        } catch (JSONException | IOException e) {
            // A failed state write does not affect data usability; next time it is rechecked as "unknown version"
            Log.w(TAG, "Failed to write install state: " + e.getMessage());
        }
    }
}
