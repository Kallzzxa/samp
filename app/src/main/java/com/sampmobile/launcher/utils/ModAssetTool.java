package com.sampmobile.launcher.utils;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;

import androidx.annotation.Nullable;

import java.io.BufferedOutputStream;
import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Built-in zero-pollution GTA IMG + TXD Tool for SAMP-Mobile.
 *
 * - Parses SAMP/peds.ide & SAMP/vehicles.ide for target Skin / Vehicle ID lookup.
 * - Decodes PC RenderWare .txd archives (DXT1/DXT3/DXT5, BGRA8888, RGB565/1555/4444, PAL8/PAL4)
 *   as well as loose .png/.jpg/.webp images.
 * - Compiles textures into WarDrum TextureDatabaseRuntime (.txt + .dxt.dat/.toc/.tmb with
 *   uncompressed RGBA8888 gl_type=0x1401 + full mipmap chain + 8x8 thumbnails) under
 *   files/modloader/<packName>/texdb/<dbName>/.
 * - Places loose .dff/.ifp/.col/.ipl models into files/modloader/<packName>/ where native
 *   ModLoader auto-packs them into the virtual VER2 archive files/modloader/models.img at startup,
 *   never touching or duplicating base gta3.img or base texdb files.
 */
public final class ModAssetTool {

    private static final int MAX_TEX_DIM = 1024;
    private static final int MIN_TEX_DIM = 16;
    private static final int GL_UNSIGNED_BYTE_RGBA = 0x1401;
    private static final int MAX_IMG_BASE_LEN = 19; // + ".dff" = 23 <= VER2 char[24]
    private static final int MAX_ZIP_ENTRIES = 2048;
    private static final long MAX_TOTAL_IMPORT_BYTES = 256L * 1024L * 1024L;

    private static final Set<String> BUILTIN_TEXDBS = new HashSet<>(Arrays.asList(
            "gta3", "gta_int", "samp", "samp_extra", "menu",
            "mobile", "cutscene", "txd", "player", "playerhi"
    ));

    private static final Set<String> MODEL_EXTS = new HashSet<>(Arrays.asList(
            ".dff", ".ifp", ".col", ".ipl"
    ));

    private static final Set<String> IMAGE_EXTS = new HashSet<>(Arrays.asList(
            ".png", ".jpg", ".jpeg", ".webp", ".bmp"
    ));

    private ModAssetTool() {}

    public static final class IdeEntry {
        public final int id;
        public final String modelName;
        public final String txdName;
        public final String extra;

        public IdeEntry(int id, String modelName, String txdName, String extra) {
            this.id = id;
            this.modelName = modelName;
            this.txdName = txdName;
            this.extra = extra;
        }

        public String displayLabel() {
            if (extra != null && !extra.isEmpty() && !extra.equalsIgnoreCase(modelName)) {
                return "ID " + id + " — " + modelName + " (" + extra + ")";
            }
            return "ID " + id + " — " + modelName;
        }
    }

    public static final class DecodedTexture {
        public final String name;
        public final int width;
        public final int height;
        public final byte[] rgba; // width * height * 4 bytes (R, G, B, A)
        public final boolean hasAlpha;

        public DecodedTexture(String name, int width, int height, byte[] rgba, boolean hasAlpha) {
            this.name = name;
            this.width = width;
            this.height = height;
            this.rgba = rgba;
            this.hasAlpha = hasAlpha;
        }
    }

    public static final class ImportResult {
        public String packName;
        public int modelCount;
        public int compiledTextureCount;
        public int passthroughFileCount;
        public String texDbName;
        public String renamedPrimaryModel;
        public final List<String> warnings = new ArrayList<>();
    }

    public interface ProgressCallback {
        void onProgress(String message);
    }

    // ══════════ IDE Catalog Parser (SAMP/peds.ide & SAMP/vehicles.ide) ══════════

    public static List<IdeEntry> loadPedSkins(Context ctx) {
        File gameDir = ModManager.getGameFilesDir(ctx);
        if (gameDir == null) return Collections.emptyList();
        File sampIde = new File(gameDir, "SAMP/peds.ide");
        File fallbackIde = new File(gameDir, "data/peds.ide");
        return parseIdeSection(sampIde.isFile() ? sampIde : fallbackIde, "peds");
    }

    public static List<IdeEntry> loadVehicles(Context ctx) {
        File gameDir = ModManager.getGameFilesDir(ctx);
        if (gameDir == null) return Collections.emptyList();
        File sampIde = new File(gameDir, "SAMP/vehicles.ide");
        File fallbackIde = new File(gameDir, "data/vehicles.ide");
        return parseIdeSection(sampIde.isFile() ? sampIde : fallbackIde, "cars");
    }

    static List<IdeEntry> parseIdeSection(File ideFile, String targetSection) {
        List<IdeEntry> out = new ArrayList<>();
        if (ideFile == null || !ideFile.isFile()) return out;
        boolean inSection = false;
        try (BufferedReader br = new BufferedReader(new FileReader(ideFile))) {
            String line;
            while ((line = br.readLine()) != null) {
                String trimmed = line.trim();
                if (trimmed.isEmpty() || trimmed.startsWith("#") || trimmed.startsWith("//")) {
                    continue;
                }
                if (!inSection) {
                    if (trimmed.equalsIgnoreCase(targetSection)) {
                        inSection = true;
                    }
                    continue;
                }
                if (trimmed.equalsIgnoreCase("end")) {
                    break;
                }
                String[] parts = trimmed.split(",");
                if (parts.length < 3) continue;
                try {
                    int id = Integer.parseInt(parts[0].trim());
                    String model = parts[1].trim().toLowerCase(Locale.ROOT);
                    String txd = parts[2].trim().toLowerCase(Locale.ROOT);
                    if (model.isEmpty()) continue;
                    String extra = "";
                    if ("cars".equalsIgnoreCase(targetSection) && parts.length >= 6) {
                        extra = parts[5].trim();
                    } else if ("peds".equalsIgnoreCase(targetSection) && parts.length >= 4) {
                        extra = parts[3].trim();
                    }
                    out.add(new IdeEntry(id, model, txd, extra));
                } catch (NumberFormatException ignored) {
                }
            }
        } catch (IOException ignored) {
        }
        Collections.sort(out, Comparator.comparingInt(a -> a.id));
        return out;
    }

