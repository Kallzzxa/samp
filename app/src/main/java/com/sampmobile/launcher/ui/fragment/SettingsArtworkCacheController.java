package com.sampmobile.launcher.ui.fragment;

import android.app.Activity;
import android.content.Context;
import android.content.res.ColorStateList;
import android.net.Uri;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AlertDialog;
import androidx.fragment.app.FragmentActivity;

import com.sampmobile.launcher.R;
import com.sampmobile.launcher.utils.ArtworkCacheManager;
import com.sampmobile.launcher.utils.GameDataVerifier;
import com.sampmobile.launcher.utils.LauncherThemeManager;

import java.io.File;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Manages the 0.3.DL custom model cache card inside {@link SettingsPageFragment}'s Multiplayer tab.
 */
final class SettingsArtworkCacheController {

    private final SettingsPageFragment host;
    private TextView tvSummary;
    private TextView tvEmpty;
    private TextView btnImport;
    private TextView btnRefresh;
    private TextView btnClearAll;
    private LinearLayout listContainer;
    private ArtworkCacheManager.CacheSummary lastSummary;

    SettingsArtworkCacheController(SettingsPageFragment host) {
        this.host = host;
    }

    void bindViews(View root) {
        tvSummary = root.findViewById(R.id.tv_dl_cache_summary);
        tvEmpty = root.findViewById(R.id.tv_dl_cache_empty);
        btnImport = root.findViewById(R.id.btn_dl_cache_import);
        btnRefresh = root.findViewById(R.id.btn_dl_cache_refresh);
        btnClearAll = root.findViewById(R.id.btn_dl_cache_clear_all);
        listContainer = root.findViewById(R.id.ll_dl_cache_list);

        if (btnImport != null) {
            btnImport.setOnClickListener(v -> host.artworkCachePicker.launch(
                    new String[]{"application/zip", "application/octet-stream", "*/*"}));
        }
        if (btnRefresh != null) {
            btnRefresh.setOnClickListener(v -> {
                refresh();
                Toast.makeText(host.requireContext(), "Model cache status refreshed", Toast.LENGTH_SHORT).show();
            });
        }
        if (btnClearAll != null) {
            btnClearAll.setOnClickListener(v -> onClickClearAll());
        }
        refresh();
    }

    void updateTheme(int accentColor) {
        if (btnImport == null || !host.isAdded()) return;
        btnImport.setBackgroundResource(R.drawable.launcher_btn_primary);
        btnImport.setBackgroundTintList(ColorStateList.valueOf(accentColor));
        btnImport.setTextColor(host.requireContext().getResources().getColor(R.color.launcher_on_accent));
    }

    void refresh() {
        Context ctx = host.getContext();
        if (ctx == null || tvSummary == null) return;
        final Context appCtx = ctx.getApplicationContext();
        host.ioExecutor.execute(() -> {
            File cacheRoot = ArtworkCacheManager.getCacheRoot(appCtx);
            ArtworkCacheManager.CacheSummary summary = ArtworkCacheManager.scan(cacheRoot);
            FragmentActivity act = host.getActivity();
            if (act != null && host.isAdded()) {
                act.runOnUiThread(() -> {
                    if (!host.isAdded() || host.getView() == null) return;
                    renderSummary(summary);
                });
            }
        });
    }

