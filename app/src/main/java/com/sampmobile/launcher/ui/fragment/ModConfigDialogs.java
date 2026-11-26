package com.sampmobile.launcher.ui.fragment;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.Typeface;
import android.view.ViewGroup;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AlertDialog;

import com.sampmobile.launcher.utils.ModManager;

import java.io.File;

/**
 * AML / JPatch configuration dialogs and mod log viewer for {@link ModsManagerFragment}.
 */
final class ModConfigDialogs {
    private final ModsManagerFragment host;

    ModConfigDialogs(ModsManagerFragment host) {
        this.host = host;
    }

    void showAmlCoreSettingsDialog() {
        Context ctx = host.requireContext();
        ModManager.AmlCoreSettings cur = ModManager.readAmlCoreSettings(ctx);

        int pad = (int) (14 * host.getResources().getDisplayMetrics().density);
        LinearLayout layout = new LinearLayout(ctx);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(pad, pad / 2, pad, 0);

        TextView lblFake = new TextView(ctx);
        lblFake.setText("Fake package name (FakePackageName, for GTA:SA plugin identification):");
        lblFake.setTextSize(12);
        layout.addView(lblFake);

        EditText etFake = new EditText(ctx);
        etFake.setText(cur.fakePackageName);
        etFake.setSingleLine(true);
        layout.addView(etFake);

        CheckBox cbOverlay = new CheckBox(ctx);
        cbOverlay.setText("Show an AML loaded-mods notice when entering the game (recommended OFF on Android 12+ so it doesn't cover the keyboard)");
        cbOverlay.setChecked(cur.showModsOverlay);
        layout.addView(cbOverlay);

        CheckBox cbLogFile = new CheckBox(ctx);
        cbLogFile.setText("Write AML runtime logs to android_log_print.txt");
        cbLogFile.setChecked(cur.printLogsToFile);
        layout.addView(cbLogFile);

        CheckBox cbVerbose = new CheckBox(ctx);
        cbVerbose.setText("Verbose logging (logs INFO-level load info)");
        cbVerbose.setChecked(cur.verboseLogs);
        layout.addView(cbVerbose);

        CheckBox cbSignal = new CheckBox(ctx);
        cbSignal.setText("Enable AML crash signal capture (off by default; keeping this client's native crash logs is recommended)");
        cbSignal.setChecked(cur.signalHandler);
        layout.addView(cbSignal);

        new AlertDialog.Builder(ctx)
                .setTitle("AML core settings (ModLoaderCore.ini)")
                .setView(layout)
                .setPositiveButton("Save", (d, w) -> {
                    ModManager.AmlCoreSettings updated = new ModManager.AmlCoreSettings();
                    updated.fakePackageName = etFake.getText().toString().trim();
                    updated.showModsOverlay = cbOverlay.isChecked();
                    updated.printLogsToFile = cbLogFile.isChecked();
                    updated.verboseLogs = cbVerbose.isChecked();
                    updated.signalHandler = cbSignal.isChecked();
                    Context appCtx = ctx.getApplicationContext();
                    host.ioExecutor.execute(() -> {
                        ModManager.saveAmlCoreSettings(appCtx, updated);
                        if (!host.isAdded()) return;
                        host.requireActivity().runOnUiThread(() ->
                                Toast.makeText(ctx, "ModLoaderCore.ini saved", Toast.LENGTH_SHORT).show());
                    });
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    void applyJPatchSafePreset() {
        Context ctx = host.requireContext();
        new AlertDialog.Builder(ctx)
                .setTitle("Apply SA-MP safe preset")
                .setMessage("This will write the recommended SA-MP 2.10 safe values to JPatch.ini: enable 2.10 crash protection, fog wall, high-FPS physics, and NEON optimizations, and disable UVAnim, forced texture formats, and repeated streaming-memory expansion that conflict with this client. Continue?")
                .setPositiveButton("Apply", (d, w) -> {
                    Context appCtx = ctx.getApplicationContext();
                    host.ioExecutor.execute(() -> {
                        ModManager.applyJPatchSampSafePreset(appCtx);
                        if (!host.isAdded()) return;
                        host.requireActivity().runOnUiThread(() -> {
                            Toast.makeText(ctx, "SA-MP safe preset applied to JPatch.ini", Toast.LENGTH_SHORT).show();
                            host.reloadCurrentTab();
                        });
                    });
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    void showAddJPatchKeyDialog() {
        Context ctx = host.requireContext();
        int pad = (int) (14 * host.getResources().getDisplayMetrics().density);
        LinearLayout layout = new LinearLayout(ctx);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(pad, pad / 2, pad, 0);

        EditText etSec = new EditText(ctx);
        etSec.setHint("Section name (default: Patches)");
        etSec.setText("Patches");
        layout.addView(etSec);

        EditText etKey = new EditText(ctx);
        etKey.setHint("Config key (e.g. FixWaterQuadrant)");
        layout.addView(etKey);

        EditText etVal = new EditText(ctx);
        etVal.setHint("Value (e.g. 1 or 0)");
        etVal.setText("1");
        layout.addView(etVal);

        new AlertDialog.Builder(ctx)
                .setTitle("Add JPatch.ini setting")
                .setView(layout)
                .setPositiveButton("Save", (d, w) -> {
                    String rawSec = etSec.getText().toString().trim();
                    final String sec = rawSec.isEmpty() ? "Patches" : rawSec;
                    final String key = etKey.getText().toString().trim();
                    final String val = etVal.getText().toString().trim();
                    if (key.isEmpty()) return;
                    Context appCtx = ctx.getApplicationContext();
                    host.ioExecutor.execute(() -> {
                        ModManager.saveJPatchOption(appCtx, sec, key, val);
                        if (!host.isAdded()) return;
                        host.requireActivity().runOnUiThread(() -> {
                            Toast.makeText(ctx, "Wrote " + key + "=" + val, Toast.LENGTH_SHORT).show();
                            host.reloadCurrentTab();
                        });
                    });
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    void showEditJPatchValueDialog(ModManager.JPatchOption opt) {
        Context ctx = host.requireContext();
        EditText etVal = new EditText(ctx);
        etVal.setText(opt.value);
        etVal.setSingleLine(true);
        int pad = (int) (16 * host.getResources().getDisplayMetrics().density);
        LinearLayout wrap = new LinearLayout(ctx);
        wrap.setPadding(pad, pad / 2, pad, 0);
        wrap.addView(etVal, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        new AlertDialog.Builder(ctx)
                .setTitle("Edit " + opt.key)
                .setView(wrap)
                .setPositiveButton("Save", (d, w) -> {
                    String val = etVal.getText().toString().trim();
                    opt.value = val;
                    Context appCtx = ctx.getApplicationContext();
                    host.ioExecutor.execute(() -> {
                        ModManager.saveJPatchOption(appCtx, opt.section, opt.key, val);
                        if (!host.isAdded()) return;
                        host.requireActivity().runOnUiThread(host::reloadCurrentTab);
                    });
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    void showLogsDialog() {
        Context ctx = host.requireContext();
        String[] choices = {
                "AML logs (android_log_print.txt)",
                "Latest client logs (ModLoader / Hook)",
                "MonetLoader logs (logs/monetloader.log)",
                "CLEO engine logs (cleo/cleo.log)"
        };
        new AlertDialog.Builder(ctx)
                .setTitle("Choose a log to view")
                .setItems(choices, (d, which) -> {
                    Context appCtx = ctx.getApplicationContext();
                    host.ioExecutor.execute(() -> {
                        File logFile;
                        if (which == 0) logFile = ModManager.getAmlLogFile(appCtx);
                        else if (which == 1) logFile = ModManager.getLatestClientLogFile(appCtx);
                        else if (which == 2) logFile = ModManager.getMonetLogFile(appCtx);
                        else logFile = ModManager.getCleoLogFile(appCtx);

                        String content = ModManager.readLogTail(logFile, 32 * 1024);
                        if (content.isEmpty()) {
                            content = "No log content yet.\nFile path: " + (logFile != null ? logFile.getAbsolutePath() : "Not created yet");
                        }
                        final String finalContent = content;
                        final File finalFile = logFile;
                        if (!host.isAdded()) return;
                        host.requireActivity().runOnUiThread(() -> showLogTextPopup(choices[which], finalContent, finalFile));
                    });
                })
                .show();
    }

    private void showLogTextPopup(String title, String content, @Nullable File file) {
        Context ctx = host.requireContext();
        ScrollView sv = new ScrollView(ctx);
        TextView tv = new TextView(ctx);
        int pad = (int) (12 * host.getResources().getDisplayMetrics().density);
        tv.setPadding(pad, pad, pad, pad);
        tv.setTypeface(Typeface.MONOSPACE);
        tv.setTextSize(10);
        tv.setTextIsSelectable(true);
        tv.setText(content);
        sv.addView(tv);

        AlertDialog.Builder b = new AlertDialog.Builder(ctx)
                .setTitle(title)
                .setView(sv)
                .setPositiveButton("Copy", (d, w) -> {
                    ClipboardManager cm = (ClipboardManager) ctx.getSystemService(Context.CLIPBOARD_SERVICE);
                    if (cm != null) {
                        cm.setPrimaryClip(ClipData.newPlainText("mod_log", content));
                        Toast.makeText(ctx, "Log copied", Toast.LENGTH_SHORT).show();
                    }
                })
                .setNegativeButton("Close", null);
        if (file != null && file.isFile()) {
            b.setNeutralButton("Clear", (d, w) -> host.ioExecutor.execute(() -> {
                file.delete();
                if (!host.isAdded()) return;
                host.requireActivity().runOnUiThread(() -> Toast.makeText(ctx, "Log file cleared", Toast.LENGTH_SHORT).show());
            }));
        }
        b.show();
    }
}
