package com.sampmobile.launcher.ui.fragment;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.graphics.BitmapFactory;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.StatFs;
import android.provider.MediaStore;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.Filter;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.RelativeLayout;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.annotation.NonNull;
import androidx.appcompat.app.AlertDialog;
import androidx.core.content.FileProvider;
import androidx.fragment.app.Fragment;
import androidx.recyclerview.widget.GridLayoutManager;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.bumptech.glide.Glide;
import com.google.android.material.textfield.MaterialAutoCompleteTextView;
import com.joom.paranoid.Obfuscate;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

import com.sampmobile.launcher.BuildConfig;
import com.sampmobile.launcher.R;
import com.sampmobile.launcher.download.GamedataInstallerTask;
import com.sampmobile.launcher.download.GamedataState;
import com.sampmobile.launcher.ui.GameDataImportDialog;
import com.sampmobile.launcher.ui.widget.GtaOptionView;
import com.sampmobile.launcher.ui.widget.GtaSaSliderView;
import com.sampmobile.launcher.ui.widget.GtaTabStrip;
import com.sampmobile.launcher.utils.ClientSettings;
import com.sampmobile.launcher.utils.GameDataImporter;
import com.sampmobile.launcher.utils.GameDataVerifier;
import com.sampmobile.launcher.utils.LauncherThemeManager;

@Obfuscate
public class SettingsPageFragment extends Fragment implements LauncherThemeManager.ThemeListener {

    private static final int CAT_MULTIPLAYER = 0;
    private static final int CAT_GAMEDATA    = 1;
    private static final int CAT_GRAPHICS    = 2;
    private static final int CAT_CAMERA      = 3;
    private static final int CAT_CONTROLS    = 4;
    private static final int CAT_AUDIO       = 5;
    private static final int CAT_HUD_CHAT    = 6;
    private static final int CAT_SCREENSHOTS = 7;
    private static final int CAT_LOGS        = 8;
    private static final int CAT_HELP_ABOUT  = 9;

    private static final String[] CATEGORY_TITLES = {
            "Online",
            "File verification",
            "Graphics",
            "Camera",
            "Controls",
            "Audio",
            "UI & chat",
            "Screenshots",
            "Report an issue",
            "Help & about"
    };

    private static final String[] GAMEDATA_EDITION_ITEMS = {
            "Lite (Recommended)",
            "Full (2.10 GiB)"
    };

    private int currentCategory = CAT_MULTIPLAYER;
    final ExecutorService ioExecutor = Executors.newSingleThreadExecutor();
    private final SettingsScreenshotController screenshotController = new SettingsScreenshotController(this);
    private final SettingsDiagnosticsController diagnosticsController = new SettingsDiagnosticsController(this);
    private final SettingsGameDataController gameDataController = new SettingsGameDataController(this);
    private final SettingsArtworkCacheController artworkCacheController = new SettingsArtworkCacheController(this);
    private View rootView;
    private GtaTabStrip settingsTabs;

    private View[] categoryPanels;

    private EditText et_settings_nickname;
    private GtaOptionView samp_version, charset, fps_limit;
    private MaterialAutoCompleteTextView game_version;
    private GtaOptionView dropdown_aspect_ratio, dropdown_visual_fx, dropdown_shadows, dropdown_car_reflections, dropdown_traffic_mode;
    private GtaOptionView dropdown_ped_cam_zoom, dropdown_veh_cam_height;
    private GtaOptionView dropdown_targeting_mode, dropdown_touch_steering, dropdown_touch_layout, dropdown_accelerometer;
    private GtaOptionView dropdown_radio_station, dropdown_voice_mic_mode;
    private GtaOptionView dropdown_crosshair_style, dropdown_font_family;

    private TextView btn_gfx_preset_low, btn_gfx_preset_med, btn_gfx_preset_high, btn_gfx_preset_ultra;
    private TextView btn_open_hud_editor, btn_reset_hud_editor;

    final ActivityResultLauncher<String[]> gameDataZipPicker =
            registerForActivityResult(new ActivityResultContracts.OpenDocument(), uri -> {
                if (uri != null && isAdded()) {
                    GameDataImportDialog.startImport(requireActivity(), uri, gameDataController::refreshVerification);
                }
            });

    final ActivityResultLauncher<String[]> artworkCachePicker =
            registerForActivityResult(new ActivityResultContracts.OpenMultipleDocuments(), uris -> {
                if (uris != null && !uris.isEmpty() && isAdded()) {
                    artworkCacheController.onDocumentsPicked(uris);
                }
            });

    private Switch voice, timestamp, fullscreen, displayfps, systemkeyboard;
    private Switch modify_files, fastconnect;
    private Switch headmove_switch, dlmodels_switch, switch_invert_look, switch_auto_climb;
    private Switch switch_frame_limiter, switch_free_cam, switch_first_person_look;
    private Switch switch_bike_steering, switch_vibration, switch_radio_autotune;
    private Switch switch_km_enabled, switch_joypad_invert_x, switch_joypad_invert_y;
    private Switch switch_voice_mic_enable, switch_voice_speaker_list, switch_voice_balancer, switch_voice_filter;
    private Switch switch_show_gta_hud, switch_show_radar, switch_radar_tap_map, switch_show_nametag_weapon;
    private Switch switch_show_death_window, switch_show_nametags, switch_show_button_panel;
    private Switch switch_show_screenshot_btn, switch_gta_subtitles, switch_chat_shadow;

    private View restore_button;
    private View chat_minus, chat_plus;
    private TextView chat_strings;

    private GtaSaSliderView slider_theme_hue;
    private TextView tv_theme_hue_val;

    private GtaSaSliderView font_size_slider, chatposx_slider, chatposy_slider, chatsizex_slider, chatsizey_slider;
    private TextView font_size_value, chatposx_value, chatposy_value, chatsizex_value, chatsizey_value;

    private GtaSaSliderView slider_resolution, slider_draw_distance, slider_brightness;
    private TextView tv_resolution_val, tv_draw_distance_val, tv_brightness_val;

    private GtaSaSliderView slider_cam_sens_x, slider_cam_sens_y, slider_cam_sens_foot, slider_cam_sens_veh, slider_cam_sens_aim;
    private TextView tv_cam_sens_x_val, tv_cam_sens_y_val, tv_cam_sens_foot_val, tv_cam_sens_veh_val, tv_cam_sens_aim_val;

    private GtaSaSliderView slider_joystick_size, slider_shoot_threshold, slider_analog_sens, slider_flick_sens;
    private TextView tv_joystick_size_val, tv_shoot_threshold_val, tv_analog_sens_val, tv_flick_sens_val;

    private GtaSaSliderView slider_sfx_volume, slider_music_volume, slider_voice_volume, slider_voice_mic_volume, slider_voice_btn_size;
    private TextView tv_sfx_volume_val, tv_music_volume_val, tv_voice_volume_val, tv_voice_mic_volume_val, tv_voice_btn_size_val;

    private GtaSaSliderView slider_nametag_size, slider_textlabel_size;
    private TextView tv_nametag_size_val, tv_textlabel_size_val;

    // Screenshot mode button
    private TextView btn_ss_mode_hud, btn_ss_mode_game;

    // Help & about
    private TextView tv_help_mods_body, tv_help_faq_body, tv_help_commands;
    private TextView tv_about_version_line, tv_about_summary;

    ClientSettings settings;
    private boolean updatingWidgets = false;
    private RelativeLayout game_version_layout;

