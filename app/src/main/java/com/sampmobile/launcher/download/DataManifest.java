package com.sampmobile.launcher.download;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Parsed result of the game data manifest (manifest.json).
 *
 * Matches the format in docs/GAMEDATA_DISTRIBUTION.md exactly. Parsing is **strict**:
 * Missing required fields, wrong types, or an unrecognized schema all throw {@link ManifestException},
 * no "guess a default and carry on" — the cost of a bad data package is a player waiting half an hour before hitting an error,
 * failing early is better than failing late.
 */
public final class DataManifest {

    /** The manifest schema this client understands. A mismatch is rejected outright. */
    public static final int SCHEMA = 1;

    public static final class Chunk {
        public final int index;
        public final String url;
        public final long bytes;
        public final String sha256;

        Chunk(int index, String url, long bytes, String sha256) {
            this.index = index;
            this.url = url;
            this.bytes = bytes;
            this.sha256 = sha256;
        }

        /** The chunk's filename in the local cache (fixed 4-digit index, for easy sorting and manual inspection). */
        public String fileName() {
            return String.format(java.util.Locale.US, "%04d", index);
        }
    }

    public static final class Package {
        public final String id;
        public final String label;
        public final long bytes;
        public final String sha256;
        public final List<Chunk> chunks;

        Package(String id, String label, long bytes, String sha256, List<Chunk> chunks) {
            this.id = id;
            this.label = label;
            this.bytes = bytes;
            this.sha256 = sha256;
            this.chunks = Collections.unmodifiableList(chunks);
        }
    }

    public static final class Group {
        public final String key;          // "game" / "online"
        public final String label;
        public final long totalBytes;
        public final boolean required;
        public final List<Package> packages;

        Group(String key, String label, long totalBytes, boolean required, List<Package> packages) {
            this.key = key;
            this.label = label;
            this.totalBytes = totalBytes;
            this.required = required;
            this.packages = Collections.unmodifiableList(packages);
        }
    }

    public final int schema;
    public final String version;
    public final int minClientVersionCode;
    public final List<Group> groups;

    private DataManifest(int schema, String version, int minClientVersionCode, List<Group> groups) {
        this.schema = schema;
        this.version = version;
        this.minClientVersionCode = minClientVersionCode;
        this.groups = Collections.unmodifiableList(groups);
    }

    public long totalBytes() {
        long total = 0;
        for (Group g : groups) total += g.totalBytes;
        return total;
    }

    /** Returns null when the group is not found (a manifest may contain only game, not online). */
    public Group group(String key) {
        for (Group g : groups) {
            if (g.key.equals(key)) return g;
        }
        return null;
    }

    // ------------------------------------------------------------------ Parsing

    public static DataManifest parse(String json, int clientVersionCode) throws ManifestException {
        final JSONObject root;
        try {
            root = new JSONObject(json);
        } catch (JSONException e) {
            throw new ManifestException("Manifest is not valid JSON: " + e.getMessage());
        }

        int schema = root.optInt("schema", -1);
        if (schema != SCHEMA) {
            throw new ManifestException("Manifest schema=" + schema + ", this client only supports " + SCHEMA
                    + "(please update the client)");
        }

        String version = root.optString("version", "");
        if (version.isEmpty()) {
            throw new ManifestException("Manifest is missing version");
        }

        int minClient = root.optInt("minClientVersionCode", 0);
        if (minClient > 0 && clientVersionCode < minClient) {
            throw new ManifestException("This data package requires client versionCode >= " + minClient
                    + ", current is " + clientVersionCode);
        }

        List<Group> groups = new ArrayList<>();
        // Fixed order: the base game first, then the online package. Players see the "big one installed first".
        for (String key : new String[]{"game", "online"}) {
            JSONObject g = root.optJSONObject(key);
            if (g == null) continue;
            groups.add(parseGroup(key, g));
        }

        if (groups.isEmpty()) {
            throw new ManifestException("Manifest contains neither a game nor an online group");
        }

        return new DataManifest(schema, version, minClient, groups);
    }

