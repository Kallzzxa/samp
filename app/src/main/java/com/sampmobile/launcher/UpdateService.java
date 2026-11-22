package com.sampmobile.launcher;

import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.os.RemoteException;
import android.util.Log;

import androidx.core.app.NotificationCompat;

import com.downloader.Error;
import com.downloader.OnDownloadListener;
import com.downloader.PRDownloader;
import com.downloader.PRDownloaderConfig;
import com.joom.paranoid.Obfuscate;

import org.json.JSONObject;

import java.io.File;
import java.util.ArrayList;

import kotlin.jvm.internal.Ref;
import com.sampmobile.launcher.utils.FileData;
import com.sampmobile.launcher.utils.Utils;

@Obfuscate
public class UpdateService extends Service {
    public Messenger mMessenger;
    public IncomingHandler mInHandler;
    public volatile Messenger mActivityMessenger;
    private HandlerThread mHandlerThread;

    public volatile UpdateActivity.UpdateStatus mUpdateStatus = UpdateActivity.UpdateStatus.Undefined;
    public volatile UpdateActivity.GameStatus mGameStatus = UpdateActivity.GameStatus.Undefined;

    public volatile boolean mDownloadingStatus = false;
    public ArrayList<FileData> mUpdateFiles = new ArrayList<>();
    public volatile long mUpdateFilesSizeTotal = 0;
    public volatile int mDownloadFailedOffset;

    public volatile String mUpdateVersion;

    private static File resolveSafeTargetFile(File baseDir, String relativePath) {
        if (baseDir == null || relativePath == null || relativePath.isEmpty()) {
            return null;
        }
        String normalized = relativePath.replace('\\', '/');
        if (normalized.startsWith("/") || normalized.contains("..")) {
            return null;
        }
        try {
            File target = new File(baseDir, normalized);
            String canonicalBase = baseDir.getCanonicalPath() + File.separator;
            String canonicalTarget = target.getCanonicalPath();
            if (!canonicalTarget.startsWith(canonicalBase)) {
                return null;
            }
            return target;
        } catch (Exception e) {
            return null;
        }
    }

    public void onCreate() {
        mHandlerThread = new HandlerThread("ServiceStartArguments", 10);
        mHandlerThread.start();
        PRDownloader.initialize(getApplicationContext(), PRDownloaderConfig.newBuilder().setDatabaseEnabled(true).setReadTimeout(30000).setConnectTimeout(30000).build());
        mInHandler = new IncomingHandler(mHandlerThread.getLooper());
        mMessenger = new Messenger(mInHandler);
    }

    public int onStartCommand(Intent intent, int flags, int startId) {
        return Service.START_STICKY;
    }

    public IBinder onBind(Intent intent) {
        if (mMessenger != null) {
            return mMessenger.getBinder();
        }
        return null;
    }

    public boolean onUnbind(Intent intent) {
        return false;
    }

    public void onRebind(Intent intent) {
    }

    public void onDestroy() {
        if (mHandlerThread != null) {
            mHandlerThread.quitSafely();
            mHandlerThread = null;
        }
    }

    private final class IncomingHandler extends Handler {
        public IncomingHandler(Looper looper) {
            super(looper);
        }

        public void handleMessage(Message msg) {
            mActivityMessenger = msg.replyTo;
            // Log.i("UpdateService", "handleMessage -> " + msg.what);
            if (msg.what == 0) checkUpdate(); // check update
            else if (msg.what == 1) updateGameFiles(); // update files
            else if (msg.what == 2) updateGame(); // update game
            else if (msg.what == 4) { // get update status
                Message outMsg = Message.obtain(mInHandler, 4);
                outMsg.getData().putString(NotificationCompat.CATEGORY_STATUS, mUpdateStatus.name());
                outMsg.replyTo = mMessenger;
                if (mActivityMessenger != null) {
                    try {
                        mActivityMessenger.send(outMsg);
                    } catch (RemoteException e) {
                        e.printStackTrace();
                    }
                }
            } else if (msg.what == 5) { // get game status
                Message outMsg = Message.obtain(mInHandler, 5);
                outMsg.getData().putString(NotificationCompat.CATEGORY_STATUS, mGameStatus.name());
                outMsg.replyTo = mMessenger;
                if (mActivityMessenger != null) {
                    try {
                        mActivityMessenger.send(outMsg);
                    } catch (RemoteException e) {
                        e.printStackTrace();
                    }
                }
            } else if (msg.what == 7) { // check update
                Log.i("UpdateService", "UPDATE_STATUS_GAME");
                checkUpdate();
            } else if (msg.what == 8) { // check update
                checkUpdate();
            }
        }
    }

