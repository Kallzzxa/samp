package com.sampmobile.launcher.ui.fragment;

import android.content.Context;
import android.content.res.ColorStateList;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AlertDialog;
import androidx.fragment.app.FragmentActivity;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.sampmobile.launcher.R;
import com.sampmobile.launcher.download.GamedataInstallerTask;
import com.sampmobile.launcher.download.GamedataState;
import com.sampmobile.launcher.ui.widget.GtaOptionView;
import com.sampmobile.launcher.utils.ClientSettings;
import com.sampmobile.launcher.utils.GameDataImporter;
import com.sampmobile.launcher.utils.GameDataVerifier;
import com.sampmobile.launcher.utils.LauncherThemeManager;
import com.sampmobile.launcher.utils.Utils;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Manages the Game Data Verification & Manual Download tab inside {@link SettingsPageFragment}.
 */
final class SettingsGameDataController {
    private static final String[] EDITION_ITEMS = {"Lite (Recommended · ~1.04 GiB)", "Full (with all audio · 2.10 GiB)"};

    private final SettingsPageFragment host;
    private GtaOptionView optionEdition;
    private TextView tvSummary, tvStorageMeta, tvListSummary, tvListEmpty;
    private TextView btnImport, btnDownload, btnVerify, btnFilterAll, btnFilterIssues;
    private View layoutDownloadProgress;
    private TextView tvDlStatus, tvDlBytes;
    private ProgressBar pbDownload;
    private RecyclerView rvFiles;

    private final List<GameDataVerifier.FileCheckItem> allItems = new ArrayList<>();
    private final List<GameDataVerifier.FileCheckItem> displayedItems = new ArrayList<>();
    private GameDataFileAdapter fileAdapter;
    private GameDataVerifier.Edition selectedEdition = GameDataVerifier.Edition.LITE;
    private GameDataVerifier.Report lastReport;
    private boolean filterIssuesOnly = false;
    private final AtomicBoolean downloading = new AtomicBoolean(false);
    private volatile GamedataInstallerTask activeDownloadTask;
    private long lastDownloadProgressPostMs = 0L;

    SettingsGameDataController(SettingsPageFragment host) {
        this.host = host;
    }

    void bindViews(View view) {
        optionEdition = view.findViewById(R.id.option_gd_edition);
        tvSummary = view.findViewById(R.id.tv_gd_summary);
        tvStorageMeta = view.findViewById(R.id.tv_gd_storage_meta);
        tvListSummary = view.findViewById(R.id.tv_gd_list_summary);
        tvListEmpty = view.findViewById(R.id.tv_gd_list_empty);
        btnImport = view.findViewById(R.id.btn_gd_import);
        btnDownload = view.findViewById(R.id.btn_gd_download);
        btnVerify = view.findViewById(R.id.btn_gd_verify);
        btnFilterAll = view.findViewById(R.id.btn_gd_filter_all);
        btnFilterIssues = view.findViewById(R.id.btn_gd_filter_issues);
        layoutDownloadProgress = view.findViewById(R.id.layout_gd_download_progress);
        tvDlStatus = view.findViewById(R.id.tv_gd_dl_status);
        tvDlBytes = view.findViewById(R.id.tv_gd_dl_bytes);
        pbDownload = view.findViewById(R.id.pb_gd_download);
        rvFiles = view.findViewById(R.id.rv_gd_files);
        if (rvFiles != null) {
            rvFiles.setLayoutManager(new LinearLayoutManager(host.requireContext()));
            fileAdapter = new GameDataFileAdapter();
            rvFiles.setAdapter(fileAdapter);
        }
        setupControls();
        refreshVerification();
    }

