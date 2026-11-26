package com.sampmobile.launcher.ui.fragment;

import android.content.Context;
import android.net.Uri;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AlertDialog;
import androidx.fragment.app.FragmentActivity;

import com.sampmobile.launcher.utils.ClientSettings;
import com.sampmobile.launcher.utils.ModAssetTool;
import com.sampmobile.launcher.utils.ModManager;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Built-in GTA IMG + TXD zero-pollution asset import wizard for {@link ModsManagerFragment}.
 */
final class ModAssetImportWizard {
    private final ModsManagerFragment host;

    ModAssetImportWizard(ModsManagerFragment host) {
        this.host = host;
    }

    void showSourceChooser() {
        Context ctx = host.requireContext();
        String[] choices = {
                "Import from a ZIP archive (auto-detects .dff + .png / PC .txd)",
                "Import multiple loose files (.dff / .ifp / .col / .txd / .png)"
        };
        new AlertDialog.Builder(ctx)
                .setTitle("Built-in IMG + TXD Clean Import")
                .setItems(choices, (d, which) -> {
                    if (which == 0) {
                        host.launchPicker(ModsManagerFragment.IMPORT_MODE_ASSET_TOOL_ZIP,
                                new String[]{"application/zip", "application/x-zip-compressed", "*/*"});
                    } else {
                        try {
                            host.multiFilePickerLauncher.launch(new String[]{"*/*"});
                        } catch (Exception e) {
                            Toast.makeText(ctx, "Could not open the multi-file picker: " + e.getMessage(), Toast.LENGTH_LONG).show();
                        }
                    }
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    void showConfigDialog(List<Uri> uris) {
        if (!host.isAdded() || uris == null || uris.isEmpty()) return;
        Context ctx = host.requireContext();

        String suggestedPack = "";
        StringBuilder summary = new StringBuilder();
        if (uris.size() == 1) {
            String name = ModManager.queryDisplayName(ctx, uris.get(0));
            if (name == null) name = "Selected file";
            summary.append("Selected file: ").append(name);
            int dot = name.lastIndexOf('.');
            suggestedPack = (dot > 0) ? name.substring(0, dot) : name;
        } else {
            summary.append("Selected ").append(uris.size()).append(" files:");
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
            if (uris.size() > 3) summary.append(" etc.");
            if (suggestedPack.isEmpty()) suggestedPack = "CustomSkin";
        }
        suggestedPack = suggestedPack.replaceAll("[\\\\/:*?\"<>|]", "_").trim();
        if (suggestedPack.length() > 36) suggestedPack = suggestedPack.substring(0, 36);

        int pad = (int) (14 * host.getResources().getDisplayMetrics().density);
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
        lblPack.setText("\nMod pack name (created under modloader/<name>/, can be toggled or deleted anytime):");
        lblPack.setTextSize(12);
        layout.addView(lblPack);

        EditText etPack = new EditText(ctx);
        etPack.setText(suggestedPack);
        etPack.setHint("e.g. MaiShiranui_Skin1");
        etPack.setSingleLine(true);
        layout.addView(etPack);

        TextView lblTarget = new TextView(ctx);
        lblTarget.setText("\nTarget model name (optional; leave empty to keep the original .dff file name — tap the quick lookup below to rename a skin/vehicle):");
        lblTarget.setTextSize(12);
        layout.addView(lblTarget);

        EditText etTarget = new EditText(ctx);
        etTarget.setHint("Leave empty to keep the original name, or enter e.g. truth / infernus");
        etTarget.setSingleLine(true);
        layout.addView(etTarget);

        LinearLayout btnRow = new LinearLayout(ctx);
        btnRow.setOrientation(LinearLayout.HORIZONTAL);
        Button btnPickSkin = new Button(ctx);
        btnPickSkin.setText("Pick skin ID");
        btnPickSkin.setOnClickListener(v -> showIdeSelectorDialog(true, etTarget));
        Button btnPickVeh = new Button(ctx);
        btnPickVeh.setText("Pick vehicle ID");
        btnPickVeh.setOnClickListener(v -> showIdeSelectorDialog(false, etTarget));
        Button btnClearTarget = new Button(ctx);
        btnClearTarget.setText("Keep original name");
        btnClearTarget.setOnClickListener(v -> etTarget.setText(""));

        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        btnRow.addView(btnPickSkin, lp);
        btnRow.addView(btnPickVeh, lp);
        btnRow.addView(btnClearTarget, lp);
        layout.addView(btnRow);

        TextView tvHint = new TextView(ctx);
        tvHint.setText("\nNote: PNG/JPG/WEBP and PC .txd files are auto-compiled into an Android texture library, and .dff/.ifp/.col files are auto-loaded into a virtual models.img when you enter the game — the official gta3.img is never modified.");
        tvHint.setTextSize(11);
        layout.addView(tvHint);

        new AlertDialog.Builder(ctx)
                .setTitle("Configure & Compile Model / Texture Pack")
                .setView(sv)
                .setPositiveButton("Compile & Install", (d, w) -> {
                    String packName = etPack.getText().toString().trim();
                    String targetModel = etTarget.getText().toString().trim();
                    runImport(uris, packName, targetModel.isEmpty() ? null : targetModel);
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void showIdeSelectorDialog(boolean isSkins, EditText targetField) {
        Context ctx = host.requireContext();
        Context appCtx = ctx.getApplicationContext();
        Toast.makeText(ctx, isSkins ? "Reading SAMP/peds.ide…" : "Reading SAMP/vehicles.ide…", Toast.LENGTH_SHORT).show();

        host.ioExecutor.execute(() -> {
            List<ModAssetTool.IdeEntry> allEntries = isSkins
                    ? ModAssetTool.loadPedSkins(appCtx)
                    : ModAssetTool.loadVehicles(appCtx);

            if (!host.isAdded()) return;
            host.requireActivity().runOnUiThread(() -> {
                if (!host.isAdded()) return;
                if (allEntries.isEmpty()) {
                    Toast.makeText(ctx,
                            isSkins ? "SAMP/peds.ide not found (deploy the game data first)" : "SAMP/vehicles.ide not found (deploy the game data first)",
                            Toast.LENGTH_LONG).show();
                    return;
                }

                int pad = (int) (12 * host.getResources().getDisplayMetrics().density);
                LinearLayout box = new LinearLayout(ctx);
                box.setOrientation(LinearLayout.VERTICAL);
                box.setPadding(pad, pad / 2, pad, pad / 2);

                EditText etSearch = new EditText(ctx);
                etSearch.setHint(isSkins ? "Search skin ID or name (e.g. 1 / truth / swat)" : "Search vehicle ID or name (e.g. 411 / infernus / sultan)");
                etSearch.setSingleLine(true);
                box.addView(etSearch);

                List<ModAssetTool.IdeEntry> filtered = new ArrayList<>(allEntries);
                List<String> labels = new ArrayList<>(allEntries.size());
                for (ModAssetTool.IdeEntry e : allEntries) labels.add(e.displayLabel());

                ArrayAdapter<String> listAdapter = new ArrayAdapter<>(ctx, android.R.layout.simple_list_item_1, labels);
                ListView lv = new ListView(ctx);
                lv.setAdapter(listAdapter);
                int listHeight = (int) (220 * host.getResources().getDisplayMetrics().density);
                box.addView(lv, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, listHeight));

                AlertDialog pickerDlg = new AlertDialog.Builder(ctx)
                        .setTitle(isSkins ? "Pick a target SA-MP skin" : "Pick a target SA-MP vehicle")
                        .setView(box)
                        .setNegativeButton("Cancel", null)
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
                        Toast.makeText(ctx, "Selected " + picked.displayLabel(), Toast.LENGTH_SHORT).show();
                        pickerDlg.dismiss();
                    }
                });

                pickerDlg.show();
            });
        });
    }

    private void runImport(List<Uri> uris, String packName, @Nullable String targetModel) {
        Context ctx = host.requireContext();
        Context appCtx = ctx.getApplicationContext();

        TextView tvProg = new TextView(ctx);
        int pad = (int) (18 * host.getResources().getDisplayMetrics().density);
        tvProg.setPadding(pad, pad, pad, pad);
        tvProg.setText("Preparing to parse and compile assets…");

        AlertDialog progDlg = new AlertDialog.Builder(ctx)
                .setTitle("Compiling and installing mod")
                .setView(tvProg)
                .setCancelable(false)
                .create();
        progDlg.show();

        host.ioExecutor.execute(() -> {
            try {
                ModAssetTool.ImportResult res = ModAssetTool.importAssetPack(
                        appCtx,
                        uris,
                        packName,
                        targetModel,
                        msg -> {
                            if (!host.isAdded()) return;
                            host.requireActivity().runOnUiThread(() -> {
                                if (progDlg.isShowing()) tvProg.setText(msg);
                            });
                        }
                );

                boolean autoEnabledMaster = false;
                if (!ClientSettings.loadLauncher(appCtx).getBoolean("modloader")) {
                    ClientSettings.setSync(appCtx, "modloader", true);
                    autoEnabledMaster = true;
                }
                final boolean enabledNow = autoEnabledMaster;

                FragmentActivity act = host.getActivity();
                if (act == null || !host.isAdded()) return;
                act.runOnUiThread(() -> {
                    if (progDlg.isShowing()) progDlg.dismiss();
                    if (!host.isAdded() || host.getContext() == null) return;

                    if (enabledNow) {
                        host.configureSectionPanel();
                    }
                    host.reloadCurrentTab();

                    StringBuilder detail = new StringBuilder();
                    detail.append("Mod pack folder: modloader/").append(res.packName).append("\n");
                    detail.append("• Model files: ").append(res.modelCount).append(" (auto-loaded into the virtual models.img in game)\n");
                    if (res.renamedPrimaryModel != null) {
                        detail.append("• Main model renamed to: ").append(res.renamedPrimaryModel).append("\n");
                    }
                    if (res.compiledTextureCount > 0) {
                        detail.append("• Compiled textures: ").append(res.compiledTextureCount)
                                .append(" → texdb/").append(res.texDbName).append("/\n");
                    }
                    if (res.passthroughFileCount > 0) {
                        detail.append("• Extra files imported as-is: ").append(res.passthroughFileCount).append(" files\n");
                    }
                    if (enabledNow) {
                        detail.append("• Modloader master switch turned on automatically\n");
                    }
                    if (!res.warnings.isEmpty()) {
                        detail.append("\nNotes:\n");
                        for (String w : res.warnings) {
                            detail.append("- ").append(w).append("\n");
                        }
                    }

                    new AlertDialog.Builder(host.getContext())
                            .setTitle("Compiled and installed")
                            .setMessage(detail.toString().trim())
                            .setPositiveButton("OK", null)
                            .show();
                });
            } catch (Exception e) {
                FragmentActivity act = host.getActivity();
                if (act == null || !host.isAdded()) return;
                act.runOnUiThread(() -> {
                    if (progDlg.isShowing()) progDlg.dismiss();
                    if (!host.isAdded() || host.getContext() == null) return;
                    new AlertDialog.Builder(host.getContext())
                            .setTitle("Import failed")
                            .setMessage(e.getMessage() != null ? e.getMessage() : e.toString())
                            .setPositiveButton("OK", null)
                            .show();
                });
            }
        });
    }
}