    public void checkUpdate() {
        // Remote update check is disabled: no update server is configured for this build.
        // Report Updated so the launcher proceeds to the main screen.
        Log.d("UpdateService", "checkUpdate() - remote check disabled, reporting Updated");

        mUpdateFiles = new ArrayList<>();
        mGameStatus = UpdateActivity.GameStatus.Updated;

        Message outMsg = Message.obtain(mInHandler, 5);
        outMsg.getData().putString(NotificationCompat.CATEGORY_STATUS, mGameStatus.name());
        outMsg.replyTo = mMessenger;
        if (mActivityMessenger != null) {
            try {
                mActivityMessenger.send(outMsg);
            } catch (RemoteException e) {
                e.printStackTrace();
            }
        }
        setUpdateStatus(UpdateActivity.UpdateStatus.Undefined);
    }

    public boolean isGameUpdateExists() {
        PackageInfo packageInfo;
        Log.i("UpdateService", "isGameUpdateExists");
        PackageManager packageManager = getPackageManager();
        String currentVersion = null;
        if (packageManager != null) {
            try {
                packageInfo = packageManager.getPackageInfo(getPackageName(), PackageManager.GET_ACTIVITIES);
            } catch (PackageManager.NameNotFoundException e) {
                return true;
            }
        } else {
            packageInfo = null;
        }
        if (packageInfo != null) {
            currentVersion = packageInfo.versionName;
        }
        String sb = "isGameUpdateExists -> currentVersion " + currentVersion + " | mUpdateVersion " + mUpdateVersion;
        Log.d("UpdateService", sb);
        return (currentVersion == null || !currentVersion.equals(mUpdateVersion));
    }

    public boolean isGamePackageExists() {
        try {
            getPackageManager().getPackageInfo(getPackageName(), PackageManager.GET_META_DATA);
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        }
        return true;
    }

    public void updateGame() {
        setUpdateStatus(UpdateActivity.UpdateStatus.Undefined);
        Message finishMsg = Message.obtain(mInHandler, 2);
        finishMsg.getData().putBoolean(NotificationCompat.CATEGORY_STATUS, true);
        finishMsg.replyTo = mMessenger;
        Messenger messenger = mActivityMessenger;
        if (messenger != null) {
            try {
                messenger.send(finishMsg);
            } catch (RemoteException e) {
                e.printStackTrace();
            }
        }
        setUpdateStatus(UpdateActivity.UpdateStatus.Undefined);
    }

