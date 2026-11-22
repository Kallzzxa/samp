package com.sampmobile.launcher;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.app.AppCompatDelegate;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;
import androidx.swiperefreshlayout.widget.SwipeRefreshLayout;

import com.joom.paranoid.Obfuscate;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import com.sampmobile.launcher.ui.GameDataImportDialog;
import com.sampmobile.launcher.ui.adapter.PlayersAdapter;
import com.sampmobile.launcher.ui.adapter.RulesAdapter;
import com.sampmobile.launcher.ui.adapter.ServerAdapter;
import com.sampmobile.launcher.ui.fragment.ModsManagerFragment;
import com.sampmobile.launcher.ui.fragment.SettingsPageFragment;
import com.sampmobile.launcher.utils.GameDataImporter;
import com.sampmobile.launcher.utils.LauncherThemeManager;
import com.sampmobile.launcher.utils.SampQuery;
import com.sampmobile.launcher.utils.ServerModel;
import com.sampmobile.launcher.utils.Utils;

@Obfuscate
public class MainActivity extends AppCompatActivity implements LauncherThemeManager.ThemeListener {

    private ServerAdapter serverAdapter;
    private List<ServerModel> serverModel;
    private SwipeRefreshLayout swipeRefreshLayout;

    private EditText searchEditText;

    private ViewGroup[] tabs;
    private int currentTab = ServerAdapter.MODE_FAVORITES;

    private View infoEmpty;
    private View infoBody;
    private TextView infoServerName;
    private TextView infoServerMeta;
    private View playersHeader;
    private View linksContainer;
    private View discordRow;
    private TextView discordText;
    private View rulesHeader;
    private ImageView btnFavorite;
    private View btnDelete;
    private RecyclerView playersList;
    private TextView playersEmpty;
    private PlayersAdapter playersAdapter;
    private RecyclerView rulesList;
    private RulesAdapter rulesAdapter;
    private final List<String[]> playersData = new ArrayList<>();
    private final List<String[]> rulesData = new ArrayList<>();

    private View settingsContainer;
    private boolean settingsVisible = false;

    private volatile int detailsGeneration = 0;
    private final ExecutorService detailExecutor = Executors.newSingleThreadExecutor();

    private final ActivityResultLauncher<String[]> gameDataZipPicker =
            registerForActivityResult(new ActivityResultContracts.OpenDocument(), uri -> {
                if (uri != null) {
                    GameDataImportDialog.startImport(this, uri, null);
                }
            });

    private boolean ensureGameDataOrPrompt() {
        List<String> missing = GameDataImporter.findMissingCoreFiles(getExternalFilesDir(null));
        if (missing.isEmpty()) {
            return true;
        }
        if (isFinishing() || isDestroyed()) {
            return false;
        }
        StringBuilder msg = new StringBuilder("未检测到完整游戏数据包（缺少 ");
        msg.append(String.join("、", missing));
        msg.append("）。\n\n请先使用 tools/build_gamedata.py 生成 gamedata.zip 传到手机任意位置，然后点击下方一键导入。");
        new AlertDialog.Builder(this)
                .setTitle("缺少游戏数据包")
                .setMessage(msg.toString())
                .setPositiveButton("一键导入 .zip", (d, w) ->
                        gameDataZipPicker.launch(new String[]{"application/zip", "application/octet-stream", "*/*"}))
                .setNegativeButton("取消", null)
                .show();
        return false;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        AppCompatDelegate.setDefaultNightMode(AppCompatDelegate.MODE_NIGHT_YES);
        super.onCreate(savedInstanceState);

        Intent launchIntent = getIntent();
        if (launchIntent == null || !"sampmobile1337".equals(launchIntent.getStringExtra("extra_check"))) {
            Log.e("MainActivity", "Not joined from launcher!");
            finish();
            return;
        }

        setContentView(R.layout.activity_main);

        androidx.core.view.WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
        androidx.core.view.WindowInsetsControllerCompat insets =
                new androidx.core.view.WindowInsetsControllerCompat(getWindow(), getWindow().getDecorView());
        insets.setSystemBarsBehavior(
                androidx.core.view.WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        insets.hide(androidx.core.view.WindowInsetsCompat.Type.systemBars());

        getSharedPreferences("samp_settings", Context.MODE_PRIVATE).edit().putBoolean("offline_mode", false).apply();
        com.sampmobile.launcher.utils.ModManager.ensureDirectories(this);

        initViews();
        initServerList();
        initTabs();
        initButtons();
        LauncherThemeManager.addListener(this);
        onThemeColorChanged(LauncherThemeManager.getAccentColor(this));

        // Pre-warm SettingsPageFragment hidden after first frame so opening Settings is instant (0ms)
        getWindow().getDecorView().post(() -> {
            if (!isFinishing() && !isDestroyed() && getSupportFragmentManager().findFragmentByTag("frag_settings") == null) {
                SettingsPageFragment frag = new SettingsPageFragment();
                getSupportFragmentManager().beginTransaction()
                        .setReorderingAllowed(true)
                        .add(R.id.settings_fragment, frag, "frag_settings")
                        .hide(frag)
                        .commitAllowingStateLoss();
            }
        });
    }

    private void initViews() {
        swipeRefreshLayout = findViewById(R.id.refresh);
        swipeRefreshLayout.setColorSchemeColors(getColor(R.color.launcher_accent));
        swipeRefreshLayout.setOnRefreshListener(this::refreshServers);

        searchEditText = findViewById(R.id.search_edit_text);
        searchEditText.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {
                if (serverAdapter != null) serverAdapter.filter(s.toString());
            }

            @Override
            public void afterTextChanged(Editable s) {
            }
        });