    private static Group parseGroup(String key, JSONObject g) throws ManifestException {
        String label = g.optString("label", key);
        boolean required = g.optBoolean("required", true);

        JSONArray pkgArray = g.optJSONArray("packages");
        if (pkgArray == null || pkgArray.length() == 0) {
            throw new ManifestException("Group " + key + " has no packages");
        }

        List<Package> packages = new ArrayList<>();
        for (int i = 0; i < pkgArray.length(); i++) {
            JSONObject p = pkgArray.optJSONObject(i);
            if (p == null) throw new ManifestException("Group " + key + "'s packages[" + i + "] is not an object");
            packages.add(parsePackage(key, p, i));
        }

        // A group's total size prefers the manifest's declared value; if missing or unreasonable, it falls back to summing the packages.
        long declared = g.optLong("totalBytes", 0);
        long sum = 0;
        for (Package p : packages) sum += p.bytes;
        long total = declared > 0 ? declared : sum;

        return new Group(key, label, total, required, packages);
    }

    private static Package parsePackage(String groupKey, JSONObject p, int pos)
            throws ManifestException {
        String where = groupKey + ".packages[" + pos + "]";

        String id = p.optString("id", "");
        if (id.isEmpty()) throw new ManifestException(where + " is missing id");
        // id is joined into a local path, so traversal must be blocked.
        if (id.contains("/") || id.contains("\\") || id.contains("..")) {
            throw new ManifestException(where + " 's id contains illegal characters: " + id);
        }

        long bytes = p.optLong("bytes", -1);
        if (bytes <= 0) throw new ManifestException(where + " 's bytes is invalid: " + bytes);

        String sha = p.optString("sha256", "").toLowerCase(java.util.Locale.US);
        if (!isHex64(sha)) throw new ManifestException(where + " 's sha256 is not 64-bit hex");

        JSONArray chunkArray = p.optJSONArray("chunks");
        if (chunkArray == null || chunkArray.length() == 0) {
            throw new ManifestException(where + " has no chunks");
        }

        List<Chunk> chunks = new ArrayList<>();
        long chunkSum = 0;
        for (int i = 0; i < chunkArray.length(); i++) {
            JSONObject c = chunkArray.optJSONObject(i);
            if (c == null) throw new ManifestException(where + ".chunks[" + i + "] is not an object");
            Chunk chunk = parseChunk(where, c, i);
            chunks.add(chunk);
            chunkSum += chunk.bytes;
        }

        // Chunk sizes must sum to the package size, otherwise the assembled file is guaranteed wrong — fail early.
        if (chunkSum != bytes) {
            throw new ManifestException(where + "'s total chunk size " + chunkSum
                    + " does not match bytes " + bytes);
        }

        String label = p.optString("label", id);
        return new Package(id, label, bytes, sha, chunks);
    }

    private static Chunk parseChunk(String where, JSONObject c, int pos) throws ManifestException {
        String w = where + ".chunks[" + pos + "]";
        int index = c.optInt("index", -1);
        if (index < 0) throw new ManifestException(w + " 's index is invalid: " + index);

        String url = c.optString("url", "");
        if (url.isEmpty()) throw new ManifestException(w + " is missing url");

        long bytes = c.optLong("bytes", -1);
        if (bytes <= 0) throw new ManifestException(w + " 's bytes is invalid: " + bytes);

        // Chunk sha256 is optional: when missing, per-chunk verification is skipped, but the whole package is still verified.
        String sha = c.optString("sha256", "").toLowerCase(java.util.Locale.US);
        if (!sha.isEmpty() && !isHex64(sha)) {
            throw new ManifestException(w + " 's sha256 is not 64-bit hex");
        }

        return new Chunk(index, url, bytes, sha);
    }

    private static boolean isHex64(String s) {
        if (s == null || s.length() != 64) return false;
        for (int i = 0; i < 64; i++) {
            char ch = s.charAt(i);
            boolean hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
            if (!hex) return false;
        }
        return true;
    }

    public static final class ManifestException extends Exception {
        public ManifestException(String message) {
            super(message);
        }
    }
}