    public void checkGameFilesUpdate(String list_url, String samp_list_url) throws Exception {
        Log.d("UpdateService", "checkGameFilesUpdate");

        mUpdateFilesSizeTotal = 0;
        File baseDir = getExternalFilesDir(null);
        if (baseDir == null) {
            throw new IllegalStateException("External files directory unavailable");
        }

        String data_str = Utils.getStringOutputByURL(list_url);
        if (data_str == null || data_str.isEmpty()) {
            throw new IllegalStateException("Empty update manifest");
        }
        JSONObject data_json = new JSONObject(data_str);
        ArrayList<FileData> dataList = FileData.getListByJson(data_json);

        if (samp_list_url != null && !samp_list_url.isEmpty()) {
            Log.d("UpdateService", "checkGameFilesUpdate -> samp_list_url");
            String sampStr = Utils.getStringOutputByURL(samp_list_url);
            if (sampStr != null && !sampStr.isEmpty()) {
                ArrayList<FileData> sampDataList = FileData.getListByJson(new JSONObject(sampStr));
                dataList.addAll(sampDataList);
            }
        } else {
            Log.d("UpdateService", "checkGameFilesUpdate -> samp_list_url is empty");
        }

        boolean modifyFiles = getSharedPreferences("samp_settings", Context.MODE_PRIVATE).getBoolean("modify_files", false);
        for (FileData fileData : dataList) {
            File forCheck = resolveSafeTargetFile(baseDir, fileData.getPath());
            if (forCheck == null) {
                Log.w("UpdateService", "Skipping unsafe remote path: " + fileData.getPath());
                continue;
            }

            // skip checking file size if not using modify_files
            if (modifyFiles ? forCheck.exists() : (forCheck.exists() && forCheck.length() == fileData.getSize())) {
                continue; // The file exists and has the correct size; no need to update.
            }

            if (!fileData.getGpu().equals("all")) {
                if ((fileData.getGpu().equals("dxt") && Utils.GPU_TYPE != Utils.GPUType.DXT) ||
                        (fileData.getGpu().equals("pvr") && Utils.GPU_TYPE != Utils.GPUType.PVR) ||
                        (fileData.getGpu().equals("etc") && Utils.GPU_TYPE != Utils.GPUType.ETC)) {
                    continue; // GPU type doesn't match; skip this file.
                }
            }

            System.out.println("Missing/Corrupted file: " + fileData.getPath() + " | " + fileData.getSize() + " bytes");
            System.out.println("File: " + forCheck.getAbsolutePath() + " | " + (forCheck.exists() ? forCheck.length() + " bytes" : "missing"));

            mUpdateFiles.add(fileData);
            mUpdateFilesSizeTotal += fileData.getSize();
        }
    }

    public void setUpdateStatus(UpdateActivity.UpdateStatus status) {
        if (!(status.name().isEmpty()) && mUpdateStatus != status) {
            mUpdateStatus = status;
            Message outMsg = Message.obtain(mInHandler, 4);
            outMsg.getData().putString(NotificationCompat.CATEGORY_STATUS, mUpdateStatus.name());
            outMsg.replyTo = mMessenger;
            Messenger messenger = mActivityMessenger;
            if (messenger != null) {
                try {
                    messenger.send(outMsg);
                } catch (RemoteException e) {
                    e.printStackTrace();
                }
            }
        }
    }

    public void updateGameFiles() {
        if (isGameFilesUpdateExists()) {
            setUpdateStatus(UpdateActivity.UpdateStatus.DownloadGameFiles);
            downloadGameFiles();
            return;
        }
        Log.d("UpdateService", "updateGameFiles");
        Message outMsg = Message.obtain(mInHandler, 1);
        outMsg.getData().putBoolean(NotificationCompat.CATEGORY_STATUS, true);
        outMsg.replyTo = mMessenger;
        if (mActivityMessenger != null) {
            try {
                mActivityMessenger.send(outMsg);
            } catch (RemoteException e) {
                e.printStackTrace();
            }
        }
    }

