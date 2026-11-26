package com.sampmobile.launcher.download;

import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Chunk downloader: resumable downloads + limited concurrency + per-chunk verification.
 *
 * We chose HttpURLConnection over OkHttp: this project has historically had zero third-party network dependencies,
 * pulling in 800KB of OkHttp (plus Okio) for one downloader is not worth it. HttpURLConnection
 * supports Range, timeouts, and cancellation — good enough.
 *
 * State machine (per chunk, independent):
 *   Missing -> .part (downloading) -> verified -> .chunk (done)
 *                          └ Verification failed -> delete .part, re-download next round
 *
 * Resuming relies on HTTP Range: if a .part already exists, request from its length onward; if the server does not support
 * Range (returns 200 instead of 206), fall back to re-downloading the whole chunk instead of silently assembling a corrupt file.
 */
public final class ChunkDownloader {

    private static final String TAG = "ChunkDownloader";

    /** Timeout for a single chunk. Cross-border links are slow, so allow plenty of time, but do not wait forever. */
    private static final int CONNECT_TIMEOUT_MS = 15000;
    private static final int READ_TIMEOUT_MS = 30000;

    /** Max retries per chunk (including the first attempt). Exceeding it fails the whole download; no endless retries. */
    private static final int MAX_ATTEMPTS = 3;

    /** Callback (invoked on the download thread; the caller switches to the main thread itself). */
    public interface Listener {
        /** @param bytesDone Bytes completed (accumulated across all chunks) */
        void onProgress(long bytesDone, long bytesTotal);

        /** A chunk finished. */
        void onChunkDone(int chunkIndex, int chunkCount);

        /** Short status hint while a chunk downloads, e.g. "Retry 2/3". */
        void onStatus(String message);
    }

    private final File cacheDir;
    private final AtomicBoolean cancelled = new AtomicBoolean(false);

    public ChunkDownloader(File cacheDir) {
        this.cacheDir = cacheDir;
    }

    public void cancel() {
        cancelled.set(true);
    }

    public boolean isCancelled() {
        return cancelled.get();
    }

    /**
     * Download all chunks of a package.
     *
     * @param baseUrl Absolute prefix of the directory containing the manifest (e.g. https://cdn.example.com/),
     *                chunk relative URLs are appended to it
     * @return true if all chunks are in place and verified
     */
    public boolean downloadPackage(String baseUrl, DataManifest.Package pkg, Listener listener)
            throws IOException, InterruptedException {

        File dir = new File(cacheDir, pkg.id);
        if (!dir.isDirectory() && !dir.mkdirs()) {
            throw new IOException("Cannot create cache directory: " + dir);
        }

        final int count = pkg.chunks.size();

        // Drop .chunk files with the wrong length (treating a half-file as finished would make assembly fail outright),
        // then report initial progress from the actual disk state so the progress bar does not jump from 0.
        for (DataManifest.Chunk c : pkg.chunks) {
            File finalized = chunkFile(dir, c, true);
            if (finalized.isFile() && finalized.length() != c.bytes) {
                if (!finalized.delete()) {
                    Log.w(TAG, "Cannot delete the chunk with the wrong length " + finalized);
                }
            }
        }
        final AtomicLong done = new AtomicLong(completedBytes(dir, pkg));
        listener.onProgress(done.get(), pkg.bytes);

        // Download chunks sequentially. Concurrency is left to the upper layer running multiple packages in parallel (game and online can download at the same time),
        // serial within one package is easier to debug and does not slice bandwidth too thin.
        for (DataManifest.Chunk c : pkg.chunks) {
            if (cancelled.get()) throw new InterruptedException("Cancelled");

            File finalized = chunkFile(dir, c, true);
            if (finalized.isFile() && finalized.length() == c.bytes) {
                listener.onChunkDone(c.index, count);
                continue;
            }

            boolean ok = downloadChunk(baseUrl, dir, c, listener, done, pkg.bytes);
            if (!ok) {
                throw new IOException("Chunk " + c.index + " download failed (retried " + MAX_ATTEMPTS + " times)");
            }
            // After a chunk lands, recompute from the disk state as the progress baseline for the next chunk.
            // It can only grow, so it cannot go wrong: completedBytes only counts bytes that are actually in place.
            done.set(completedBytes(dir, pkg));
            listener.onProgress(done.get(), pkg.bytes);
            listener.onChunkDone(c.index, count);
        }

        if (cancelled.get()) throw new InterruptedException("Cancelled");
        return true;
    }

