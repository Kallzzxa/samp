package com.sampmobile.launcher.utils;

import android.content.Context;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

import com.sampmobile.launcher.utils.ModManager.FileModEntry;
import com.sampmobile.launcher.utils.ModManager.JPatchOption;

final class JPatchConfigManager {
    private JPatchConfigManager() {}

    // ══════════ JPatch Binary + Config Editor ══════════

    public static FileModEntry findJPatchBinary(Context ctx) {
        for (FileModEntry e : ModManager.listAmlPlugins(ctx)) {
            if (e.isJPatch) return e;
        }
        return null;
    }

    public static boolean syncJPatchBinaryState(Context ctx, boolean enable) {
        FileModEntry jp = findJPatchBinary(ctx);
        if (jp == null) return false;
        if (jp.enabled == enable) return true;
        return ModManager.toggleDisabledFile(jp.file, enable) != null;
    }

    public static File getJPatchConfigFile(Context ctx) {
        File cfgDir = ModManager.getAmlConfigsDir(ctx);
        // Official AML JPatch (32-bit & 64-bit) uses AML Config("net.rusjj.jpatch") -> configs/net.rusjj.jpatch.ini
        File official = new File(cfgDir, "net.rusjj.jpatch.ini");
        if (official.exists()) return official;
        File legacyIni = new File(cfgDir, "JPatch.ini");
        if (legacyIni.exists()) return legacyIni;
        File legacyCfg = new File(cfgDir, "JPatch.cfg");
        if (legacyCfg.exists()) return legacyCfg;
        return official;
    }

    static List<JPatchOption> buildKnownJPatchCatalog() {
        List<JPatchOption> list = new ArrayList<>();
        // Safe & high-value GTA:SA 2.10 fixes (exact keys from official net.rusjj.jpatch.ini)
        list.add(new JPatchOption("Gameplay", "Fix210Crash", "Fix random crashes in stock 2.10",
                "Apply the 2.10 engine crash patch (recommended)", true, "1", true));
        list.add(new JPatchOption("Gameplay", "GetCompositeMatrixFixPossibleCrash", "Fix composite-matrix crash",
                "Prevent model/skeleton matrix null-pointer crashes", true, "1", true));
        list.add(new JPatchOption("Gameplay", "FixCollisionAllocatingData", "Fix collision allocation fault",
                "Fix dynamic collision data allocation out-of-bounds crash", true, "1", true));
        list.add(new JPatchOption("Gameplay", "FixEnterVehicleTasks", "Fix enter-vehicle task fault",
                "Fix the enter-vehicle action getting stuck in some cases", true, "1", true));
        list.add(new JPatchOption("Gameplay", "FixChassisSwingingHighFPS", "High-FPS vehicle anti-jitter",
                "Fix violent vehicle chassis shaking at 60+ FPS", true, "1", true));
        list.add(new JPatchOption("Gameplay", "FixVehiclesSpeedHighFPS", "High-FPS vehicle speed fix",
                "Fix abnormal vehicle acceleration and drag at high FPS", true, "1", true));
        list.add(new JPatchOption("Visual", "FixFogWall", "Fix black wall behind fog",
                "Fix distant fog-wall rendering glitches under the default shader", true, "1", true));
        list.add(new JPatchOption("Visual", "FixWheelsRotationSpeed", "High-FPS wheel rotation speed fix",
                "Fix wheels spinning too fast at high FPS", true, "1", true));
        list.add(new JPatchOption("Visual", "FixGreenTextures", "Fix green texture glitch",
                "Fix some GPU/texture combinations rendering green", true, "1", true));
        list.add(new JPatchOption("Visual", "SP_BilinearFilterForPlate", "Fix license-plate bilinear filtering",
                "Sharpen vehicle license plates (SilentPatch)", true, "1", true));
        list.add(new JPatchOption("Visual", "FixAdjustableSizeLowering", "Fix HUD scaling offset",
                "Fix Adjustable.cfg custom HUD button size offset", true, "1", true));
        list.add(new JPatchOption("Visual", "FixXclipseCrash", "Fix Xclipse/Adreno shader crash",
                "Improve rendering stability on some newer GPU drivers", true, "1", true));

        // Caution: overlap with SAMPMOBILE hooks/patches — disabled in SA-MP safe preset
        list.add(new JPatchOption("Gameplay", "MultiTouch4", "JPatch four-finger touch patch [online conflict]",
                "This client already has built-in MultiTouch interception and MonetLoader/CLEO touch routing; keep it off to avoid conflicts", false, "0", true));
        list.add(new JPatchOption("Gameplay", "BuffStreamingMem", "JPatch fixed streaming memory expansion [use with caution online]",
                "This client already has built-in CStreaming x3 expansion; enabling it again may increase memory pressure", false, "0", true));
        list.add(new JPatchOption("Gameplay", "DynamicStreamingMem", "JPatch dynamic streaming memory expansion [use with caution online]",
                "Overlaps this client's built-in streaming memory policy; off by default in the online-safe preset", false, "0", true));
        list.add(new JPatchOption("Visual", "EnableAnimatedTextures", "JPatch UV animation patch [online conflict]",
                "This client already has its own bounded 2.10 UVAnim driver; keep it off to avoid duplicate hooks", false, "0", true));
        list.add(new JPatchOption("Visual", "FixCrosshair", "JPatch crosshair fix [online conflict]",
                "This client already has a built-in online crosshair hook; keep it off to avoid conflicts", false, "0", true));
        list.add(new JPatchOption("Gameplay", "ForceLoadDXT", "Force DXT texture format [online conflict]",
                "This client already loads automatically based on the texdb format actually present; forcing it causes black textures", false, "0", true));
        list.add(new JPatchOption("Gameplay", "ForceLoadETC", "Force ETC texture format [online conflict]",
                "This client already picks the texture format automatically; forcing an override is not recommended", false, "0", true));
        list.add(new JPatchOption("Gameplay", "ForceLoadPVR", "Force PVR texture format [online conflict]",
                "This client already picks the texture format automatically; forcing an override is not recommended", false, "0", true));
        list.add(new JPatchOption("Visual", "AlwaysDrawWantedStars", "Always show wanted stars [use with caution online]",
                "Online wanted stars are controlled by the server; off by default", false, "0", true));
        return list;
    }