    void setupControls() {
        if (optionEdition != null) {
            optionEdition.setAdapter(host.createDropdownAdapter(EDITION_ITEMS));
            optionEdition.setText(EDITION_ITEMS[0], false);
            optionEdition.setOnItemClickListener((parent, v, pos, id) -> {
                selectedEdition = (pos == 1)
                        ? GameDataVerifier.Edition.FULL
                        : GameDataVerifier.Edition.LITE;
                refreshVerification();
            });
        }
        if (btnImport != null) {
            btnImport.setOnClickListener(v -> {
                if (downloading.get()) {
                    Toast.makeText(host.requireContext(), "A data pack is downloading — cancel it before importing", Toast.LENGTH_SHORT).show();
                    return;
                }
                host.gameDataZipPicker.launch(new String[]{"application/zip", "application/octet-stream", "*/*"});
            });
        }
        if (btnDownload != null) {
            btnDownload.setOnClickListener(v -> {
                if (downloading.get()) {
                    cancelActiveDownload();
                } else {
                    onClickManualDownload();
                }
            });
        }
        if (btnVerify != null) {
            btnVerify.setOnClickListener(v -> {
                refreshVerification();
                Toast.makeText(host.requireContext(), "Re-verifying game files…", Toast.LENGTH_SHORT).show();
            });
        }
        if (btnFilterAll != null) {
            btnFilterAll.setOnClickListener(v -> {
                filterIssuesOnly = false;
                updateFilterUI();
                applyListFilter();
            });
        }
        if (btnFilterIssues != null) {
            btnFilterIssues.setOnClickListener(v -> {
                filterIssuesOnly = true;
                updateFilterUI();
                applyListFilter();
            });
        }
        updateFilterUI();
        updateDownloadButtonUI();
    }

    void updateFilterUI() {
        if (btnFilterAll == null || btnFilterIssues == null || !host.isAdded()) return;
        int accentColor = LauncherThemeManager.getAccentColor(host.requireContext());
        int colorOnAccent = host.requireContext().getResources().getColor(R.color.launcher_on_accent);
        int colorText = host.requireContext().getResources().getColor(R.color.launcher_text);

        boolean all = !filterIssuesOnly;
        btnFilterAll.setBackgroundResource(all ? R.drawable.launcher_btn_primary : R.drawable.launcher_btn_outline);
        btnFilterAll.setBackgroundTintList(all ? ColorStateList.valueOf(accentColor) : null);
        btnFilterAll.setTextColor(all ? colorOnAccent : colorText);

        btnFilterIssues.setBackgroundResource(!all ? R.drawable.launcher_btn_primary : R.drawable.launcher_btn_outline);
        btnFilterIssues.setBackgroundTintList(!all ? ColorStateList.valueOf(accentColor) : null);
        btnFilterIssues.setTextColor(!all ? colorOnAccent : colorText);
    }

    void updateDownloadButtonUI() {
        if (btnDownload == null || !host.isAdded()) return;
        if (downloading.get()) {
            btnDownload.setBackgroundResource(R.drawable.launcher_btn_danger);
            btnDownload.setBackgroundTintList(null);
            btnDownload.setTextColor(host.requireContext().getResources().getColor(R.color.launcher_danger));
            btnDownload.setText("Cancel download");
        } else {
            int accentColor = LauncherThemeManager.getAccentColor(host.requireContext());
            btnDownload.setBackgroundResource(R.drawable.launcher_btn_primary);
            btnDownload.setBackgroundTintList(ColorStateList.valueOf(accentColor));
            btnDownload.setTextColor(host.requireContext().getResources().getColor(R.color.launcher_on_accent));
            btnDownload.setText("Download via launcher");
        }
    }

    void refreshVerification() {
        Context ctx = host.getContext();
        if (ctx == null || tvSummary == null) return;
        final Context appCtx = ctx.getApplicationContext();
        final GameDataVerifier.Edition edition = selectedEdition;
        host.ioExecutor.execute(() -> {
            GameDataVerifier.Report report = GameDataVerifier.verify(appCtx, edition);
            FragmentActivity act = host.getActivity();
            if (act != null && host.isAdded()) {
                act.runOnUiThread(() -> {
                    if (!host.isAdded() || host.getView() == null) return;
                    renderReport(report);
                });
            }
        });
    }

    void cancelActiveDownload() {
        GamedataInstallerTask task = activeDownloadTask;
        if (task != null) {
            task.cancel();
            if (host.isAdded() && tvDlStatus != null) {
                tvDlStatus.setText("Cancelling download…");
            }
        }
    }