    private static final int[] PANEL_LAYOUTS = {
            R.layout.settings_panel_multiplayer,
            R.layout.settings_panel_gamedata,
            R.layout.settings_panel_graphics,
            R.layout.settings_panel_camera,
            R.layout.settings_panel_controls,
            R.layout.settings_panel_audio,
            R.layout.settings_panel_hud_chat,
            R.layout.settings_panel_screenshots,
            R.layout.settings_panel_logs,
            R.layout.settings_panel_help_about
    };

    /**
     * Inflates the settings page piecewise: the frame first, then one panel per {@link #step()}.
     * The launcher runs a step per idle slot, so pre-warming 600+ views never blocks a frame for long.
     */
    public static final class ViewBuilder {
        private final LayoutInflater inflater;
        private final View root;
        private final ViewGroup panels;
        private int next;

        public ViewBuilder(LayoutInflater inflater, ViewGroup parent) {
            this.inflater = inflater;
            this.root = inflater.inflate(R.layout.fragment_settings_page, parent, false);
            this.panels = root.findViewById(R.id.settings_panels);
        }

        /** Inflates the next panel; returns true while panels remain. */
        public boolean step() {
            if (next < PANEL_LAYOUTS.length) {
                inflater.inflate(PANEL_LAYOUTS[next++], panels, true);
            }
            return next < PANEL_LAYOUTS.length;
        }

        public View finish() {
            while (step()) {
            }
            return root;
        }
    }

    private View prebuiltView;

    public SettingsPageFragment() {
    }

    /** Hands over a page view built ahead of time by {@link ViewBuilder}. */
    public void setPrebuiltView(View view) {
        this.prebuiltView = view;
    }

    @Override
    public View onCreateView(LayoutInflater inflater, ViewGroup container, Bundle savedInstanceState) {
        View view = prebuiltView != null ? prebuiltView : new ViewBuilder(inflater, container).finish();
        prebuiltView = null;
        this.rootView = view;
        settings = ClientSettings.loadLauncher(requireContext());

        initViews(view);
        setupCategoryNavigation(view);
        setupListeners(view);
        setWidgetsForSettings();
        populateHelpAndAbout();

        LauncherThemeManager.addListener(this);
        onThemeColorChanged(LauncherThemeManager.getAccentColor(requireContext()));

        return view;
    }

    public void onPageShown() {
        if (!isAdded()) return;
        settings = ClientSettings.reloadLauncher(requireContext());
        setWidgetsForSettings();
        if (currentCategory == CAT_MULTIPLAYER) {
            artworkCacheController.refresh();
        } else if (currentCategory == CAT_GAMEDATA) {
            gameDataController.refreshVerification();
        } else if (currentCategory == CAT_SCREENSHOTS) {
            screenshotController.refresh();
        } else if (currentCategory == CAT_LOGS) {
            diagnosticsController.refresh();
        }
    }

    /** Lets MainActivity jump straight to the "File verification" tab when game data is missing. */
    public void openGameDataTab() {
        if (categoryPanels == null) return;
        if (settingsTabs != null) {
            settingsTabs.setSelectedTab(CAT_GAMEDATA, false);
        }
        switchCategory(CAT_GAMEDATA);
    }

    @Override
    public void onDestroyView() {
        gameDataController.cancelActiveDownload();
        LauncherThemeManager.removeListener(this);
        super.onDestroyView();
    }

    @Override
    public void onDestroy() {
        gameDataController.cancelActiveDownload();
        super.onDestroy();
        ioExecutor.shutdownNow();
    }

    @Override
    public void onThemeColorChanged(int accentColor) {
        if (rootView == null || !isAdded()) return;
        LauncherThemeManager.applyThemeToTree(rootView, accentColor);
        updateGraphicsPresetUI();
        updateScreenshotModeUI();
        updateHudEditorButtonUI();
        gameDataController.updateFilterUI();
        gameDataController.updateDownloadButtonUI();
        artworkCacheController.updateTheme(accentColor);
        screenshotController.notifyThemeChanged();
    }