    /**
     * Completed bytes = sum of the lengths of finalized .chunk files + the length of the current .part.
     *
     * Do not use an accumulating counter: hash-failure re-downloads, servers ignoring Range and restarting, resume points
     * left over from a previous run... in these cases "subtract what was added" is hard to keep balanced,
     * subtract too much and progress goes negative; subtract too little and progress is inflated. Recomputing from disk state is always right,
     * at the cost of one directory scan per chunk (at most a few hundred chunks).
     */
    private static long completedBytes(File dir, DataManifest.Package pkg) {
        long total = 0;
        for (DataManifest.Chunk c : pkg.chunks) {
            File finalized = chunkFile(dir, c, true);
            if (finalized.isFile() && finalized.length() == c.bytes) {
                total += c.bytes;
                continue;
            }
            File part = chunkFile(dir, c, false);
            if (part.isFile() && part.length() > 0) {
                total += Math.min(part.length(), c.bytes);
            }
        }
        return total;
    }

    private boolean downloadChunk(String baseUrl, File dir, DataManifest.Chunk c,
                                  Listener listener, AtomicLong done, long totalBytes)
            throws InterruptedException {

        String urlStr = resolveUrl(baseUrl, c.url);
        File part = chunkFile(dir, c, false);
        File finalized = chunkFile(dir, c, true);

        for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
            if (cancelled.get()) throw new InterruptedException("Cancelled");

            try {
                long existing = part.isFile() ? part.length() : 0;
                // Already downloaded but not yet renamed: verify it directly, then finalize it.
                if (existing == c.bytes) {
                    if (verify(part, c.sha256)) {
                        if (!part.renameTo(finalized)) {
                            throw new IOException("Failed to rename chunk: " + part);
                        }
                        return true;
                    }
                    // Size matches but hash does not: delete the whole chunk and start over; resuming is not possible (the content is corrupt).
                    // Note: do not subtract from done — this chunk is only addAndGet'ed when it is verified and finalized,
                    // it was never counted here, so subtracting would make progress go negative.
                    if (!part.delete()) {
                        throw new IOException("Cannot delete the corrupt chunk: " + part);
                    }
                    existing = 0;
                }

                if (attempt > 1) {
                    listener.onStatus("Chunk " + c.index + " retry " + attempt + "/" + MAX_ATTEMPTS);
                }

                boolean got = fetchTo(urlStr, part, existing, c.bytes, done, totalBytes, listener);
                if (!got) {
                    // fetchTo returning false means the server does not support Range and there was leftover data; it has been reset, so try again next round
                    continue;
                }

                if (part.length() != c.bytes) {
                    throw new IOException("Chunk length mismatch: expected " + c.bytes + " actual " + part.length());
                }
                if (!verify(part, c.sha256)) {
                    throw new IOException("Chunk hash mismatch: " + c.index);
                }
                if (!part.renameTo(finalized)) {
                    throw new IOException("Failed to rename chunk: " + part);
                }
                return true;

            } catch (IOException e) {
                Log.w(TAG, "Chunk " + c.index + " attempt " + attempt + " failed: " + e.getMessage());
                if (attempt == MAX_ATTEMPTS) return false;
                // Back off, so repeated failures do not hammer the server
                Thread.sleep(800L * attempt);
            }
        }
        return false;
    }

    /**
     * Download/resume one chunk into {@code target}.
     *
     * @param skip Bytes already present, used as the Range start
     * @return false means the server ignored Range (returned the full 200 response); target has been cleared so the caller can retry
     */
    private boolean fetchTo(String urlStr, File target, long skip, long expected,
                            AtomicLong done, long totalBytes, Listener listener)
            throws IOException, InterruptedException {

        HttpURLConnection conn = null;
        try {
            URL url = new URL(urlStr);
            conn = (HttpURLConnection) url.openConnection();
            conn.setConnectTimeout(CONNECT_TIMEOUT_MS);
            conn.setReadTimeout(READ_TIMEOUT_MS);
            conn.setInstanceFollowRedirects(true);
            conn.setRequestProperty("Accept-Encoding", "identity");  // Compression breaks Range
            if (skip > 0) {
                conn.setRequestProperty("Range", "bytes=" + skip + "-");
            }

            int code = conn.getResponseCode();
            if (code == 416) {
                // Requested range is invalid: the local part is larger than the remote file, so re-download the whole chunk
                if (target.isFile() && !target.delete()) {
                    throw new IOException("Cannot reset chunk: " + target);
                }
                return false;
            }
            if (code != HttpURLConnection.HTTP_OK && code != HttpURLConnection.HTTP_PARTIAL) {
                throw new IOException("HTTP " + code + " from " + urlStr);
            }

            boolean serverHonouredRange = (code == HttpURLConnection.HTTP_PARTIAL);
            if (skip > 0 && !serverHonouredRange) {
                // Server does not support Range: must write from the start, so clear the old content first.
                // skip is the part's existing length, which may come from a file left by a previous run,
                // and was not necessarily counted in this run's done, so subtracting it would make progress go negative.
                if (!target.delete() && target.exists()) {
                    throw new IOException("Cannot reset chunk: " + target);
                }
                skip = 0;
            }

            try (InputStream in = conn.getInputStream();
                 FileOutputStream out = new FileOutputStream(target, skip > 0)) {

                byte[] buf = new byte[64 * 1024];
                // done records the bytes of "finalized chunks"; the current chunk is added by its actual written amount,
                // so progress stays correct no matter where the resume point came from or how many retries failed.
                final long base = done.get();
                long written = skip;
                int n;
                while ((n = in.read(buf)) > 0) {
                    if (cancelled.get()) throw new InterruptedException("Cancelled");
                    out.write(buf, 0, n);
                    written += n;
                    listener.onProgress(base + written, totalBytes);
                    if (written > expected) {
                        // Remote file is longer than the manifest declares: the data is wrong, stop writing.
                        throw new IOException("Chunk exceeds the expected length " + expected);
                    }
                }
                out.flush();
            }
            return true;

        } finally {
            if (conn != null) conn.disconnect();
        }
    }

    // ------------------------------------------------------------------ Utilities

    private static String resolveUrl(String baseUrl, String relative) {
        if (relative.startsWith("http://") || relative.startsWith("https://")) return relative;
        if (!baseUrl.endsWith("/")) baseUrl = baseUrl + "/";
        // Strip a leading ./ or / from a relative path
        while (relative.startsWith("./")) relative = relative.substring(2);
        while (relative.startsWith("/")) relative = relative.substring(1);
        return baseUrl + relative;
    }

    private static File chunkFile(File dir, DataManifest.Chunk c, boolean finalized) {
        return new File(dir, c.fileName() + (finalized ? ".chunk" : ".part"));
    }

    /** Compute a file's SHA-256 and compare. An empty sha means no verification (the whole package is still verified). */
    static boolean verify(File f, String sha256) {
        if (sha256 == null || sha256.isEmpty()) return true;
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            try (InputStream in = new java.io.FileInputStream(f)) {
                byte[] buf = new byte[128 * 1024];
                int n;
                while ((n = in.read(buf)) > 0) md.update(buf, 0, n);
            }
            return toHex(md.digest()).equalsIgnoreCase(sha256);
        } catch (NoSuchAlgorithmException | IOException e) {
            Log.w(TAG, "Verification failed " + f + ": " + e.getMessage());
            return false;
        }
    }

    /**
     * Concatenate all chunks of a package, in order, into a single file.
     *
     * Delete as you assemble: each chunk is deleted right after being copied into the output stream, so peak usage is only "output file + one chunk",
     * instead of "output + all chunks". On a 2GB package this saves 2GB of space — critical on a phone.
     *
     * @param keepChunks true keeps the chunks (for debugging); the normal path passes false
     */
    public static boolean concatChunks(File dir, DataManifest.Package pkg, File out,
                                       boolean keepChunks) throws IOException {
        File parent = out.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("Cannot create output directory: " + parent);
        }
        File tmp = new File(out.getPath() + ".tmp");
        try (FileOutputStream fos = new FileOutputStream(tmp)) {
            for (DataManifest.Chunk c : pkg.chunks) {
                File f = chunkFile(dir, c, true);
                if (!f.isFile()) throw new IOException("Missing chunk " + f);
                try (InputStream in = new java.io.FileInputStream(f)) {
                    byte[] buf = new byte[256 * 1024];
                    int n;
                    while ((n = in.read(buf)) > 0) fos.write(buf, 0, n);
                }
                if (!keepChunks) {
                    // Already assembled into tmp, so the original chunk is no longer needed. A failed delete does not affect correctness, only space.
                    if (!f.delete()) Log.w(TAG, "Failed to delete chunk (does not affect the result): " + f);
                }
            }
            fos.flush();
            fos.getFD().sync();
        }
        if (tmp.length() != pkg.bytes) {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            throw new IOException("Assembled size mismatch: expected " + pkg.bytes + " actual " + tmp.length());
        }
        //noinspection ResultOfMethodCallIgnored
        out.delete();
        if (!tmp.renameTo(out)) throw new IOException("Cannot rename to " + out);
        return true;
    }

    /** Kept for old callers (keeps the chunks). */
    public static boolean concatChunks(File dir, DataManifest.Package pkg, File out)
            throws IOException {
        return concatChunks(dir, pkg, out, true);
    }

    static String toHex(byte[] bytes) {
        StringBuilder sb = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) sb.append(String.format(Locale.US, "%02x", b));
        return sb.toString();
    }

    /** Read the bytes of a given range (not used when generating manifests; kept for upload scripts to spot-check consistency). */
    static byte[] readRange(File f, long offset, int len) throws IOException {
        try (RandomAccessFile raf = new RandomAccessFile(f, "r")) {
            raf.seek(offset);
            byte[] buf = new byte[len];
            int read = raf.read(buf);
            if (read <= 0) return new byte[0];
            if (read == len) return buf;
            byte[] trimmed = new byte[read];
            System.arraycopy(buf, 0, trimmed, 0, read);
            return trimmed;
        }
    }
}
