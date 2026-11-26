package com.sampmobile.launcher.ui.fragment;

import android.content.Context;
import android.os.Build;
import android.os.StatFs;
import android.view.View;
import android.widget.TextView;
import android.widget.Toast;

import androidx.fragment.app.FragmentActivity;

import com.sampmobile.launcher.BuildConfig;
import com.sampmobile.launcher.R;
import com.sampmobile.launcher.utils.ClientSettings;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * Manages the Diagnostics & Crash Feedback tab inside {@link SettingsPageFragment}.
 */
final class SettingsDiagnosticsController {
    private final SettingsPageFragment host;
    private TextView tvStatusTitle;
    private TextView tvStatusDesc;

    SettingsDiagnosticsController(SettingsPageFragment host) {
        this.host = host;
    }

    void bindViews(View root) {
        tvStatusTitle = root.findViewById(R.id.tv_diag_status_title);
        tvStatusDesc = root.findViewById(R.id.tv_diag_status_desc);
    }

    void refresh() {
        Context ctx = host.getContext();
        if (ctx == null) return;
        final Context appCtx = ctx.getApplicationContext();
        host.ioExecutor.execute(() -> {
            File baseDir = appCtx.getExternalFilesDir(null);
            if (baseDir == null) return;
            File logcatDir = new File(baseDir, "logcat");
            int logFileCount = 0;
            int crashHits = 0;

            if (logcatDir.exists()) {
                File[] all = logcatDir.listFiles((d, n) -> n.endsWith(".log"));
                if (all != null) {
                    logFileCount = all.length;
                    Arrays.sort(all, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
                    for (int i = 0; i < Math.min(all.length, 3); i++) {
                        crashHits += extractCrashAndErrorLines(all[i]).size();
                    }
                }
            }

            final int finalLogCount = logFileCount;
            final boolean hasCrash = crashHits > 0;
            FragmentActivity act = host.getActivity();
            if (act != null && host.isAdded()) {
                act.runOnUiThread(() -> {
                    if (!host.isAdded() || host.getView() == null) return;
                    if (hasCrash) {
                        tvStatusTitle.setText("Diagnostics status: past crash records found");
                        tvStatusDesc.setText(String.format(Locale.US,
                                "The system has auto-saved %d diagnostics records in the background. If you just had a crash or freeze, tap “Copy bug report” below and send it to support or a developer for help.",
                                finalLogCount));
                    } else if (finalLogCount > 0) {
                        tvStatusTitle.setText("Diagnostics status: OK");
                        tvStatusDesc.setText(String.format(Locale.US,
                                "The client environment self-check passed; %d run records are archived and no recent crashes were found.",
                                finalLogCount));
                    } else {
                        tvStatusTitle.setText("Diagnostics status: Ready");
                        tvStatusDesc.setText("No past crash records yet. After you enter the game, the system will monitor and record run status in the background.");
                    }
                });
            }
        });
    }

    void copyReport() {
        Context ctx = host.getContext();
        if (ctx == null) return;
        final Context appCtx = ctx.getApplicationContext();
        host.ioExecutor.execute(() -> {
            String report = buildDiagnosticReportText(appCtx);
            FragmentActivity act = host.getActivity();
            if (act != null && host.isAdded()) {
                act.runOnUiThread(() -> {
                    if (!host.isAdded() || host.getContext() == null) return;
                    host.copyTextToClipboard("SA-MP Mobile bug report", report);
                    Toast.makeText(host.getContext(), "Bug report copied to the clipboard — just paste it to send!", Toast.LENGTH_LONG).show();
                });
            }
        });
    }

    void shareLogFile() {
        Context ctx = host.getContext();
        if (ctx == null) return;
        final Context appCtx = ctx.getApplicationContext();
        host.ioExecutor.execute(() -> {
            try {
                File baseDir = appCtx.getExternalFilesDir(null);
                if (baseDir == null) throw new IllegalStateException("Storage folder unavailable");
                File dir = new File(baseDir, "logcat");
                if (!dir.exists()) dir.mkdirs();
                File reportFile = new File(dir, "SAMP_Feedback_Report.log");
                String report = buildDiagnosticReportText(appCtx);
                try (FileOutputStream fos = new FileOutputStream(reportFile)) {
                    fos.write(report.getBytes(StandardCharsets.UTF_8));
                }
                FragmentActivity act = host.getActivity();
                if (act != null && host.isAdded()) {
                    act.runOnUiThread(() -> {
                        if (!host.isAdded() || host.getContext() == null) return;
                        host.shareFile(reportFile, "text/plain", "Export bug report log file");
                    });
                }
            } catch (Exception e) {
                FragmentActivity act = host.getActivity();
                if (act != null && host.isAdded()) {
                    act.runOnUiThread(() -> {
                        if (!host.isAdded() || host.getContext() == null) return;
                        Toast.makeText(host.getContext(), "Failed to export report file: " + e.getMessage(), Toast.LENGTH_SHORT).show();
                    });
                }
            }
        });
    }

    void confirmClearLogs() {
        Context ctx = host.getContext();
        if (ctx == null) return;
        final Context appCtx = ctx.getApplicationContext();
        host.showAlertDialog("Delete all archived log cache files?", (d, w) -> {
            host.ioExecutor.execute(() -> {
                File baseDir = appCtx.getExternalFilesDir(null);
                File logcatDir = baseDir != null ? new File(baseDir, "logcat") : null;
                if (logcatDir != null && logcatDir.exists()) {
                    File[] files = logcatDir.listFiles();
                    if (files != null) {
                        for (File f : files) f.delete();
                    }
                }
                FragmentActivity act = host.getActivity();
                if (act != null && host.isAdded()) {
                    act.runOnUiThread(() -> {
                        if (!host.isAdded() || host.getContext() == null) return;
                        refresh();
                        Toast.makeText(host.getContext(), "Log cache cleared", Toast.LENGTH_SHORT).show();
                    });
                }
            });
            d.dismiss();
        }, (d, w) -> d.dismiss());
    }

    private String buildDiagnosticReportText(Context appCtx) {
        File baseDir = appCtx.getExternalFilesDir(null);
        File logcatDir = baseDir != null ? new File(baseDir, "logcat") : null;
        StringBuilder sb = new StringBuilder();
        sb.append("========== SA-MP Mobile bug report diagnostics ==========\n");
        sb.append(buildDeviceSummaryLine(appCtx)).append("\n\n");

        File latest = findLatestClientLog(logcatDir);
        if (latest != null && latest.exists()) {
            List<String> crashes = extractCrashAndErrorLines(latest);
            if (!crashes.isEmpty()) {
                sb.append("--- Crash summary (").append(latest.getName()).append(") ---\n");
                for (String line : crashes) {
                    sb.append(line).append("\n");
                }
                sb.append("\n");
            }
            sb.append("--- Latest client run record (").append(latest.getName()).append(") ---\n");
            sb.append(readFileTail(latest, 160));
        } else {
            sb.append("No client run log records have been generated yet.\n");
        }
        return sb.toString();
    }

    private File findLatestClientLog(File logcatDir) {
        if (logcatDir == null || !logcatDir.exists()) return null;
        File[] files = logcatDir.listFiles((d, name) -> name.startsWith("client_") && name.endsWith(".log"));
        if (files == null || files.length == 0) return null;
        Arrays.sort(files, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        return files[0];
    }

    private String readFileTail(File file, int maxLines) {
        if (file == null || !file.exists()) return "";
        List<String> lines = new ArrayList<>();
        try (BufferedReader br = new BufferedReader(new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            while ((line = br.readLine()) != null) {
                lines.add(line);
                if (lines.size() > maxLines * 2) {
                    lines.subList(0, maxLines).clear();
                }
            }
        } catch (Exception ignored) {
        }
        int start = Math.max(0, lines.size() - maxLines);
        StringBuilder sb = new StringBuilder();
        for (int i = start; i < lines.size(); i++) {
            sb.append(lines.get(i)).append('\n');
        }
        return sb.toString();
    }

    private List<String> extractCrashAndErrorLines(File file) {
        List<String> result = new ArrayList<>();
        try (BufferedReader br = new BufferedReader(new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            int contextAfterHit = 0;
            while ((line = br.readLine()) != null) {
                String lower = line.toLowerCase(Locale.US);
                boolean hit = lower.contains("crashed") || lower.contains("fatal signal")
                        || lower.contains("backtrace") || lower.contains("[error]")
                        || lower.contains("[critical]") || lower.contains("sigsegv")
                        || lower.contains("sigabrt") || lower.contains("last script command")
                        || lower.contains("last rendered object");
                if (hit) {
                    result.add(line);
                    contextAfterHit = 10;
                } else if (contextAfterHit > 0) {
                    result.add(line);
                    contextAfterHit--;
                }
                if (result.size() > 150) break;
            }
        } catch (Exception ignored) {
        }
        return result;
    }

    private String buildDeviceSummaryLine(Context appCtx) {
        boolean is64 = android.os.Process.is64Bit();
        String abi = is64 ? "64-bit" : "32-bit";
        File extDir = appCtx.getExternalFilesDir(null);
        long freeMB = 0;
        if (extDir != null && extDir.exists()) {
            StatFs stat = new StatFs(extDir.getAbsolutePath());
            freeMB = (stat.getAvailableBlocksLong() * stat.getBlockSizeLong()) / (1024 * 1024);
        }
        ClientSettings settings = ClientSettings.loadLauncher(appCtx);
        return String.format(Locale.US,
                "Version: v%s (%s) | Device: %s %s | System: Android %s | Free space: %d MB\n"
                + "Mod switches: Modloader=%b, AML=%b, JPatch=%b, MonetLoader=%b, CLEO=%b",
                BuildConfig.VERSION_NAME, abi,
                Build.MANUFACTURER, Build.MODEL, Build.VERSION.RELEASE, freeMB,
                settings.getBoolean("modloader"),
                settings.getBoolean("aml_scripts"),
                settings.getBoolean("jpatch_enabled"),
                settings.getBoolean("monet_scripts"),
                settings.getBoolean("cleo_scripts"));
    }
}