    private void initViews(View view) {

        et_settings_nickname = view.findViewById(R.id.et_settings_nickname);
        samp_version = view.findViewById(R.id.samp_version);
        game_version = view.findViewById(R.id.game_version);
        charset = view.findViewById(R.id.charset);
        game_version_layout = view.findViewById(R.id.game_version_layout);

        slider_theme_hue = view.findViewById(R.id.slider_theme_hue);
        tv_theme_hue_val = view.findViewById(R.id.tv_theme_hue_val);
        if (slider_theme_hue != null) {
            slider_theme_hue.setRainbowMode(true);
        }

        btn_gfx_preset_low = view.findViewById(R.id.btn_gfx_preset_low);
        btn_gfx_preset_med = view.findViewById(R.id.btn_gfx_preset_med);
        btn_gfx_preset_high = view.findViewById(R.id.btn_gfx_preset_high);
        btn_gfx_preset_ultra = view.findViewById(R.id.btn_gfx_preset_ultra);

        btn_open_hud_editor = view.findViewById(R.id.btn_open_hud_editor);
        btn_reset_hud_editor = view.findViewById(R.id.btn_reset_hud_editor);

        dropdown_aspect_ratio = view.findViewById(R.id.dropdown_aspect_ratio);
        dropdown_visual_fx = view.findViewById(R.id.dropdown_visual_fx);
        dropdown_shadows = view.findViewById(R.id.dropdown_shadows);
        dropdown_car_reflections = view.findViewById(R.id.dropdown_car_reflections);
        dropdown_traffic_mode = view.findViewById(R.id.dropdown_traffic_mode);
        dropdown_ped_cam_zoom = view.findViewById(R.id.dropdown_ped_cam_zoom);
        dropdown_veh_cam_height = view.findViewById(R.id.dropdown_veh_cam_height);
        dropdown_targeting_mode = view.findViewById(R.id.dropdown_targeting_mode);
        dropdown_touch_steering = view.findViewById(R.id.dropdown_touch_steering);
        dropdown_touch_layout = view.findViewById(R.id.dropdown_touch_layout);
        dropdown_accelerometer = view.findViewById(R.id.dropdown_accelerometer);
        dropdown_radio_station = view.findViewById(R.id.dropdown_radio_station);
        dropdown_voice_mic_mode = view.findViewById(R.id.dropdown_voice_mic_mode);
        dropdown_crosshair_style = view.findViewById(R.id.dropdown_crosshair_style);
        dropdown_font_family = view.findViewById(R.id.dropdown_font_family);

        voice = view.findViewById(R.id.voice_switch);
        timestamp = view.findViewById(R.id.timestamp_switch);
        fullscreen = view.findViewById(R.id.fullscreen_switch);
        displayfps = view.findViewById(R.id.displayfps_switch);
        systemkeyboard = view.findViewById(R.id.systemkeyboard_switch);
        modify_files = view.findViewById(R.id.modify_switch);
        fastconnect = view.findViewById(R.id.fastconnect_switch);

        headmove_switch = view.findViewById(R.id.headmove_switch);
        dlmodels_switch = view.findViewById(R.id.dlmodels_switch);
        switch_invert_look = view.findViewById(R.id.switch_invert_look);
        switch_auto_climb = view.findViewById(R.id.switch_auto_climb);
        switch_frame_limiter = view.findViewById(R.id.switch_frame_limiter);
        switch_free_cam = view.findViewById(R.id.switch_free_cam);
        switch_first_person_look = view.findViewById(R.id.switch_first_person_look);
        switch_bike_steering = view.findViewById(R.id.switch_bike_steering);
        switch_vibration = view.findViewById(R.id.switch_vibration);
        switch_radio_autotune = view.findViewById(R.id.switch_radio_autotune);

        switch_km_enabled = view.findViewById(R.id.switch_km_enabled);
        switch_joypad_invert_x = view.findViewById(R.id.switch_joypad_invert_x);
        switch_joypad_invert_y = view.findViewById(R.id.switch_joypad_invert_y);

        switch_voice_mic_enable = view.findViewById(R.id.switch_voice_mic_enable);
        switch_voice_speaker_list = view.findViewById(R.id.switch_voice_speaker_list);
        switch_voice_balancer = view.findViewById(R.id.switch_voice_balancer);
        switch_voice_filter = view.findViewById(R.id.switch_voice_filter);

        switch_show_gta_hud = view.findViewById(R.id.switch_show_gta_hud);
        switch_show_radar = view.findViewById(R.id.switch_show_radar);
        switch_radar_tap_map = view.findViewById(R.id.switch_radar_tap_map);
        switch_show_nametag_weapon = view.findViewById(R.id.switch_show_nametag_weapon);
        switch_show_death_window = view.findViewById(R.id.switch_show_death_window);
        switch_show_nametags = view.findViewById(R.id.switch_show_nametags);
        switch_show_button_panel = view.findViewById(R.id.switch_show_button_panel);
        switch_show_screenshot_btn = view.findViewById(R.id.switch_show_screenshot_btn);
        switch_gta_subtitles = view.findViewById(R.id.switch_gta_subtitles);
        switch_chat_shadow = view.findViewById(R.id.switch_chat_shadow);

        restore_button = view.findViewById(R.id.restore_button);
        chat_minus = view.findViewById(R.id.chat_minus);
        chat_strings = view.findViewById(R.id.chat_strings);
        chat_plus = view.findViewById(R.id.chat_plus);
        fps_limit = view.findViewById(R.id.fps_limit);

        font_size_slider = view.findViewById(R.id.fontsize_slider);
        font_size_value = view.findViewById(R.id.fontsize_value);
        chatposx_slider = view.findViewById(R.id.chatposx_slider);
        chatposx_value = view.findViewById(R.id.chatposx_value);
        chatposy_slider = view.findViewById(R.id.chatposy_slider);
        chatposy_value = view.findViewById(R.id.chatposy_value);
        chatsizex_slider = view.findViewById(R.id.chatsizex_slider);
        chatsizex_value = view.findViewById(R.id.chatsizex_value);
        chatsizey_slider = view.findViewById(R.id.chatsizey_slider);
        chatsizey_value = view.findViewById(R.id.chatsizey_value);

        slider_resolution = view.findViewById(R.id.slider_resolution);
        tv_resolution_val = view.findViewById(R.id.tv_resolution_val);
        slider_draw_distance = view.findViewById(R.id.slider_draw_distance);
        tv_draw_distance_val = view.findViewById(R.id.tv_draw_distance_val);
        slider_brightness = view.findViewById(R.id.slider_brightness);
        tv_brightness_val = view.findViewById(R.id.tv_brightness_val);

        slider_cam_sens_x = view.findViewById(R.id.slider_cam_sens_x);
        tv_cam_sens_x_val = view.findViewById(R.id.tv_cam_sens_x_val);
        slider_cam_sens_y = view.findViewById(R.id.slider_cam_sens_y);
        tv_cam_sens_y_val = view.findViewById(R.id.tv_cam_sens_y_val);
        slider_cam_sens_foot = view.findViewById(R.id.slider_cam_sens_foot);
        tv_cam_sens_foot_val = view.findViewById(R.id.tv_cam_sens_foot_val);
        slider_cam_sens_veh = view.findViewById(R.id.slider_cam_sens_veh);
        tv_cam_sens_veh_val = view.findViewById(R.id.tv_cam_sens_veh_val);
        slider_cam_sens_aim = view.findViewById(R.id.slider_cam_sens_aim);
        tv_cam_sens_aim_val = view.findViewById(R.id.tv_cam_sens_aim_val);

        slider_joystick_size = view.findViewById(R.id.slider_joystick_size);
        tv_joystick_size_val = view.findViewById(R.id.tv_joystick_size_val);
        slider_shoot_threshold = view.findViewById(R.id.slider_shoot_threshold);
        tv_shoot_threshold_val = view.findViewById(R.id.tv_shoot_threshold_val);
        slider_analog_sens = view.findViewById(R.id.slider_analog_sens);
        tv_analog_sens_val = view.findViewById(R.id.tv_analog_sens_val);
        slider_flick_sens = view.findViewById(R.id.slider_flick_sens);
        tv_flick_sens_val = view.findViewById(R.id.tv_flick_sens_val);

        slider_sfx_volume = view.findViewById(R.id.slider_sfx_volume);
        tv_sfx_volume_val = view.findViewById(R.id.tv_sfx_volume_val);
        slider_music_volume = view.findViewById(R.id.slider_music_volume);
        tv_music_volume_val = view.findViewById(R.id.tv_music_volume_val);
        slider_voice_volume = view.findViewById(R.id.slider_voice_volume);
        tv_voice_volume_val = view.findViewById(R.id.tv_voice_volume_val);
        slider_voice_mic_volume = view.findViewById(R.id.slider_voice_mic_volume);
        tv_voice_mic_volume_val = view.findViewById(R.id.tv_voice_mic_volume_val);
        slider_voice_btn_size = view.findViewById(R.id.slider_voice_btn_size);
        tv_voice_btn_size_val = view.findViewById(R.id.tv_voice_btn_size_val);

        slider_nametag_size = view.findViewById(R.id.slider_nametag_size);
        tv_nametag_size_val = view.findViewById(R.id.tv_nametag_size_val);
        slider_textlabel_size = view.findViewById(R.id.slider_textlabel_size);
        tv_textlabel_size_val = view.findViewById(R.id.tv_textlabel_size_val);

        // Screenshots
        btn_ss_mode_hud = view.findViewById(R.id.btn_ss_mode_hud);
        btn_ss_mode_game = view.findViewById(R.id.btn_ss_mode_game);
        screenshotController.bindViews(view);

        // Report an issue
        diagnosticsController.bindViews(view);

        // Help & about
        tv_help_mods_body = view.findViewById(R.id.tv_help_mods_body);
        tv_help_faq_body = view.findViewById(R.id.tv_help_faq_body);
        tv_help_commands = view.findViewById(R.id.tv_help_commands);
        tv_about_version_line = view.findViewById(R.id.tv_about_version_line);
        tv_about_summary = view.findViewById(R.id.tv_about_summary);

        // File verification & game data tab
        gameDataController.bindViews(view);

        // 0.3.DL custom model cache management
        artworkCacheController.bindViews(view);
    }