    private void renderReport(GameDataVerifier.Report report) {
        lastReport = report;
        String editionLabel = (report.edition == GameDataVerifier.Edition.FULL)
                ? "Full edition check (2.10 GiB)"
                : "Lite edition check";
        String detectedPack = report.hasFullAudio ? "Full-edition audio included" : "Lite edition structure";
        if (report.problemCount() == 0) {
            tvSummary.setText(String.format(Locale.US,
                    "Status: Ready (%s · %s)", editionLabel, detectedPack));
            tvSummary.setTextColor(host.requireContext().getResources().getColor(R.color.ui_good));
        } else if (report.coreReady && report.edition == GameDataVerifier.Edition.FULL) {
            tvSummary.setText(String.format(Locale.US,
                    "Status: Core files OK for play, missing %d full-edition audio items (switch back to the Lite edition to play normally)", report.problemCount()));
            tvSummary.setTextColor(host.requireContext().getResources().getColor(R.color.ui_warn));
        } else {
            tvSummary.setText(String.format(Locale.US,
                    "Status: Not ready (%d missing, %d broken) · Import or download the game data",
                    report.missingCount, report.invalidCount));
            tvSummary.setTextColor(host.requireContext().getResources().getColor(R.color.ui_bad));
        }

        String verText = (report.installedVersion != null && !report.installedVersion.isEmpty())
                ? " · Manifest version " + report.installedVersion
                : "";
        tvStorageMeta.setText(String.format(Locale.US,
                "Currently using %s · Free space %s%s · Only the Lite edition is offered for download (the Full edition, 2.10 GiB, currently supports local import and verification only)",
                GameDataVerifier.formatBytes(report.totalDataBytes),
                GameDataVerifier.formatBytes(report.freeSpaceBytes),
                verText));

        String skipHint = report.skippedCount > 0
                ? String.format(Locale.US, " · Lite edition skipped %d items", report.skippedCount)
                : "";
        tvListSummary.setText(String.format(Locale.US,
                "Passed %d/%d · Missing %d · Broken %d%s",
                report.okCount, report.requiredCount(),
                report.missingCount, report.invalidCount, skipHint));

        allItems.clear();
        allItems.addAll(report.items);
        applyListFilter();
    }

    private void applyListFilter() {
        displayedItems.clear();
        for (GameDataVerifier.FileCheckItem item : allItems) {
            if (!filterIssuesOnly || item.isProblem()) {
                displayedItems.add(item);
            }
        }
        if (fileAdapter != null) {
            fileAdapter.notifyDataSetChanged();
        }
        if (tvListEmpty != null && rvFiles != null) {
            boolean empty = displayedItems.isEmpty();
            tvListEmpty.setVisibility(empty ? View.VISIBLE : View.GONE);
            rvFiles.setVisibility(empty ? View.GONE : View.VISIBLE);
        }
    }

    private void onClickManualDownload() {
        if (!host.isAdded()) return;
        if (selectedEdition == GameDataVerifier.Edition.FULL) {
            new AlertDialog.Builder(host.requireActivity())
                    .setTitle("Full edition download not available yet")
                    .setMessage("The launcher currently only offers the Lite game data download; the Full edition (2.10 GiB) can only be installed and verified via "Local import".\n\nSwitch to the Lite edition and start downloading?")
                    .setPositiveButton("Switch to Lite and download", (d, w) -> {
                        selectedEdition = GameDataVerifier.Edition.LITE;
                        if (optionEdition != null) {
                            optionEdition.setText(EDITION_ITEMS[0], false);
                        }
                        refreshVerification();
                        proceedLiteManualDownload();
                    })
                    .setNegativeButton("Cancel", null)
                    .show();
            return;
        }
        proceedLiteManualDownload();
    }

