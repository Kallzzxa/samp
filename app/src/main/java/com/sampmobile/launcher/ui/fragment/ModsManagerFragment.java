package com.sampmobile.launcher.ui.fragment;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.app.AlertDialog;
import androidx.fragment.app.Fragment;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.joom.paranoid.Obfuscate;
import com.sampmobile.launcher.R;
import com.sampmobile.launcher.ui.widget.GtaTabStrip;
import com.sampmobile.launcher.utils.ClientSettings;
import com.sampmobile.launcher.utils.LauncherThemeManager;
import com.sampmobile.launcher.utils.ModAssetTool;
import com.sampmobile.launcher.utils.ModManager;
import com.sampmobile.launcher.utils.Utils;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

@Obfuscate
public class ModsManagerFragment extends Fragment implements LauncherThemeManager.ThemeListener {

    private View rootView;

    private static final int TAB_MODLOADER = 0;
    private static final int TAB_AML = 1;
    private static final int TAB_JPATCH = 2;
    private static final int TAB_MONET = 3;
    private static final int TAB_CLEO = 4;

    private int currentTab = TAB_MODLOADER;
    final ExecutorService ioExecutor = Executors.newSingleThreadExecutor();
    private final ModAssetImportWizard assetImportWizard = new ModAssetImportWizard(this);
    private final ModConfigDialogs configDialogs = new ModConfigDialogs(this);

    private View modsContent;
    private TextView tvSectionTitle, tvSectionStatus, tvSectionDesc, tvSectionPath, tvModEmpty;
    private Switch switchSectionMaster;
    private TextView btnActionPrimary, btnActionSecondary, btnActionTertiary, btnActionQuaternary;
    private RecyclerView rvModItems;

    private final List<RowItem> rows = new ArrayList<>();
    private final List<ModManager.ModLoaderEntry> modLoaderCache = new ArrayList<>();
    private ModListAdapter adapter;

    static final int IMPORT_MODE_ASSET_TOOL_ZIP = 100;

    private int pendingImportTab = -1;
    private ActivityResultLauncher<String[]> filePickerLauncher;
    ActivityResultLauncher<String[]> multiFilePickerLauncher;

    private static class RowItem {
        String title;
        String subtitle;
        String badge;
        boolean badgeDanger;
        boolean showSwitch = true;
        boolean checked;
        boolean showUpDown;
        boolean showDelete = true;
        String valueText; // non-null for non-boolean JPatch INI values
        Object payload;
    }