    private void setupCategoryNavigation(View view) {
        categoryPanels = new View[]{
                view.findViewById(R.id.panel_multiplayer),
                view.findViewById(R.id.panel_gamedata),
                view.findViewById(R.id.panel_graphics),
                view.findViewById(R.id.panel_camera),
                view.findViewById(R.id.panel_controls),
                view.findViewById(R.id.panel_audio),
                view.findViewById(R.id.panel_hud_chat),
                view.findViewById(R.id.panel_screenshots),
                view.findViewById(R.id.panel_logs),
                view.findViewById(R.id.panel_help_about)
        };

        settingsTabs = view.findViewById(R.id.settings_tabs);
        settingsTabs.setTabs(CATEGORY_TITLES);
        settingsTabs.setOnTabSelectedListener((index, reselected) -> {
            if (reselected) {
                if (categoryPanels[index] instanceof android.widget.ScrollView) {
                    ((android.widget.ScrollView) categoryPanels[index]).smoothScrollTo(0, 0);
                }
                return;
            }
            switchCategory(index);
            View panel = categoryPanels[index];
            panel.animate().cancel();
            panel.setAlpha(0f);
            panel.setTranslationY(8f * getResources().getDisplayMetrics().density);
            panel.animate().alpha(1f).translationY(0f).setDuration(180).start();
        });
        switchCategory(CAT_MULTIPLAYER);
    }

    private void switchCategory(int catIdx) {
        currentCategory = catIdx;

        for (int i = 0; i < categoryPanels.length; i++) {
            categoryPanels[i].setVisibility((i == catIdx) ? View.VISIBLE : View.GONE);
        }

        if (catIdx == CAT_MULTIPLAYER) {
            artworkCacheController.refresh();
        } else if (catIdx == CAT_GAMEDATA) {
            gameDataController.refreshVerification();
        } else if (catIdx == CAT_SCREENSHOTS) {
            screenshotController.refresh();
        } else if (catIdx == CAT_LOGS) {
            diagnosticsController.refresh();
        } else if (catIdx == CAT_HELP_ABOUT) {
            populateHelpAndAbout();
        }
    }