    private void proceedLiteManualDownload() {
        if (!host.isAdded()) return;
        String manifestUrl = GamedataState.resolveManifestUrl(host.requireContext());
        if (manifestUrl.isEmpty()) {
            new AlertDialog.Builder(host.requireActivity())
                    .setTitle("No download source configured")
                    .setMessage("This build has no built-in Lite data download source.\n\nPrepare a game data .zip first, then tap "Local import" to import it into the launcher.")
                    .setPositiveButton("Go to local import", (d, w) ->
                            host.gameDataZipPicker.launch(new String[]{"application/zip", "application/octet-stream", "*/*"}))
                    .setNegativeButton("Got it", null)
                    .show();
            return;
        }

        boolean coreReady = GameDataImporter.isGameDataReady(host.requireContext().getExternalFilesDir(null));
        boolean hasProblems = lastReport != null && lastReport.problemCount() > 0;
        if (coreReady && !hasProblems) {
            new AlertDialog.Builder(host.requireActivity())
                    .setTitle("Local files are complete")
                    .setMessage("All current Lite core files passed verification. Download again and overwrite to repair anyway? (Your settings and server list will be kept.)")
                    .setPositiveButton("Force re-download", (d, w) -> startManualDownload(manifestUrl, true))
                    .setNeutralButton("Check for updates only", (d, w) -> startManualDownload(manifestUrl, false))
                    .setNegativeButton("Cancel", null)
                    .show();
            return;
        }

        startManualDownload(manifestUrl, true);
    }

    private void startManualDownload(String manifestUrl, boolean forceRedownload) {
        if (!downloading.compareAndSet(false, true)) return;
        lastDownloadProgressPostMs = 0L;

        if (layoutDownloadProgress != null) {
            layoutDownloadProgress.setVisibility(View.VISIBLE);
        }
        if (pbDownload != null) {
            pbDownload.setProgress(0);
        }
        if (tvDlStatus != null) {
            tvDlStatus.setText("Connecting to the download service…");
            tvDlStatus.setTextColor(host.requireContext().getResources().getColor(R.color.launcher_text));
        }
        if (tvDlBytes != null) {
            tvDlBytes.setText("0%");
        }
        updateDownloadButtonUI();

        final Context appCtx = host.requireContext().getApplicationContext();
        final GamedataInstallerTask task = new GamedataInstallerTask(appCtx, manifestUrl, forceRedownload);
        activeDownloadTask = task;

        Thread worker = new Thread(() -> task.run(new GamedataInstallerTask.Listener() {
            @Override
            public void onStage(GamedataInstallerTask.Stage stage) {
                postToUi(() -> {
                    if (tvDlStatus == null) return;
                    switch (stage) {
                        case FETCHING_MANIFEST:
                            tvDlStatus.setText("Fetching the Lite data manifest…");
                            break;
                        case CHECKING:
                            tvDlStatus.setText("Checking data version…");
                            break;
                        case DOWNLOADING:
                            tvDlStatus.setText("Downloading game data (resumable)…");
                            break;
                        case VERIFYING:
                            tvDlStatus.setText("Verifying data pack integrity (SHA-256)…");
                            break;
                        case EXTRACTING:
                            tvDlStatus.setText("Extracting and installing game files…");
                            break;
                        case ACTIVATING:
                            tvDlStatus.setText("Enabling installed files…");
                            break;
                        case READY:
                        default:
                            break;
                    }
                });
            }

            @Override
            public void onProgress(long bytesDone, long bytesTotal) {
                long now = System.currentTimeMillis();
                int perMille = bytesTotal > 0
                        ? (int) Math.min(1000L, (bytesDone * 1000L) / bytesTotal)
                        : 0;
                boolean force = perMille >= 1000 || lastDownloadProgressPostMs == 0L;
                if (!force && (now - lastDownloadProgressPostMs) < 200L) return;
                lastDownloadProgressPostMs = now;
                postToUi(() -> {
                    if (pbDownload != null && bytesTotal > 0) {
                        pbDownload.setProgress(perMille);
                    }
                    if (tvDlBytes != null && bytesTotal > 0) {
                        tvDlBytes.setText(String.format(Locale.US, "%d%%  (%s / %s)",
                                perMille / 10,
                                GameDataVerifier.formatBytes(bytesDone),
                                GameDataVerifier.formatBytes(bytesTotal)));
                    }
                });
            }

            @Override
            public void onStatus(String message) {
                if (message == null || message.isEmpty()) return;
                postToUi(() -> {
                    if (tvDlStatus != null) {
                        tvDlStatus.setText(message);
                    }
                });
            }

            @Override
            public void onError(String message, Throwable cause) {
                downloading.set(false);
                activeDownloadTask = null;
                postToUi(() -> {
                    updateDownloadButtonUI();
                    if (tvDlStatus != null) {
                        tvDlStatus.setText(message != null ? message : "Download failed");
                        tvDlStatus.setTextColor(host.requireContext().getResources().getColor(R.color.ui_bad));
                    }
                    refreshVerification();
                });
            }

            @Override
            public void onDone(boolean installedNow) {
                downloading.set(false);
                activeDownloadTask = null;
                if (installedNow) {
                    try {
                        ClientSettings.ensureExists(appCtx);
                    } catch (Exception ignored) {
                    }
                }
                postToUi(() -> {
                    updateDownloadButtonUI();
                    if (pbDownload != null) {
                        pbDownload.setProgress(1000);
                    }
                    if (tvDlBytes != null) {
                        tvDlBytes.setText("100%");
                    }
                    if (tvDlStatus != null) {
                        tvDlStatus.setText(installedNow ? "Lite resources downloaded and installed" : "Local data is already up to date, no need to re-download");
                        tvDlStatus.setTextColor(host.requireContext().getResources().getColor(R.color.ui_good));
                    }
                    refreshVerification();
                });
            }
        }), "gamedata-manual-download");
        worker.start();
    }