    public static String sanitizeModelBaseName(@Nullable String raw) {
        if (raw == null) return "";
        String s = raw.trim().toLowerCase(Locale.ROOT);
        if (s.endsWith(".dff")) s = s.substring(0, s.length() - 4);
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
                sb.append(c);
            }
        }
        if (sb.length() > MAX_IMG_BASE_LEN) {
            return sb.substring(0, MAX_IMG_BASE_LEN);
        }
        return sb.toString();
    }

    public static String sanitizeTexDbName(String raw) {
        StringBuilder sb = new StringBuilder();
        if (raw != null) {
            String lower = raw.toLowerCase(Locale.ROOT);
            for (int i = 0; i < lower.length(); i++) {
                char c = lower.charAt(i);
                if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
                    sb.append(c);
                } else if (c == '-' || c == ' ') {
                    if (sb.length() > 0 && sb.charAt(sb.length() - 1) != '_') {
                        sb.append('_');
                    }
                }
            }
        }
        String res = sb.toString().replaceAll("^_+|_+$", "");
        if (res.length() > 20) res = res.substring(0, 20);
        if (res.isEmpty() || BUILTIN_TEXDBS.contains(res)) {
            res = "ml_" + (res.isEmpty() ? Long.toHexString(System.currentTimeMillis() & 0xFFFFFL) : res);
        }
        return res;
    }

    // ══════════ Smart Pack Importer (ZIP or Multi-File) ══════════

    private static final class RawAsset {
        final String relPath; // forward-slash normalized path inside zip or bare filename
        final String baseName;
        final String lowerExt;
        final byte[] bytes;

        RawAsset(String relPath, byte[] bytes) {
            this.relPath = relPath.replace('\\', '/');
            int slash = this.relPath.lastIndexOf('/');
            this.baseName = (slash >= 0) ? this.relPath.substring(slash + 1) : this.relPath;
            int dot = this.baseName.lastIndexOf('.');
            this.lowerExt = (dot >= 0) ? this.baseName.substring(dot).toLowerCase(Locale.ROOT) : "";
            this.bytes = bytes;
        }
    }

    public static ImportResult importAssetPack(
            Context ctx,
            List<Uri> uris,
            String rawPackName,
            @Nullable String rawTargetModelName,
            @Nullable ProgressCallback progress
    ) throws IOException {
        if (uris == null || uris.isEmpty()) {
            throw new IllegalArgumentException("未选择任何文件");
        }
        ModManager.ensureDirectories(ctx);

        notifyProgress(progress, "正在读取所选文件…");
        List<RawAsset> assets = collectAssetsFromUris(ctx, uris);
        if (assets.isEmpty()) {
            throw new IOException("未在所选文件中找到可导入的资产（.dff / .ifp / .col / .txd / .png）");
        }

        String cleanPack = sanitizePackFolder(rawPackName);
        if (cleanPack.isEmpty()) {
            cleanPack = deriveDefaultPackName(assets);
        }
        File modRoot = ModManager.getModLoaderDir(ctx);
        File packDir = new File(modRoot, cleanPack);
        int suffix = 1;
        while (packDir.exists()) {
            packDir = new File(modRoot, cleanPack + "_" + (suffix++));
        }
        if (!packDir.mkdirs()) {
            throw new IOException("无法创建模组目录: " + packDir.getName());
        }

        ImportResult res = new ImportResult();
        res.packName = packDir.getName();

        try {
            List<RawAsset> modelAssets = new ArrayList<>();
            List<RawAsset> imageAssets = new ArrayList<>();
            List<RawAsset> txdAssets = new ArrayList<>();
            List<RawAsset> prebuiltTexDbAssets = new ArrayList<>();
            List<RawAsset> extraConfigAssets = new ArrayList<>();

            for (RawAsset a : assets) {
                String lowerPath = a.relPath.toLowerCase(Locale.ROOT);
                // Ignore macOS / junk metadata
                if (lowerPath.contains("__macosx/") || a.baseName.startsWith("._") || a.baseName.startsWith(".")) {
                    continue;
                }
                int texdbIdx = findPathSegment(lowerPath, "texdb/");
                if (texdbIdx >= 0) {
                    // Pre-built Android texdb folder inside ZIP -> preserve as-is
                    String sub = a.relPath.substring(texdbIdx);
                    prebuiltTexDbAssets.add(new RawAsset(sub, a.bytes));
                    continue;
                }
                if (MODEL_EXTS.contains(a.lowerExt)) {
                    modelAssets.add(a);
                } else if (IMAGE_EXTS.contains(a.lowerExt)) {
                    imageAssets.add(a);
                } else if (".txd".equals(a.lowerExt)) {
                    txdAssets.add(a);
                } else if (".dat".equals(a.lowerExt) || ".cfg".equals(a.lowerExt) || ".ide".equals(a.lowerExt)) {
                    extraConfigAssets.add(a);
                }
            }

            // 1. Write model files (.dff / .ifp / .col / .ipl) to pack root
            String targetBase = sanitizeModelBaseName(rawTargetModelName);
            RawAsset primaryDff = null;
            if (!targetBase.isEmpty()) {
                for (RawAsset m : modelAssets) {
                    if (".dff".equals(m.lowerExt)) {
                        if (primaryDff == null || m.bytes.length > primaryDff.bytes.length) {
                            primaryDff = m;
                        }
                    }
                }
            }

            Set<String> writtenModels = new HashSet<>();
            for (RawAsset m : modelAssets) {
                String outName;
                if (m == primaryDff && !targetBase.isEmpty()) {
                    outName = targetBase + ".dff";
                    res.renamedPrimaryModel = outName;
                } else {
                    outName = m.baseName.toLowerCase(Locale.ROOT);
                }
                if (outName.length() > 23) {
                    res.warnings.add("跳过过长模型名（>23字符）: " + outName);
                    continue;
                }
                if (!writtenModels.add(outName)) {
                    continue;
                }
                writeBytes(new File(packDir, outName), m.bytes);
                res.modelCount++;
            }

            // 2. Copy pre-built Android texdb/ files if any
            for (RawAsset p : prebuiltTexDbAssets) {
                File out = safeSubFile(packDir, p.relPath);
                if (out != null) {
                    writeBytes(out, p.bytes);
                    res.passthroughFileCount++;
                }
            }

            // 3. Copy optional data/ config files
            for (RawAsset cfg : extraConfigAssets) {
                int dataIdx = findPathSegment(cfg.relPath.toLowerCase(Locale.ROOT), "data/");
                String sub = (dataIdx >= 0) ? cfg.relPath.substring(dataIdx) : ("data/" + cfg.baseName);
                File out = safeSubFile(packDir, sub);
                if (out != null) {
                    writeBytes(out, cfg.bytes);
                    res.passthroughFileCount++;
                }
            }

            // 4. Decode loose images (.png/.jpg/.webp) first (highest quality / Android exports)
            Map<String, DecodedTexture> texMap = new LinkedHashMap<>();
            int imgTotal = imageAssets.size();
            for (int i = 0; i < imgTotal; i++) {
                RawAsset img = imageAssets.get(i);
                notifyProgress(progress, "正在解码图片贴图 (" + (i + 1) + "/" + imgTotal + "): " + img.baseName);
                String texName = stripExtension(img.baseName);
                if (texName.isEmpty()) continue;
                String lowerKey = texName.toLowerCase(Locale.ROOT);
                if (texMap.containsKey(lowerKey)) continue;

                Boolean folderAlphaHint = detectAlphaHintFromPath(img.relPath);
                DecodedTexture dt = decodeImageBytes(texName, img.bytes, folderAlphaHint);
                if (dt != null) {
                    texMap.put(lowerKey, dt);
                } else {
                    res.warnings.add("无法解码图片: " + img.baseName);
                }
            }

            // 5. Decode PC RenderWare .txd archives (fill any textures not already provided by PNGs)
            for (int i = 0; i < txdAssets.size(); i++) {
                RawAsset txd = txdAssets.get(i);
                notifyProgress(progress, "正在解析 PC TXD 贴图库 (" + (i + 1) + "/" + txdAssets.size() + "): " + txd.baseName);
                List<DecodedTexture> extracted = decodeTxdBytes(txd.bytes, res.warnings);
                for (DecodedTexture dt : extracted) {
                    String lowerKey = dt.name.toLowerCase(Locale.ROOT);
                    if (!texMap.containsKey(lowerKey)) {
                        texMap.put(lowerKey, dt);
                    }
                }
            }

            // 6. Compile collected textures into WarDrum TextureDatabaseRuntime (.dxt.* with RGBA8888)
            if (!texMap.isEmpty()) {
                String dbName = sanitizeTexDbName(res.packName);
                File texDbDir = new File(packDir, "texdb/" + dbName);
                notifyProgress(progress, "正在编译 Android 贴图库 texdb/" + dbName + " (" + texMap.size() + " 张)…");
                compileTextureDatabase(texDbDir, dbName, new ArrayList<>(texMap.values()));
                res.compiledTextureCount = texMap.size();
                res.texDbName = dbName;
            }

            if (res.modelCount == 0 && res.compiledTextureCount == 0 && res.passthroughFileCount == 0) {
                throw new IOException("未检测到任何有效的模型 (.dff/.ifp/.col) 或贴图 (.png/.txd)");
            }

            // 7. Register pack in modloader.json at top priority so it overrides base/Remastered packs
            registerPackAtTopPriority(ctx, res.packName);
            return res;
        } catch (IOException | RuntimeException e) {
            ModManager.deleteRecursive(packDir);
            throw e;
        }
    }

    private static void registerPackAtTopPriority(Context ctx, String packName) {
        List<ModManager.ModLoaderEntry> list = ModManager.listModLoaderPacks(ctx);
        int maxPrio = 50;
        ModManager.ModLoaderEntry found = null;
        for (ModManager.ModLoaderEntry e : list) {
            if (e.name.equals(packName)) {
                found = e;
            } else if (e.priority > maxPrio) {
                maxPrio = e.priority;
            }
        }
        if (found != null) {
            found.enabled = true;
            found.priority = maxPrio + 10;
        } else {
            ModManager.ModLoaderEntry e = new ModManager.ModLoaderEntry();
            e.name = packName;
            e.enabled = true;
            e.priority = maxPrio + 10;
            list.add(0, e);
        }
        ModManager.saveModLoaderConfig(ctx, list);
    }

    private static List<RawAsset> collectAssetsFromUris(Context ctx, List<Uri> uris) throws IOException {
        List<RawAsset> out = new ArrayList<>();
        long totalBytes = 0;
        int entryCount = 0;
        for (Uri uri : uris) {
            String displayName = ModManager.queryDisplayName(ctx, uri);
            if (displayName == null || displayName.trim().isEmpty()) {
                displayName = "asset_" + out.size();
            }
            String lowerName = displayName.toLowerCase(Locale.ROOT);
            if (lowerName.endsWith(".zip")) {
                try (InputStream raw = ctx.getContentResolver().openInputStream(uri)) {
                    if (raw == null) throw new IOException("无法读取压缩包: " + displayName);
                    try (ZipInputStream zis = new ZipInputStream(raw)) {
                        ZipEntry entry;
                        while ((entry = zis.getNextEntry()) != null) {
                            if (++entryCount > MAX_ZIP_ENTRIES) {
                                throw new IOException("压缩包内文件数超过限制 (" + MAX_ZIP_ENTRIES + ")");
                            }
                            if (!entry.isDirectory()) {
                                String entryName = entry.getName().replace('\\', '/');
                                if (!entryName.startsWith("/") && !entryName.contains("..")) {
                                    long remaining = MAX_TOTAL_IMPORT_BYTES - totalBytes;
                                    if (remaining <= 0) {
                                        throw new IOException("导入资源总大小超过 256 MB 限制");
                                    }
                                    int cap = (int) Math.min(128L * 1024L * 1024L, remaining);
                                    byte[] data = readAllBytesBounded(zis, cap);
                                    totalBytes += data.length;
                                    out.add(new RawAsset(entryName, data));
                                }
                            }
                            zis.closeEntry();
                        }
                    }
                }
            } else {
                if (++entryCount > MAX_ZIP_ENTRIES) {
                    throw new IOException("导入文件数超过限制 (" + MAX_ZIP_ENTRIES + ")");
                }
                try (InputStream is = ctx.getContentResolver().openInputStream(uri)) {
                    if (is == null) throw new IOException("无法读取文件: " + displayName);
                    long remaining = MAX_TOTAL_IMPORT_BYTES - totalBytes;
                    if (remaining <= 0) {
                        throw new IOException("导入资源总大小超过 256 MB 限制");
                    }
                    int cap = (int) Math.min(128L * 1024L * 1024L, remaining);
                    byte[] data = readAllBytesBounded(is, cap);
                    totalBytes += data.length;
                    out.add(new RawAsset(displayName, data));
                }
            }
        }
        return out;
    }

    // ══════════ Loose Image Decoder (PNG / JPG / WEBP -> PoT RGBA8888) ══════════

    @Nullable
    private static Boolean detectAlphaHintFromPath(String relPath) {
        String lower = relPath.toLowerCase(Locale.ROOT);
        if (lower.contains("without alpha") || lower.contains("without_alpha")
                || lower.contains("no alpha") || lower.contains("no_alpha") || lower.contains("noalpha")) {
            return Boolean.FALSE;
        }
        if (lower.contains("with alpha") || lower.contains("with_alpha")) {
            return Boolean.TRUE;
        }
        return null;
    }

    @Nullable
    private static DecodedTexture decodeImageBytes(String name, byte[] data, @Nullable Boolean forceAlpha) {
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeByteArray(data, 0, data.length, bounds);
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null;

        int sampleSize = 1;
        while ((bounds.outWidth / sampleSize) > (MAX_TEX_DIM * 2)
                || (bounds.outHeight / sampleSize) > (MAX_TEX_DIM * 2)) {
            sampleSize <<= 1;
        }

        BitmapFactory.Options opts = new BitmapFactory.Options();
        opts.inPreferredConfig = Bitmap.Config.ARGB_8888;
        opts.inPremultiplied = false; // Keep raw straight-alpha RGB for GTA:SA shaders
        opts.inSampleSize = sampleSize;
        Bitmap bmp = BitmapFactory.decodeByteArray(data, 0, data.length, opts);
        if (bmp == null) return null;
        try {
            return bitmapToDecodedTexture(name, bmp, forceAlpha);
        } finally {
            bmp.recycle();
        }
    }

    private static DecodedTexture bitmapToDecodedTexture(String name, Bitmap src, @Nullable Boolean forceAlpha) {
        int sw = src.getWidth();
        int sh = src.getHeight();
        if (sw <= 0 || sh <= 0) return null;

        int[] srcArgb = new int[sw * sh];
        src.getPixels(srcArgb, 0, sw, 0, 0, sw, sh);

        int tw = nearestPow2Clamped(sw);
        int th = nearestPow2Clamped(sh);
        int[] argb = (sw == tw && sh == th) ? srcArgb : resizeArgbBilinear(srcArgb, sw, sh, tw, th);

        int pixelCount = tw * th;
        byte[] rgba = new byte[pixelCount * 4];
        boolean detectedAlpha = false;
        for (int i = 0; i < pixelCount; i++) {
            int p = argb[i];
            int a = (p >>> 24) & 0xFF;
            if (a < 250) detectedAlpha = true;
            int off = i << 2;
            rgba[off] = (byte) ((p >> 16) & 0xFF);
            rgba[off + 1] = (byte) ((p >> 8) & 0xFF);
            rgba[off + 2] = (byte) (p & 0xFF);
            rgba[off + 3] = (byte) ((forceAlpha != null && !forceAlpha) ? 0xFF : a);
        }
        boolean hasAlpha = (forceAlpha != null) ? forceAlpha : detectedAlpha;
        return new DecodedTexture(name, tw, th, rgba, hasAlpha);
    }

    // Pure pixel-array bilinear resize: safely handles straight-alpha (non-premultiplied) bitmaps
    // without touching Android Canvas (which throws RuntimeException on non-premultiplied bitmaps).
    private static int[] resizeArgbBilinear(int[] src, int sw, int sh, int dw, int dh) {
        int[] dst = new int[dw * dh];
        float scaleX = (float) sw / (float) dw;
        float scaleY = (float) sh / (float) dh;
        for (int y = 0; y < dh; y++) {
            float fy = (y + 0.5f) * scaleY - 0.5f;
            int y0 = Math.max(0, Math.min(sh - 1, (int) Math.floor(fy)));
            int y1 = Math.min(sh - 1, y0 + 1);
            float wy = Math.max(0f, Math.min(1f, fy - y0));
            float iwy = 1f - wy;
            int row0 = y0 * sw;
            int row1 = y1 * sw;
            int dstRow = y * dw;
            for (int x = 0; x < dw; x++) {
                float fx = (x + 0.5f) * scaleX - 0.5f;
                int x0 = Math.max(0, Math.min(sw - 1, (int) Math.floor(fx)));
                int x1 = Math.min(sw - 1, x0 + 1);
                float wx = Math.max(0f, Math.min(1f, fx - x0));
                float iwx = 1f - wx;

                int p00 = src[row0 + x0];
                int p10 = src[row0 + x1];
                int p01 = src[row1 + x0];
                int p11 = src[row1 + x1];

                float w00 = iwx * iwy;
                float w10 = wx * iwy;
                float w01 = iwx * wy;
                float w11 = wx * wy;

                int a = Math.round(((p00 >>> 24) & 0xFF) * w00 + ((p10 >>> 24) & 0xFF) * w10
                        + ((p01 >>> 24) & 0xFF) * w01 + ((p11 >>> 24) & 0xFF) * w11);
                int r = Math.round(((p00 >>> 16) & 0xFF) * w00 + ((p10 >>> 16) & 0xFF) * w10
                        + ((p01 >>> 16) & 0xFF) * w01 + ((p11 >>> 16) & 0xFF) * w11);
                int g = Math.round(((p00 >>> 8) & 0xFF) * w00 + ((p10 >>> 8) & 0xFF) * w10
                        + ((p01 >>> 8) & 0xFF) * w01 + ((p11 >>> 8) & 0xFF) * w11);
                int b = Math.round((p00 & 0xFF) * w00 + (p10 & 0xFF) * w10
                        + (p01 & 0xFF) * w01 + (p11 & 0xFF) * w11);

                dst[dstRow + x] = ((a & 0xFF) << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
            }
        }
        return dst;
    }

    private static DecodedTexture normalizeRgbaToPow2(String name, int w, int h, byte[] rgba, boolean hasAlpha) {
        int tw = nearestPow2Clamped(w);
        int th = nearestPow2Clamped(h);
        if (w == tw && h == th) {
            return new DecodedTexture(name, w, h, rgba, hasAlpha);
        }
        int[] srcArgb = new int[w * h];
        for (int i = 0; i < w * h; i++) {
            int o = i << 2;
            int r = rgba[o] & 0xFF;
            int g = rgba[o + 1] & 0xFF;
            int b = rgba[o + 2] & 0xFF;
            int a = rgba[o + 3] & 0xFF;
            srcArgb[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
        int[] resized = resizeArgbBilinear(srcArgb, w, h, tw, th);
        byte[] outRgba = new byte[tw * th * 4];
        for (int i = 0; i < resized.length; i++) {
            int p = resized[i];
            int o = i << 2;
            outRgba[o] = (byte) ((p >>> 16) & 0xFF);
            outRgba[o + 1] = (byte) ((p >>> 8) & 0xFF);
            outRgba[o + 2] = (byte) (p & 0xFF);
            outRgba[o + 3] = (byte) (hasAlpha ? ((p >>> 24) & 0xFF) : 0xFF);
        }
        return new DecodedTexture(name, tw, th, outRgba, hasAlpha);
    }

    private static int nearestPow2Clamped(int v) {
        if (v <= MIN_TEX_DIM) return MIN_TEX_DIM;
        if (v >= MAX_TEX_DIM) return MAX_TEX_DIM;
        int p = MIN_TEX_DIM;
        while (p < v && p < MAX_TEX_DIM) {
            p <<= 1;
        }
        if (p == v) return p;
        int prev = p >> 1;
        return (v - prev < p - v) ? prev : p;
    }

    // ══════════ Pure-Java PC RenderWare .txd Decoder ══════════

    private static final int RW_STRUCT = 0x0001;
    private static final int RW_TEXTURENATIVE = 0x0015;
    private static final int RW_TEXDICTIONARY = 0x0016;

    private static final int FOURCC_DXT1 = 0x31545844; // "DXT1"
    private static final int FOURCC_DXT3 = 0x33545844; // "DXT3"
    private static final int FOURCC_DXT5 = 0x35545844; // "DXT5"

    public static List<DecodedTexture> decodeTxdBytes(byte[] data, @Nullable List<String> warnings) {
        List<DecodedTexture> result = new ArrayList<>();
        if (data == null || data.length < 24) return result;
        int rootType = readLE32(data, 0);
        long rootSize = readLE32(data, 4) & 0xFFFFFFFFL;
        if (rootType != RW_TEXDICTIONARY) {
            if (warnings != null) warnings.add("TXD 文件头无效 (非 RenderWare TEXDICTIONARY)");
            return result;
        }
        int end = (int) Math.min((long) data.length, 12L + rootSize);
        int off = 12;
        while (off + 12 <= end) {
            int type = readLE32(data, off);
            long size = readLE32(data, off + 4) & 0xFFFFFFFFL;
            long childStart = off + 12L;
            long childEndLong = Math.min((long) end, childStart + size);
            if (childEndLong < childStart || childEndLong > end) break;
            int childEnd = (int) childEndLong;
            if (type == RW_TEXTURENATIVE) {
                DecodedTexture tex = parseTextureNative(data, (int) childStart, childEnd, warnings);
                if (tex != null) {
                    result.add(tex);
                }
            }
            if (childEnd <= off) break;
            off = childEnd;
        }
        return result;
    }

    @Nullable
    private static DecodedTexture parseTextureNative(byte[] data, int start, int end, @Nullable List<String> warnings) {
        int off = start;
        while (off + 12 <= end) {
            int type = readLE32(data, off);
            long size = readLE32(data, off + 4) & 0xFFFFFFFFL;
            long payloadStart = off + 12L;
            long payloadEndLong = Math.min((long) end, payloadStart + size);
            if (payloadEndLong < payloadStart || payloadEndLong > end) break;
            int payloadEnd = (int) payloadEndLong;
            if (type == RW_STRUCT && (payloadEnd - (int) payloadStart) >= 92) {
                return decodePcRasterStruct(data, (int) payloadStart, payloadEnd, warnings);
            }
            if (payloadEnd <= off) break;
            off = payloadEnd;
        }
        return null;
    }

    @Nullable
    private static DecodedTexture decodePcRasterStruct(byte[] data, int s, int end, @Nullable List<String> warnings) {
        int platformId = readLE32(data, s);
        // 8 = D3D8 (GTA3/VC/SA), 9 = D3D9 (GTA:SA PC)
        if (platformId != 8 && platformId != 9) {
            if (warnings != null) warnings.add("跳过非 PC 平台 TXD 贴图 (platform=" + platformId + ")");
            return null;
        }
        String name = readNullTermAscii(data, s + 8, 32).trim();
        if (name.isEmpty()) return null;

        int rasterFormat = readLE32(data, s + 72);
        int d3dFmtOrAlpha = readLE32(data, s + 76);
        int width = readLE16(data, s + 80);
        int height = readLE16(data, s + 82);
        int depth = data[s + 84] & 0xFF;
        int flagsOrComp = data[s + 87] & 0xFF;

        if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
            if (warnings != null) warnings.add("跳过异常尺寸贴图: " + name + " (" + width + "x" + height + ")");
            return null;
        }

        int cur = s + 88;
        int formatMask = rasterFormat & 0x0F00;
        boolean isPal8 = (rasterFormat & 0x2000) != 0;
        boolean isPal4 = (rasterFormat & 0x4000) != 0;

        byte[] palette = null;
        if (isPal8) {
            if (cur + 1024 > end) return null;
            palette = Arrays.copyOfRange(data, cur, cur + 1024);
            cur += 1024;
        } else if (isPal4) {
            if (cur + 64 > end) return null;
            palette = Arrays.copyOfRange(data, cur, cur + 64);
            cur += 64;
        }

        if (cur + 4 > end) return null;
        int mip0Size = readLE32(data, cur);
        cur += 4;
        if (mip0Size <= 0 || cur + mip0Size > end) {
            mip0Size = Math.max(0, end - cur);
        }
        if (mip0Size <= 0) return null;

        boolean headerHasAlpha = (platformId == 9) ? ((flagsOrComp & 0x01) != 0) : (d3dFmtOrAlpha != 0);
        byte[] rgba;

        int dxtMode = 0;
        if (platformId == 9) {
            if (d3dFmtOrAlpha == FOURCC_DXT1) dxtMode = 1;
            else if (d3dFmtOrAlpha == FOURCC_DXT3) dxtMode = 3;
            else if (d3dFmtOrAlpha == FOURCC_DXT5) dxtMode = 5;
        } else {
            if (flagsOrComp == 1) dxtMode = 1;
            else if (flagsOrComp == 3) dxtMode = 3;
            else if (flagsOrComp == 5) dxtMode = 5;
        }

        if (dxtMode == 1) {
            rgba = decodeDxt1(data, cur, mip0Size, width, height, headerHasAlpha);
        } else if (dxtMode == 3) {
            rgba = decodeDxt3(data, cur, mip0Size, width, height);
            headerHasAlpha = true;
        } else if (dxtMode == 5) {
            rgba = decodeDxt5(data, cur, mip0Size, width, height);
            headerHasAlpha = true;
        } else if (palette != null && isPal8) {
            rgba = decodePal8(data, cur, mip0Size, palette, width, height);
        } else if (palette != null && isPal4) {
            rgba = decodePal4(data, cur, mip0Size, palette, width, height);
        } else if (depth == 32 || formatMask == 0x0500 || formatMask == 0x0600) {
            boolean hasAlphaChan = (formatMask == 0x0500) || (platformId == 9 && d3dFmtOrAlpha == 21) || headerHasAlpha;
            rgba = decodeBgra32(data, cur, mip0Size, width, height, hasAlphaChan);
        } else if (depth == 16) {
            rgba = decode16Bit(data, cur, mip0Size, width, height, formatMask);
        } else {
            if (warnings != null) {
                warnings.add("不支持的 TXD 贴图格式: " + name + " (depth=" + depth + ", fmt=0x"
                        + Integer.toHexString(rasterFormat) + ", d3d=0x" + Integer.toHexString(d3dFmtOrAlpha) + ")");
            }
            return null;
        }

        boolean pixelAlpha = false;
        for (int i = 3; i < rgba.length; i += 4) {
            if ((rgba[i] & 0xFF) < 250) {
                pixelAlpha = true;
                break;
            }
        }
        return normalizeRgbaToPow2(name, width, height, rgba, pixelAlpha || (headerHasAlpha && dxtMode != 1));
    }

    private static byte[] decodeDxt1(byte[] src, int off, int len, int w, int h, boolean allowPunchthroughAlpha) {
        byte[] out = new byte[w * h * 4];
        int bw = Math.max(1, (w + 3) >> 2);
        int bh = Math.max(1, (h + 3) >> 2);
        int[] c = new int[4];
        int p = off;
        int limit = off + len;
        for (int by = 0; by < bh; by++) {
            for (int bx = 0; bx < bw; bx++) {
                if (p + 8 > limit) return out;
                int c0 = readLE16(src, p);
                int c1 = readLE16(src, p + 2);
                int idx = readLE32(src, p + 4);
                p += 8;

                c[0] = rgb565ToRgba(c0);
                c[1] = rgb565ToRgba(c1);
                if (c0 > c1) {
                    c[2] = lerpRgba(c[0], c[1], 2, 1, 3);
                    c[3] = lerpRgba(c[0], c[1], 1, 2, 3);
                } else {
                    c[2] = lerpRgba(c[0], c[1], 1, 1, 2);
                    c[3] = allowPunchthroughAlpha ? 0x00000000 : 0x000000FF;
                }

                for (int py = 0; py < 4; py++) {
                    int y = (by << 2) + py;
                    if (y >= h) continue;
                    for (int px = 0; px < 4; px++) {
                        int x = (bx << 2) + px;
                        int ci = (idx >>> (((py << 2) + px) << 1)) & 0x3;
                        if (x >= w) continue;
                        int col = c[ci];
                        int dst = (y * w + x) << 2;
                        out[dst] = (byte) ((col >>> 24) & 0xFF);
                        out[dst + 1] = (byte) ((col >>> 16) & 0xFF);
                        out[dst + 2] = (byte) ((col >>> 8) & 0xFF);
                        out[dst + 3] = (byte) (col & 0xFF);
                    }
                }
            }
        }
        return out;
    }

    private static byte[] decodeDxt3(byte[] src, int off, int len, int w, int h) {
        byte[] out = new byte[w * h * 4];
        int bw = Math.max(1, (w + 3) >> 2);
        int bh = Math.max(1, (h + 3) >> 2);
        int[] c = new int[4];
        int p = off;
        int limit = off + len;
        for (int by = 0; by < bh; by++) {
            for (int bx = 0; bx < bw; bx++) {
                if (p + 16 > limit) return out;
                long alphaBits = readLE64(src, p);
                int c0 = readLE16(src, p + 8);
                int c1 = readLE16(src, p + 10);
                int idx = readLE32(src, p + 12);
                p += 16;

                c[0] = rgb565ToRgba(c0);
                c[1] = rgb565ToRgba(c1);
                c[2] = lerpRgba(c[0], c[1], 2, 1, 3);
                c[3] = lerpRgba(c[0], c[1], 1, 2, 3);

                for (int py = 0; py < 4; py++) {
                    int y = (by << 2) + py;
                    if (y >= h) continue;
                    for (int px = 0; px < 4; px++) {
                        int x = (bx << 2) + px;
                        if (x >= w) continue;
                        int bitIdx = (py << 2) + px;
                        int ci = (idx >>> (bitIdx << 1)) & 0x3;
                        int a4 = (int) ((alphaBits >>> (bitIdx << 2)) & 0xF);
                        int a8 = (a4 << 4) | a4;
                        int col = c[ci];
                        int dst = (y * w + x) << 2;
                        out[dst] = (byte) ((col >>> 24) & 0xFF);
                        out[dst + 1] = (byte) ((col >>> 16) & 0xFF);
                        out[dst + 2] = (byte) ((col >>> 8) & 0xFF);
                        out[dst + 3] = (byte) a8;
                    }
                }
            }
        }
        return out;
    }

    private static byte[] decodeDxt5(byte[] src, int off, int len, int w, int h) {
        byte[] out = new byte[w * h * 4];
        int bw = Math.max(1, (w + 3) >> 2);
        int bh = Math.max(1, (h + 3) >> 2);
        int[] c = new int[4];
        int[] aTable = new int[8];
        int p = off;
        int limit = off + len;
        for (int by = 0; by < bh; by++) {
            for (int bx = 0; bx < bw; bx++) {
                if (p + 16 > limit) return out;
                int a0 = src[p] & 0xFF;
                int a1 = src[p + 1] & 0xFF;
                long aIdx = (src[p + 2] & 0xFFL)
                        | ((src[p + 3] & 0xFFL) << 8)
                        | ((src[p + 4] & 0xFFL) << 16)
                        | ((src[p + 5] & 0xFFL) << 24)
                        | ((src[p + 6] & 0xFFL) << 32)
                        | ((src[p + 7] & 0xFFL) << 40);
                aTable[0] = a0;
                aTable[1] = a1;
                if (a0 > a1) {
                    aTable[2] = (6 * a0 + 1 * a1) / 7;
                    aTable[3] = (5 * a0 + 2 * a1) / 7;
                    aTable[4] = (4 * a0 + 3 * a1) / 7;
                    aTable[5] = (3 * a0 + 4 * a1) / 7;
                    aTable[6] = (2 * a0 + 5 * a1) / 7;
                    aTable[7] = (1 * a0 + 6 * a1) / 7;
                } else {
                    aTable[2] = (4 * a0 + 1 * a1) / 5;
                    aTable[3] = (3 * a0 + 2 * a1) / 5;
                    aTable[4] = (2 * a0 + 3 * a1) / 5;
                    aTable[5] = (1 * a0 + 4 * a1) / 5;
                    aTable[6] = 0;
                    aTable[7] = 255;
                }

                int c0 = readLE16(src, p + 8);
                int c1 = readLE16(src, p + 10);
                int idx = readLE32(src, p + 12);
                p += 16;

                c[0] = rgb565ToRgba(c0);
                c[1] = rgb565ToRgba(c1);
                c[2] = lerpRgba(c[0], c[1], 2, 1, 3);
                c[3] = lerpRgba(c[0], c[1], 1, 2, 3);

                for (int py = 0; py < 4; py++) {
                    int y = (by << 2) + py;
                    if (y >= h) continue;
                    for (int px = 0; px < 4; px++) {
                        int x = (bx << 2) + px;
                        if (x >= w) continue;
                        int bitIdx = (py << 2) + px;
                        int ci = (idx >>> (bitIdx << 1)) & 0x3;
                        int ai = (int) ((aIdx >>> (bitIdx * 3)) & 0x7);
                        int col = c[ci];
                        int dst = (y * w + x) << 2;
                        out[dst] = (byte) ((col >>> 24) & 0xFF);
                        out[dst + 1] = (byte) ((col >>> 16) & 0xFF);
                        out[dst + 2] = (byte) ((col >>> 8) & 0xFF);
                        out[dst + 3] = (byte) aTable[ai];
                    }
                }
            }
        }
        return out;
    }

    private static byte[] decodeBgra32(byte[] src, int off, int len, int w, int h, boolean hasAlpha) {
        byte[] out = new byte[w * h * 4];
        int pixels = Math.min(w * h, len / 4);
        for (int i = 0; i < pixels; i++) {
            int s = off + (i << 2);
            int d = i << 2;
            out[d] = src[s + 2];     // R
            out[d + 1] = src[s + 1]; // G
            out[d + 2] = src[s];     // B
            out[d + 3] = hasAlpha ? src[s + 3] : (byte) 0xFF;
        }
        return out;
    }

    private static byte[] decode16Bit(byte[] src, int off, int len, int w, int h, int formatMask) {
        byte[] out = new byte[w * h * 4];
        int pixels = Math.min(w * h, len / 2);
        for (int i = 0; i < pixels; i++) {
            int v = readLE16(src, off + (i << 1));
            int r, g, b, a;
            if (formatMask == 0x0100) { // C1555
                a = ((v >>> 15) & 1) != 0 ? 255 : 0;
                r = (((v >>> 10) & 0x1F) * 255) / 31;
                g = (((v >>> 5) & 0x1F) * 255) / 31;
                b = ((v & 0x1F) * 255) / 31;
            } else if (formatMask == 0x0300) { // C4444
                a = ((v >>> 12) & 0xF) * 17;
                r = ((v >>> 8) & 0xF) * 17;
                g = ((v >>> 4) & 0xF) * 17;
                b = (v & 0xF) * 17;
            } else { // C565
                a = 255;
                r = (((v >>> 11) & 0x1F) * 255) / 31;
                g = (((v >>> 5) & 0x3F) * 255) / 63;
                b = ((v & 0x1F) * 255) / 31;
            }
            int d = i << 2;
            out[d] = (byte) r;
            out[d + 1] = (byte) g;
            out[d + 2] = (byte) b;
            out[d + 3] = (byte) a;
        }
        return out;
    }

    private static byte[] decodePal8(byte[] src, int off, int len, byte[] pal, int w, int h) {
        byte[] out = new byte[w * h * 4];
        int pixels = Math.min(w * h, len);
        for (int i = 0; i < pixels; i++) {
            int idx = (src[off + i] & 0xFF) << 2;
            int d = i << 2;
            out[d] = pal[idx];
            out[d + 1] = pal[idx + 1];
            out[d + 2] = pal[idx + 2];
            out[d + 3] = pal[idx + 3];
        }
        return out;
    }

    private static byte[] decodePal4(byte[] src, int off, int len, byte[] pal, int w, int h) {
        byte[] out = new byte[w * h * 4];
        int pixels = Math.min(w * h, len * 2);
        for (int i = 0; i < pixels; i++) {
            int b = src[off + (i >> 1)] & 0xFF;
            int idx = ((i & 1) == 0 ? (b & 0x0F) : ((b >>> 4) & 0x0F)) << 2;
            int d = i << 2;
            out[d] = pal[idx];
            out[d + 1] = pal[idx + 1];
            out[d + 2] = pal[idx + 2];
            out[d + 3] = pal[idx + 3];
        }
        return out;
    }

    private static int rgb565ToRgba(int v) {
        int r = ((v >>> 11) & 0x1F);
        int g = ((v >>> 5) & 0x3F);
        int b = (v & 0x1F);
        r = (r << 3) | (r >>> 2);
        g = (g << 2) | (g >>> 4);
        b = (b << 3) | (b >>> 2);
        return (r << 24) | (g << 16) | (b << 8) | 0xFF;
    }

    private static int lerpRgba(int c0, int c1, int w0, int w1, int div) {
        int r = (((c0 >>> 24) & 0xFF) * w0 + ((c1 >>> 24) & 0xFF) * w1) / div;
        int g = (((c0 >>> 16) & 0xFF) * w0 + ((c1 >>> 16) & 0xFF) * w1) / div;
        int b = (((c0 >>> 8) & 0xFF) * w0 + ((c1 >>> 8) & 0xFF) * w1) / div;
        return (r << 24) | (g << 16) | (b << 8) | 0xFF;
    }

    // ══════════ WarDrum TextureDatabaseRuntime Compiler (.dxt.* w/ RGBA8888) ══════════

    public static void compileTextureDatabase(File outDir, String dbName, List<DecodedTexture> textures) throws IOException {
        if (!outDir.exists() && !outDir.mkdirs()) {
            throw new IOException("无法创建贴图库目录: " + outDir.getAbsolutePath());
        }

        File txtFile = new File(outDir, dbName + ".txt");
        File datFile = new File(outDir, dbName + ".dxt.dat");
        File tocFile = new File(outDir, dbName + ".dxt.toc");
        File tmbFile = new File(outDir, dbName + ".dxt.tmb");

        int count = textures.size();
        int[] offsets = new int[count];
        int datPos = 0;

        StringBuilder txt = new StringBuilder();
        txt.append("cat=0 name=Default onfoot=0 slow=0 fast=0 defaultstream=0\n");

        try (BufferedOutputStream datOut = new BufferedOutputStream(new FileOutputStream(datFile), 65536);
             BufferedOutputStream tmbOut = new BufferedOutputStream(new FileOutputStream(tmbFile), 16384)) {

            byte[] hdr12 = new byte[12];
            byte[] zero4 = new byte[4];

            for (int i = 0; i < count; i++) {
                DecodedTexture tex = textures.get(i);
                offsets[i] = datPos;

                // Generate mipmap chain from w*h down to 1*1 (2x2 box filter)
                List<byte[]> mips = new ArrayList<>();
                byte[] curBytes = tex.rgba;
                int mw = tex.width;
                int mh = tex.height;
                int totalMipBytes = 0;
                byte[] thumb8x8 = null;

                while (true) {
                    mips.add(curBytes);
                    totalMipBytes += curBytes.length;
                    if (mw == 8 && mh == 8) {
                        thumb8x8 = curBytes;
                    }
                    if (mw == 1 && mh == 1) break;
                    int nw = Math.max(1, mw >> 1);
                    int nh = Math.max(1, mh >> 1);
                    curBytes = downsampleBox2x2(curBytes, mw, mh, nw, nh);
                    mw = nw;
                    mh = nh;
                }
                if (thumb8x8 == null) {
                    thumb8x8 = buildThumb8x8(tex.rgba, tex.width, tex.height);
                }

                // 12-byte WarDrum header: <HHHHI (streamId, gl_type=0x1401, w, h|0x8000, 4+mipBytes)
                putLE16(hdr12, 0, i);
                putLE16(hdr12, 2, GL_UNSIGNED_BYTE_RGBA);
                putLE16(hdr12, 4, tex.width);
                putLE16(hdr12, 6, tex.height | 0x8000);
                putLE32(hdr12, 8, 4 + totalMipBytes);

                datOut.write(hdr12);
                datOut.write(zero4);
                for (byte[] mip : mips) {
                    datOut.write(mip);
                }
                datPos += 12 + 4 + totalMipBytes;

                // 8x8 thumbnail entry (no mipmap bit on height)
                putLE16(hdr12, 0, i);
                putLE16(hdr12, 2, GL_UNSIGNED_BYTE_RGBA);
                putLE16(hdr12, 4, 8);
                putLE16(hdr12, 6, 8);
                putLE32(hdr12, 8, 4 + 256);
                tmbOut.write(hdr12);
                tmbOut.write(zero4);
                tmbOut.write(thumb8x8, 0, 256);

                String safeTexName = tex.name.replace('"', '_').replace('\r', '_').replace('\n', '_').trim();
                if (safeTexName.isEmpty()) safeTexName = "tex_" + i;
                CRC32 crc = new CRC32();
                crc.update(safeTexName.getBytes(StandardCharsets.UTF_8));
                String hashHex = String.format(Locale.ROOT, "%08x", crc.getValue() & 0xFFFFFFFFL);
                txt.append('"').append(safeTexName).append('"')
                        .append(" width=").append(tex.width)
                        .append(" height=").append(tex.height)
                        .append(" img=").append(hashHex)
                        .append(" alphamode=").append(tex.hasAlpha ? 2 : 0)
                        .append('\n');
            }
            datOut.flush();
            tmbOut.flush();
        }

        // Write .dxt.toc: uint32[N+1] = [totalDatSize, off_0, ..., off_{N-1}]
        try (BufferedOutputStream tocOut = new BufferedOutputStream(new FileOutputStream(tocFile))) {
            byte[] u32 = new byte[4];
            putLE32(u32, 0, datPos);
            tocOut.write(u32);
            for (int off : offsets) {
                putLE32(u32, 0, off);
                tocOut.write(u32);
            }
            tocOut.flush();
        }

        // Write .txt manifest
        try (BufferedWriter bw = new BufferedWriter(new FileWriter(txtFile))) {
            bw.write(txt.toString());
        }
    }

    private static byte[] downsampleBox2x2(byte[] src, int sw, int sh, int dw, int dh) {
        byte[] dst = new byte[dw * dh * 4];
        for (int y = 0; y < dh; y++) {
            int sy0 = Math.min(sh - 1, y << 1);
            int sy1 = Math.min(sh - 1, sy0 + 1);
            for (int x = 0; x < dw; x++) {
                int sx0 = Math.min(sw - 1, x << 1);
                int sx1 = Math.min(sw - 1, sx0 + 1);
                int i00 = (sy0 * sw + sx0) << 2;
                int i10 = (sy0 * sw + sx1) << 2;
                int i01 = (sy1 * sw + sx0) << 2;
                int i11 = (sy1 * sw + sx1) << 2;
                int d = (y * dw + x) << 2;
                dst[d] = (byte) (((src[i00] & 0xFF) + (src[i10] & 0xFF) + (src[i01] & 0xFF) + (src[i11] & 0xFF) + 2) >> 2);
                dst[d + 1] = (byte) (((src[i00 + 1] & 0xFF) + (src[i10 + 1] & 0xFF) + (src[i01 + 1] & 0xFF) + (src[i11 + 1] & 0xFF) + 2) >> 2);
                dst[d + 2] = (byte) (((src[i00 + 2] & 0xFF) + (src[i10 + 2] & 0xFF) + (src[i01 + 2] & 0xFF) + (src[i11 + 2] & 0xFF) + 2) >> 2);
                dst[d + 3] = (byte) (((src[i00 + 3] & 0xFF) + (src[i10 + 3] & 0xFF) + (src[i01 + 3] & 0xFF) + (src[i11 + 3] & 0xFF) + 2) >> 2);
            }
        }
        return dst;
    }

    private static byte[] buildThumb8x8(byte[] rgba, int w, int h) {
        byte[] dst = new byte[8 * 8 * 4];
        for (int y = 0; y < 8; y++) {
            int sy = Math.min(h - 1, (y * h) / 8);
            for (int x = 0; x < 8; x++) {
                int sx = Math.min(w - 1, (x * w) / 8);
                int s = (sy * w + sx) << 2;
                int d = (y * 8 + x) << 2;
                dst[d] = rgba[s];
                dst[d + 1] = rgba[s + 1];
                dst[d + 2] = rgba[s + 2];
                dst[d + 3] = rgba[s + 3];
            }
        }
        return dst;
    }

    // ══════════ Helpers ══════════

    private static void notifyProgress(@Nullable ProgressCallback cb, String msg) {
        if (cb != null) cb.onProgress(msg);
    }

    private static String sanitizePackFolder(@Nullable String raw) {
        if (raw == null) return "";
        String cleaned = raw.trim().replaceAll("[\\\\/:*?\"<>|]", "_");
        while (cleaned.startsWith(".") || cleaned.startsWith(" ")) {
            cleaned = cleaned.substring(1);
        }
        cleaned = cleaned.trim();
        if (".".equals(cleaned) || "..".equals(cleaned)) return "";
        return cleaned;
    }

    private static String deriveDefaultPackName(List<RawAsset> assets) {
        for (RawAsset a : assets) {
            if (".dff".equals(a.lowerExt) || ".txd".equals(a.lowerExt)) {
                String base = sanitizePackFolder(stripExtension(a.baseName));
                if (!base.isEmpty()) return base;
            }
        }
        return "CustomMod_" + (System.currentTimeMillis() % 100000);
    }

    private static String stripExtension(String name) {
        int dot = name.lastIndexOf('.');
        return (dot > 0) ? name.substring(0, dot) : name;
    }

    private static int findPathSegment(String lowerPath, String segmentWithSlash) {
        if (lowerPath.startsWith(segmentWithSlash)) return 0;
        int idx = lowerPath.indexOf("/" + segmentWithSlash);
        return (idx >= 0) ? (idx + 1) : -1;
    }

    @Nullable
    private static File safeSubFile(File root, String relPath) throws IOException {
        File out = new File(root, relPath);
        String canonicalRoot = root.getCanonicalPath() + File.separator;
        if (!out.getCanonicalPath().startsWith(canonicalRoot)) return null;
        return out;
    }

    private static void writeBytes(File dest, byte[] bytes) throws IOException {
        File parent = dest.getParentFile();
        if (parent != null && !parent.exists()) parent.mkdirs();
        try (FileOutputStream fos = new FileOutputStream(dest)) {
            fos.write(bytes);
        }
    }

    private static byte[] readAllBytesBounded(InputStream is, int maxBytes) throws IOException {
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        byte[] buf = new byte[16384];
        int total = 0;
        int n;
        while ((n = is.read(buf)) > 0) {
            total += n;
            if (total > maxBytes) {
                throw new IOException("单个文件超过 128 MB 限制");
            }
            bos.write(buf, 0, n);
        }
        return bos.toByteArray();
    }

    private static int readLE16(byte[] b, int o) {
        return (b[o] & 0xFF) | ((b[o + 1] & 0xFF) << 8);
    }

    private static int readLE32(byte[] b, int o) {
        return (b[o] & 0xFF)
                | ((b[o + 1] & 0xFF) << 8)
                | ((b[o + 2] & 0xFF) << 16)
                | ((b[o + 3] & 0xFF) << 24);
    }

    private static long readLE64(byte[] b, int o) {
        return (readLE32(b, o) & 0xFFFFFFFFL) | ((readLE32(b, o + 4) & 0xFFFFFFFFL) << 32);
    }

    private static void putLE16(byte[] b, int o, int v) {
        b[o] = (byte) (v & 0xFF);
        b[o + 1] = (byte) ((v >>> 8) & 0xFF);
    }

    private static void putLE32(byte[] b, int o, int v) {
        b[o] = (byte) (v & 0xFF);
        b[o + 1] = (byte) ((v >>> 8) & 0xFF);
        b[o + 2] = (byte) ((v >>> 16) & 0xFF);
        b[o + 3] = (byte) ((v >>> 24) & 0xFF);
    }

    private static String readNullTermAscii(byte[] b, int o, int maxLen) {
        int len = 0;
        while (len < maxLen && (o + len) < b.length && b[o + len] != 0) {
            len++;
        }
        return new String(b, o, len, StandardCharsets.ISO_8859_1);
    }
}