        infoEmpty = findViewById(R.id.info_empty);
        infoBody = findViewById(R.id.info_body);
        infoServerName = findViewById(R.id.info_server_name);
        infoServerMeta = findViewById(R.id.info_server_meta);
        playersHeader = findViewById(R.id.players_header);
        linksContainer = findViewById(R.id.links_container);
        discordRow = findViewById(R.id.discord_row);
        discordText = findViewById(R.id.discord_text);
        rulesHeader = findViewById(R.id.rules_header);
        btnFavorite = findViewById(R.id.btn_favorite);
        btnDelete = findViewById(R.id.btn_delete);
        playersList = findViewById(R.id.players_list);
        playersEmpty = findViewById(R.id.players_empty);
        rulesList = findViewById(R.id.rules_list);

        playersAdapter = new PlayersAdapter(playersData);
        playersList.setLayoutManager(new LinearLayoutManager(this));
        playersList.setAdapter(playersAdapter);

        rulesAdapter = new RulesAdapter(rulesData);
        rulesList.setLayoutManager(new LinearLayoutManager(this));
        rulesList.setAdapter(rulesAdapter);

        settingsContainer = findViewById(R.id.settings_container);
    }

    private void initServerList() {
        serverModel = new ArrayList<>();

        RecyclerView recyclerView = findViewById(R.id.servers_list);
        recyclerView.setLayoutManager(new LinearLayoutManager(this));

        serverAdapter = new ServerAdapter(this, serverModel, ServerAdapter.MODE_FAVORITES);
        serverAdapter.setOnServerSelectListener(this::onServerSelected);
        serverAdapter.setOnListLoadedListener(() -> swipeRefreshLayout.setRefreshing(false));
        recyclerView.setAdapter(serverAdapter);

        serverAdapter.refresh();
    }

    private void initTabs() {
        tabs = new ViewGroup[]{
                findViewById(R.id.tab_favorites),
                findViewById(R.id.tab_internet),
                findViewById(R.id.tab_recent)
        };

        int[] modes = {
                ServerAdapter.MODE_FAVORITES,
                ServerAdapter.MODE_INTERNET,
                ServerAdapter.MODE_RECENT
        };

        for (int i = 0; i < tabs.length; i++) {
            int mode = modes[i];
            tabs[i].setOnClickListener(v -> selectTab(mode));
        }

        findViewById(R.id.tab_mods).setOnClickListener(v -> showModsManager());
        findViewById(R.id.tab_settings).setOnClickListener(v -> showSettings());
        findViewById(R.id.tab_offline).setOnClickListener(v -> showOfflineDialog());
        findViewById(R.id.tab_exit).setOnClickListener(v -> quitApp());

        updateTabStyles();
    }

    private void selectTab(int mode) {
        if (mode == currentTab) {
            refreshServers();
            return;
        }
        currentTab = mode;
        serverAdapter.selectMode(mode);
        updateTabStyles();
        updateDeleteVisibility();
    }

    private void updateTabStyles() {
        int[] modes = {
                ServerAdapter.MODE_FAVORITES,
                ServerAdapter.MODE_INTERNET,
                ServerAdapter.MODE_RECENT
        };

        int accentColor = LauncherThemeManager.getAccentColor(this);
        int dimColor = getColor(R.color.launcher_text_dim);

        for (int i = 0; i < tabs.length; i++) {
            boolean selected = modes[i] == currentTab;
            if (selected) {
                tabs[i].setBackground(LauncherThemeManager.createSelectedRowDrawable(this, accentColor));
            } else {
                tabs[i].setBackgroundResource(android.R.color.transparent);
            }
            int color = selected ? accentColor : dimColor;
            ((ImageView) tabs[i].getChildAt(0)).setColorFilter(color);
            ((TextView) tabs[i].getChildAt(1)).setTextColor(color);
        }
    }

    @Override
    public void onThemeColorChanged(int accentColor) {
        LauncherThemeManager.applyThemeToTree(getWindow().getDecorView(), accentColor);
        if (swipeRefreshLayout != null) {
            swipeRefreshLayout.setColorSchemeColors(accentColor);
        }
        updateTabStyles();
        View btnJoin = findViewById(R.id.btn_join);
        if (btnJoin != null) {
            btnJoin.setBackgroundTintList(android.content.res.ColorStateList.valueOf(accentColor));
        }
        if (serverAdapter != null) {
            serverAdapter.notifyDataSetChanged();
            if (serverAdapter.getSelected() != null) {
                updateFavoriteButton(serverAdapter.getSelected());
            }
        }
    }

    private void initButtons() {
        findViewById(R.id.btn_join).setOnClickListener(v -> connectToSelectedServer());
        findViewById(R.id.btn_add).setOnClickListener(v -> showAddServerDialog());
        findViewById(R.id.btn_refresh).setOnClickListener(v -> refreshServers());
        findViewById(R.id.btn_delete).setOnClickListener(v -> deleteSelectedServer());
    }

    public void refreshServers() {
        swipeRefreshLayout.setRefreshing(true);
        serverAdapter.refresh();
    }

    private void updateDeleteVisibility() {
        boolean deletable = serverAdapter.getSelected() != null
                && currentTab == ServerAdapter.MODE_RECENT;
        btnDelete.setVisibility(deletable ? View.VISIBLE : View.GONE);
    }

    private void deleteSelectedServer() {
        if (serverAdapter.deleteSelected()) {
            Toast.makeText(this, R.string.server_removed, Toast.LENGTH_SHORT).show();
        } else {
            Toast.makeText(this, R.string.delete_not_supported, Toast.LENGTH_SHORT).show();
        }
        updateDeleteVisibility();
    }

    private void onServerSelected(ServerModel server) {
        int gen = ++detailsGeneration;
        updateDeleteVisibility();

        if (server == null) {
            infoEmpty.setVisibility(View.VISIBLE);
            infoBody.setVisibility(View.GONE);
            playersData.clear();
            playersAdapter.notifyDataSetChanged();
            rulesData.clear();
            rulesAdapter.notifyDataSetChanged();
            return;
        }

        infoEmpty.setVisibility(View.GONE);
        infoBody.setVisibility(View.VISIBLE);
        infoServerName.setText(server.server_name);
        String website = sanitizeUrl(server.server_website);
        infoServerMeta.setText(website);
        infoServerMeta.setVisibility(website.isEmpty() ? View.GONE : View.VISIBLE);
        infoServerMeta.setOnClickListener(null);
        updateFavoriteButton(server);
        btnFavorite.setOnClickListener(v -> toggleFavorite(server));

        playersData.clear();
        playersAdapter.notifyDataSetChanged();
        rulesData.clear();
        rulesAdapter.notifyDataSetChanged();
        playersList.setVisibility(View.GONE);
        playersEmpty.setVisibility(View.VISIBLE);
        playersEmpty.setText(R.string.loading);
        rulesHeader.setVisibility(View.GONE);
        rulesList.setVisibility(View.GONE);
        updateLinkButtons(server, "", "");

        // fetch player list & rules in background
        String address = server.address();
        String charset = getSharedPreferences("samp_settings", Context.MODE_PRIVATE).getString("charset", "GBK");
        detailExecutor.execute(() -> {
            List<String[]> players = new ArrayList<>();
            List<String[]> rules = new ArrayList<>();
            SampQuery query = null;
            try {
                query = new SampQuery(server.server_host, Integer.parseInt(server.server_port), charset);
                players = query.getPlayers();
                rules = query.getRules();
            } catch (Exception e) {
                e.printStackTrace();
            } finally {
                if (query != null) {
                    query.close();
                }
            }

            String weburl = "", discordFromRules = "";
            List<String[]> visibleRules = new ArrayList<>();
            for (String[] rule : rules) {
                String key = rule[0].toLowerCase();
                if ((key.equals("weburl") || key.equals("url") || key.equals("website")) && weburl.isEmpty()) {
                    weburl = rule[1];
                }
                if (key.contains("discord") && discordFromRules.isEmpty()) {
                    discordFromRules = rule[1];
                }
                if (!sanitizeUrl(rule[1]).isEmpty()) {
                    visibleRules.add(rule);
                }
            }

            List<String[]> finalPlayers = players;
            List<String[]> finalRules = visibleRules;
            String finalWeburl = weburl;
            String finalDiscord = discordFromRules;
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) return;
                if (gen != detailsGeneration || !address.equals(serverAdapter.getSelected() != null ? serverAdapter.getSelected().address() : null)) {
                    return;
                }
                playersData.clear();
                playersData.addAll(finalPlayers);
                playersAdapter.notifyDataSetChanged();
                playersList.setVisibility(finalPlayers.isEmpty() ? View.GONE : View.VISIBLE);
                playersEmpty.setVisibility(finalPlayers.isEmpty() ? View.VISIBLE : View.GONE);
                playersEmpty.setText(R.string.no_players);

                rulesData.clear();
                rulesData.addAll(finalRules);
                rulesAdapter.notifyDataSetChanged();
                rulesHeader.setVisibility(finalRules.isEmpty() ? View.GONE : View.VISIBLE);
                rulesList.setVisibility(finalRules.isEmpty() ? View.GONE : View.VISIBLE);

                updateLinkButtons(server, finalWeburl, finalDiscord);
            });
        });
    }

    private void updateLinkButtons(ServerModel server, String weburl, String discordFromRules) {
        String website = sanitizeUrl(!server.server_website.isEmpty() ? server.server_website : weburl);
        String discord = sanitizeUrl(!server.server_discord.isEmpty() ? server.server_discord : discordFromRules);

        if (!website.isEmpty()) {
            infoServerMeta.setText(website);
            infoServerMeta.setVisibility(View.VISIBLE);
            infoServerMeta.setOnClickListener(v -> openLink(website));
        } else {
            infoServerMeta.setVisibility(View.GONE);
        }

        if (discord.isEmpty()) {
            linksContainer.setVisibility(View.GONE);
            return;
        }
        linksContainer.setVisibility(View.VISIBLE);
        discordRow.setVisibility(View.VISIBLE);
        discordText.setText(discord);
        discordRow.setOnClickListener(v -> openLink(discord));
    }

    private static String sanitizeUrl(String url) {
        if (url == null) return "";
        String trimmed = url.trim();
        return trimmed.equalsIgnoreCase("null") || trimmed.length() < 4 ? "" : trimmed;
    }

    private void updateFavoriteButton(ServerModel server) {
        boolean fav = Utils.isServerInFavorites(this, server.address());
        btnFavorite.setColorFilter(fav ? LauncherThemeManager.getAccentColor(this) : getColor(R.color.launcher_text_dim));
        btnFavorite.setContentDescription(getString(fav ? R.string.remove_favorite : R.string.add_to_favorites));
    }

    private void toggleFavorite(ServerModel server) {
        String address = server.address();
        if (Utils.isServerInFavorites(this, address)) {
            try {
                Utils.deleteServerFromFavorites(this, address);
                Toast.makeText(this, R.string.removed_from_favorites, Toast.LENGTH_LONG).show();
            } catch (Exception e) {
                e.printStackTrace();
            }
        } else if (Utils.addServerToFavorites(this, address)) {
            Toast.makeText(this, R.string.server_added, Toast.LENGTH_LONG).show();
        }
        updateFavoriteButton(server);
        if (currentTab == ServerAdapter.MODE_FAVORITES) {
            refreshServers();
        }
    }

    private void openLink(String url) {
        try {
            if (!url.startsWith("http://") && !url.startsWith("https://")) {
                url = "https://" + url;
            }
            startActivity(new Intent(Intent.ACTION_VIEW, android.net.Uri.parse(url)));
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    private void connectToSelectedServer() {
        ServerModel server = serverAdapter.getSelected();
        if (server == null) {
            Toast.makeText(this, R.string.no_server_selected, Toast.LENGTH_LONG).show();
            return;
        }

        if (!Utils.isOnline(this)) {
            Toast.makeText(this, R.string.no_internet, Toast.LENGTH_LONG).show();
            return;
        }

        showJoinDialog(server);
    }

    private void showJoinDialog(ServerModel server) {
        View v = getLayoutInflater().inflate(R.layout.layout_join_dialog, null);

        AlertDialog dialog = new AlertDialog.Builder(this).setView(v).create();
        Objects.requireNonNull(dialog.getWindow()).setBackgroundDrawableResource(android.R.color.transparent);

        SharedPreferences prefs = getSharedPreferences("samp_settings", Context.MODE_PRIVATE);

        TextView serverName = v.findViewById(R.id.join_server_name);
        serverName.setText(server.server_name);

        EditText nickname = v.findViewById(R.id.join_nickname);
        nickname.setText(prefs.getString("nick_name", ""));
        nickname.setSelection(Objects.requireNonNull(nickname.getText()).length());

        TextView[] versionButtons = {
                v.findViewById(R.id.join_version_0),
                v.findViewById(R.id.join_version_1),
                v.findViewById(R.id.join_version_2)};
        final int[] versionValues = {0, 2, 3};
        final int[] version = {0};
        int savedVersion = prefs.getInt("samp_version", 0);
        for (int i = 0; i < versionValues.length; i++) {
            if (versionValues[i] == savedVersion) version[0] = i;
        }
        int accentColor = LauncherThemeManager.getAccentColor(this);
        LauncherThemeManager.applyThemeToTree(v, accentColor);
        View joinConnectBtn = v.findViewById(R.id.join_connect);
        if (joinConnectBtn != null) {
            joinConnectBtn.setBackgroundTintList(android.content.res.ColorStateList.valueOf(accentColor));
        }

        Runnable applyVersionStyles = () -> {
            for (int i = 0; i < versionButtons.length; i++) {
                boolean selected = version[0] == i;
                versionButtons[i].setBackgroundResource(
                        selected ? R.drawable.launcher_btn_primary : R.drawable.launcher_btn_outline);
                versionButtons[i].setBackgroundTintList(
                        selected ? android.content.res.ColorStateList.valueOf(accentColor) : null);
                versionButtons[i].setTextColor(getColor(
                        selected ? R.color.launcher_on_accent : R.color.launcher_text_dim));
            }
        };
        applyVersionStyles.run();
        for (int i = 0; i < versionButtons.length; i++) {
            final int index = i;
            versionButtons[i].setOnClickListener(view -> { version[0] = index; applyVersionStyles.run(); });
        }

        boolean hasPassword = "1".equals(server.server_password);
        EditText password = v.findViewById(R.id.join_password);
        password.setVisibility(hasPassword ? View.VISIBLE : View.GONE);

        v.findViewById(R.id.join_cancel).setOnClickListener(view -> dialog.dismiss());

        v.findViewById(R.id.join_connect).setOnClickListener(view -> {
            String nick = Objects.requireNonNull(nickname.getText()).toString().trim();
            if (nick.isEmpty()) {
                Toast.makeText(this, R.string.set_nickname, Toast.LENGTH_LONG).show();
                return;
            }
            if (nick.length() > 24) {
                Toast.makeText(this, R.string.nickname_too_long, Toast.LENGTH_LONG).show();
                return;
            }

            prefs.edit()
                    .putBoolean("offline_mode", false)
                    .putString("nick_name", nick)
                    .putInt("samp_version", versionValues[version[0]])
                    .commit();

            SharedPreferences.Editor edit = getSharedPreferences("samp_server", Context.MODE_PRIVATE).edit();
            edit.putString("host", server.server_host);
            edit.putString("port", server.server_port);
            edit.putString("password", hasPassword ? Objects.requireNonNull(password.getText()).toString() : "");
            edit.commit();

            Utils.saveSettings(this);
            Utils.addServerToRecent(this, server.address());

            dialog.dismiss();
            if (!ensureGameDataOrPrompt()) {
                return;
            }
            try {
                Intent intent = new Intent(this, com.sampmobile.game.SAMP.class);
                intent.putExtra("extra_check", "sampmobile1337");
                startActivity(intent);
                prefs.edit().putBoolean("open_hud_edit_on_start", false).apply();
            } catch (Exception e) {
                e.printStackTrace();
            }
        });

        if (!isFinishing() && !isDestroyed()) {
            dialog.show();
        }
    }

    private void showAddServerDialog() {
        View v = getLayoutInflater().inflate(R.layout.layout_server_add, null);

        AlertDialog dialog = new AlertDialog.Builder(this).setView(v).create();
        Objects.requireNonNull(dialog.getWindow()).setBackgroundDrawableResource(android.R.color.transparent);

        EditText address = v.findViewById(R.id.address_input_text);
        TextView addButton = v.findViewById(R.id.add_server);
        int accentColor = LauncherThemeManager.getAccentColor(this);
        LauncherThemeManager.applyThemeToTree(v, accentColor);
        if (addButton != null) {
            addButton.setBackgroundTintList(android.content.res.ColorStateList.valueOf(accentColor));
        }

        address.setOnEditorActionListener((textView, i, keyEvent) -> i == EditorInfo.IME_ACTION_DONE || i == EditorInfo.IME_ACTION_NEXT || i == EditorInfo.IME_ACTION_UNSPECIFIED);

        addButton.setOnClickListener(view -> {
            address.setText(Objects.requireNonNull(address.getText()).toString().replace("\n", ""));

            Pattern pattern = Pattern.compile("^(((?!-)[A-Za-z0-9-]{1,63}(?<!-)\\.)+[A-Za-z]{2,6}|localhost|(([0-9]{1,3}\\.){3})[0-9]{1,3}):[0-9]{1,5}$");
            Pattern pattern2 = Pattern.compile("^(((?!-)[A-Za-z0-9-]{1,63}(?<!-)\\.)+[A-Za-z]{2,6}|localhost|(([0-9]{1,3}\\.){3})[0-9]{1,3})$");

            Matcher matcher = pattern.matcher(address.getText().toString());
            Matcher matcher2 = pattern2.matcher(address.getText().toString());

            if (matcher.find() || matcher2.find()) {
                if (!address.getText().toString().contains(":")) {
                    address.setText(address.getText().toString() + ":7777");
                }
                try {
                    if (Utils.addServerToFavorites(this, address.getText().toString())) {
                        Toast.makeText(this, R.string.server_added, Toast.LENGTH_LONG).show();
                        dialog.dismiss();
                        if (currentTab == ServerAdapter.MODE_FAVORITES) {
                            refreshServers();
                        }
                    } else {
                        Toast.makeText(this, R.string.server_already_exists, Toast.LENGTH_LONG).show();
                    }
                } catch (Exception e) {
                    e.printStackTrace();
                }
            } else {
                Toast.makeText(this, R.string.invalid_host, Toast.LENGTH_LONG).show();
            }
        });

        if (!isFinishing() && !isDestroyed()) {
            dialog.show();
        }
    }

    private void showOfflineDialog() {
        if (isFinishing() || isDestroyed()) return;
        new AlertDialog.Builder(this)
                .setTitle(R.string.offline_mode)
                .setMessage(R.string.offline_mode_confirm)
                .setPositiveButton(R.string.yes, (dialog, which) -> {
                    SharedPreferences prefs = getSharedPreferences("samp_settings", Context.MODE_PRIVATE);
                    prefs.edit().putBoolean("offline_mode", true).commit();
                    Utils.saveSettings(this);
                    if (!ensureGameDataOrPrompt()) {
                        return;
                    }
                    try {
                        Intent intent = new Intent(this, com.sampmobile.game.SAMP.class);
                        intent.putExtra("extra_check", "sampmobile1337");
                        startActivity(intent);
                        prefs.edit().putBoolean("open_hud_edit_on_start", false).apply();
                    } catch (Exception e) {
                        e.printStackTrace();
                    }
                })
                .setNegativeButton(R.string.no, null)
                .show();
    }

    public void showSettings() {
        settingsVisible = true;
        androidx.fragment.app.FragmentManager fm = getSupportFragmentManager();
        androidx.fragment.app.FragmentTransaction ft = fm.beginTransaction().setReorderingAllowed(true);
        androidx.fragment.app.Fragment modsFrag = fm.findFragmentByTag("frag_mods");
        if (modsFrag != null && !modsFrag.isHidden()) {
            ft.hide(modsFrag);
        }
        androidx.fragment.app.Fragment settingsFrag = fm.findFragmentByTag("frag_settings");
        if (settingsFrag == null) {
            settingsFrag = new SettingsPageFragment();
            ft.add(R.id.settings_fragment, settingsFrag, "frag_settings");
        } else {
            ft.show(settingsFrag);
            if (settingsFrag instanceof SettingsPageFragment) {
                ((SettingsPageFragment) settingsFrag).onPageShown();
            }
        }
        ft.commitNowAllowingStateLoss();
        settingsContainer.setVisibility(View.VISIBLE);
    }

    public void showModsManager() {
        settingsVisible = true;
        androidx.fragment.app.FragmentManager fm = getSupportFragmentManager();
        androidx.fragment.app.FragmentTransaction ft = fm.beginTransaction().setReorderingAllowed(true);
        androidx.fragment.app.Fragment settingsFrag = fm.findFragmentByTag("frag_settings");
        if (settingsFrag != null && !settingsFrag.isHidden()) {
            ft.hide(settingsFrag);
        }
        androidx.fragment.app.Fragment modsFrag = fm.findFragmentByTag("frag_mods");
        if (modsFrag == null) {
            modsFrag = new ModsManagerFragment();
            ft.add(R.id.settings_fragment, modsFrag, "frag_mods");
        } else {
            ft.show(modsFrag);
        }
        ft.commitNowAllowingStateLoss();
        settingsContainer.setVisibility(View.VISIBLE);
    }

    public void hideSettings() {
        settingsVisible = false;
        settingsContainer.setVisibility(View.GONE);
        androidx.fragment.app.FragmentManager fm = getSupportFragmentManager();
        androidx.fragment.app.FragmentTransaction ft = fm.beginTransaction().setReorderingAllowed(true);
        androidx.fragment.app.Fragment settingsFrag = fm.findFragmentByTag("frag_settings");
        if (settingsFrag != null && !settingsFrag.isHidden()) {
            ft.hide(settingsFrag);
        }
        androidx.fragment.app.Fragment modsFrag = fm.findFragmentByTag("frag_mods");
        if (modsFrag != null && !modsFrag.isHidden()) {
            ft.hide(modsFrag);
        }
        ft.commitAllowingStateLoss();
    }

    @Override
    protected void onDestroy() {
        LauncherThemeManager.removeListener(this);
        detailExecutor.shutdownNow();
        if (serverAdapter != null) serverAdapter.shutdown();
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        if (settingsVisible) {
            hideSettings();
            return;
        }
        quitApp();
    }

    private long quit_time = 0;

    public void quitApp() {
        if ((System.currentTimeMillis() - quit_time) > 2000) {
            Toast.makeText(this, R.string.press_again_quit, Toast.LENGTH_LONG).show();
            quit_time = System.currentTimeMillis();
        } else {
            finish();
        }
    }
}