    public static List<JPatchOption> loadJPatchOptions(Context ctx) {
        ModManager.ensureDirectories(ctx);
        File cfgFile = getJPatchConfigFile(ctx);
        IniDocument doc = IniDocument.load(cfgFile);

        List<JPatchOption> options = buildKnownJPatchCatalog();
        Set<String> seenKeys = new HashSet<>();
        for (JPatchOption opt : options) {
            seenKeys.add((opt.section + ":" + opt.key).toLowerCase(Locale.ROOT));
            // Check both opt.section and any other section in JPatch.ini (JPatch may use [Patches]/[Main]/[Preferences])
            String val = findIniValueAnySection(doc, opt.section, opt.key);
            if (val != null) {
                opt.value = val;
            }
        }

        // Also include every extra key present in the user's real JPatch.ini so nothing is hidden!
        for (Map.Entry<String, LinkedHashMap<String, String>> se : doc.sections.entrySet()) {
            String sec = se.getKey();
            for (Map.Entry<String, String> kv : se.getValue().entrySet()) {
                String k = kv.getKey();
                String v = kv.getValue();
                if (seenKeys.contains((sec + ":" + k).toLowerCase(Locale.ROOT))) continue;
                // Avoid duplicating a known key if it lived in another section
                boolean dupKey = false;
                for (JPatchOption opt : options) {
                    if (opt.key.equalsIgnoreCase(k)) {
                        opt.section = sec;
                        opt.value = v;
                        dupKey = true;
                        break;
                    }
                }
                if (dupKey) continue;

                boolean isBool = isBooleanLike(v);
                JPatchOption custom = new JPatchOption(sec, k, k, "From " + cfgFile.getName() + " [" + sec + "]", true, v, isBool);
                custom.value = v;
                custom.fromKnownCatalog = false;
                options.add(custom);
            }
        }
        return options;
    }

    private static String findIniValueAnySection(IniDocument doc, String preferredSection, String key) {
        if (doc.has(preferredSection, key)) {
            return doc.get(preferredSection, key, null);
        }
        for (Map.Entry<String, LinkedHashMap<String, String>> se : doc.sections.entrySet()) {
            for (Map.Entry<String, String> kv : se.getValue().entrySet()) {
                if (kv.getKey().equalsIgnoreCase(key)) {
                    return kv.getValue();
                }
            }
        }
        return null;
    }

    private static boolean isBooleanLike(String val) {
        if (val == null) return false;
        String v = val.trim().toLowerCase(Locale.ROOT);
        return v.equals("0") || v.equals("1") || v.equals("true") || v.equals("false");
    }

    public static void saveJPatchOption(Context ctx, String section, String key, String value) {
        ModManager.ensureDirectories(ctx);
        File cfgFile = getJPatchConfigFile(ctx);
        IniDocument doc = IniDocument.load(cfgFile);
        // Update existing section if key already exists elsewhere
        String targetSec = section;
        String targetKey = key;
        outer:
        for (Map.Entry<String, LinkedHashMap<String, String>> se : doc.sections.entrySet()) {
            for (String existingKey : se.getValue().keySet()) {
                if (existingKey.equalsIgnoreCase(key)) {
                    targetSec = se.getKey();
                    targetKey = existingKey;
                    break outer;
                }
            }
        }
        doc.set(targetSec, targetKey, value);
        try {
            doc.save(cfgFile);
        } catch (IOException e) {
            e.printStackTrace();
        }
    }

    public static void applyJPatchSampSafePreset(Context ctx) {
        ModManager.ensureDirectories(ctx);
        File cfgFile = getJPatchConfigFile(ctx);
        IniDocument doc = IniDocument.load(cfgFile);
        for (JPatchOption opt : buildKnownJPatchCatalog()) {
            String targetSec = opt.section;
            String targetKey = opt.key;
            outer:
            for (Map.Entry<String, LinkedHashMap<String, String>> se : doc.sections.entrySet()) {
                for (String existingKey : se.getValue().keySet()) {
                    if (existingKey.equalsIgnoreCase(opt.key)) {
                        targetSec = se.getKey();
                        targetKey = existingKey;
                        break outer;
                    }
                }
            }
            doc.set(targetSec, targetKey, opt.defaultSafeValue);
        }
        try {
            doc.save(cfgFile);
        } catch (IOException e) {
            e.printStackTrace();
        }
    }

    /**
     * JPatch's MultiTouch4 conflicts with SAMPMOBILE's built-in 4-finger touch
     * pipeline (g_points/g_pointers relocation & NVEventGetNextEvent hook),
     * injecting phantom ptr=0 DOWN/UP events that cut off the virtual joystick.
     * Ensure MultiTouch4=0 before AML loads libJPatch.so without disturbing any
     * other user-configured JPatch options.
     */
    public static void enforceTouchSafety(Context ctx) {
        File cfgFile = getJPatchConfigFile(ctx);
        if (!cfgFile.exists()) return;
        IniDocument doc = IniDocument.load(cfgFile);
        String cur = findIniValueAnySection(doc, "Gameplay", "MultiTouch4");
        if ("0".equals(cur)) return;
        saveJPatchOption(ctx, "Gameplay", "MultiTouch4", "0");
    }
}