    private void downloadGameFiles() {
        Log.i("UpdateService", "Download Game Files");
        mDownloadFailedOffset = 0;
        final ArrayList<FileData> tempUpdateFiles = new ArrayList<>(mUpdateFiles);
        mUpdateFiles.clear();
        final Ref.IntRef i = new Ref.IntRef();
        final Ref.LongRef mUpdateFilesSizeCurrent = new Ref.LongRef();
        mUpdateFilesSizeCurrent.element = 0;

        File baseDir = getExternalFilesDir(null);
        if (baseDir == null) {
            Log.e("UpdateService", "External files directory unavailable during download");
            mDownloadingStatus = false;
            updateGame();
            return;
        }

        sendLoadingScreen(false, "", 0, 0);
        for (i.element = 0; i.element < tempUpdateFiles.size(); i.element++) {
            mDownloadingStatus = true;
            final FileData fileData = tempUpdateFiles.get(i.element);

            Log.i("UpdateService", i.element + " | filePath = " + fileData.getPath());
            Log.i("UpdateService", "Request uri = " + fileData.getUrl());

            final File file = resolveSafeTargetFile(baseDir, fileData.getPath());
            if (file == null) {
                Log.w("UpdateService", "Skipping unsafe download path: " + fileData.getPath());
                mDownloadingStatus = false;
                continue;
            }
            File dir = file.getParentFile();
            if (dir == null) {
                mDownloadingStatus = false;
                continue;
            }
            if (!dir.exists()) dir.mkdirs();

            if (file.exists()) file.delete();

            final Ref.LongRef mLastDownloadedBytesTime = new Ref.LongRef();
            mLastDownloadedBytesTime.element = System.currentTimeMillis();
            mDownloadingStatus = true;
            PRDownloader.download(fileData.getUrl(), dir.toString(), fileData.getName()).build().
                    setOnStartOrResumeListener(null).
                    setOnPauseListener(null).
                    setOnCancelListener(null).
                    setOnProgressListener(progress -> {
                        mDownloadingStatus = true;
                        if (System.currentTimeMillis() - mLastDownloadedBytesTime.element > ((long) 100)) {
                            mLastDownloadedBytesTime.element = System.currentTimeMillis();
                            Message outMsg = Message.obtain(mInHandler, 4);
                            outMsg.getData().putString(NotificationCompat.CATEGORY_STATUS, UpdateActivity.UpdateStatus.DownloadGameFiles.name());
                            outMsg.getData().putBoolean("withProgress", true);
                            outMsg.getData().putLong("current", (mUpdateFilesSizeCurrent.element + progress.currentBytes));
                            outMsg.getData().putLong("total", mUpdateFilesSizeTotal);
                            outMsg.getData().putString("filename", fileData.getName());
                            outMsg.getData().putLong("totalfiles", ((long) tempUpdateFiles.size()) - ((long) mDownloadFailedOffset));
                            outMsg.getData().putLong("currentfile", ((long) i.element) - ((long) mDownloadFailedOffset));
                            outMsg.replyTo = mMessenger;
                            if (mActivityMessenger != null) {
                                try {
                                    mActivityMessenger.send(outMsg);
                                } catch (RemoteException e) {
                                    e.printStackTrace();
                                }
                            }
                        }
                    }).
                    start(new OnDownloadListener() {
                        @Override
                        public void onDownloadComplete() {
                            Log.d("UpdateService", "onDownloadComplete");
                            mDownloadingStatus = false;
                            mUpdateFilesSizeCurrent.element += fileData.getSize();
                        }

                        @Override
                        public void onError(Error error) {
                            mDownloadingStatus = false;
                            mUpdateFiles.add(tempUpdateFiles.get(i.element));
                            mDownloadFailedOffset += 1;
                            mUpdateFilesSizeTotal -= fileData.getSize();
                            Log.d("UpdateService", "onError. ServerError = " + (error != null ? error.isServerError() : null) + ". ConnectionError = " + (error != null ? error.isConnectionError() : null));
                        }
                    });
            do {
                try {
                    Thread.sleep(30);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    break;
                }
            } while (mDownloadingStatus);
        }
        mDownloadingStatus = false;
        sendLoadingScreen(false, "", 0, 0);

        updateGame();
    }

    private void sendLoadingScreen(final boolean unpacking, final String fileName, final long current, final long total) {
        new Thread(() -> {
            Message outMsg = Message.obtain(mInHandler, 4);
            outMsg.getData().putString(NotificationCompat.CATEGORY_STATUS, UpdateActivity.UpdateStatus.CheckUpdate.name());
            outMsg.getData().putBoolean("withProgress", true);
            outMsg.getData().putString("filename", fileName);
            outMsg.getData().putBoolean("unpacking", unpacking);
            outMsg.getData().putLong("current", current);
            outMsg.getData().putLong("total", total);
            outMsg.replyTo = mMessenger;
            if (mActivityMessenger != null) {
                try {
                    mActivityMessenger.send(outMsg);
                } catch (RemoteException e) {
                    e.printStackTrace();
                }
            }
        }).start();
    }

    public boolean isGameFilesUpdateExists() {
        Log.i("UpdateService", "isGameFilesUpdateExists");
        return !mUpdateFiles.isEmpty();
    }
}