    @Override
    public void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        filePickerLauncher = registerForActivityResult(
                new ActivityResultContracts.OpenDocument(),
                uri -> {
                    if (uri == null || pendingImportTab < 0) return;
                    if (pendingImportTab == IMPORT_MODE_ASSET_TOOL_ZIP) {
                        assetImportWizard.showConfigDialog(Collections.singletonList(uri));
                    } else {
                        handleImportedFile(pendingImportTab, uri);
                    }
                }
        );
        multiFilePickerLauncher = registerForActivityResult(
                new ActivityResultContracts.OpenMultipleDocuments(),
                uris -> {
                    if (uris != null && !uris.isEmpty()) {
                        assetImportWizard.showConfigDialog(uris);
                    }
                }
        );
    }

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container, @Nullable Bundle savedInstanceState) {
        View v = inflater.inflate(R.layout.fragment_mods_manager, container, false);
        rootView = v;

        TextView tvAbi = v.findViewById(R.id.tv_process_abi);
        tvAbi.setText(ModManager.getProcessAbiLabel());

        v.findViewById(R.id.btn_mods_logs).setOnClickListener(view -> configDialogs.showLogsDialog());
        v.findViewById(R.id.btn_mods_refresh).setOnClickListener(view -> reloadCurrentTab());

        modsContent = v.findViewById(R.id.mods_content);
        GtaTabStrip modTabs = v.findViewById(R.id.mod_tabs);
        modTabs.setTabs("Modloader", "AML Plugins", "JPatch Patches", "MonetLoader", "CLEO scripts");
        modTabs.setOnTabSelectedListener((index, reselected) -> {
            if (reselected) {
                reloadCurrentTab();
            } else {
                selectTab(index);
                modsContent.animate().cancel();
                modsContent.setAlpha(0f);
                modsContent.animate().alpha(1f).setDuration(180).start();
            }
        });

        tvSectionTitle = v.findViewById(R.id.tv_section_title);
        tvSectionStatus = v.findViewById(R.id.tv_section_status);
        tvSectionDesc = v.findViewById(R.id.tv_section_desc);
        tvSectionPath = v.findViewById(R.id.tv_section_path);
        tvModEmpty = v.findViewById(R.id.tv_mod_empty);
        switchSectionMaster = v.findViewById(R.id.switch_section_master);

        btnActionPrimary = v.findViewById(R.id.btn_action_primary);
        btnActionSecondary = v.findViewById(R.id.btn_action_secondary);
        btnActionTertiary = v.findViewById(R.id.btn_action_tertiary);
        btnActionQuaternary = v.findViewById(R.id.btn_action_quaternary);

        rvModItems = v.findViewById(R.id.rv_mod_items);
        rvModItems.setLayoutManager(new LinearLayoutManager(requireContext()));
        adapter = new ModListAdapter();
        rvModItems.setAdapter(adapter);

        ioExecutor.execute(() -> ModManager.ensureDirectories(requireContext().getApplicationContext()));

        LauncherThemeManager.addListener(this);
        selectTab(TAB_MODLOADER);
        onThemeColorChanged(LauncherThemeManager.getAccentColor(requireContext()));
        return v;
    }

    @Override
    public void onDestroyView() {
        LauncherThemeManager.removeListener(this);
        super.onDestroyView();
        ioExecutor.shutdownNow();
    }

    @Override
    public void onThemeColorChanged(int accentColor) {
        if (rootView == null || !isAdded()) return;
        LauncherThemeManager.applyThemeToTree(rootView, accentColor);
        if (btnActionPrimary != null) {
            btnActionPrimary.setBackgroundTintList(ColorStateList.valueOf(accentColor));
        }
        if (currentTab == TAB_MODLOADER || currentTab == TAB_AML || currentTab == TAB_MONET) {
            if (tvSectionStatus != null) tvSectionStatus.setTextColor(accentColor);
        }
        if (adapter != null) {
            adapter.notifyDataSetChanged();
        }
    }

    private void selectTab(int tab) {
        currentTab = tab;
        configureSectionPanel();
        reloadCurrentTab();
    }

    private String prefKeyForTab(int tab) {
        switch (tab) {
            case TAB_MODLOADER: return "modloader";
            case TAB_AML: return "aml_scripts";
            case TAB_JPATCH: return "jpatch_enabled";
            case TAB_MONET: return "monet_scripts";
            case TAB_CLEO: return "cleo_scripts";
            default: return "modloader";
        }
    }

    void configureSectionPanel() {
        Context ctx = requireContext();
        String key = prefKeyForTab(currentTab);
        ClientSettings settings = ClientSettings.loadLauncher(ctx);
        boolean enabled = settings.getBoolean(key);

        switchSectionMaster.setOnCheckedChangeListener(null);
        switchSectionMaster.setChecked(enabled);
        switchSectionMaster.setOnCheckedChangeListener((btn, isChecked) -> {
            if (currentTab == TAB_JPATCH) {
                if (isChecked && !ClientSettings.loadLauncher(ctx).getBoolean("aml_scripts")) {
                    java.util.Map<String, Object> updates = new java.util.HashMap<>();
                    updates.put(key, true);
                    updates.put("aml_scripts", true);
                    ClientSettings.updateBatchSync(ctx, updates);
                    Toast.makeText(ctx, "AML was auto-enabled (JPatch needs AML to run)", Toast.LENGTH_SHORT).show();
                } else {
                    ClientSettings.setSync(ctx, key, isChecked);
                }
                ioExecutor.execute(() -> ModManager.syncJPatchBinaryState(ctx.getApplicationContext(), isChecked));
            } else {
                ClientSettings.setSync(ctx, key, isChecked);
            }
            reloadCurrentTab();
        });

        btnActionSecondary.setVisibility(View.VISIBLE);
        btnActionTertiary.setVisibility(View.GONE);
        btnActionQuaternary.setVisibility(View.GONE);

        int accentColor = LauncherThemeManager.getAccentColor(ctx);
        btnActionPrimary.setBackgroundTintList(ColorStateList.valueOf(accentColor));
        btnActionTertiary.setTextColor(accentColor);
        LauncherThemeManager.applySwitchTheme(switchSectionMaster, accentColor);

        File pathDir;
        switch (currentTab) {
            case TAB_MODLOADER:
                tvSectionTitle.setText("Modloader asset overrides");
                tvSectionStatus.setText("Status: Ready · Built-in IMG+TXD clean override");
                tvSectionStatus.setTextColor(accentColor);
                tvSectionDesc.setText("No need to modify the official gta3.img or base textures: the built-in IMG+TXD tool imports skin/vehicle/weapon models (.dff/.ifp/.col) and PNG/PC .txd textures directly, and auto-packs a virtual models.img and mounts a custom texdb when you enter the game.");
                pathDir = ModManager.getModLoaderDir(ctx);
                btnActionPrimary.setText("Import skins/models (IMG+TXD)");
                btnActionPrimary.setOnClickListener(v -> assetImportWizard.showSourceChooser());
                btnActionSecondary.setText("Import full ZIP mod pack");
                btnActionSecondary.setOnClickListener(v -> launchPicker(TAB_MODLOADER, new String[]{"application/zip", "application/x-zip-compressed", "*/*"}));
                btnActionTertiary.setVisibility(View.VISIBLE);
                btnActionTertiary.setText("New empty mod folder");
                btnActionTertiary.setOnClickListener(v -> showCreateModFolderDialog());
                break;

            case TAB_AML:
                tvSectionTitle.setText("AndroidModLoader (AML)");
                tvSectionStatus.setText("Status: AML 1.5.0 built in · 64-bit/32-bit supported");
                tvSectionStatus.setTextColor(accentColor);
                tvSectionDesc.setText("Loads native .so plugins from the mods/ directory. On launch it spoofs the package name to com.rockstargames.gtasa and writes logs; make sure to import a plugin matching the current process " + ModManager.getProcessAbiName() + " plugin.");
                pathDir = ModManager.getAmlModsDir(ctx);
                btnActionPrimary.setText("Import AML plugin (.so)");
                btnActionPrimary.setOnClickListener(v -> launchPicker(TAB_AML, new String[]{"*/*"}));
                btnActionSecondary.setText("AML core settings");
                btnActionSecondary.setOnClickListener(v -> configDialogs.showAmlCoreSettingsDialog());
                break;

            case TAB_JPATCH:
                tvSectionTitle.setText("JPatch engine patches");
                tvSectionStatus.setText("Checking libJPatch.so status…");
                tvSectionStatus.setTextColor(ctx.getColor(R.color.launcher_text_dim));
                tvSectionDesc.setText("RusJJ's GTA:SA 2.10 fix patch set (requires AML). Its license forbids redistributing the binary, so download the " + ModManager.getProcessAbiName() + " libJPatch.so from the official page, then tap Import below; you can edit JPatch.ini visually on the right.");
                pathDir = ModManager.getJPatchConfigFile(ctx);
                btnActionPrimary.setText("Import libJPatch.so");
                btnActionPrimary.setOnClickListener(v -> launchPicker(TAB_JPATCH, new String[]{"*/*"}));
                btnActionSecondary.setText("Apply SA-MP safe preset");
                btnActionSecondary.setOnClickListener(v -> configDialogs.applyJPatchSafePreset());
                btnActionTertiary.setVisibility(View.VISIBLE);
                btnActionTertiary.setText("Add custom setting");
                btnActionTertiary.setOnClickListener(v -> configDialogs.showAddJPatchKeyDialog());
                btnActionQuaternary.setVisibility(View.VISIBLE);
                btnActionQuaternary.setText("Open JPatch official page");
                btnActionQuaternary.setOnClickListener(v -> openUrl(ModManager.JPATCH_OFFICIAL_URL));
                break;

            case TAB_MONET:
                tvSectionTitle.setText("MonetLoader (Lua)");
                tvSectionStatus.setText("Status: MonetLoader 3.8.0 built in · Dual ABI + native SA-MP bridge");
                tvSectionStatus.setTextColor(accentColor);
                tvSectionDesc.setText("Runs .lua scripts from files/monetloader/ (LuaJIT 5.1 + mimgui + samp.events + native RakHook bridge, compatible with the MoonLoader ecosystem); the built-in Lua standard library is extracted automatically on first launch.");
                pathDir = ModManager.getMonetDir(ctx);
                btnActionPrimary.setText("Import Lua script (.lua)");
                btnActionPrimary.setOnClickListener(v -> launchPicker(TAB_MONET, new String[]{"*/*"}));
                btnActionSecondary.setVisibility(View.VISIBLE);
                btnActionSecondary.setText("Reinstall built-in Lua standard library");
                btnActionSecondary.setOnClickListener(v -> {
                    final Context appCtx = ctx.getApplicationContext();
                    Toast.makeText(ctx, "Extracting MonetLoader's built-in Lua libraries…", Toast.LENGTH_SHORT).show();
                    ioExecutor.execute(() -> {
                        boolean ok = ModManager.ensureMonetLibsInstalled(appCtx, true);
                        if (isAdded()) {
                            requireActivity().runOnUiThread(() -> Toast.makeText(ctx,
                                    ok ? "Reinstalled the mimgui / samp.events / moonloader standard libraries" : "Failed to extract the standard libraries",
                                    Toast.LENGTH_SHORT).show());
                        }
                    });
                });
                break;

            case TAB_CLEO:
            default:
                tvSectionTitle.setText("CLEO scripts");
                tvSectionStatus.setText("Status: Native CLEO engine built in · 64-bit/32-bit supported");
                tvSectionStatus.setTextColor(accentColor);
                tvSectionDesc.setText("Runs .csa/.cs files from files/cleo/ (auto-start), .csi (swipe down from the middle of the top of the screen in game to open the CLEO menu), and .fxt custom text. Dual-ABI native support, auto-syncs online player handles.");
                pathDir = ModManager.getCleoDir(ctx);
                btnActionPrimary.setText("Import CLEO script");
                btnActionPrimary.setOnClickListener(v -> launchPicker(TAB_CLEO, new String[]{"*/*"}));
                btnActionSecondary.setVisibility(View.GONE);
                break;
        }

        final String pathStr = pathDir != null ? pathDir.getAbsolutePath() : "";
        tvSectionPath.setText("Path (tap to copy):\n" + pathStr);
        tvSectionPath.setOnClickListener(v -> {
            ClipboardManager cm = (ClipboardManager) ctx.getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm != null) {
                cm.setPrimaryClip(ClipData.newPlainText("mod_path", pathStr));
                Toast.makeText(ctx, "Path copied", Toast.LENGTH_SHORT).show();
            }
        });
    }

    void reloadCurrentTab() {
        final int tab = currentTab;
        final Context appCtx = requireContext().getApplicationContext();

        ioExecutor.execute(() -> {
            List<RowItem> loaded = new ArrayList<>();
            String jpatchStatusText = null;
            boolean jpatchStatusOk = false;

            if (tab == TAB_MODLOADER) {
                List<ModManager.ModLoaderEntry> list = ModManager.listModLoaderPacks(appCtx);
                synchronized (modLoaderCache) {
                    modLoaderCache.clear();
                    modLoaderCache.addAll(list);
                }
                for (ModManager.ModLoaderEntry e : list) {
                    RowItem r = new RowItem();
                    r.title = e.name;
                    r.subtitle = e.fileCount + " files · " + Utils.convertBytesToString(e.totalBytes) + " · Priority " + e.priority;
                    r.badge = e.enabled ? "Enabled" : "Disabled";
                    r.badgeDanger = !e.enabled;
                    r.checked = e.enabled;
                    r.showUpDown = true;
                    r.showDelete = true;
                    r.payload = e;
                    loaded.add(r);
                }
            } else if (tab == TAB_AML) {
                List<ModManager.FileModEntry> list = ModManager.listAmlPlugins(appCtx);
                for (ModManager.FileModEntry e : list) {
                    RowItem r = new RowItem();
                    r.title = e.displayName;
                    r.subtitle = Utils.convertBytesToString(e.sizeBytes)
                            + (e.isJPatch ? " · JPatch patch plugin" : " · AML native plugin")
                            + (e.abiCompatible ? "" : " · doesn't match the current process architecture!");
                    r.badge = e.abiCompatible ? e.abiLabel : ("Incompatible: " + e.abiLabel);
                    r.badgeDanger = !e.abiCompatible;
                    r.checked = e.enabled;
                    r.showDelete = true;
                    r.payload = e;
                    loaded.add(r);
                }
            } else if (tab == TAB_JPATCH) {
                ModManager.FileModEntry jpBin = ModManager.findJPatchBinary(appCtx);
                if (jpBin == null) {
                    jpatchStatusText = "libJPatch.so not detected — tap Import below first (you can edit the config meanwhile)";
                    jpatchStatusOk = false;
                } else if (!jpBin.abiCompatible) {
                    jpatchStatusText = "Found " + jpBin.displayName + ", but its architecture is " + jpBin.abiLabel + " (doesn't match the current process!)";
                    jpatchStatusOk = false;
                } else {
                    jpatchStatusText = "Installed: " + jpBin.displayName + " (" + jpBin.abiLabel + ") · "
                            + (jpBin.enabled ? "Enabled" : "Disabled");
                    jpatchStatusOk = true;
                }

                List<ModManager.JPatchOption> opts = ModManager.loadJPatchOptions(appCtx);
                for (ModManager.JPatchOption opt : opts) {
                    RowItem r = new RowItem();
                    r.title = opt.title;
                    r.subtitle = opt.key + " — " + opt.description;
                    if (!opt.safeForSamp) {
                        r.badge = "Use with caution online";
                        r.badgeDanger = true;
                    } else if (!opt.fromKnownCatalog) {
                        r.badge = opt.section;
                        r.badgeDanger = false;
                    }
                    r.showDelete = false;
                    if (opt.isBoolean) {
                        r.showSwitch = true;
                        r.checked = ModManager.parseIniBool(opt.value, false);
                    } else {
                        r.showSwitch = false;
                        r.valueText = opt.value;
                    }
                    r.payload = opt;
                    loaded.add(r);
                }
            } else if (tab == TAB_MONET) {
                List<ModManager.FileModEntry> list = ModManager.listMonetScripts(appCtx);
                for (ModManager.FileModEntry e : list) {
                    RowItem r = new RowItem();
                    r.title = e.displayName;
                    r.subtitle = Utils.convertBytesToString(e.sizeBytes) + " · Lua script";
                    r.badge = e.enabled ? "Enabled" : "Disabled";
                    r.badgeDanger = !e.enabled;
                    r.checked = e.enabled;
                    r.showDelete = true;
                    r.payload = e;
                    loaded.add(r);
                }
            } else if (tab == TAB_CLEO) {
                List<ModManager.FileModEntry> list = ModManager.listCleoScripts(appCtx);
                for (ModManager.FileModEntry e : list) {
                    RowItem r = new RowItem();
                    r.title = e.displayName;
                    r.subtitle = Utils.convertBytesToString(e.sizeBytes) + " · CLEO script";
                    r.badge = e.enabled ? "Enabled" : "Disabled";
                    r.badgeDanger = !e.enabled;
                    r.checked = e.enabled;
                    r.showDelete = true;
                    r.payload = e;
                    loaded.add(r);
                }
            }

            final String finalJpStatus = jpatchStatusText;
            final boolean finalJpOk = jpatchStatusOk;

            if (!isAdded()) return;
            requireActivity().runOnUiThread(() -> {
                if (!isAdded() || currentTab != tab) return;
                if (tab == TAB_JPATCH && finalJpStatus != null) {
                    tvSectionStatus.setText(finalJpStatus);
                    tvSectionStatus.setTextColor(finalJpOk
                            ? LauncherThemeManager.getAccentColor(requireContext())
                            : requireContext().getColor(R.color.launcher_danger));
                }
                rows.clear();
                rows.addAll(loaded);
                adapter.notifyDataSetChanged();

                if (rows.isEmpty()) {
                    tvModEmpty.setVisibility(View.VISIBLE);
                    tvModEmpty.setText(emptyHintForTab(tab));
                } else {
                    tvModEmpty.setVisibility(View.GONE);
                }
            });
        });
    }

    private String emptyHintForTab(int tab) {
        switch (tab) {
            case TAB_MODLOADER:
                return "No Modloader mod packs yet\nTap “Import skins/models (IMG+TXD)” or “Import full ZIP mod pack” on the right, or place files in files/modloader/ manually";
            case TAB_AML:
                return "No AML plugins (.so) yet\nTap “Import AML plugin (.so)” on the right, or place files in Android/data/com.sampmobile.launcher/mods/";
            case TAB_MONET:
                return "No MonetLoader Lua scripts yet\nTap “Import Lua script (.lua)” on the right, or place files in files/monetloader/";
            case TAB_CLEO:
                return "No CLEO scripts yet\nTap “Import CLEO script” on the right, or place files in files/cleo/";
            default:
                return "No entries";
        }
    }

    void launchPicker(int tab, String[] mimeTypes) {
        pendingImportTab = tab;
        try {
            filePickerLauncher.launch(mimeTypes);
        } catch (Exception e) {
            Toast.makeText(requireContext(), "Could not open the system file picker: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private void handleImportedFile(int tab, Uri uri) {
        Context appCtx = requireContext().getApplicationContext();
        Toast.makeText(requireContext(), "Importing…", Toast.LENGTH_SHORT).show();
        ioExecutor.execute(() -> {
            String msg;
            try {
                if (tab == TAB_MODLOADER) {
                    String folder = ModManager.importModLoaderZip(appCtx, uri);
                    msg = "Imported mod pack: " + folder;
                } else if (tab == TAB_AML || tab == TAB_JPATCH) {
                    String origName = ModManager.queryDisplayName(appCtx, uri);
                    if (origName == null || !origName.toLowerCase(Locale.ROOT).endsWith(".so")) {
                        throw new IllegalArgumentException("Please pick a .so plugin file");
                    }
                    File dest = ModManager.importFileToDir(appCtx, uri, ModManager.getAmlModsDir(appCtx),
                            tab == TAB_JPATCH ? "libJPatch.so" : "plugin.so");
                    ModManager.ElfInfo elf = ModManager.inspectElfAbi(dest);
                    if (!elf.valid) {
                        dest.delete();
                        throw new IllegalArgumentException("The imported file is not a valid Android .so (ELF) library");
                    }
                    if (!elf.matchesProcess) {
                        msg = "Imported " + dest.getName() + ", note: this plugin is " + elf.abiName + ", which doesn't match the current " + ModManager.getProcessAbiLabel() + "!";
                    } else {
                        msg = "Imported " + dest.getName() + " (" + elf.abiName + ")";
                    }
                } else if (tab == TAB_MONET) {
                    File dest = ModManager.importFileToDir(appCtx, uri, ModManager.getMonetDir(appCtx), "script.lua");
                    msg = "Imported Lua script: " + dest.getName();
                } else {
                    File dest = ModManager.importFileToDir(appCtx, uri, ModManager.getCleoDir(appCtx), "script.csa");
                    msg = "Imported CLEO script: " + dest.getName();
                }
            } catch (Exception e) {
                msg = "Import failed: " + e.getMessage();
            }

            final String finalMsg = msg;
            if (!isAdded()) return;
            requireActivity().runOnUiThread(() -> {
                if (!isAdded()) return;
                Toast.makeText(requireContext(), finalMsg, Toast.LENGTH_LONG).show();
                reloadCurrentTab();
            });
        });
    }

    private void showCreateModFolderDialog() {
        EditText input = new EditText(requireContext());
        input.setHint("Enter a mod folder name (e.g. HD_Roads)");
        int pad = (int) (16 * getResources().getDisplayMetrics().density);
        LinearLayout wrap = new LinearLayout(requireContext());
        wrap.setPadding(pad, pad / 2, pad, 0);
        wrap.addView(input, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        new AlertDialog.Builder(requireContext())
                .setTitle("New Modloader folder")
                .setView(wrap)
                .setPositiveButton("Create", (d, w) -> {
                    String name = input.getText().toString().trim();
                    if (name.isEmpty()) return;
                    Context appCtx = requireContext().getApplicationContext();
                    ioExecutor.execute(() -> {
                        File created = ModManager.createModLoaderFolder(appCtx, name);
                        if (!isAdded()) return;
                        requireActivity().runOnUiThread(() -> {
                            if (!isAdded()) return;
                            Toast.makeText(requireContext(),
                                    created != null ? ("Created folder: " + created.getName()) : "Failed to create folder",
                                    Toast.LENGTH_SHORT).show();
                            reloadCurrentTab();
                        });
                    });
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void openUrl(String url) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (Exception e) {
            Toast.makeText(requireContext(), "Could not open link: " + url, Toast.LENGTH_SHORT).show();
        }
    }

    private void moveModLoaderPriority(ModManager.ModLoaderEntry target, int delta) {
        Context appCtx = requireContext().getApplicationContext();
        ioExecutor.execute(() -> {
            synchronized (modLoaderCache) {
                int idx = modLoaderCache.indexOf(target);
                int swapIdx = idx + delta;
                if (idx < 0 || swapIdx < 0 || swapIdx >= modLoaderCache.size()) return;
                ModManager.ModLoaderEntry other = modLoaderCache.get(swapIdx);
                modLoaderCache.set(idx, other);
                modLoaderCache.set(swapIdx, target);
                // Reassign descending priorities cleanly (e.g. 100, 90, 80...)
                int base = modLoaderCache.size() * 10;
                for (int i = 0; i < modLoaderCache.size(); i++) {
                    modLoaderCache.get(i).priority = base - i * 10;
                }
                ModManager.saveModLoaderConfig(appCtx, modLoaderCache);
            }
            if (!isAdded()) return;
            requireActivity().runOnUiThread(this::reloadCurrentTab);
        });
    }

    // ══════════ RecyclerView Adapter ══════════

    private class ModListAdapter extends RecyclerView.Adapter<ModListAdapter.VH> {

        class VH extends RecyclerView.ViewHolder {
            final TextView title, subtitle, badge, btnValue;
            final ImageView btnUp, btnDown, btnDelete;
            final Switch swEnabled;

            VH(@NonNull View itemView) {
                super(itemView);
                title = itemView.findViewById(R.id.tv_item_title);
                subtitle = itemView.findViewById(R.id.tv_item_subtitle);
                badge = itemView.findViewById(R.id.tv_item_badge);
                btnUp = itemView.findViewById(R.id.btn_item_up);
                btnDown = itemView.findViewById(R.id.btn_item_down);
                btnValue = itemView.findViewById(R.id.btn_item_value);
                swEnabled = itemView.findViewById(R.id.switch_item_enabled);
                btnDelete = itemView.findViewById(R.id.btn_item_delete);
            }
        }

        @NonNull
        @Override
        public VH onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            View v = LayoutInflater.from(parent.getContext()).inflate(R.layout.item_mod_entry, parent, false);
            return new VH(v);
        }

        @Override
        public void onBindViewHolder(@NonNull VH h, int position) {
            RowItem item = rows.get(position);
            Context ctx = h.itemView.getContext();

            h.title.setText(item.title);
            h.subtitle.setText(item.subtitle);

            int accentColor = LauncherThemeManager.getAccentColor(ctx);
            if (item.badge != null && !item.badge.isEmpty()) {
                h.badge.setVisibility(View.VISIBLE);
                h.badge.setText(item.badge);
                h.badge.setTextColor(item.badgeDanger ? ctx.getColor(R.color.launcher_danger) : accentColor);
            } else {
                h.badge.setVisibility(View.GONE);
            }

            h.btnUp.setVisibility(item.showUpDown ? View.VISIBLE : View.GONE);
            h.btnDown.setVisibility(item.showUpDown ? View.VISIBLE : View.GONE);
            if (item.showUpDown && item.payload instanceof ModManager.ModLoaderEntry) {
                ModManager.ModLoaderEntry entry = (ModManager.ModLoaderEntry) item.payload;
                h.btnUp.setOnClickListener(v -> moveModLoaderPriority(entry, -1));
                h.btnDown.setOnClickListener(v -> moveModLoaderPriority(entry, 1));
            }

            if (item.showSwitch) {
                h.swEnabled.setVisibility(View.VISIBLE);
                h.btnValue.setVisibility(View.GONE);
                LauncherThemeManager.applySwitchTheme(h.swEnabled, accentColor);
                h.swEnabled.setOnCheckedChangeListener(null);
                h.swEnabled.setChecked(item.checked);
                h.swEnabled.setOnCheckedChangeListener((btn, isChecked) -> {
                    item.checked = isChecked;
                    onRowToggle(item, isChecked);
                });
            } else {
                h.swEnabled.setVisibility(View.GONE);
                h.btnValue.setVisibility(View.VISIBLE);
                h.btnValue.setTextColor(accentColor);
                h.btnValue.setText(item.valueText != null ? item.valueText : "");
                h.btnValue.setOnClickListener(v -> {
                    if (item.payload instanceof ModManager.JPatchOption) {
                        configDialogs.showEditJPatchValueDialog((ModManager.JPatchOption) item.payload);
                    }
                });
            }

            h.btnDelete.setVisibility(item.showDelete ? View.VISIBLE : View.GONE);
            h.btnDelete.setOnClickListener(v -> confirmDelete(item));
        }

        @Override
        public int getItemCount() {
            return rows.size();
        }
    }

    private void onRowToggle(RowItem item, boolean isChecked) {
        Context appCtx = requireContext().getApplicationContext();
        if (item.payload instanceof ModManager.ModLoaderEntry) {
            ModManager.ModLoaderEntry entry = (ModManager.ModLoaderEntry) item.payload;
            entry.enabled = isChecked;
            ioExecutor.execute(() -> {
                synchronized (modLoaderCache) {
                    ModManager.saveModLoaderConfig(appCtx, modLoaderCache);
                }
                if (!isAdded()) return;
                requireActivity().runOnUiThread(this::reloadCurrentTab);
            });
        } else if (item.payload instanceof ModManager.FileModEntry) {
            ModManager.FileModEntry entry = (ModManager.FileModEntry) item.payload;
            ioExecutor.execute(() -> {
                File renamed = ModManager.toggleDisabledFile(entry.file, isChecked);
                if (renamed != null) {
                    entry.file = renamed;
                    entry.enabled = isChecked;
                    if (entry.isJPatch) {
                        ClientSettings.setSync(appCtx, "jpatch_enabled", isChecked);
                    }
                }
                androidx.fragment.app.FragmentActivity act = getActivity();
                if (act == null || !isAdded()) return;
                act.runOnUiThread(() -> {
                    if (isAdded()) reloadCurrentTab();
                });
            });
        } else if (item.payload instanceof ModManager.JPatchOption) {
            ModManager.JPatchOption opt = (ModManager.JPatchOption) item.payload;
            opt.value = isChecked ? "1" : "0";
            ioExecutor.execute(() -> ModManager.saveJPatchOption(appCtx, opt.section, opt.key, opt.value));
        }
    }

    private void confirmDelete(RowItem item) {
        Context ctx = requireContext();
        new AlertDialog.Builder(ctx)
                .setTitle("Confirm delete")
                .setMessage("Permanently delete “" + item.title + "”? This can't be undone.")
                .setPositiveButton("Delete", (d, w) -> {
                    Context appCtx = ctx.getApplicationContext();
                    ioExecutor.execute(() -> {
                        if (item.payload instanceof ModManager.ModLoaderEntry) {
                            ModManager.ModLoaderEntry entry = (ModManager.ModLoaderEntry) item.payload;
                            ModManager.deleteRecursive(new File(ModManager.getModLoaderDir(appCtx), entry.name));
                            synchronized (modLoaderCache) {
                                modLoaderCache.remove(entry);
                                ModManager.saveModLoaderConfig(appCtx, modLoaderCache);
                            }
                        } else if (item.payload instanceof ModManager.FileModEntry) {
                            ModManager.FileModEntry entry = (ModManager.FileModEntry) item.payload;
                            ModManager.deleteRecursive(entry.file);
                        }
                        if (!isAdded()) return;
                        requireActivity().runOnUiThread(() -> {
                            Toast.makeText(ctx, "Deleted " + item.title, Toast.LENGTH_SHORT).show();
                            reloadCurrentTab();
                        });
                    });
                })
                .setNegativeButton("Cancel", null)
                .show();
    }
}
