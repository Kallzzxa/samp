package com.sampmobile.launcher.download;

import android.content.Context;
import android.util.Log;

import com.sampmobile.launcher.utils.GameDataImporter;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Extract/install a downloaded data package into the app's private directory.
 *
 * Installed at: {@code getExternalFilesDir(null)}, i.e.
 * {@code /sdcard/Android/data/<pkg>/files} — **not** a files/gamedata subdirectory.
 * The game (SAMP.java) and GameDataImporter both read texdb/, SAMP/, and audio/ from files/ as the root,
 * installing into a subdirectory would make the game unable to find them.
 *
 * On Android 11+, the app itself can freely write to this path, without MANAGE_EXTERNAL_STORAGE.
 * Only third parties (adb, file managers) are restricted by FUSE secure_mkdirs, which is why
 * push_gamedata_to_device.py used to take the "tar it up, then extract on-device" detour. The downloader now writes directly
 * from the app process, so that detour is no longer needed at all.
 *
 * Installation **extracts to a temp directory first, then merges it in as a whole**: a power cut or killed process mid-way cannot leave a half-installed data set
 * that crashes the game halfway through reading it.
 */
public final class GamedataInstaller {

    private static final String TAG = "GamedataInstaller";

    /** Temp directory used while extracting (a sibling of files/, so the game cannot mistake it for data). */
    private static final String STAGING_DIR = ".gamedata.staging";

    public interface Listener {
        /** @param entryName The entry currently being extracted (normalized relative path) */
        void onProgress(String entryName, long bytesWritten, long bytesTotal);

        void onStatus(String message);
    }

    private final Context context;

    public GamedataInstaller(Context context) {
        this.context = context.getApplicationContext();
    }

    public File filesRoot() {
        File dir = context.getExternalFilesDir(null);
        if (dir == null) {
            // On the rare device where external storage is not mounted, fall back to internal storage. Capacity may be insufficient, but at least it does not crash.
            dir = new File(context.getFilesDir(), "ext");
        }
        return dir;
    }

    /** The install target is the files/ root directory. */
    public File installDir() {
        return filesRoot();
    }

    public File stagingDir() {
        return new File(filesRoot(), STAGING_DIR);
    }

    /** Whether the data is ready: reuses GameDataImporter's core-file check, so it agrees with manual imports. */
    public boolean isInstalled() {
        return GameDataImporter.isGameDataReady(filesRoot());
    }

    /** Which core files are missing (an empty list means ready). */
    public java.util.List<String> missingCoreFiles() {
        return GameDataImporter.findMissingCoreFiles(filesRoot());
    }

    /**
     * Extract a zip into the staging directory.
     *
     * Entry paths are normalized with {@link GameDataImporter#normalizeZipEntryPath}:
     * A package's top level may carry a files/ or gamedata/files/ wrapper; after normalization everything lands at the root,
     * so the install comes out right no matter how the packaging script wrapped it.
     *
     * @param zip        The downloaded and verified zip
     * @param totalBytes Progress denominator (approximated by the archive size)
     */
    public void extractToStaging(File zip, long totalBytes, Listener listener) throws IOException {
        extractToStaging(zip, totalBytes, listener, true);
    }

    /**
     * @param fresh true = clear staging first (for the first package);
     *              false = append to the existing staging directory (for later packages, to overwrite same-named resources)
     */
    public void extractToStaging(File zip, long totalBytes, Listener listener, boolean fresh)
            throws IOException {
        File staging = stagingDir();
        if (fresh) {
            deleteRecursively(staging);
        }
        if (!staging.isDirectory() && !staging.mkdirs()) {
            throw new IOException("Cannot create temp directory: " + staging);
        }

        long written = 0;
        byte[] buf = new byte[128 * 1024];

        try (ZipInputStream zis = new ZipInputStream(
                new BufferedInputStream(new FileInputStream(zip)))) {
            ZipEntry entry;
            while ((entry = zis.getNextEntry()) != null) {
                String raw = entry.getName();

                if (entry.isDirectory()) {
                    zis.closeEntry();
                    continue;
                }

                // Normalize + safety check (reject .. / absolute paths / __MACOSX)
                String rel = GameDataImporter.normalizeZipEntryPath(raw);
                if (rel == null) {
                    Log.d(TAG, "Skipping entry: " + raw);
                    zis.closeEntry();
                    continue;
                }
                if (!isSafeEntryName(rel)) {
                    Log.w(TAG, "Skipping unsafe zip entry: " + raw);
                    zis.closeEntry();
                    continue;
                }

                File out = new File(staging, rel);
                File parent = out.getParentFile();
                if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
                    throw new IOException("Cannot create parent directory: " + parent);
                }

                try (OutputStream os = new FileOutputStream(out)) {
                    int n;
                    while ((n = zis.read(buf)) > 0) {
                        os.write(buf, 0, n);
                        written += n;
                        if (listener != null) {
                            listener.onProgress(rel, Math.min(written, totalBytes), totalBytes);
                        }
                    }
                }
                zis.closeEntry();
            }
        }

