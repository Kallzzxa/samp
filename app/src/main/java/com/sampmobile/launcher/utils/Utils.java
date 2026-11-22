package com.sampmobile.launcher.utils;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.net.ConnectivityManager;
import android.net.NetworkInfo;

import com.joom.paranoid.Obfuscate;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.text.CharacterIterator;
import java.text.StringCharacterIterator;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;

@Obfuscate
public class Utils {
    public static String copyright = "SA-MP Mobile";

    public enum GPUType {
        NONE,
        DXT,
        PVR,
        ETC
    }

    public static GPUType GPU_TYPE = GPUType.NONE;

    public static boolean isOnline(Activity activity) {
        System.out.println("isOnline");
        ConnectivityManager connectivityManager = (ConnectivityManager) activity.getSystemService(Context.CONNECTIVITY_SERVICE);
        NetworkInfo activeNetworkInfo = connectivityManager != null ? connectivityManager.getActiveNetworkInfo() : null;
        return activeNetworkInfo != null && activeNetworkInfo.isConnectedOrConnecting();
    }

    public static void saveSettings(Context ctx) {
        System.out.println("saveSettings");

        try {
            File extDir = ctx != null ? ctx.getExternalFilesDir(null) : null;
            if (extDir == null) return;
            File file = new File(extDir, "SAMP/settings.json");

            SharedPreferences settings_prefs = ctx.getSharedPreferences("samp_settings", Context.MODE_PRIVATE);
            if (!settings_prefs.getBoolean("prefs_defaults_v2", false)) {
                settings_prefs.edit()
                        .putBoolean("voice_chat", false)
                        .putBoolean("show_nametag_weapon", false)
                        .putBoolean("prefs_defaults_v2", true)
                        .apply();
            }
            SharedPreferences server_prefs = ctx.getSharedPreferences("samp_server", Context.MODE_PRIVATE);

            JSONObject jSONObject = new JSONObject();
            jSONObject.put("client", new JSONObject());

            jSONObject.getJSONObject("client").put("server", new JSONObject());
            jSONObject.getJSONObject("client").getJSONObject("server").put("host", server_prefs.getString("host", "127.0.0.1"));
            int port;
            try {
                port = Integer.parseInt(server_prefs.getString("port", "7777"));
            } catch (NumberFormatException e) {
                port = 7777;
            }
            jSONObject.getJSONObject("client").getJSONObject("server").put("port", port);
            jSONObject.getJSONObject("client").getJSONObject("server").put("password", server_prefs.getString("password", ""));

            JSONObject sObj = new JSONObject();
            jSONObject.getJSONObject("client").put("settings", sObj);
            sObj.put("nick_name", settings_prefs.getString("nick_name", "samp_player"));
            sObj.put("samp_version", settings_prefs.getInt("samp_version", 0));
            sObj.put("game_version", settings_prefs.getInt("game_version", 0));
            sObj.put("new_interface", settings_prefs.getBoolean("new_interface", false));
            sObj.put("system_keyboard", settings_prefs.getBoolean("system_keyboard", true));
            sObj.put("timestamp", settings_prefs.getBoolean("timestamp", false));
            sObj.put("fullscreen", settings_prefs.getBoolean("fullscreen", false));
            sObj.put("fast_connect", settings_prefs.getBoolean("fast_connect", false));
            sObj.put("voice_chat", settings_prefs.getBoolean("voice_chat", false));
            sObj.put("display_fps", settings_prefs.getBoolean("display_fps", false));
            sObj.put("cleo_scripts", settings_prefs.getBoolean("cleo_scripts", false));
            sObj.put("aml_scripts", settings_prefs.getBoolean("aml_scripts", false));
            sObj.put("jpatch_enabled", settings_prefs.getBoolean("jpatch_enabled", false));
            sObj.put("monet_scripts", settings_prefs.getBoolean("monet_scripts", false));
            sObj.put("modloader", settings_prefs.getBoolean("modloader", false));
            sObj.put("modify_files", settings_prefs.getBoolean("modify_files", false));
            sObj.put("fps_limit", settings_prefs.getInt("fps_limit", 0));
            sObj.put("chat_strings", settings_prefs.getInt("chat_strings", 8));
            sObj.put("font_size", settings_prefs.getFloat("font_size", 25.0f));
            sObj.put("chat_posx", settings_prefs.getInt("chat_posx", 100));
            sObj.put("chat_posy", settings_prefs.getInt("chat_posy", 20));
            sObj.put("chat_sizex", settings_prefs.getInt("chat_sizex", 360));
            sObj.put("chat_sizey", settings_prefs.getInt("chat_sizey", 110));
            sObj.put("charset", settings_prefs.getString("charset", "GBK"));

            // Extended multiplayer & gameplay
            sObj.put("head_move", settings_prefs.getBoolean("head_move", true));
            sObj.put("auto_climb", settings_prefs.getBoolean("auto_climb", true));
            sObj.put("dl_custom_models", settings_prefs.getBoolean("dl_custom_models", true));

            // Graphics & display (GTA:SA MobileSettings)
            sObj.put("gta_visual_fx", settings_prefs.getInt("gta_visual_fx", 2));
            sObj.put("gta_resolution", settings_prefs.getInt("gta_resolution", 100));
            sObj.put("gta_draw_distance", settings_prefs.getInt("gta_draw_distance", 70));
            sObj.put("gta_shadows", settings_prefs.getInt("gta_shadows", 1));
            sObj.put("gta_car_reflections", settings_prefs.getInt("gta_car_reflections", 2));
            sObj.put("gta_traffic_mode", settings_prefs.getInt("gta_traffic_mode", 2));
            sObj.put("gta_brightness", settings_prefs.getInt("gta_brightness", 60));
            sObj.put("aspect_ratio", settings_prefs.getInt("aspect_ratio", 0));
            sObj.put("frame_limiter", settings_prefs.getBoolean("frame_limiter", false));

            // Camera, controls & single-player GTA:SA options
            sObj.put("cam_sens_x", settings_prefs.getInt("cam_sens_x", 50));
            sObj.put("cam_sens_y", settings_prefs.getInt("cam_sens_y", 50));
            sObj.put("cam_sens_foot", settings_prefs.getInt("cam_sens_foot", 100));
            sObj.put("cam_sens_veh", settings_prefs.getInt("cam_sens_veh", 100));
            sObj.put("cam_sens_aim", settings_prefs.getInt("cam_sens_aim", 70));
            sObj.put("free_cam", settings_prefs.getBoolean("free_cam", false));
            sObj.put("first_person_look", settings_prefs.getBoolean("first_person_look", true));
            sObj.put("invert_look", settings_prefs.getBoolean("invert_look", false));
            sObj.put("veh_cam_height", settings_prefs.getInt("veh_cam_height", 1));
            sObj.put("ped_cam_zoom", settings_prefs.getInt("ped_cam_zoom", 2));
            sObj.put("targeting_mode", settings_prefs.getInt("targeting_mode", 0));
            sObj.put("joystick_size", settings_prefs.getInt("joystick_size", 100));
            sObj.put("analog_sensitivity", settings_prefs.getInt("analog_sensitivity", 50));
            sObj.put("flick_sensitivity", settings_prefs.getInt("flick_sensitivity", 50));
            sObj.put("shoot_threshold", settings_prefs.getInt("shoot_threshold", 50));
            sObj.put("touch_steering", settings_prefs.getInt("touch_steering", 0));
            sObj.put("touch_layout", settings_prefs.getInt("touch_layout", 0));
            sObj.put("bike_steering", settings_prefs.getBoolean("bike_steering", false));
            sObj.put("accelerometer", settings_prefs.getInt("accelerometer", 0));
            sObj.put("vibration", settings_prefs.getBoolean("vibration", true));
            sObj.put("joypad_invert_x", settings_prefs.getBoolean("joypad_invert_x", false));
            sObj.put("joypad_invert_y", settings_prefs.getBoolean("joypad_invert_y", false));
            sObj.put("km_enabled", settings_prefs.getBoolean("km_enabled", true));
            sObj.put("open_hud_edit_on_start", settings_prefs.getBoolean("open_hud_edit_on_start", false));

            // Audio, radio & voice chat
            sObj.put("sfx_volume", settings_prefs.getInt("sfx_volume", 80));
            sObj.put("music_volume", settings_prefs.getInt("music_volume", 80));
            sObj.put("radio_autotune", settings_prefs.getBoolean("radio_autotune", true));
            sObj.put("radio_station", settings_prefs.getInt("radio_station", 0));
            sObj.put("voice_volume", settings_prefs.getInt("voice_volume", 100));
            sObj.put("voice_mic_enable", settings_prefs.getBoolean("voice_mic_enable", true));
            sObj.put("voice_mic_volume", settings_prefs.getInt("voice_mic_volume", 75));
            sObj.put("voice_mic_mode", settings_prefs.getInt("voice_mic_mode", 0));
            sObj.put("voice_balancer", settings_prefs.getBoolean("voice_balancer", false));
            sObj.put("voice_filter", settings_prefs.getBoolean("voice_filter", true));
            sObj.put("voice_speaker_list", settings_prefs.getBoolean("voice_speaker_list", true));
            sObj.put("voice_btn_size", settings_prefs.getInt("voice_btn_size", 100));

            // HUD, fonts, chat & screenshot
            sObj.put("show_gta_hud", settings_prefs.getBoolean("show_gta_hud", true));
            sObj.put("show_radar", settings_prefs.getBoolean("show_radar", true));
            sObj.put("radar_tap_map", settings_prefs.getBoolean("radar_tap_map", true));
            sObj.put("crosshair_style", settings_prefs.getInt("crosshair_style", 0));
            sObj.put("show_death_window", settings_prefs.getBoolean("show_death_window", true));
            sObj.put("show_nametags", settings_prefs.getBoolean("show_nametags", true));
            sObj.put("show_nametag_weapon", settings_prefs.getBoolean("show_nametag_weapon", false));
            sObj.put("nametag_size", settings_prefs.getInt("nametag_size", 100));
            sObj.put("textlabel_size", settings_prefs.getInt("textlabel_size", 100));
            sObj.put("font_family", settings_prefs.getInt("font_family", 0));
            sObj.put("show_button_panel", settings_prefs.getBoolean("show_button_panel", true));
            sObj.put("show_screenshot_btn", settings_prefs.getBoolean("show_screenshot_btn", true));
            sObj.put("gta_subtitles", settings_prefs.getBoolean("gta_subtitles", true));
            sObj.put("chat_shadow", settings_prefs.getBoolean("chat_shadow", true));
            sObj.put("screenshot_mode", settings_prefs.getInt("screenshot_mode", 0));

            File parent = file.getParentFile();
            if (parent != null && !parent.exists()) {
                parent.mkdirs();
            }

            writeJSONToFile(file, jSONObject);
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    public static void restoreSettings(Context ctx) {
        System.out.println("restoreSettings");

        SharedPreferences.Editor edit = ctx.getSharedPreferences("samp_settings", Context.MODE_PRIVATE).edit();
        edit.putString("nick_name", "samp_player");
        edit.putInt("samp_version", 0);
        edit.putInt("game_version", 0);
        edit.putBoolean("new_interface", false);
        edit.putBoolean("system_keyboard", true);
        edit.putBoolean("timestamp", false);
        edit.putBoolean("fullscreen", false);
        edit.putBoolean("fast_connect", false);
        edit.putBoolean("voice_chat", false);
        edit.putBoolean("display_fps", false);
        edit.putBoolean("cleo_scripts", false);
        edit.putBoolean("aml_scripts", false);
        edit.putBoolean("jpatch_enabled", false);
        edit.putBoolean("monet_scripts", false);
        edit.putBoolean("modloader", false);
        edit.putBoolean("modify_files", false);
        edit.putInt("fps_limit", 0);
        edit.putInt("chat_strings", 8);
        edit.putFloat("font_size", 25.0f);
        edit.putInt("chat_posx", 100);
        edit.putInt("chat_posy", 20);
        edit.putInt("chat_sizex", 360);
        edit.putInt("chat_sizey", 110);
        edit.putString("charset", "GBK");

        edit.putBoolean("head_move", true);
        edit.putBoolean("auto_climb", true);
        edit.putBoolean("dl_custom_models", true);

        edit.putInt("gta_visual_fx", 2);
        edit.putInt("gta_resolution", 100);
        edit.putInt("gta_draw_distance", 70);
        edit.putInt("gta_shadows", 1);
        edit.putInt("gta_car_reflections", 2);
        edit.putInt("gta_traffic_mode", 2);
        edit.putInt("gta_brightness", 60);
        edit.putInt("aspect_ratio", 0);
        edit.putBoolean("frame_limiter", false);

        edit.putInt("cam_sens_x", 50);
        edit.putInt("cam_sens_y", 50);
        edit.putInt("cam_sens_foot", 100);
        edit.putInt("cam_sens_veh", 100);
        edit.putInt("cam_sens_aim", 70);
        edit.putBoolean("free_cam", false);
        edit.putBoolean("first_person_look", true);
        edit.putBoolean("invert_look", false);
        edit.putInt("veh_cam_height", 1);
        edit.putInt("ped_cam_zoom", 2);
        edit.putInt("targeting_mode", 0);
        edit.putInt("joystick_size", 100);
        edit.putInt("analog_sensitivity", 50);
        edit.putInt("flick_sensitivity", 50);
        edit.putInt("shoot_threshold", 50);
        edit.putInt("touch_steering", 0);
        edit.putInt("touch_layout", 0);
        edit.putBoolean("bike_steering", false);
        edit.putInt("accelerometer", 0);
        edit.putBoolean("vibration", true);
        edit.putBoolean("joypad_invert_x", false);
        edit.putBoolean("joypad_invert_y", false);
        edit.putBoolean("km_enabled", true);
        edit.putBoolean("open_hud_edit_on_start", false);

        edit.putInt("sfx_volume", 80);
        edit.putInt("music_volume", 80);
        edit.putBoolean("radio_autotune", true);
        edit.putInt("radio_station", 0);
        edit.putInt("voice_volume", 100);
        edit.putBoolean("voice_mic_enable", true);
        edit.putInt("voice_mic_volume", 75);
        edit.putInt("voice_mic_mode", 0);
        edit.putBoolean("voice_balancer", false);
        edit.putBoolean("voice_filter", true);
        edit.putBoolean("voice_speaker_list", true);
        edit.putInt("voice_btn_size", 100);

        edit.putBoolean("show_gta_hud", true);
        edit.putBoolean("show_radar", true);
        edit.putBoolean("radar_tap_map", true);
        edit.putInt("crosshair_style", 0);
        edit.putBoolean("show_death_window", true);
        edit.putBoolean("show_nametags", true);
        edit.putBoolean("show_nametag_weapon", false);
        edit.putInt("nametag_size", 100);
        edit.putInt("textlabel_size", 100);
        edit.putInt("font_family", 0);
        edit.putBoolean("show_button_panel", true);
        edit.putBoolean("show_screenshot_btn", true);
        edit.putBoolean("gta_subtitles", true);
        edit.putBoolean("chat_shadow", true);
        edit.putInt("screenshot_mode", 0);
        edit.putInt(LauncherThemeManager.PREF_HUE, LauncherThemeManager.DEFAULT_HUE);
        edit.commit();

        saveSettings(ctx);
    }

    public static boolean addServerToFavorites(Context ctx, String address) {
        try {
            if (ctx == null || address == null) return false;
            String[] parts = address.split(":");
            if (parts.length < 2) return false;

            File extDir = ctx.getExternalFilesDir(null);
            if (extDir == null) return false;
            File serversFile = new File(extDir, "servers.json");

            if (!serversFile.exists() || serversFile.length() == 0) {
                // initialize servers.json with an empty array
                JSONObject initialJson = new JSONObject();
                initialJson.put("servers", new JSONArray());
                writeJSONToFile(serversFile, initialJson);
            }

            String jsonString = readJSONFromFile(serversFile);
            JSONObject json = new JSONObject(jsonString);
            JSONArray serversArray = json.getJSONArray("servers");

            // check if the server already exists in favorites
            for (int i = 0; i < serversArray.length(); i++) {
                JSONObject serverJson = serversArray.getJSONObject(i);
                String host = serverJson.getString("host");
                String port = serverJson.getString("port");

                if (address.equals(host + ":" + port)) {
                    return false; // Server already exists in favorites
                }
            }

            // add the new server to the JSON array
            String host = parts[0];
            String port = parts[1];

            JSONObject newServerJson = new JSONObject();
            newServerJson.put("host", host);
            newServerJson.put("port", port);
            serversArray.put(newServerJson);

            // update the JSON object and write it back to the file
            writeJSONToFile(serversFile, json);

            return true;
        } catch (Exception e) {
            e.printStackTrace();
            return false;
        }
    }

    public static boolean isServerInFavorites(Context ctx, String address) {
        try {
            if (ctx == null || address == null) return false;
            File extDir = ctx.getExternalFilesDir(null);
            if (extDir == null) return false;
            File serversFile = new File(extDir, "servers.json");
            if (!serversFile.exists() || serversFile.length() == 0) return false;

            JSONObject json = new JSONObject(readJSONFromFile(serversFile));
            JSONArray serversArray = json.getJSONArray("servers");
            for (int i = 0; i < serversArray.length(); i++) {
                JSONObject serverJson = serversArray.getJSONObject(i);
                if (address.equals(serverJson.getString("host") + ":" + serverJson.getString("port"))) {
                    return true;
                }
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
        return false;
    }

    public static void deleteServerFromFavorites(Context ctx, String address) throws Exception {
        if (ctx == null || address == null) return;
        String[] parts = address.split(":");
        if (parts.length < 2) return;
        File extDir = ctx.getExternalFilesDir(null);
        if (extDir == null) return;
        File serversFile = new File(extDir, "servers.json");
        if (!serversFile.exists() || serversFile.length() == 0) return;

        String jsonString = readJSONFromFile(serversFile);
        JSONObject json = new JSONObject(jsonString);
        JSONArray serversArray = json.getJSONArray("servers");

        JSONArray newServersArray = new JSONArray();
        String host = parts[0];
        String port = parts[1];

        for (int i = 0; i < serversArray.length(); i++) {
            JSONObject serverJson = serversArray.getJSONObject(i);
            String serverHost = serverJson.getString("host");
            String serverPort = serverJson.getString("port");

            if (!host.equals(serverHost) || !port.equals(serverPort)) {
                newServersArray.put(serverJson);
            }
        }

        json.put("servers", newServersArray);
        writeJSONToFile(serversFile, json);
    }

    public static void addServerToRecent(Context ctx, String address) {
        try {
            if (ctx == null || address == null) return;
            String[] parts = address.split(":");
            if (parts.length < 2) return;
            File extDir = ctx.getExternalFilesDir(null);
            if (extDir == null) return;
            File recentFile = new File(extDir, "recent.json");

            JSONObject json;
            if (recentFile.exists() && recentFile.length() > 0) {
                json = new JSONObject(readJSONFromFile(recentFile));
            } else {
                json = new JSONObject();
                json.put("servers", new JSONArray());
            }

            JSONArray serversArray = json.getJSONArray("servers");
            String host = parts[0];
            String port = parts[1];

            JSONArray newServersArray = new JSONArray();
            JSONObject entry = new JSONObject();
            entry.put("host", host);
            entry.put("port", port);
            newServersArray.put(entry);

            for (int i = 0; i < serversArray.length() && newServersArray.length() < 30; i++) {
                JSONObject serverJson = serversArray.getJSONObject(i);
                String serverHost = serverJson.getString("host");
                String serverPort = serverJson.getString("port");
                if (!host.equals(serverHost) || !port.equals(serverPort)) {
                    newServersArray.put(serverJson);
                }
            }

            json.put("servers", newServersArray);
            writeJSONToFile(recentFile, json);
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    public static void deleteServerFromRecent(Context ctx, String address) throws Exception {
        if (ctx == null || address == null) return;
        String[] parts = address.split(":");
        if (parts.length < 2) return;
        File extDir = ctx.getExternalFilesDir(null);
        if (extDir == null) return;
        File recentFile = new File(extDir, "recent.json");
        if (!recentFile.exists() || recentFile.length() == 0) return;

        JSONObject json = new JSONObject(readJSONFromFile(recentFile));
        JSONArray serversArray = json.getJSONArray("servers");

        JSONArray newServersArray = new JSONArray();
        String host = parts[0];
        String port = parts[1];

        for (int i = 0; i < serversArray.length(); i++) {
            JSONObject serverJson = serversArray.getJSONObject(i);
            if (!host.equals(serverJson.getString("host")) || !port.equals(serverJson.getString("port"))) {
                newServersArray.put(serverJson);
            }
        }

        json.put("servers", newServersArray);
        writeJSONToFile(recentFile, json);
    }

    public static String readJSONFromFile(File file) throws IOException {
        StringBuilder sb = new StringBuilder();
        try (BufferedReader br = new BufferedReader(new FileReader(file))) {
            String line;
            while ((line = br.readLine()) != null) {
                sb.append(line);
            }
        }
        return sb.toString();
    }

    public static void writeJSONToFile(File file, JSONObject json) throws IOException, JSONException {
        File parent = file.getParentFile();
        if (parent != null && !parent.exists()) {
            parent.mkdirs();
        }
        // Write to a temp file then atomically rename so a crash mid-write never
        // leaves a truncated/empty JSON file behind.
        File tmp = new File(file.getParentFile(), file.getName() + ".tmp");
        try (BufferedWriter bw = new BufferedWriter(new FileWriter(tmp))) {
            bw.write(json.toString(4));
            bw.write(System.lineSeparator());
        }
        if (!tmp.renameTo(file)) {
            // renameTo should not fail on Linux filesystems; fall back to direct write
            try (BufferedWriter bw = new BufferedWriter(new FileWriter(file))) {
                bw.write(json.toString(4));
                bw.write(System.lineSeparator());
            }
            tmp.delete();
        }
    }

    private static final OkHttpClient HTTP_CLIENT = new OkHttpClient();

    public static String getStringOutputByURL(final String url) throws IOException {
        Request request = new Request.Builder()
                .url(url)
                .build();

        try (Response response = HTTP_CLIENT.newCall(request).execute()) {
            if (!response.isSuccessful()) {
                throw new IOException("Unexpected response code: " + response.code());
            }

            assert response.body() != null;
            return response.body().string();
        }
    }

    public static String convertBytesToString(long bytes) {
        if (-1000 < bytes && bytes < 1000) {
            return bytes + " B";
        }
        CharacterIterator ci = new StringCharacterIterator("kMGTPE");
        while (bytes <= -999_950 || bytes >= 999_950) {
            bytes /= 1000;
            ci.next();
        }
        return String.format("%.1f %cB", bytes / 1000.0, ci.current());
    }
}
