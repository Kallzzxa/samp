package com.sampmobile.launcher.ui.fragment;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
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
import com.sampmobile.launcher.MainActivity;
import com.sampmobile.launcher.R;
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
    private SharedPreferences prefs;
    private final ExecutorService ioExecutor = Executors.newSingleThreadExecutor();

    private TextView[] tabViews;
    private TextView tvSectionTitle, tvSectionStatus, tvSectionDesc, tvSectionPath, tvModEmpty;
    private Switch switchSectionMaster;
    private TextView btnActionPrimary, btnActionSecondary, btnActionTertiary, btnActionQuaternary;
    private RecyclerView rvModItems;

    private final List<RowItem> rows = new ArrayList<>();
    private final List<ModManager.ModLoaderEntry> modLoaderCache = new ArrayList<>();
    private ModListAdapter adapter;

    private static final int IMPORT_MODE_ASSET_TOOL_ZIP = 100;

    private int pendingImportTab = -1;
    private ActivityResultLauncher<String[]> filePickerLauncher;
    private ActivityResultLauncher<String[]> multiFilePickerLauncher;

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
                        showAssetImportConfigDialog(Collections.singletonList(uri));
                    } else {
                        handleImportedFile(pendingImportTab, uri);
                    }
                }
        );
        multiFilePickerLauncher = registerForActivityResult(
                new ActivityResultContracts.OpenMultipleDocuments(),
                uris -> {
                    if (uris != null && !uris.isEmpty()) {
                        showAssetImportConfigDialog(uris);
                    }
                }
        );
    }

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container, @Nullable Bundle savedInstanceState) {
        View v = inflater.inflate(R.layout.fragment_mods_manager, container, false);
        rootView = v;
        prefs = requireActivity().getSharedPreferences("samp_settings", Context.MODE_PRIVATE);

        v.findViewById(R.id.btn_mods_back).setOnClickListener(view -> {
            if (requireActivity() instanceof MainActivity) {
                ((MainActivity) requireActivity()).hideSettings();
            }
        });

        TextView tvAbi = v.findViewById(R.id.tv_process_abi);
        tvAbi.setText("当前进程：" + ModManager.getProcessAbiLabel());

        v.findViewById(R.id.btn_mods_logs).setOnClickListener(view -> showLogsDialog());
        v.findViewById(R.id.btn_mods_refresh).setOnClickListener(view -> reloadCurrentTab());

        tabViews = new TextView[]{
                v.findViewById(R.id.mod_tab_modloader),
                v.findViewById(R.id.mod_tab_aml),
                v.findViewById(R.id.mod_tab_jpatch),
                v.findViewById(R.id.mod_tab_monet),
                v.findViewById(R.id.mod_tab_cleo)
        };
        for (int i = 0; i < tabViews.length; i++) {
            final int idx = i;
            tabViews[i].setOnClickListener(view -> selectTab(idx));
        }

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
        updateTabStyles();
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
        updateTabStyles();
        configureSectionPanel();
        reloadCurrentTab();
    }

    private void updateTabStyles() {
        if (tabViews == null || !isAdded()) return;
        int accentColor = LauncherThemeManager.getAccentColor(requireContext());
        for (int i = 0; i < tabViews.length; i++) {
            boolean sel = (i == currentTab);
            tabViews[i].setBackgroundResource(sel ? R.drawable.launcher_btn_primary : R.drawable.launcher_btn_outline);
            tabViews[i].setBackgroundTintList(sel ? ColorStateList.valueOf(accentColor) : null);
            tabViews[i].setTextColor(requireContext().getColor(
                    sel ? R.color.launcher_on_accent : R.color.launcher_text_dim));
        }
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

    private void configureSectionPanel() {
        Context ctx = requireContext();
        String key = prefKeyForTab(currentTab);
        boolean enabled = prefs.getBoolean(key, false);

        switchSectionMaster.setOnCheckedChangeListener(null);
        switchSectionMaster.setChecked(enabled);
        switchSectionMaster.setOnCheckedChangeListener((btn, isChecked) -> {
            prefs.edit().putBoolean(key, isChecked).commit();
            if (currentTab == TAB_JPATCH) {
                if (isChecked && !prefs.getBoolean("aml_scripts", false)) {
                    prefs.edit().putBoolean("aml_scripts", true).commit();
                    Toast.makeText(ctx, "已自动同步开启 AML（JPatch 依赖 AML 运行）", Toast.LENGTH_SHORT).show();
                }
                ioExecutor.execute(() -> ModManager.syncJPatchBinaryState(ctx.getApplicationContext(), isChecked));
            }
            Utils.saveSettings(ctx);
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
                tvSectionTitle.setText("Modloader 资源覆写");
                tvSectionStatus.setText("状态：已就绪 · 内置 IMG+TXD 零污染覆写");
                tvSectionStatus.setTextColor(accentColor);
                tvSectionDesc.setText("无需修改官方 gta3.img 或基础贴图：内置 IMG+TXD 工具可直接导入皮肤/载具/武器模型（.dff/.ifp/.col）与 PNG/PC .txd 贴图，进游戏自动打包虚拟 models.img 并挂载自定义 texdb。");
                pathDir = ModManager.getModLoaderDir(ctx);
                btnActionPrimary.setText("导入皮肤/模型 (IMG+TXD)");
                btnActionPrimary.setOnClickListener(v -> showAssetImportSourceChooser());
                btnActionSecondary.setText("导入完整 ZIP 模组包");
                btnActionSecondary.setOnClickListener(v -> launchPicker(TAB_MODLOADER, new String[]{"application/zip", "application/x-zip-compressed", "*/*"}));
                btnActionTertiary.setVisibility(View.VISIBLE);
                btnActionTertiary.setText("新建空模组目录");
                btnActionTertiary.setOnClickListener(v -> showCreateModFolderDialog());
                break;

            case TAB_AML:
                tvSectionTitle.setText("AndroidModLoader (AML)");
                tvSectionStatus.setText("状态：已内置 AML 1.5.0 · 支持 64位/32位");
                tvSectionStatus.setTextColor(accentColor);
                tvSectionDesc.setText("加载 mods/ 目录下的原生 .so 插件。启动时会自动将包名伪装为 com.rockstargames.gtasa 并输出日志，请确保导入与当前进程一致的 " + ModManager.getProcessAbiName() + " 插件。");
                pathDir = ModManager.getAmlModsDir(ctx);
                btnActionPrimary.setText("导入 AML 插件 (.so)");
                btnActionPrimary.setOnClickListener(v -> launchPicker(TAB_AML, new String[]{"*/*"}));
                btnActionSecondary.setText("AML 核心配置");
                btnActionSecondary.setOnClickListener(v -> showAmlCoreSettingsDialog());
                break;

            case TAB_JPATCH:
                tvSectionTitle.setText("JPatch 引擎补丁");
                tvSectionStatus.setText("正在检查 libJPatch.so 状态…");
                tvSectionStatus.setTextColor(ctx.getColor(R.color.launcher_text_dim));
                tvSectionDesc.setText("RusJJ 的 GTA:SA 2.10 修复补丁集（依赖 AML）。由于版权协议禁止重新分发二进制，请从官方页面下载对应 " + ModManager.getProcessAbiName() + " 的 libJPatch.so 后点击下方导入；右侧可直接可视化编辑 JPatch.ini。");
                pathDir = ModManager.getJPatchConfigFile(ctx);
                btnActionPrimary.setText("导入 libJPatch.so");
                btnActionPrimary.setOnClickListener(v -> launchPicker(TAB_JPATCH, new String[]{"*/*"}));
                btnActionSecondary.setText("应用 SA-MP 安全预设");
                btnActionSecondary.setOnClickListener(v -> applyJPatchSafePreset());
                btnActionTertiary.setVisibility(View.VISIBLE);
                btnActionTertiary.setText("新增自定义配置项");
                btnActionTertiary.setOnClickListener(v -> showAddJPatchKeyDialog());
                btnActionQuaternary.setVisibility(View.VISIBLE);
                btnActionQuaternary.setText("打开 JPatch 官方页");
                btnActionQuaternary.setOnClickListener(v -> openUrl(ModManager.JPATCH_OFFICIAL_URL));
                break;

            case TAB_MONET:
                tvSectionTitle.setText("MonetLoader (Lua)");
                tvSectionStatus.setText("状态：已内置 MonetLoader 3.8.0 · 双 ABI + 原生 SA-MP 桥接");
                tvSectionStatus.setTextColor(accentColor);
                tvSectionDesc.setText("运行 files/monetloader/ 目录下的 .lua 脚本（LuaJIT 5.1 + mimgui + samp.events + 原生 RakHook 桥接，兼容 MoonLoader 生态），首次开启将自动释放内置 Lua 标准库。");
                pathDir = ModManager.getMonetDir(ctx);
                btnActionPrimary.setText("导入 Lua 脚本 (.lua)");
                btnActionPrimary.setOnClickListener(v -> launchPicker(TAB_MONET, new String[]{"*/*"}));
                btnActionSecondary.setVisibility(View.VISIBLE);
                btnActionSecondary.setText("重装内置 Lua 标准库");
                btnActionSecondary.setOnClickListener(v -> {
                    final Context appCtx = ctx.getApplicationContext();
                    Toast.makeText(ctx, "正在释放 MonetLoader 内置 Lua 库…", Toast.LENGTH_SHORT).show();
                    ioExecutor.execute(() -> {
                        boolean ok = ModManager.ensureMonetLibsInstalled(appCtx, true);
                        if (isAdded()) {
                            requireActivity().runOnUiThread(() -> Toast.makeText(ctx,
                                    ok ? "已重装 mimgui / samp.events / moonloader 标准库" : "释放标准库失败",
                                    Toast.LENGTH_SHORT).show());
                        }
                    });
                });
                break;

            case TAB_CLEO:
            default:
                tvSectionTitle.setText("CLEO 脚本");
                tvSectionStatus.setText("状态：已内置原生 CLEO 引擎 · 支持 64位/32位");
                tvSectionStatus.setTextColor(accentColor);
                tvSectionDesc.setText("运行 files/cleo/ 目录下的 .csa/.cs（自动启动）、.csi（游戏内屏幕顶部中段向下滑动打开 CLEO 菜单调用）与 .fxt 自定义文本。双 ABI 原生兼容，自动同步联机玩家句柄。");
                pathDir = ModManager.getCleoDir(ctx);
                btnActionPrimary.setText("导入 CLEO 脚本");
                btnActionPrimary.setOnClickListener(v -> launchPicker(TAB_CLEO, new String[]{"*/*"}));
                btnActionSecondary.setVisibility(View.GONE);
                break;
        }

        final String pathStr = pathDir != null ? pathDir.getAbsolutePath() : "";
        tvSectionPath.setText("路径（点按复制）：\n" + pathStr);
        tvSectionPath.setOnClickListener(v -> {
            ClipboardManager cm = (ClipboardManager) ctx.getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm != null) {
                cm.setPrimaryClip(ClipData.newPlainText("mod_path", pathStr));
                Toast.makeText(ctx, "已复制路径", Toast.LENGTH_SHORT).show();
            }
        });
    }

    private void reloadCurrentTab() {
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
                    r.subtitle = e.fileCount + " 个文件 · " + Utils.convertBytesToString(e.totalBytes) + " · 优先级 " + e.priority;
                    r.badge = e.enabled ? "已启用" : "已禁用";
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
                            + (e.isJPatch ? " · JPatch 补丁插件" : " · AML 原生插件")
                            + (e.abiCompatible ? "" : " · 与当前进程架构不匹配！");
                    r.badge = e.abiCompatible ? e.abiLabel : ("不兼容: " + e.abiLabel);
                    r.badgeDanger = !e.abiCompatible;
                    r.checked = e.enabled;
                    r.showDelete = true;
                    r.payload = e;
                    loaded.add(r);
                }
            } else if (tab == TAB_JPATCH) {
                ModManager.FileModEntry jpBin = ModManager.findJPatchBinary(appCtx);
                if (jpBin == null) {
                    jpatchStatusText = "未检测到 libJPatch.so — 请先点击下方导入（当前可预先编辑配置）";
                    jpatchStatusOk = false;
                } else if (!jpBin.abiCompatible) {
                    jpatchStatusText = "已检测到 " + jpBin.displayName + "，但架构为 " + jpBin.abiLabel + "（与当前进程不匹配！）";
                    jpatchStatusOk = false;
                } else {
                    jpatchStatusText = "已安装：" + jpBin.displayName + " (" + jpBin.abiLabel + ") · "
                            + (jpBin.enabled ? "已启用" : "已禁用");
                    jpatchStatusOk = true;
                }

                List<ModManager.JPatchOption> opts = ModManager.loadJPatchOptions(appCtx);
                for (ModManager.JPatchOption opt : opts) {
                    RowItem r = new RowItem();
                    r.title = opt.title;
                    r.subtitle = opt.key + " — " + opt.description;
                    if (!opt.safeForSamp) {
                        r.badge = "联机慎用";
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
                    r.subtitle = Utils.convertBytesToString(e.sizeBytes) + " · Lua 脚本";
                    r.badge = e.enabled ? "已启用" : "已禁用";
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
                    r.subtitle = Utils.convertBytesToString(e.sizeBytes) + " · CLEO 脚本";
                    r.badge = e.enabled ? "已启用" : "已禁用";
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
                return "暂无 Modloader 模组包\n点击左侧「导入皮肤/模型 (IMG+TXD)」或「导入完整 ZIP 模组包」，也可手动放入 files/modloader/";
            case TAB_AML:
                return "暂无 AML 插件 (.so)\n点击左侧「导入 AML 插件 (.so)」或放入 Android/data/com.sampmobile.launcher/mods/";
            case TAB_MONET:
                return "暂无 MonetLoader Lua 脚本\n点击左侧「导入 Lua 脚本 (.lua)」或放入 files/monetloader/";
            case TAB_CLEO:
                return "暂无 CLEO 脚本\n点击左侧「导入 CLEO 脚本」或放入 files/cleo/";
            default:
                return "暂无条目";
        }
    }

    private void launchPicker(int tab, String[] mimeTypes) {
        pendingImportTab = tab;
        try {
            filePickerLauncher.launch(mimeTypes);
        } catch (Exception e) {
            Toast.makeText(requireContext(), "无法打开系统文件选择器: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private void handleImportedFile(int tab, Uri uri) {
        Context appCtx = requireContext().getApplicationContext();
        Toast.makeText(requireContext(), "正在导入…", Toast.LENGTH_SHORT).show();
        ioExecutor.execute(() -> {
            String msg;
            try {
                if (tab == TAB_MODLOADER) {
                    String folder = ModManager.importModLoaderZip(appCtx, uri);
                    msg = "已导入模组包：" + folder;
                } else if (tab == TAB_AML || tab == TAB_JPATCH) {
                    String origName = ModManager.queryDisplayName(appCtx, uri);
                    if (origName == null || !origName.toLowerCase(Locale.ROOT).endsWith(".so")) {
                        throw new IllegalArgumentException("请选择 .so 格式的插件文件");
                    }
                    File dest = ModManager.importFileToDir(appCtx, uri, ModManager.getAmlModsDir(appCtx),
                            tab == TAB_JPATCH ? "libJPatch.so" : "plugin.so");
                    ModManager.ElfInfo elf = ModManager.inspectElfAbi(dest);
                    if (!elf.valid) {
                        dest.delete();
                        throw new IllegalArgumentException("导入的文件不是有效的 Android .so (ELF) 库");
                    }
                    if (!elf.matchesProcess) {
                        msg = "已导入 " + dest.getName() + "，注意：该插件为 " + elf.abiName + "，与当前 " + ModManager.getProcessAbiLabel() + " 不匹配！";
                    } else {
                        msg = "已导入 " + dest.getName() + " (" + elf.abiName + ")";
                    }
                } else if (tab == TAB_MONET) {
                    File dest = ModManager.importFileToDir(appCtx, uri, ModManager.getMonetDir(appCtx), "script.lua");
                    msg = "已导入 Lua 脚本：" + dest.getName();
                } else {
                    File dest = ModManager.importFileToDir(appCtx, uri, ModManager.getCleoDir(appCtx), "script.csa");
                    msg = "已导入 CLEO 脚本：" + dest.getName();
                }
            } catch (Exception e) {
                msg = "导入失败：" + e.getMessage();
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
        input.setHint("输入模组文件夹名称（如 HD_Roads）");
        int pad = (int) (16 * getResources().getDisplayMetrics().density);
        LinearLayout wrap = new LinearLayout(requireContext());
        wrap.setPadding(pad, pad / 2, pad, 0);
        wrap.addView(input, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        new AlertDialog.Builder(requireContext())
                .setTitle("新建 Modloader 目录")
                .setView(wrap)
                .setPositiveButton("创建", (d, w) -> {
                    String name = input.getText().toString().trim();
                    if (name.isEmpty()) return;
                    Context appCtx = requireContext().getApplicationContext();
                    ioExecutor.execute(() -> {
                        File created = ModManager.createModLoaderFolder(appCtx, name);
                        if (!isAdded()) return;
                        requireActivity().runOnUiThread(() -> {
                            if (!isAdded()) return;
                            Toast.makeText(requireContext(),
                                    created != null ? ("已创建目录：" + created.getName()) : "创建目录失败",
                                    Toast.LENGTH_SHORT).show();
                            reloadCurrentTab();
                        });
                    });
                })
                .setNegativeButton("取消", null)
                .show();
    }

    // ══════════ Built-in GTA IMG + TXD Tool Wizard ══════════

    private void showAssetImportSourceChooser() {
        Context ctx = requireContext();
        String[] choices = {
                "从 ZIP 压缩包导入（自动识别 .dff + .png / PC .txd）",
                "多选散文件导入（.dff / .ifp / .col / .txd / .png）"
        };
        new AlertDialog.Builder(ctx)
                .setTitle("内置 IMG + TXD 零污染导入")
                .setItems(choices, (d, which) -> {
                    if (which == 0) {
                        launchPicker(IMPORT_MODE_ASSET_TOOL_ZIP,
                                new String[]{"application/zip", "application/x-zip-compressed", "*/*"});
                    } else {
                        try {
                            multiFilePickerLauncher.launch(new String[]{"*/*"});
                        } catch (Exception e) {
                            Toast.makeText(ctx, "无法打开多文件选择器: " + e.getMessage(), Toast.LENGTH_LONG).show();
                        }
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void showAssetImportConfigDialog(List<Uri> uris) {
        if (!isAdded() || uris == null || uris.isEmpty()) return;
        Context ctx = requireContext();

        String suggestedPack = "";
        StringBuilder summary = new StringBuilder();
        if (uris.size() == 1) {
            String name = ModManager.queryDisplayName(ctx, uris.get(0));
            if (name == null) name = "所选文件";
            summary.append("已选文件：").append(name);
            int dot = name.lastIndexOf('.');
            suggestedPack = (dot > 0) ? name.substring(0, dot) : name;
        } else {
            summary.append("已多选 ").append(uris.size()).append(" 个文件：");
            int shown = 0;
            for (Uri u : uris) {
                String n = ModManager.queryDisplayName(ctx, u);
                if (n == null) continue;
                if (suggestedPack.isEmpty() && n.toLowerCase(Locale.ROOT).endsWith(".dff")) {
                    int dot = n.lastIndexOf('.');
                    suggestedPack = (dot > 0) ? n.substring(0, dot) : n;
                }
                if (shown < 3) {
                    if (shown > 0) summary.append("、");
                    summary.append(n);
                    shown++;
                }
            }
            if (uris.size() > 3) summary.append(" 等");
            if (suggestedPack.isEmpty()) suggestedPack = "CustomSkin";
        }
        suggestedPack = suggestedPack.replaceAll("[\\\\/:*?\"<>|]", "_").trim();
        if (suggestedPack.length() > 36) suggestedPack = suggestedPack.substring(0, 36);

        int pad = (int) (14 * getResources().getDisplayMetrics().density);
        ScrollView sv = new ScrollView(ctx);
        LinearLayout layout = new LinearLayout(ctx);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(pad, pad / 2, pad, pad / 2);
        sv.addView(layout);

        TextView tvSummary = new TextView(ctx);
        tvSummary.setText(summary.toString());
        tvSummary.setTextSize(12);
        layout.addView(tvSummary);

        TextView lblPack = new TextView(ctx);
        lblPack.setText("\n模组包名称（生成在 modloader/<名称>/，可随时开关或删除）：");
        lblPack.setTextSize(12);
        layout.addView(lblPack);

        EditText etPack = new EditText(ctx);
        etPack.setText(suggestedPack);
        etPack.setHint("如 MaiShiranui_Skin1");
        etPack.setSingleLine(true);
        layout.addView(etPack);

        TextView lblTarget = new TextView(ctx);
        lblTarget.setText("\n目标模型名（可选；留空 = 保持原 .dff 文件名，如皮肤/载具需改名请点击下方速查）：");
        lblTarget.setTextSize(12);
        layout.addView(lblTarget);

        EditText etTarget = new EditText(ctx);
        etTarget.setHint("留空保持原名，或填如 truth / infernus");
        etTarget.setSingleLine(true);
        layout.addView(etTarget);

        LinearLayout btnRow = new LinearLayout(ctx);
        btnRow.setOrientation(LinearLayout.HORIZONTAL);
        Button btnPickSkin = new Button(ctx);
        btnPickSkin.setText("选择皮肤 ID");
        btnPickSkin.setOnClickListener(v -> showIdeSelectorDialog(true, etTarget));
        Button btnPickVeh = new Button(ctx);
        btnPickVeh.setText("选择载具 ID");
        btnPickVeh.setOnClickListener(v -> showIdeSelectorDialog(false, etTarget));
        Button btnClearTarget = new Button(ctx);
        btnClearTarget.setText("保持原名");
        btnClearTarget.setOnClickListener(v -> etTarget.setText(""));

        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        btnRow.addView(btnPickSkin, lp);
        btnRow.addView(btnPickVeh, lp);
        btnRow.addView(btnClearTarget, lp);
        layout.addView(btnRow);

        TextView tvHint = new TextView(ctx);
        tvHint.setText("\n说明：PNG/JPG/WEBP 与 PC .txd 会自动编译为 Android 贴图库，.dff/.ifp/.col 在进游戏时自动装入虚拟 models.img，完全不修改官方 gta3.img。");
        tvHint.setTextSize(11);
        layout.addView(tvHint);

        new AlertDialog.Builder(ctx)
                .setTitle("配置并编译模型 / 贴图包")
                .setView(sv)
                .setPositiveButton("开始编译并安装", (d, w) -> {
                    String packName = etPack.getText().toString().trim();
                    String targetModel = etTarget.getText().toString().trim();
                    runAssetImport(uris, packName, targetModel.isEmpty() ? null : targetModel);
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void showIdeSelectorDialog(boolean isSkins, EditText targetField) {
        Context ctx = requireContext();
        Context appCtx = ctx.getApplicationContext();
        Toast.makeText(ctx, isSkins ? "正在读取 SAMP/peds.ide…" : "正在读取 SAMP/vehicles.ide…", Toast.LENGTH_SHORT).show();

        ioExecutor.execute(() -> {
            List<ModAssetTool.IdeEntry> allEntries = isSkins
                    ? ModAssetTool.loadPedSkins(appCtx)
                    : ModAssetTool.loadVehicles(appCtx);

            if (!isAdded()) return;
            requireActivity().runOnUiThread(() -> {
                if (!isAdded()) return;
                if (allEntries.isEmpty()) {
                    Toast.makeText(ctx,
                            isSkins ? "未找到 SAMP/peds.ide（请先部署游戏数据）" : "未找到 SAMP/vehicles.ide（请先部署游戏数据）",
                            Toast.LENGTH_LONG).show();
                    return;
                }

                int pad = (int) (12 * getResources().getDisplayMetrics().density);
                LinearLayout box = new LinearLayout(ctx);
                box.setOrientation(LinearLayout.VERTICAL);
                box.setPadding(pad, pad / 2, pad, pad / 2);

                EditText etSearch = new EditText(ctx);
                etSearch.setHint(isSkins ? "搜索皮肤 ID 或名称（如 1 / truth / swat）" : "搜索载具 ID 或名称（如 411 / infernus / sultan）");
                etSearch.setSingleLine(true);
                box.addView(etSearch);

                List<ModAssetTool.IdeEntry> filtered = new ArrayList<>(allEntries);
                List<String> labels = new ArrayList<>(allEntries.size());
                for (ModAssetTool.IdeEntry e : allEntries) labels.add(e.displayLabel());

                ArrayAdapter<String> listAdapter = new ArrayAdapter<>(ctx, android.R.layout.simple_list_item_1, labels);
                ListView lv = new ListView(ctx);
                lv.setAdapter(listAdapter);
                int listHeight = (int) (220 * getResources().getDisplayMetrics().density);
                box.addView(lv, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, listHeight));

                AlertDialog pickerDlg = new AlertDialog.Builder(ctx)
                        .setTitle(isSkins ? "选择目标 SA-MP 皮肤" : "选择目标 SA-MP 载具")
                        .setView(box)
                        .setNegativeButton("取消", null)
                        .create();

                etSearch.addTextChangedListener(new TextWatcher() {
                    @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
                    @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
                    @Override public void afterTextChanged(Editable s) {
                        String q = s.toString().trim().toLowerCase(Locale.ROOT);
                        filtered.clear();
                        labels.clear();
                        for (ModAssetTool.IdeEntry e : allEntries) {
                            if (q.isEmpty()
                                    || String.valueOf(e.id).contains(q)
                                    || e.modelName.toLowerCase(Locale.ROOT).contains(q)
                                    || e.extra.toLowerCase(Locale.ROOT).contains(q)) {
                                filtered.add(e);
                                labels.add(e.displayLabel());
                            }
                        }
                        listAdapter.notifyDataSetChanged();
                    }
                });

                lv.setOnItemClickListener((parent, view, position, id) -> {
                    if (position >= 0 && position < filtered.size()) {
                        ModAssetTool.IdeEntry picked = filtered.get(position);
                        targetField.setText(picked.modelName);
                        Toast.makeText(ctx, "已选 " + picked.displayLabel(), Toast.LENGTH_SHORT).show();
                        pickerDlg.dismiss();
                    }
                });

                pickerDlg.show();
            });
        });
    }

    private void runAssetImport(List<Uri> uris, String packName, @Nullable String targetModel) {
        Context ctx = requireContext();
        Context appCtx = ctx.getApplicationContext();

        TextView tvProg = new TextView(ctx);
        int pad = (int) (18 * getResources().getDisplayMetrics().density);
        tvProg.setPadding(pad, pad, pad, pad);
        tvProg.setText("准备解析并编译资产…");

        AlertDialog progDlg = new AlertDialog.Builder(ctx)
                .setTitle("正在编译安装模组")
                .setView(tvProg)
                .setCancelable(false)
                .create();
        progDlg.show();

        ioExecutor.execute(() -> {
            try {
                ModAssetTool.ImportResult res = ModAssetTool.importAssetPack(
                        appCtx,
                        uris,
                        packName,
                        targetModel,
                        msg -> {
                            if (!isAdded()) return;
                            requireActivity().runOnUiThread(() -> {
                                if (progDlg.isShowing()) tvProg.setText(msg);
                            });
                        }
                );

                // Ensure Modloader master toggle is enabled so the installed pack works right away
                boolean autoEnabledMaster = false;
                if (!prefs.getBoolean("modloader", false)) {
                    prefs.edit().putBoolean("modloader", true).commit();
                    Utils.saveSettings(appCtx);
                    autoEnabledMaster = true;
                }
                final boolean enabledNow = autoEnabledMaster;

                androidx.fragment.app.FragmentActivity act = getActivity();
                if (act == null || !isAdded()) return;
                act.runOnUiThread(() -> {
                    if (progDlg.isShowing()) progDlg.dismiss();
                    if (!isAdded() || getContext() == null) return;

                    if (enabledNow) {
                        configureSectionPanel();
                    }
                    reloadCurrentTab();

                    StringBuilder detail = new StringBuilder();
                    detail.append("模组包目录：modloader/").append(res.packName).append("\n");
                    detail.append("• 模型文件：").append(res.modelCount).append(" 个（进游戏自动装入虚拟 models.img）\n");
                    if (res.renamedPrimaryModel != null) {
                        detail.append("• 主模型已重命名为：").append(res.renamedPrimaryModel).append("\n");
                    }
                    if (res.compiledTextureCount > 0) {
                        detail.append("• 已编译贴图：").append(res.compiledTextureCount)
                                .append(" 张 → texdb/").append(res.texDbName).append("/\n");
                    }
                    if (res.passthroughFileCount > 0) {
                        detail.append("• 原样导入附带文件：").append(res.passthroughFileCount).append(" 个\n");
                    }
                    if (enabledNow) {
                        detail.append("• 已自动开启 Modloader 总开关\n");
                    }
                    if (!res.warnings.isEmpty()) {
                        detail.append("\n注意事项：\n");
                        for (String w : res.warnings) {
                            detail.append("- ").append(w).append("\n");
                        }
                    }

                    new AlertDialog.Builder(getContext())
                            .setTitle("编译并安装成功")
                            .setMessage(detail.toString().trim())
                            .setPositiveButton("确定", null)
                            .show();
                });
            } catch (Exception e) {
                androidx.fragment.app.FragmentActivity act = getActivity();
                if (act == null || !isAdded()) return;
                act.runOnUiThread(() -> {
                    if (progDlg.isShowing()) progDlg.dismiss();
                    if (!isAdded() || getContext() == null) return;
                    new AlertDialog.Builder(getContext())
                            .setTitle("导入失败")
                            .setMessage(e.getMessage() != null ? e.getMessage() : e.toString())
                            .setPositiveButton("确定", null)
                            .show();
                });
            }
        });
    }

    private void showAmlCoreSettingsDialog() {
        Context ctx = requireContext();
        ModManager.AmlCoreSettings cur = ModManager.readAmlCoreSettings(ctx);

        int pad = (int) (14 * getResources().getDisplayMetrics().density);
        LinearLayout layout = new LinearLayout(ctx);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(pad, pad / 2, pad, 0);

        TextView lblFake = new TextView(ctx);
        lblFake.setText("伪装包名 (FakePackageName，供 GTA:SA 插件识别)：");
        lblFake.setTextSize(12);
        layout.addView(lblFake);

        EditText etFake = new EditText(ctx);
        etFake.setText(cur.fakePackageName);
        etFake.setSingleLine(true);
        layout.addView(etFake);

        CheckBox cbOverlay = new CheckBox(ctx);
        cbOverlay.setText("进游戏时显示 AML 已加载模组提示（Android 12+ 建议关闭，以免遮挡输入法）");
        cbOverlay.setChecked(cur.showModsOverlay);
        layout.addView(cbOverlay);

        CheckBox cbLogFile = new CheckBox(ctx);
        cbLogFile.setText("将 AML 运行日志写入 android_log_print.txt");
        cbLogFile.setChecked(cur.printLogsToFile);
        layout.addView(cbLogFile);

        CheckBox cbVerbose = new CheckBox(ctx);
        cbVerbose.setText("详细日志输出（记录 INFO 级别加载信息）");
        cbVerbose.setChecked(cur.verboseLogs);
        layout.addView(cbVerbose);

        CheckBox cbSignal = new CheckBox(ctx);
        cbSignal.setText("启用 AML 崩溃信号捕获（默认关闭，推荐保留本端原生崩溃日志）");
        cbSignal.setChecked(cur.signalHandler);
        layout.addView(cbSignal);

        new AlertDialog.Builder(ctx)
                .setTitle("AML 核心配置 (ModLoaderCore.ini)")
                .setView(layout)
                .setPositiveButton("保存", (d, w) -> {
                    ModManager.AmlCoreSettings updated = new ModManager.AmlCoreSettings();
                    updated.fakePackageName = etFake.getText().toString().trim();
                    updated.showModsOverlay = cbOverlay.isChecked();
                    updated.printLogsToFile = cbLogFile.isChecked();
                    updated.verboseLogs = cbVerbose.isChecked();
                    updated.signalHandler = cbSignal.isChecked();
                    Context appCtx = ctx.getApplicationContext();
                    ioExecutor.execute(() -> {
                        ModManager.saveAmlCoreSettings(appCtx, updated);
                        if (!isAdded()) return;
                        requireActivity().runOnUiThread(() ->
                                Toast.makeText(ctx, "已保存 ModLoaderCore.ini", Toast.LENGTH_SHORT).show());
                    });
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void applyJPatchSafePreset() {
        Context ctx = requireContext();
        new AlertDialog.Builder(ctx)
                .setTitle("应用 SA-MP 安全预设")
                .setMessage("将为 JPatch.ini 写入 SA-MP 2.10 安全推荐值：开启 2.10 防崩/雾墙/高帧率物理/NEON 优化，关闭与本端冲突的 UVAnim/强锁贴图格式/流送内存重复扩容项。是否继续？")
                .setPositiveButton("应用", (d, w) -> {
                    Context appCtx = ctx.getApplicationContext();
                    ioExecutor.execute(() -> {
                        ModManager.applyJPatchSampSafePreset(appCtx);
                        if (!isAdded()) return;
                        requireActivity().runOnUiThread(() -> {
                            Toast.makeText(ctx, "已应用 SA-MP 安全预设到 JPatch.ini", Toast.LENGTH_SHORT).show();
                            reloadCurrentTab();
                        });
                    });
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void showAddJPatchKeyDialog() {
        Context ctx = requireContext();
        int pad = (int) (14 * getResources().getDisplayMetrics().density);
        LinearLayout layout = new LinearLayout(ctx);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(pad, pad / 2, pad, 0);

        EditText etSec = new EditText(ctx);
        etSec.setHint("节名 Section（默认 Patches）");
        etSec.setText("Patches");
        layout.addView(etSec);

        EditText etKey = new EditText(ctx);
        etKey.setHint("配置键 Key（如 FixWaterQuadrant）");
        layout.addView(etKey);

        EditText etVal = new EditText(ctx);
        etVal.setHint("值 Value（如 1 或 0）");
        etVal.setText("1");
        layout.addView(etVal);

        new AlertDialog.Builder(ctx)
                .setTitle("新增 JPatch.ini 配置项")
                .setView(layout)
                .setPositiveButton("保存", (d, w) -> {
                    String rawSec = etSec.getText().toString().trim();
                    final String sec = rawSec.isEmpty() ? "Patches" : rawSec;
                    final String key = etKey.getText().toString().trim();
                    final String val = etVal.getText().toString().trim();
                    if (key.isEmpty()) return;
                    Context appCtx = ctx.getApplicationContext();
                    ioExecutor.execute(() -> {
                        ModManager.saveJPatchOption(appCtx, sec, key, val);
                        if (!isAdded()) return;
                        requireActivity().runOnUiThread(() -> {
                            Toast.makeText(ctx, "已写入 " + key + "=" + val, Toast.LENGTH_SHORT).show();
                            reloadCurrentTab();
                        });
                    });
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void showEditJPatchValueDialog(ModManager.JPatchOption opt) {
        Context ctx = requireContext();
        EditText etVal = new EditText(ctx);
        etVal.setText(opt.value);
        etVal.setSingleLine(true);
        int pad = (int) (16 * getResources().getDisplayMetrics().density);
        LinearLayout wrap = new LinearLayout(ctx);
        wrap.setPadding(pad, pad / 2, pad, 0);
        wrap.addView(etVal, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        new AlertDialog.Builder(ctx)
                .setTitle("编辑 " + opt.key)
                .setView(wrap)
                .setPositiveButton("保存", (d, w) -> {
                    String val = etVal.getText().toString().trim();
                    opt.value = val;
                    Context appCtx = ctx.getApplicationContext();
                    ioExecutor.execute(() -> {
                        ModManager.saveJPatchOption(appCtx, opt.section, opt.key, val);
                        if (!isAdded()) return;
                        requireActivity().runOnUiThread(this::reloadCurrentTab);
                    });
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void showLogsDialog() {
        Context ctx = requireContext();
        String[] choices = {
                "AML 日志 (android_log_print.txt)",
                "最新客户端日志 (ModLoader / Hook)",
                "MonetLoader 日志 (logs/monetloader.log)",
                "CLEO 引擎日志 (cleo/cleo.log)"
        };
        new AlertDialog.Builder(ctx)
                .setTitle("选择要查看的日志")
                .setItems(choices, (d, which) -> {
                    Context appCtx = ctx.getApplicationContext();
                    ioExecutor.execute(() -> {
                        File logFile;
                        if (which == 0) logFile = ModManager.getAmlLogFile(appCtx);
                        else if (which == 1) logFile = ModManager.getLatestClientLogFile(appCtx);
                        else if (which == 2) logFile = ModManager.getMonetLogFile(appCtx);
                        else logFile = ModManager.getCleoLogFile(appCtx);

                        String content = ModManager.readLogTail(logFile, 32 * 1024);
                        if (content.isEmpty()) {
                            content = "暂无日志内容。\n文件路径：" + (logFile != null ? logFile.getAbsolutePath() : "未生成");
                        }
                        final String finalContent = content;
                        final File finalFile = logFile;
                        if (!isAdded()) return;
                        requireActivity().runOnUiThread(() -> showLogTextPopup(choices[which], finalContent, finalFile));
                    });
                })
                .show();
    }

    private void showLogTextPopup(String title, String content, @Nullable File file) {
        Context ctx = requireContext();
        ScrollView sv = new ScrollView(ctx);
        TextView tv = new TextView(ctx);
        int pad = (int) (12 * getResources().getDisplayMetrics().density);
        tv.setPadding(pad, pad, pad, pad);
        tv.setTypeface(Typeface.MONOSPACE);
        tv.setTextSize(10);
        tv.setTextIsSelectable(true);
        tv.setText(content);
        sv.addView(tv);

        AlertDialog.Builder b = new AlertDialog.Builder(ctx)
                .setTitle(title)
                .setView(sv)
                .setPositiveButton("复制", (d, w) -> {
                    ClipboardManager cm = (ClipboardManager) ctx.getSystemService(Context.CLIPBOARD_SERVICE);
                    if (cm != null) {
                        cm.setPrimaryClip(ClipData.newPlainText("mod_log", content));
                        Toast.makeText(ctx, "已复制日志", Toast.LENGTH_SHORT).show();
                    }
                })
                .setNegativeButton("关闭", null);
        if (file != null && file.isFile()) {
            b.setNeutralButton("清空", (d, w) -> ioExecutor.execute(() -> {
                file.delete();
                if (!isAdded()) return;
                requireActivity().runOnUiThread(() -> Toast.makeText(ctx, "已清空日志文件", Toast.LENGTH_SHORT).show());
            }));
        }
        b.show();
    }

    private void openUrl(String url) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (Exception e) {
            Toast.makeText(requireContext(), "无法打开链接: " + url, Toast.LENGTH_SHORT).show();
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
                        showEditJPatchValueDialog((ModManager.JPatchOption) item.payload);
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
                        prefs.edit().putBoolean("jpatch_enabled", isChecked).commit();
                        Utils.saveSettings(appCtx);
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
                .setTitle("确认删除")
                .setMessage("确定要永久删除「" + item.title + "」吗？此操作无法撤销。")
                .setPositiveButton("删除", (d, w) -> {
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
                            Toast.makeText(ctx, "已删除 " + item.title, Toast.LENGTH_SHORT).show();
                            reloadCurrentTab();
                        });
                    });
                })
                .setNegativeButton("取消", null)
                .show();
    }
}