    private void postToUi(Runnable action) {
        FragmentActivity act = host.getActivity();
        if (act != null && host.isAdded()) {
            act.runOnUiThread(() -> {
                if (host.isAdded() && host.getView() != null) {
                    action.run();
                }
            });
        }
    }

    private class GameDataFileAdapter extends RecyclerView.Adapter<GameDataFileAdapter.VH> {
        class VH extends RecyclerView.ViewHolder {
            final View statusDot;
            final TextView tvPath, tvDesc, tvSize, tvBadge;

            VH(View itemView) {
                super(itemView);
                statusDot = itemView.findViewById(R.id.v_gd_status_dot);
                tvPath = itemView.findViewById(R.id.tv_gd_file_path);
                tvDesc = itemView.findViewById(R.id.tv_gd_file_desc);
                tvSize = itemView.findViewById(R.id.tv_gd_file_size);
                tvBadge = itemView.findViewById(R.id.tv_gd_file_badge);
            }
        }

        @NonNull
        @Override
        public VH onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            View v = LayoutInflater.from(parent.getContext())
                    .inflate(R.layout.item_gamedata_file, parent, false);
            return new VH(v);
        }

        @Override
        public void onBindViewHolder(@NonNull VH h, int position) {
            GameDataVerifier.FileCheckItem item = displayedItems.get(position);
            Context ctx = h.itemView.getContext();
            h.tvPath.setText(item.path);
            h.tvDesc.setText(item.group + " · " + item.label);

            if (item.status == GameDataVerifier.Status.OK) {
                String sizeText = item.directory
                        ? String.format(Locale.US, "%d files · %s", item.fileCount, GameDataVerifier.formatBytes(item.bytes))
                        : GameDataVerifier.formatBytes(item.bytes);
                h.tvSize.setText(sizeText);
            } else if (item.status == GameDataVerifier.Status.INVALID) {
                h.tvSize.setText(GameDataVerifier.formatBytes(item.bytes));
            } else {
                h.tvSize.setText("—");
            }

            int colorRes;
            String badgeText;
            switch (item.status) {
                case OK:
                    colorRes = R.color.ui_good;
                    badgeText = "OK";
                    break;
                case INVALID:
                    colorRes = R.color.ui_warn;
                    badgeText = "File broken";
                    break;
                case SKIPPED_LITE:
                    colorRes = R.color.launcher_text_dim;
                    badgeText = "Skipped in Lite";
                    break;
                case MISSING:
                default:
                    colorRes = R.color.ui_bad;
                    badgeText = "Missing";
                    break;
            }
            int color = ctx.getResources().getColor(colorRes);
            h.statusDot.setBackgroundTintList(ColorStateList.valueOf(color));
            h.tvBadge.setText(badgeText);
            h.tvBadge.setTextColor(color);
        }

        @Override
        public int getItemCount() {
            return displayedItems.size();
        }
    }
}
