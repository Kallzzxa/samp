package com.sampmobile.launcher.download;

import android.content.Context;

import com.sampmobile.launcher.BuildConfig;
import com.sampmobile.launcher.R;
import com.sampmobile.launcher.utils.GameDataImporter;

import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

/**
 * Data-readiness check — only here, so Splash and the download task do not each write their own copy and drift apart.
 *
 * Rule: **an install record exists** AND **GTA core files are complete**.
 *
 * Why not check core files only:
 *   If a data package contains only online resources (no texdb/gta3.img etc.), every launch would judge it "not ready",
 *   forming a "downloaded → restarted → asked to download again" loop.
 *
 * Why not check the install record only:
 *   The record may exist while the user deleted the files from a file manager, and the game would crash on entry. Core files are the final proof.
 */
public final class GamedataState {

    private GamedataState() {}

    /** Install record file (same directory as ChunkDownloader's cache). */
    public static File stateFile(Context context) {
        File root = context.getExternalFilesDir(null);
        if (root == null) root = new File(context.getFilesDir(), "ext");
        return new File(new File(root, ".dl"), "installed.json");
    }

    /** Installed version; null if there is no record. */
    public static String installedVersion(Context context) {
        File f = stateFile(context);
        if (!f.isFile()) return null;
        try (InputStream in = new FileInputStream(f)) {
            java.io.ByteArrayOutputStream bos = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int n;
            while ((n = in.read(buf)) > 0) bos.write(buf, 0, n);
            return new JSONObject(new String(bos.toByteArray(), StandardCharsets.UTF_8))
                    .optString("version", null);
        } catch (Exception e) {
            return null;
        }
    }

    /** Whether the data is ready (usable by the game directly). */
    public static boolean isReady(Context context) {
        File root = context.getExternalFilesDir(null);

        // Complete core files means usable — whether those files were installed by this downloader, or the player used
        // GameDataImporter to import them manually, or pushed them in with adb.
        // An early implementation required installed.json, which forced long-time players who had already placed their data manually
        // to re-download as soon as a download URL was configured. That was wrong.
        if (GameDataImporter.isGameDataReady(root)) return true;

        // When core files are incomplete, if the downloader once installed successfully (there is a version record),
        // it means the data was damaged afterwards (e.g. the user deleted files from a file manager) and needs re-downloading.
        // No record and no files means a first launch.
        return false;
    }

    /**
     * Whether the data was installed by this downloader (has a version record).
     *
     * Difference from {@link #isReady}: this one is for "should we check for a new version",
     * while isReady is for "can we enter the game directly". For long-time players who imported manually, the former is false and
     * the latter is true — they can play normally, they just do not get automatic updates.
     */
    public static boolean isManagedByDownloader(Context context) {
        return installedVersion(context) != null;
    }

    /** Which core files are missing (empty means complete), to give the player a more specific hint. */
    public static java.util.List<String> missingCoreFiles(Context context) {
        return GameDataImporter.findMissingCoreFiles(context.getExternalFilesDir(null));
    }

    /**
     * Resolve the built-in download manifest URL: prefer the build-time injected BuildConfig.GAMEDATA_MANIFEST_URL,
     * then fall back to strings.xml's gamedata_manifest_url; if both are empty, return an empty string.
     */
    public static String resolveManifestUrl(Context context) {
        String url = BuildConfig.GAMEDATA_MANIFEST_URL;
        if (url != null && !url.trim().isEmpty()) {
            return url.trim();
        }
        if (context != null) {
            try {
                String resUrl = context.getString(R.string.gamedata_manifest_url);
                if (resUrl != null && !resUrl.trim().isEmpty()) {
                    return resUrl.trim();
                }
            } catch (Exception ignored) {
            }
        }
        return "";
    }
}