        if (listener != null) listener.onStatus("Extraction complete, activating...");
    }

    /**
     * Merge staging into the files/ root directory.
     *
     * Why "merge" instead of "rename the whole thing": the install target is the files/ root, which may also contain files placed there by hand
     * (settings, saves); replacing the whole directory would delete those too. Merging only overwrites
     * same-named files from the package and leaves the rest untouched.
     */
    public void activateStaging() throws IOException {
        File staging = stagingDir();
        if (!staging.isDirectory()) {
            throw new IOException("Temp directory does not exist, cannot activate: " + staging);
        }

        File root = installDir();
        int files = mergeDir(staging, root, "");
        Log.i(TAG, "Merged " + files + " files into " + root);

        deleteRecursively(staging);
    }

    private int mergeDir(File srcDir, File dstDir, String relPrefix) throws IOException {
        File[] children = srcDir.listFiles();
        if (children == null) return 0;

        int count = 0;
        for (File src : children) {
            String rel = relPrefix.isEmpty() ? src.getName() : relPrefix + "/" + src.getName();
            File dst = new File(dstDir, src.getName());

            if (src.isDirectory()) {
                if (!dst.isDirectory() && !dst.mkdirs()) {
                    throw new IOException("Cannot create directory: " + dst);
                }
                count += mergeDir(src, dst, rel);
            } else {
                // User config (server list, settings, logs) must not be overwritten by a data package,
                // otherwise players would lose their server bookmarks on every data update.
                if (GameDataImporter.shouldPreserveExisting(rel) && dst.exists()) {
                    Log.i(TAG, "Keeping user file, not overwriting: " + rel);
                    continue;
                }
                // Overwrite the same-named file; delete the old one first
                if (dst.exists() && !dst.delete()) {
                    throw new IOException("Cannot overwrite existing file: " + dst);
                }

                // Prefer renameTo: staging and files/ are on the same filesystem, so renaming is zero-copy,
                // 1GB+ of data needs no extra copy of the space and no byte-by-byte I/O.
                // Only fall back to copying across filesystems (which in theory should not happen, but can when external storage is split).
                if (!src.renameTo(dst)) {
                    copyFile(src, dst);
                }
                count++;
            }
        }
        return count;
    }

    /**
     * Byte-by-byte copy (fallback when renameTo fails across filesystems).
     *
     * Deliberately not using java.nio.file.Files.copy: that is API 26 only,
     * while this project targets minSdk 24, and it would throw NoClassDefFoundError outright on Android 7.x.
     */
    private static void copyFile(File src, File dst) throws IOException {
        try (InputStream in = new FileInputStream(src);
             OutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[256 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
        if (!src.delete()) {
            Log.w(TAG, "Failed to delete temp file (does not affect the result): " + src);
        }
    }

    /** Clean up the temp directory when installation fails. */
    public void discardStaging() {
        deleteRecursively(stagingDir());
    }

    /**
     * Merge an already-extracted file into the install directory (so the online package can overwrite same-named resources from the game package).
     */
    public void mergeIntoInstall(File srcFile, String relPath) throws IOException {
        if (!isSafeEntryName(relPath)) {
            throw new IOException("Unsafe relative path: " + relPath);
        }
        File dst = new File(installDir(), relPath);
        File parent = dst.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("Cannot create directory: " + parent);
        }
        try (InputStream in = new FileInputStream(srcFile);
             OutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[128 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }

    /**
     * Check whether a zip entry name is safe.
     *
     * Reject: absolute paths (starting with / or a drive letter), segments containing .., and Windows-style paths with backslashes
     * (On Android a backslash is treated as an ordinary character, but the extracted directory structure would not match expectations).
     */
    static boolean isSafeEntryName(String name) {
        if (name == null || name.isEmpty()) return false;
        if (name.startsWith("/") || name.startsWith("\\")) return false;
        if (name.length() > 1 && name.charAt(1) == ':') return false;
        if (name.contains("\\")) return false;
        for (String seg : name.split("/")) {
            if (seg.equals("..")) return false;
        }
        return true;
    }

    /** Delete recursively; failures do not throw (nobody handles exceptions in cleanup paths). */
    public static void deleteRecursively(File f) {
        if (f == null || !f.exists()) return;
        if (f.isDirectory()) {
            File[] children = f.listFiles();
            if (children != null) {
                for (File c : children) deleteRecursively(c);
            }
        }
        if (!f.delete()) Log.w(TAG, "Failed to delete: " + f);
    }
}
