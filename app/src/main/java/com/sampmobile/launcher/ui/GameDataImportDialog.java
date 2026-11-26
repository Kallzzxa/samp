package com.sampmobile.launcher.ui;

import android.app.Activity;
import android.net.Uri;
import android.text.TextUtils;
import android.util.TypedValue;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AlertDialog;

import com.sampmobile.launcher.utils.ClientSettings;
import com.sampmobile.launcher.utils.GameDataImporter;

import java.io.File;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Shared one-click game-data .zip import progress dialog used by both
 * MainActivity (when launching with missing game data) and SettingsPageFragment.
 */
public final class GameDataImportDialog {

    private GameDataImportDialog() {}

    public static void startImport(Activity activity, Uri zipUri, Runnable onCompleted) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed() || zipUri == null) {
            return;
        }
        File filesDir = activity.getExternalFilesDir(null);
        if (filesDir == null) {
            Toast.makeText(activity, "Cannot access the app's external storage directory", Toast.LENGTH_LONG).show();
            return;
        }

        int pad = dp(activity, 20);
        LinearLayout box = new LinearLayout(activity);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(pad, dp(activity, 14), pad, dp(activity, 8));

        TextView tvStatus = new TextView(activity);
        tvStatus.setText("Preparing to extract the data package...");
        tvStatus.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        box.addView(tvStatus);

        ProgressBar progressBar = new ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal);
        progressBar.setMax(100);
        progressBar.setIndeterminate(true);
        LinearLayout.LayoutParams pbLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        pbLp.topMargin = dp(activity, 12);
        box.addView(progressBar, pbLp);

        TextView tvFile = new TextView(activity);
        tvFile.setText("");
        tvFile.setSingleLine(true);
        tvFile.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        tvFile.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        tvFile.setAlpha(0.75f);
        LinearLayout.LayoutParams fLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        fLp.topMargin = dp(activity, 6);
        box.addView(tvFile, fLp);

        AtomicBoolean cancelled = new AtomicBoolean(false);
        AlertDialog dialog = new AlertDialog.Builder(activity)
                .setTitle("Importing game data package")
                .setView(box)
                .setCancelable(false)
                .setNegativeButton("Cancel", (d, w) -> cancelled.set(true))
                .create();
        dialog.show();

        Thread worker = new Thread(() -> {
            try {
                GameDataImporter.ImportResult res = GameDataImporter.importFromUri(
                        activity,
                        zipUri,
                        filesDir,
                        (pct, count, relPath) -> activity.runOnUiThread(() -> {
                            if (activity.isFinishing() || activity.isDestroyed() || !dialog.isShowing()) {
                                return;
                            }
                            if (pct >= 0) {
                                progressBar.setIndeterminate(false);
                                progressBar.setProgress(pct);
                                tvStatus.setText(String.format(Locale.US, "Extracting %d%% (%d files written)", pct, count));
                            } else {
                                progressBar.setIndeterminate(true);
                                tvStatus.setText(String.format(Locale.US, "Extracting (%d files written)...", count));
                            }
                            if (relPath != null && !relPath.isEmpty()) {
                                tvFile.setText(relPath);
                            }
                        }),
                        cancelled
                );

                // Ensure settings.json exists after clean import
                if (!res.cancelled && res.extractedFiles > 0) {
                    try {
                        ClientSettings.ensureExists(activity);
                    } catch (Exception ignored) {
                    }
                }

                activity.runOnUiThread(() -> {
                    if (dialog.isShowing()) {
                        dialog.dismiss();
                    }
                    if (activity.isFinishing() || activity.isDestroyed()) {
                        return;
                    }
                    if (res.cancelled) {
                        Toast.makeText(activity, "Import cancelled", Toast.LENGTH_SHORT).show();
                    } else if (res.extractedFiles == 0) {
                        new AlertDialog.Builder(activity)
                                .setTitle("Game data package not recognized")
                                .setMessage("No GTA:SA / SA-MP game directory found in the selected ZIP (it must contain SAMP/, texdb/, anim/, data/, models/, etc.). Please confirm you selected a .zip generated by tools/gamedata/build_gamedata.py.")
                                .setPositiveButton("OK", null)
                                .show();
                    } else if (res.ready) {
                        String mb = String.format(Locale.US, "%.1f MB", res.extractedBytes / (1024.0 * 1024.0));
                        Toast.makeText(activity, "Import complete (" + res.extractedFiles + " files, " + mb + ")", Toast.LENGTH_LONG).show();
                    } else {
                        StringBuilder sb = new StringBuilder("Extracted ");
                        sb.append(res.extractedFiles).append(" files, but the following core files are still missing:\n");
                        for (String m : res.missingCoreFiles) {
                            sb.append("• ").append(m).append('\n');
                        }
                        new AlertDialog.Builder(activity)
                                .setTitle("Incomplete data package")
                                .setMessage(sb.toString().trim())
                                .setPositiveButton("OK", null)
                                .show();
                    }
                    if (onCompleted != null) {
                        onCompleted.run();
                    }
                });
            } catch (Exception e) {
                activity.runOnUiThread(() -> {
                    if (dialog.isShowing()) {
                        dialog.dismiss();
                    }
                    if (!activity.isFinishing() && !activity.isDestroyed()) {
                        new AlertDialog.Builder(activity)
                                .setTitle("Import failed")
                                .setMessage(e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName())
                                .setPositiveButton("OK", null)
                                .show();
                    }
                    if (onCompleted != null) {
                        onCompleted.run();
                    }
                });
            }
        }, "GameDataImporter");
        worker.start();
    }

    private static int dp(Activity activity, int value) {
        return Math.round(TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, value, activity.getResources().getDisplayMetrics()));
    }
}