    private void renderSummary(ArtworkCacheManager.CacheSummary summary) {
        lastSummary = summary;
        if (tvSummary == null || listContainer == null || !host.isAdded()) return;

        if (summary.totalFiles == 0) {
            tvSummary.setText("Currently using: 0 files · 0 B (no 0.3.DL custom models cached yet)");
        } else {
            tvSummary.setText(String.format(Locale.US,
                    "Currently using: %d cache folders · %,d files · %s",
                    summary.folders.size(),
                    summary.totalFiles,
                    GameDataVerifier.formatBytes(summary.totalBytes)));
        }

        if (btnClearAll != null) {
            btnClearAll.setEnabled(summary.totalFiles > 0);
            btnClearAll.setAlpha(summary.totalFiles > 0 ? 1.0f : 0.45f);
        }

        listContainer.removeAllViews();
        if (summary.folders.isEmpty()) {
            if (tvEmpty != null) tvEmpty.setVisibility(View.VISIBLE);
            return;
        }
        if (tvEmpty != null) tvEmpty.setVisibility(View.GONE);

        Context ctx = host.requireContext();
        int padH = dp(ctx, 10);
        int padV = dp(ctx, 7);
        int gap = dp(ctx, 6);
        int colorText = ctx.getResources().getColor(R.color.launcher_text);
        int colorDim = ctx.getResources().getColor(R.color.launcher_text_dim);
        int colorDanger = ctx.getResources().getColor(R.color.launcher_danger);

        for (int i = 0; i < summary.folders.size(); i++) {
            ArtworkCacheManager.FolderEntry entry = summary.folders.get(i);
            LinearLayout row = new LinearLayout(ctx);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setBackgroundResource(R.drawable.ui_item_bg);
            row.setPadding(padH, padV, padH, padV);
            LinearLayout.LayoutParams rowLp = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            if (i > 0) rowLp.topMargin = gap;
            row.setLayoutParams(rowLp);

            LinearLayout info = new LinearLayout(ctx);
            info.setOrientation(LinearLayout.VERTICAL);
            LinearLayout.LayoutParams infoLp = new LinearLayout.LayoutParams(
                    0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f);
            infoLp.setMarginEnd(dp(ctx, 8));
            info.setLayoutParams(infoLp);

            TextView tvTitle = new TextView(ctx);
            tvTitle.setText(entry.isLocal ? "local (shared cache for all servers)" : entry.name);
            tvTitle.setTextColor(colorText);
            tvTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
            tvTitle.setTypeface(null, android.graphics.Typeface.BOLD);
            tvTitle.setSingleLine(true);
            tvTitle.setEllipsize(TextUtils.TruncateAt.MIDDLE);
            info.addView(tvTitle);

            TextView tvMeta = new TextView(ctx);
            tvMeta.setText(String.format(Locale.US, "%,d files · %s",
                    entry.fileCount, GameDataVerifier.formatBytes(entry.totalBytes)));
            tvMeta.setTextColor(colorDim);
            tvMeta.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f);
            info.addView(tvMeta);

            TextView btnDel = new TextView(ctx);
            btnDel.setText("Delete");
            btnDel.setTextColor(colorDanger);
            btnDel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
            btnDel.setTypeface(null, android.graphics.Typeface.BOLD);
            btnDel.setGravity(Gravity.CENTER);
            btnDel.setBackgroundResource(R.drawable.launcher_btn_danger);
            btnDel.setPadding(dp(ctx, 10), dp(ctx, 4), dp(ctx, 10), dp(ctx, 4));
            btnDel.setOnClickListener(v -> confirmDeleteFolder(entry));

            row.addView(info);
            row.addView(btnDel);
            listContainer.addView(row);
        }
    }

    private void confirmDeleteFolder(ArtworkCacheManager.FolderEntry entry) {
        Activity act = host.getActivity();
        if (act == null || act.isFinishing()) return;
        new AlertDialog.Builder(act)
                .setTitle("Delete cache folder")
                .setMessage(String.format(Locale.US,
                        "Delete %,d cached files (%s) from "%s"?\nThe required models will be re-downloaded next time you join that server.",
                        entry.name, entry.fileCount, GameDataVerifier.formatBytes(entry.totalBytes)))
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Confirm delete", (d, w) -> {
                    final Context appCtx = act.getApplicationContext();
                    host.ioExecutor.execute(() -> {
                        File cacheRoot = ArtworkCacheManager.getCacheRoot(appCtx);
                        boolean ok = ArtworkCacheManager.deleteFolder(cacheRoot, entry.name);
                        act.runOnUiThread(() -> {
                            if (!host.isAdded()) return;
                            Toast.makeText(act, ok ? ("Deleted cache: " + entry.name) : "Delete failed", Toast.LENGTH_SHORT).show();
                            refresh();
                        });
                    });
                })
                .show();
    }

    private void onClickClearAll() {
        Activity act = host.getActivity();
        if (act == null || act.isFinishing()) return;
        ArtworkCacheManager.CacheSummary snap = lastSummary;
        if (snap == null || snap.totalFiles == 0) {
            Toast.makeText(act, "No model cache to clear", Toast.LENGTH_SHORT).show();
            return;
        }
        new AlertDialog.Builder(act)
                .setTitle("Clear all custom model cache")
                .setMessage(String.format(Locale.US,
                        "This will permanently delete all %d folders under SAMP/cache/ (%,d files in total, %s).\nContinue?",
                        snap.folders.size(), snap.totalFiles, GameDataVerifier.formatBytes(snap.totalBytes)))
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Clear all", (d, w) -> {
                    final Context appCtx = act.getApplicationContext();
                    host.ioExecutor.execute(() -> {
                        File cacheRoot = ArtworkCacheManager.getCacheRoot(appCtx);
                        ArtworkCacheManager.clearAll(cacheRoot);
                        act.runOnUiThread(() -> {
                            if (!host.isAdded()) return;
                            Toast.makeText(act, "All custom model cache cleared", Toast.LENGTH_SHORT).show();
                            refresh();
                        });
                    });
                })
                .show();
    }

    void onDocumentsPicked(List<Uri> uris) {
        Activity act = host.getActivity();
        if (act == null || act.isFinishing() || uris == null || uris.isEmpty()) return;
        File cacheRoot = ArtworkCacheManager.getCacheRoot(act);
        if (cacheRoot == null) {
            Toast.makeText(act, "Could not access the app's external storage folder", Toast.LENGTH_LONG).show();
            return;
        }

        int pad = dp(act, 20);
        LinearLayout box = new LinearLayout(act);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(pad, dp(act, 14), pad, dp(act, 8));

        TextView tvStatus = new TextView(act);
        tvStatus.setText("Importing custom model cache…");
        tvStatus.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        box.addView(tvStatus);

        ProgressBar pb = new ProgressBar(act, null, android.R.attr.progressBarStyleHorizontal);
        pb.setIndeterminate(true);
        LinearLayout.LayoutParams pbLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        pbLp.topMargin = dp(act, 12);
        box.addView(pb, pbLp);

        TextView tvFile = new TextView(act);
        tvFile.setText("");
        tvFile.setSingleLine(true);
        tvFile.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        tvFile.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        tvFile.setAlpha(0.75f);
        LinearLayout.LayoutParams fLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        fLp.topMargin = dp(act, 6);
        box.addView(tvFile, fLp);

        AtomicBoolean cancelled = new AtomicBoolean(false);
        AlertDialog dialog = new AlertDialog.Builder(act)
                .setTitle("Import 0.3.DL model cache")
                .setView(box)
                .setCancelable(false)
                .setNegativeButton("Cancel", (d, w) -> cancelled.set(true))
                .create();
        dialog.show();

        final Context appCtx = act.getApplicationContext();
        Thread worker = new Thread(() -> {
            try {
                ArtworkCacheManager.ImportResult res = ArtworkCacheManager.importFromUris(
                        appCtx,
                        uris,
                        cacheRoot,
                        (imported, skipped, relPath) -> act.runOnUiThread(() -> {
                            if (act.isFinishing() || !dialog.isShowing()) return;
                            tvStatus.setText(String.format(Locale.US,
                                    "Imported %,d files (skipped %,d non-cache items)", imported, skipped));
                            if (relPath != null) tvFile.setText(relPath);
                        }),
                        cancelled
                );

                act.runOnUiThread(() -> {
                    if (dialog.isShowing()) dialog.dismiss();
                    if (act.isFinishing() || !host.isAdded()) return;
                    refresh();
                    if (res.cancelled) {
                        Toast.makeText(act, String.format(Locale.US,
                                "Import cancelled (%,d files already written)", res.importedFiles), Toast.LENGTH_LONG).show();
                    } else if (res.importedFiles == 0) {
                        new AlertDialog.Builder(act)
                                .setTitle("No valid 0.3.DL cache files found")
                                .setMessage("No model cache files in the 0x<CRC>.dff or 0x<CRC>.txd format were found in the selected file/archive.")
                                .setPositiveButton("Got it", null)
                                .show();
                    } else {
                        Toast.makeText(act, String.format(Locale.US,
                                "Import complete: %,d files (%s), skipped %,d items",
                                res.importedFiles,
                                GameDataVerifier.formatBytes(res.importedBytes),
                                res.skippedFiles), Toast.LENGTH_LONG).show();
                    }
                });
            } catch (Throwable t) {
                act.runOnUiThread(() -> {
                    if (dialog.isShowing()) dialog.dismiss();
                    if (act.isFinishing() || !host.isAdded()) return;
                    refresh();
                    Toast.makeText(act, "Import failed: " + (t.getMessage() != null ? t.getMessage() : t.getClass().getSimpleName()),
                            Toast.LENGTH_LONG).show();
                });
            }
        }, "SAMP-ArtworkCacheImport");
        worker.start();
    }

    private static int dp(Context ctx, int v) {
        return Math.round(TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, v, ctx.getResources().getDisplayMetrics()));
    }
}