    private void setupListeners(View view) {
        et_settings_nickname.setText(settings.getString("nick_name"));
        et_settings_nickname.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {}

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {}

            @Override
            public void afterTextChanged(Editable s) {
                if (updatingWidgets) return;
                String nick = s.toString().trim();
                if (!nick.isEmpty()) {
                    savePreference("nick_name", nick);
                }
            }
        });

        // Launcher theme color presets and custom sliders
        int[][] themePresets = {
                {R.id.btn_theme_gold, 38},
                {R.id.btn_theme_cyan, 186},
                {R.id.btn_theme_green, 145},
                {R.id.btn_theme_red, 355},
                {R.id.btn_theme_purple, 271},
                {R.id.btn_theme_blue, 216}
        };
        for (int[] preset : themePresets) {
            View btn = view.findViewById(preset[0]);
            final int hue = preset[1];
            if (btn != null) {
                btn.setOnClickListener(v -> {
                    if (slider_theme_hue != null) slider_theme_hue.setValue(hue);
                    if (tv_theme_hue_val != null) tv_theme_hue_val.setText(hue + "°");
                    LauncherThemeManager.setHue(requireContext(), hue);
                });
            }
        }
        if (slider_theme_hue != null) {
            int savedHue = LauncherThemeManager.getHue(requireContext());
            slider_theme_hue.setValue(savedHue);
            if (tv_theme_hue_val != null) tv_theme_hue_val.setText(savedHue + "°");
            slider_theme_hue.addOnChangeListener((s, value, fromUser) -> {
                int hue = Math.round(value);
                if (tv_theme_hue_val != null) tv_theme_hue_val.setText(hue + "°");
                if (fromUser) {
                    LauncherThemeManager.setHue(requireContext(), hue);
                }
            });
        }

        bindStringOptionDropdown(samp_version, ClientSettings.SAMP_VERSIONS, "samp_version");
        bindStringOptionDropdown(charset, ClientSettings.CHARSETS, "charset");
        bindIntOptionDropdown(fps_limit, ClientSettings.FPS_LIMITS, "fps_limit", false);

        if (game_version_layout != null) {
            game_version_layout.setVisibility(View.GONE);
        }

        bindStringOptionDropdown(dropdown_aspect_ratio, ClientSettings.ASPECT_RATIOS, "aspect_ratio");
        bindIntOptionDropdown(dropdown_visual_fx, ClientSettings.VISUAL_FX_OPTIONS, "gta_visual_fx", true);
        bindIntOptionDropdown(dropdown_shadows, ClientSettings.SHADOWS_OPTIONS, "gta_shadows", true);
        bindIntOptionDropdown(dropdown_car_reflections, ClientSettings.CAR_REFL_OPTIONS, "gta_car_reflections", true);
        bindIntOptionDropdown(dropdown_traffic_mode, ClientSettings.TRAFFIC_OPTIONS, "gta_traffic_mode", false);
        bindIntOptionDropdown(dropdown_veh_cam_height, ClientSettings.VEH_CAM_OPTIONS, "veh_cam_height", false);
        bindIntOptionDropdown(dropdown_ped_cam_zoom, ClientSettings.PED_ZOOM_OPTIONS, "ped_cam_zoom", false);
        bindIntOptionDropdown(dropdown_targeting_mode, ClientSettings.TARGETING_OPTIONS, "targeting_mode", false);
        bindIntOptionDropdown(dropdown_touch_steering, ClientSettings.STEERING_OPTIONS, "touch_steering", false);
        bindIntOptionDropdown(dropdown_touch_layout, ClientSettings.LAYOUT_OPTIONS, "touch_layout", false);
        bindIntOptionDropdown(dropdown_accelerometer, ClientSettings.ACCEL_OPTIONS, "accelerometer", false);
        bindIntOptionDropdown(dropdown_radio_station, ClientSettings.RADIO_STATION_OPTIONS, "radio_station", false);
        bindStringOptionDropdown(dropdown_voice_mic_mode, ClientSettings.VOICE_MIC_MODES, "voice_mic_mode");
        bindStringOptionDropdown(dropdown_crosshair_style, ClientSettings.CROSSHAIR_STYLES, "crosshair_style");
        bindStringOptionDropdown(dropdown_font_family, ClientSettings.FONT_FAMILIES, "font_family");

        // Four graphics presets
        btn_gfx_preset_low.setOnClickListener(v -> applyGraphicsPreset(0, 0, 0, 60, 40, "Battery saver"));
        btn_gfx_preset_med.setOnClickListener(v -> applyGraphicsPreset(1, 1, 1, 80, 60, "Balanced"));
        btn_gfx_preset_high.setOnClickListener(v -> applyGraphicsPreset(2, 1, 2, 100, 75, "HD"));
        btn_gfx_preset_ultra.setOnClickListener(v -> applyGraphicsPreset(3, 2, 3, 100, 100, "Ultra"));

        // Custom game button positions (Change Button Position)
        if (btn_open_hud_editor != null) {
            btn_open_hud_editor.setOnClickListener(v -> {
                boolean next = !settings.getBoolean("open_hud_edit_on_start");
                savePreference("open_hud_edit_on_start", next);
                updateHudEditorButtonUI();
                if (next) {
                    Toast.makeText(requireContext(), "Enabled: the button position editor will open automatically next time you enter the game (you can also tap the HUD key at the bottom in game or type /hudedit anytime)", Toast.LENGTH_LONG).show();
                } else {
                    Toast.makeText(requireContext(), "Auto-open button position editor disabled", Toast.LENGTH_SHORT).show();
                }
            });
        }
        if (btn_reset_hud_editor != null) {
            btn_reset_hud_editor.setOnClickListener(v -> {
                showAlertDialog("Delete the custom button layout file (Adjustable.cfg) and restore the default GTA:SA button positions?", (d, w) -> {
                    File adjFile = new File(requireContext().getExternalFilesDir(null), "Adjustable.cfg");
                    if (adjFile.exists()) {
                        adjFile.delete();
                    }
                    savePreference("open_hud_edit_on_start", false);
                    updateHudEditorButtonUI();
                    Toast.makeText(requireContext(), "Default game button positions restored", Toast.LENGTH_SHORT).show();
                    d.dismiss();
                }, (d, w) -> d.dismiss());
            });
        }

        // Reset chat box layout
        view.findViewById(R.id.btn_reset_chat_layout).setOnClickListener(v -> {
            Map<String, Object> batch = new HashMap<>();
            batch.put("font_size", 25.0f);
            batch.put("chat_strings", 8);
            batch.put("chat_posx", 100);
            batch.put("chat_posy", 20);
            batch.put("chat_sizex", 360);
            batch.put("chat_sizey", 110);
            ClientSettings.updateBatchSync(requireContext(), batch);
            settings = ClientSettings.loadLauncher(requireContext());
            setWidgetsForSettings();
            Toast.makeText(requireContext(), "Chat window layout and font size restored to defaults", Toast.LENGTH_SHORT).show();
        });

        // Screenshot mode toggle
        btn_ss_mode_hud.setOnClickListener(v -> {
            savePreference("screenshot_mode", ClientSettings.SCREENSHOT_MODE_HUD);
            updateScreenshotModeUI();
            Toast.makeText(requireContext(), "Set to: With UI & chat", Toast.LENGTH_SHORT).show();
        });
        btn_ss_mode_game.setOnClickListener(v -> {
            savePreference("screenshot_mode", ClientSettings.SCREENSHOT_MODE_GAME);
            updateScreenshotModeUI();
            Toast.makeText(requireContext(), "Set to: Game view only", Toast.LENGTH_SHORT).show();
        });
        view.findViewById(R.id.btn_ss_refresh).setOnClickListener(v -> screenshotController.refresh());
        view.findViewById(R.id.btn_ss_clear_all).setOnClickListener(v -> screenshotController.confirmClear());

        // Report-an-issue action buttons
        view.findViewById(R.id.btn_log_refresh).setOnClickListener(v -> {
            diagnosticsController.refresh();
            Toast.makeText(requireContext(), "Run status check refreshed", Toast.LENGTH_SHORT).show();
        });
        view.findViewById(R.id.btn_log_copy_report).setOnClickListener(v -> diagnosticsController.copyReport());
        view.findViewById(R.id.btn_log_share).setOnClickListener(v -> diagnosticsController.shareLogFile());
        view.findViewById(R.id.btn_log_clear).setOnClickListener(v -> diagnosticsController.confirmClearLogs());

        setupSwitchListeners();
        setupRestoreButton();
        setupChatButtons();
        setupSliders();
        updateGraphicsPresetUI();
        updateScreenshotModeUI();
        updateHudEditorButtonUI();
    }

    ArrayAdapter<String> createDropdownAdapter(final String[] items) {
        return new ArrayAdapter<String>(requireContext(), R.layout.layout_setting_version, new ArrayList<>(Arrays.asList(items))) {
            private final Filter noOpFilter = new Filter() {
                @Override
                protected FilterResults performFiltering(CharSequence constraint) {
                    FilterResults results = new FilterResults();
                    results.values = Arrays.asList(items);
                    results.count = items.length;
                    return results;
                }

                @Override
                protected void publishResults(CharSequence constraint, FilterResults results) {
                    clear();
                    addAll(items);
                    notifyDataSetChanged();
                }
            };

            @NonNull
            @Override
            public Filter getFilter() {
                return noOpFilter;
            }
        };
    }

    private void bindStringOptionDropdown(GtaOptionView view, ClientSettings.StringOption[] options, String key) {
        if (view == null) return;
        ArrayAdapter<String> adapter = createDropdownAdapter(ClientSettings.labelsOf(options));
        view.setAdapter(adapter);
        view.setThreshold(0);
        view.setText(ClientSettings.labelForStringOption(options, settings.getString(key)), false);
        view.setOnClickListener(v -> view.showDropDown());
        view.setOnItemClickListener((av, v, i, l) -> {
            if (i >= 0 && i < options.length) {
                savePreference(key, options[i].id);
            }
        });
    }

    private void bindIntOptionDropdown(GtaOptionView view, ClientSettings.IntOption[] options, String key, boolean syncPreset) {
        if (view == null) return;
        ArrayAdapter<String> adapter = createDropdownAdapter(ClientSettings.labelsOf(options));
        view.setAdapter(adapter);
        view.setThreshold(0);
        view.setText(ClientSettings.labelForIntOption(options, settings.getInt(key)), false);
        view.setOnClickListener(v -> view.showDropDown());
        view.setOnItemClickListener((av, v, i, l) -> {
            if (i >= 0 && i < options.length) {
                savePreference(key, options[i].value);
                if (syncPreset) {
                    updateGraphicsPresetUI();
                }
            }
        });
    }

    private void applyGraphicsPreset(int vfx, int shadows, int carRefl, int res, int dd, String label) {
        Map<String, Object> batch = new HashMap<>();
        batch.put("gta_visual_fx", vfx);
        batch.put("gta_shadows", shadows);
        batch.put("gta_car_reflections", carRefl);
        batch.put("gta_resolution", res);
        batch.put("gta_draw_distance", dd);
        ClientSettings.updateBatchSync(requireContext(), batch);
        settings = ClientSettings.loadLauncher(requireContext());
        setWidgetsForSettings();
        Toast.makeText(requireContext(), "Graphics preset applied: " + label, Toast.LENGTH_SHORT).show();
    }

    private void updateGraphicsPresetUI() {
        if (btn_gfx_preset_low == null || !isAdded()) return;
        int vfx = settings.getInt("gta_visual_fx");
        int shd = settings.getInt("gta_shadows");
        int refl = settings.getInt("gta_car_reflections");
        int res = settings.getInt("gta_resolution");
        int dd = settings.getInt("gta_draw_distance");

        int activePreset = -1;
        if (vfx == 0 && shd == 0 && refl == 0 && res == 60 && dd == 40) activePreset = 0;
        else if (vfx == 1 && shd == 1 && refl == 1 && res == 80 && dd == 60) activePreset = 1;
        else if (vfx == 2 && shd == 1 && refl == 2 && res == 100 && dd == 75) activePreset = 2;
        else if (vfx == 3 && shd == 2 && refl == 3 && res == 100 && dd == 100) activePreset = 3;

        TextView[] btns = {btn_gfx_preset_low, btn_gfx_preset_med, btn_gfx_preset_high, btn_gfx_preset_ultra};
        int accentColor = LauncherThemeManager.getAccentColor(requireContext());
        int colorOnAccent = requireContext().getResources().getColor(R.color.launcher_on_accent);
        int colorText = requireContext().getResources().getColor(R.color.launcher_text);

        for (int i = 0; i < btns.length; i++) {
            boolean selected = (i == activePreset);
            btns[i].setBackgroundResource(selected ? R.drawable.launcher_btn_primary : R.drawable.launcher_btn_outline);
            btns[i].setBackgroundTintList(selected ? ColorStateList.valueOf(accentColor) : null);
            btns[i].setTextColor(selected ? colorOnAccent : colorText);
        }
    }

    private void updateHudEditorButtonUI() {
        if (btn_open_hud_editor == null || !isAdded()) return;
        boolean scheduled = settings.getBoolean("open_hud_edit_on_start");
        int accentColor = LauncherThemeManager.getAccentColor(requireContext());
        btn_open_hud_editor.setBackgroundTintList(ColorStateList.valueOf(accentColor));
        btn_open_hud_editor.setText(scheduled
                ? "✓ Scheduled: the button editor will open automatically next time you enter the game"
                : "Open button position editor automatically when entering the game");
    }

    private void setupSliders() {
        bindSlider(font_size_slider, font_size_value, "font_size", true, "", false);
        bindSlider(chatposx_slider, chatposx_value, "chat_posx", false, "", false);
        bindSlider(chatposy_slider, chatposy_value, "chat_posy", false, "", false);
        bindSlider(chatsizex_slider, chatsizex_value, "chat_sizex", false, "", false);
        bindSlider(chatsizey_slider, chatsizey_value, "chat_sizey", false, "", false);

        bindSlider(slider_resolution, tv_resolution_val, "gta_resolution", false, "%", true);
        bindSlider(slider_draw_distance, tv_draw_distance_val, "gta_draw_distance", false, "%", true);
        bindSlider(slider_brightness, tv_brightness_val, "gta_brightness", false, "%", false);

        bindSlider(slider_cam_sens_x, tv_cam_sens_x_val, "cam_sens_x", false, "%", false);
        bindSlider(slider_cam_sens_y, tv_cam_sens_y_val, "cam_sens_y", false, "%", false);
        bindSlider(slider_cam_sens_foot, tv_cam_sens_foot_val, "cam_sens_foot", false, "%", false);
        bindSlider(slider_cam_sens_veh, tv_cam_sens_veh_val, "cam_sens_veh", false, "%", false);
        bindSlider(slider_cam_sens_aim, tv_cam_sens_aim_val, "cam_sens_aim", false, "%", false);

        bindSlider(slider_joystick_size, tv_joystick_size_val, "joystick_size", false, "%", false);
        bindSlider(slider_shoot_threshold, tv_shoot_threshold_val, "shoot_threshold", false, "%", false);
        bindSlider(slider_analog_sens, tv_analog_sens_val, "analog_sensitivity", false, "%", false);
        bindSlider(slider_flick_sens, tv_flick_sens_val, "flick_sensitivity", false, "%", false);

        bindSlider(slider_sfx_volume, tv_sfx_volume_val, "sfx_volume", false, "%", false);
        bindSlider(slider_music_volume, tv_music_volume_val, "music_volume", false, "%", false);
        bindSlider(slider_voice_volume, tv_voice_volume_val, "voice_volume", false, "%", false);
        bindSlider(slider_voice_mic_volume, tv_voice_mic_volume_val, "voice_mic_volume", false, "%", false);
        bindSlider(slider_voice_btn_size, tv_voice_btn_size_val, "voice_btn_size", false, "%", false);

        bindSlider(slider_nametag_size, tv_nametag_size_val, "nametag_size", false, "%", false);
        bindSlider(slider_textlabel_size, tv_textlabel_size_val, "textlabel_size", false, "%", false);
    }

    private void bindSlider(GtaSaSliderView slider, TextView valueView, String key,
                            boolean asFloat, String suffix, boolean syncPreset) {
        if (slider == null || valueView == null) return;
        slider.clearOnChangeListeners();
        slider.addOnChangeListener((s, value, fromUser) -> {
            valueView.setText(Math.round(value) + suffix);
            if (fromUser && !updatingWidgets) {
                if (asFloat) savePreference(key, value);
                else savePreference(key, Math.round(value));
                if (syncPreset) {
                    updateGraphicsPresetUI();
                }
            }
        });
        syncSlider(slider, valueView, key, asFloat, suffix);
    }

    private void syncSlider(GtaSaSliderView slider, TextView valueView, String key, boolean asFloat, String suffix) {
        if (slider == null || valueView == null) return;
        float current = asFloat ? settings.getFloat(key) : settings.getInt(key);
        float clamped = Math.min(Math.max(current, slider.getValueFrom()), slider.getValueTo());
        float step = slider.getStepSize();
        if (step > 0) {
            clamped = slider.getValueFrom() + Math.round((clamped - slider.getValueFrom()) / step) * step;
            clamped = Math.min(Math.max(clamped, slider.getValueFrom()), slider.getValueTo());
        }
        slider.setValue(clamped);
        valueView.setText(Math.round(clamped) + suffix);
    }

    private Map<CompoundButton, String> buildSwitchMap() {
        Map<CompoundButton, String> map = new HashMap<>();
        map.put(systemkeyboard, "system_keyboard");
        map.put(timestamp, "timestamp");
        map.put(fullscreen, "fullscreen");
        map.put(displayfps, "display_fps");
        map.put(voice, "voice_chat");
        map.put(fastconnect, "fast_connect");
        map.put(headmove_switch, "head_move");
        map.put(dlmodels_switch, "dl_custom_models");
        map.put(switch_invert_look, "invert_look");
        map.put(switch_auto_climb, "auto_climb");
        map.put(switch_frame_limiter, "frame_limiter");
        map.put(switch_free_cam, "free_cam");
        map.put(switch_first_person_look, "first_person_look");
        map.put(switch_bike_steering, "bike_steering");
        map.put(switch_vibration, "vibration");
        map.put(switch_radio_autotune, "radio_autotune");
        map.put(switch_km_enabled, "km_enabled");
        map.put(switch_joypad_invert_x, "joypad_invert_x");
        map.put(switch_joypad_invert_y, "joypad_invert_y");
        map.put(switch_voice_mic_enable, "voice_mic_enable");
        map.put(switch_voice_speaker_list, "voice_speaker_list");
        map.put(switch_voice_balancer, "voice_balancer");
        map.put(switch_voice_filter, "voice_filter");
        map.put(switch_show_gta_hud, "show_gta_hud");
        map.put(switch_show_radar, "show_radar");
        map.put(switch_radar_tap_map, "radar_tap_map");
        map.put(switch_show_nametag_weapon, "show_nametag_weapon");
        map.put(switch_show_death_window, "show_death_window");
        map.put(switch_show_nametags, "show_nametags");
        map.put(switch_show_button_panel, "show_button_panel");
        map.put(switch_show_screenshot_btn, "show_screenshot_btn");
        map.put(switch_gta_subtitles, "gta_subtitles");
        map.put(switch_chat_shadow, "chat_shadow");
        return map;
    }

    private void setupSwitchListeners() {
        for (Map.Entry<CompoundButton, String> entry : buildSwitchMap().entrySet()) {
            setupSwitch(entry.getKey(), entry.getValue());
        }
        setupSwitchWithWarning(modify_files);
    }

    private void setupSwitch(CompoundButton switchButton, String key) {
        if (switchButton == null) return;
        switchButton.setOnCheckedChangeListener(null);
        switchButton.setChecked(settings.getBoolean(key));
        switchButton.setOnCheckedChangeListener((buttonView, isChecked) -> {
            if (!updatingWidgets) {
                savePreference(key, isChecked);
            }
        });
    }

    private void setupSwitchWithWarning(CompoundButton switchButton) {
        if (switchButton == null) return;
        switchButton.setOnCheckedChangeListener(null);
        switchButton.setChecked(settings.getBoolean("modify_files"));

        switchButton.setOnCheckedChangeListener((buttonView, isChecked) -> {
            if (updatingWidgets) return;
            if (isChecked && buttonView.isPressed()) {
                showAlertDialog("With this on, the launcher skips the original-file integrity check. If the game breaks after manually replacing data files, turn this off.",
                        (dialog, which) -> dialog.dismiss(), (dialog, which) -> {
                            switchButton.setChecked(false);
                            dialog.dismiss();
                        });
            }
            savePreference("modify_files", isChecked);
        });
    }

    private void setupRestoreButton() {
        restore_button.setOnClickListener(v -> showAlertDialog("Reset all online, graphics, camera, controls, audio, and UI settings to defaults?", (dialog, which) -> {
            settings = ClientSettings.resetDefaults(requireContext());
            LauncherThemeManager.setHue(requireContext(), LauncherThemeManager.DEFAULT_HUE);
            setWidgetsForSettings();
            Toast.makeText(requireContext(), "All settings restored to defaults", Toast.LENGTH_SHORT).show();
            dialog.dismiss();
        }, (dialog, which) -> dialog.dismiss()));
    }

    private void setupChatButtons() {
        chat_minus.setOnClickListener(v -> adjustChatStrings(-1));
        chat_plus.setOnClickListener(v -> adjustChatStrings(1));
    }

    private void adjustChatStrings(int delta) {
        int currentValue = settings.getInt("chat_strings");
        int newValue = currentValue + delta;
        if (newValue < 5 || newValue > 20) return;

        chat_strings.setText(String.valueOf(newValue));
        savePreference("chat_strings", newValue);
    }

    private void setWidgetsForSettings() {
        updatingWidgets = true;
        try {
            et_settings_nickname.setText(settings.getString("nick_name"));

            for (Map.Entry<CompoundButton, String> entry : buildSwitchMap().entrySet()) {
                if (entry.getKey() != null) {
                    entry.getKey().setChecked(settings.getBoolean(entry.getValue()));
                }
            }
            modify_files.setChecked(settings.getBoolean("modify_files"));
            chat_strings.setText(String.valueOf(settings.getInt("chat_strings")));

            int hue = LauncherThemeManager.getHue(requireContext());
            if (slider_theme_hue != null) slider_theme_hue.setValue(hue);
            if (tv_theme_hue_val != null) tv_theme_hue_val.setText(hue + "°");

            samp_version.setText(ClientSettings.labelForStringOption(ClientSettings.SAMP_VERSIONS, settings.getString("samp_version")), false);
            charset.setText(ClientSettings.labelForStringOption(ClientSettings.CHARSETS, settings.getString("charset")), false);
            fps_limit.setText(ClientSettings.labelForIntOption(ClientSettings.FPS_LIMITS, settings.getInt("fps_limit")), false);

            setStringDropdownText(dropdown_aspect_ratio, ClientSettings.ASPECT_RATIOS, "aspect_ratio");
            setIntDropdownText(dropdown_visual_fx, ClientSettings.VISUAL_FX_OPTIONS, "gta_visual_fx");
            setIntDropdownText(dropdown_shadows, ClientSettings.SHADOWS_OPTIONS, "gta_shadows");
            setIntDropdownText(dropdown_car_reflections, ClientSettings.CAR_REFL_OPTIONS, "gta_car_reflections");
            setIntDropdownText(dropdown_traffic_mode, ClientSettings.TRAFFIC_OPTIONS, "gta_traffic_mode");
            setIntDropdownText(dropdown_ped_cam_zoom, ClientSettings.PED_ZOOM_OPTIONS, "ped_cam_zoom");
            setIntDropdownText(dropdown_veh_cam_height, ClientSettings.VEH_CAM_OPTIONS, "veh_cam_height");
            setIntDropdownText(dropdown_targeting_mode, ClientSettings.TARGETING_OPTIONS, "targeting_mode");
            setIntDropdownText(dropdown_touch_steering, ClientSettings.STEERING_OPTIONS, "touch_steering");
            setIntDropdownText(dropdown_touch_layout, ClientSettings.LAYOUT_OPTIONS, "touch_layout");
            setIntDropdownText(dropdown_accelerometer, ClientSettings.ACCEL_OPTIONS, "accelerometer");
            setIntDropdownText(dropdown_radio_station, ClientSettings.RADIO_STATION_OPTIONS, "radio_station");
            setStringDropdownText(dropdown_voice_mic_mode, ClientSettings.VOICE_MIC_MODES, "voice_mic_mode");
            setStringDropdownText(dropdown_crosshair_style, ClientSettings.CROSSHAIR_STYLES, "crosshair_style");
            setStringDropdownText(dropdown_font_family, ClientSettings.FONT_FAMILIES, "font_family");

            syncSlider(font_size_slider, font_size_value, "font_size", true, "");
            syncSlider(chatposx_slider, chatposx_value, "chat_posx", false, "");
            syncSlider(chatposy_slider, chatposy_value, "chat_posy", false, "");
            syncSlider(chatsizex_slider, chatsizex_value, "chat_sizex", false, "");
            syncSlider(chatsizey_slider, chatsizey_value, "chat_sizey", false, "");

            syncSlider(slider_resolution, tv_resolution_val, "gta_resolution", false, "%");
            syncSlider(slider_draw_distance, tv_draw_distance_val, "gta_draw_distance", false, "%");
            syncSlider(slider_brightness, tv_brightness_val, "gta_brightness", false, "%");

            syncSlider(slider_cam_sens_x, tv_cam_sens_x_val, "cam_sens_x", false, "%");
            syncSlider(slider_cam_sens_y, tv_cam_sens_y_val, "cam_sens_y", false, "%");
            syncSlider(slider_cam_sens_foot, tv_cam_sens_foot_val, "cam_sens_foot", false, "%");
            syncSlider(slider_cam_sens_veh, tv_cam_sens_veh_val, "cam_sens_veh", false, "%");
            syncSlider(slider_cam_sens_aim, tv_cam_sens_aim_val, "cam_sens_aim", false, "%");

            syncSlider(slider_joystick_size, tv_joystick_size_val, "joystick_size", false, "%");
            syncSlider(slider_shoot_threshold, tv_shoot_threshold_val, "shoot_threshold", false, "%");
            syncSlider(slider_analog_sens, tv_analog_sens_val, "analog_sensitivity", false, "%");
            syncSlider(slider_flick_sens, tv_flick_sens_val, "flick_sensitivity", false, "%");

            syncSlider(slider_sfx_volume, tv_sfx_volume_val, "sfx_volume", false, "%");
            syncSlider(slider_music_volume, tv_music_volume_val, "music_volume", false, "%");
            syncSlider(slider_voice_volume, tv_voice_volume_val, "voice_volume", false, "%");
            syncSlider(slider_voice_mic_volume, tv_voice_mic_volume_val, "voice_mic_volume", false, "%");
            syncSlider(slider_voice_btn_size, tv_voice_btn_size_val, "voice_btn_size", false, "%");

            syncSlider(slider_nametag_size, tv_nametag_size_val, "nametag_size", false, "%");
            syncSlider(slider_textlabel_size, tv_textlabel_size_val, "textlabel_size", false, "%");

            updateGraphicsPresetUI();
            updateScreenshotModeUI();
            updateHudEditorButtonUI();
        } finally {
            updatingWidgets = false;
        }
    }

    private void setStringDropdownText(GtaOptionView view, ClientSettings.StringOption[] options, String key) {
        if (view == null) return;
        view.setText(ClientSettings.labelForStringOption(options, settings.getString(key)), false);
    }

    private void setIntDropdownText(GtaOptionView view, ClientSettings.IntOption[] options, String key) {
        if (view == null) return;
        view.setText(ClientSettings.labelForIntOption(options, settings.getInt(key)), false);
    }

    private void updateScreenshotModeUI() {
        if (btn_ss_mode_hud == null || btn_ss_mode_game == null || !isAdded()) return;
        String mode = settings.getString("screenshot_mode");
        int accentColor = LauncherThemeManager.getAccentColor(requireContext());
        int colorOnAccent = requireContext().getResources().getColor(R.color.launcher_on_accent);
        int colorText = requireContext().getResources().getColor(R.color.launcher_text);

        boolean isHud = !ClientSettings.SCREENSHOT_MODE_GAME.equalsIgnoreCase(mode);
        btn_ss_mode_hud.setBackgroundResource(isHud ? R.drawable.launcher_btn_primary : R.drawable.launcher_btn_outline);
        btn_ss_mode_hud.setBackgroundTintList(isHud ? ColorStateList.valueOf(accentColor) : null);
        btn_ss_mode_hud.setTextColor(isHud ? colorOnAccent : colorText);

        btn_ss_mode_game.setBackgroundResource(!isHud ? R.drawable.launcher_btn_primary : R.drawable.launcher_btn_outline);
        btn_ss_mode_game.setBackgroundTintList(!isHud ? ColorStateList.valueOf(accentColor) : null);
        btn_ss_mode_game.setTextColor(!isHud ? colorOnAccent : colorText);
    }

    // ════════════════════════════════════════════════════════════════════════
    //  Help & about
    // ════════════════════════════════════════════════════════════════════════

    private void populateHelpAndAbout() {
        tv_help_mods_body.setText(
                "1. Where do I enable and manage mods?\n"
                + "Go back to the launcher home screen and tap the “Mods” section in the leftmost navigation bar. There you can turn each loader on or off, and manage every mod pack and script individually.\n\n"
                + "2. How do I install vehicle packs, character skins, weapons, and texture/sound mods?\n"
                + "The client supports loose-file replacement with no packing needed. Just put the downloaded mod folder (with .dff models, .txd textures, or sound files) under the modloader/ folder in the game data directory, then enable the asset loader in the “Mods” section on the home screen — it takes effect automatically without touching the original data pack.\n\n"
                + "3. How do I use Lua helper scripts and native extension plugins?\n"
                + "• Lua scripts: put .lua script files in the monetloader/ directory and enable the script loader in the “Mods” section on the home screen;\n"
                + "• Native plugins and graphics patches: turn extension plugins and engine fix patches on with one tap in the “Mods” section on the home screen, and tap “Graphics patch settings” to toggle individual rendering optimizations.\n\n"
                + "4. What do I do if the game crashes after installing a new mod?\n"
                + "Different mods can conflict over model IDs or use outdated scripts. Just turn off the most recently added mod in the “Mods” section on the home screen to get back in game right away; you can also copy the bug report from “Report an issue” on this page and send it to a developer.");

        tv_help_faq_body.setText(
                "1. How do I use free camera, adjust the joystick size, or customize game button positions?\n"
                + "• Free camera: turn on “Free camera (camera won't auto-center)” in “Camera settings” — while walking or driving, the camera stays where you rotated it;\n"
                + "• Per-mode sensitivity: adjust separate camera sensitivity for walking, driving, and aiming in “Camera settings”;\n"
                + "• Custom button positions: in “Controls settings”, tap “Open button position editor automatically when entering the game” (or expand the bottom quick bar in game and tap the HUD key, or type /hudedit) to drag and resize the on-screen buttons like in single-player.\n\n"
                + "2. On foot, how do I switch camera distance, aim, block, or ride as a passenger?\n"
                + "• Switch camera distance: tap the camera icon at the top-right of the game screen to cycle near, standard, and far views; you can also set the default distance in “Camera settings”;\n"
                + "• Aim and shoot: while holding a gun, tap the separate crosshair button on the right to aim; tap the fire button for single shots, hold it for continuous fire;\n"
                + "• Melee block: while unarmed or holding a melee weapon, hold the block button to stay in a defensive stance;\n"
                + "• Ride as passenger: near a vehicle, tap the G key icon that appears on the right of the screen, or expand the bottom function keyboard and tap G.\n\n"
                + "3. How do I fix garbled Chinese text after joining a server?\n"
                + "In “Online settings”, change “Server text encoding”: most Chinese servers use GBK, some newer servers use UTF-8, and Hong Kong/Taiwan servers often use BIG5.\n\n"
                + "4. How do I take a clean game screenshot with no chat box or UI buttons?\n"
                + "In “Screenshots”, set the screenshot range to “Game view only”, or type /ss game in the in-game chat to capture a clean shot with all UI and buttons hidden.");

        tv_help_commands.setText(
                "/ss or /screenshot      Take a screenshot now (or tap F8 on the bottom quick keyboard)\n"
                + "/ss game               Force a clean screenshot with no UI or chat\n"
                + "/hudedit               Open the in-game touch button position & size editor\n"
                + "/freecam               Toggle free camera live (camera won't auto-center)\n"
                + "/headmove              Toggle head turning with the camera\n"
                + "/pagesize [5-20]       Change how many chat lines show at once\n"
                + "/timestamp             Toggle receive-time display before chat messages\n"
                + "/clearmychat           Clear the current chat window's history\n"
                + "/save [note]           Save the current player or vehicle coordinates\n"
                + "/dl                    Toggle nearby vehicle IDs and distance display\n"
                + "/dw                    Toggle the kill feed on the right\n"
                + "/q or /quit            Disconnect and quit the game");

        tv_about_version_line.setText(String.format(Locale.US,
                "SA-MP Mobile mobile multiplayer client  ·  Version v%s",
                BuildConfig.VERSION_NAME));

        tv_about_summary.setText(
                "A San Andreas multiplayer launcher and game client built for mobile, with full support for the 0.3.7 and 0.3.DL server protocols, native graphics controls, a clean screenshot library, and modular extension support.\n\n"
                + "Open-source credits: based on Alyn SA-MP Mobile, with thanks to AML / MonetLoader / CLEO and other community open-source projects.");
    }

    // ════════════════════════════════════════════════════════════════════════
    //  Common helper methods
    // ════════════════════════════════════════════════════════════════════════

    void shareFile(File file, String mimeType, String chooserTitle) {
        if (file == null || !file.exists()) {
            Toast.makeText(requireContext(), "File not found", Toast.LENGTH_SHORT).show();
            return;
        }
        try {
            Uri uri = FileProvider.getUriForFile(requireContext(),
                    requireContext().getPackageName() + ".provider", file);
            Intent intent = new Intent(Intent.ACTION_SEND);
            intent.setType(mimeType);
            intent.putExtra(Intent.EXTRA_STREAM, uri);
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(intent, chooserTitle));
        } catch (Exception e) {
            Toast.makeText(requireContext(), "Share failed: " + e.getMessage(), Toast.LENGTH_SHORT).show();
        }
    }

    void copyTextToClipboard(String label, String text) {
        ClipboardManager cm = (ClipboardManager) requireContext().getSystemService(Context.CLIPBOARD_SERVICE);
        if (cm != null) {
            cm.setPrimaryClip(ClipData.newPlainText(label, text));
        }
    }

    private void savePreference(String key, Object value) {
        settings.put(key, value);
        ClientSettings.setAsync(requireContext(), key, value);
    }

    void showAlertDialog(String message, DialogInterface.OnClickListener positiveListener, DialogInterface.OnClickListener negativeListener) {
        new AlertDialog.Builder(requireActivity())
                .setTitle("Notice")
                .setMessage(message)
                .setPositiveButton("OK", positiveListener)
                .setNegativeButton("Cancel", negativeListener)
                .show();
    }
}
