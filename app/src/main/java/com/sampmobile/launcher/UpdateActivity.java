package com.sampmobile.launcher;

import android.annotation.SuppressLint;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Message;
import android.os.Messenger;
import android.os.RemoteException;
import android.util.Log;
import android.view.WindowManager;
import android.widget.ProgressBar;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.NotificationCompat;

import com.google.android.material.textview.MaterialTextView;
import com.joom.paranoid.Obfuscate;

import com.sampmobile.launcher.utils.Utils;

@Obfuscate
public class UpdateActivity extends AppCompatActivity {
    private final Messenger mMessenger = new Messenger(new IncomingHandler());
    private Messenger mService;
    public UpdateMode mUpdateMode = UpdateMode.Undefined;

    private boolean mIsStartingUpdate;

    public enum UpdateMode {
        Undefined,
        GameUpdate
    }

    public enum GameStatus {
        Undefined,
        GameUpdateRequired,
        GameFilesUpdateRequired,
        Updated
    }

    public enum UpdateStatus {
        Undefined,
        CheckUpdate,
        DownloadGame,
        DownloadGameFiles
    }

    private boolean mBound;

    private final ServiceConnection mConnection = new ServiceConnection() {
        @Override
        public void onServiceConnected(@NonNull ComponentName componentName, @NonNull IBinder service) {
            mService = new Messenger(service);
            if (mUpdateMode == UpdateMode.GameUpdate) {
                Message outMsg = Message.obtain(null, 7);
                outMsg.replyTo = mMessenger;
                try {
                    mService.send(outMsg);
                } catch (RemoteException e) {
                    e.printStackTrace();
                }
            }
        }

        @Override
        public void onServiceDisconnected(ComponentName componentName) {
            mService = null;
        }
    };

    private final class IncomingHandler extends Handler {
        @SuppressLint("SetTextI18n")
        public void handleMessage(@NonNull Message msg) {
            if (isFinishing() || isDestroyed()) {
                return;
            }
            // Log.i("UpdateActivity", "handleMessage -> " + msg.what);
            switch (msg.what) {
                case 1:
                    Log.i("UpdateActivity", "UpdateService.UPDATE_GAME");
                    if (msg.getData().getBoolean(NotificationCompat.CATEGORY_STATUS, false)) {
                        if (mService == null) return;
                        Message outMsg = Message.obtain(null, 2);
                        outMsg.replyTo = mMessenger;
                        try {
                            mService.send(outMsg);
                        } catch (RemoteException e) {
                            e.printStackTrace();
                        }
                    } else {
                        Log.e("UpdateActivity", "Error update game");
                        return;
                    }
                    break;
                case 2:
                    Log.i("UpdateActivity", "UpdateService.FINISH_UPDATE_GAME");
                    resetProgress(true, 100, 100);
                    ((MaterialTextView) findViewById(R.id.update_state)).setText("更新中…");
                    if (msg.getData().getBoolean(NotificationCompat.CATEGORY_STATUS, false)) {
                        startActivity(new Intent(UpdateActivity.this, SplashActivity.class));
                        finish();
                    } else {
                        Log.e("UpdateActivity", "Error update game");
                        return;
                    }
                    break;
                case 4:
                    Log.i("UpdateActivity", "UpdateService.UPDATE_STATUS");
                    String statusName = msg.getData().getString(NotificationCompat.CATEGORY_STATUS, "");
                    Log.i("UpdateActivity", "statusname = " + statusName);
                    if (statusName.length() >= 2) {
                        UpdateStatus status;
                        try {
                            status = UpdateStatus.valueOf(statusName);
                        } catch (IllegalArgumentException e) {
                            break;
                        }
                        switch (status) {
                            case Undefined:
                                if (!mIsStartingUpdate && mService != null) {
                                    Message outMsg = Message.obtain(null, 1);
                                    outMsg.replyTo = mMessenger;
                                    try {
                                        mService.send(outMsg);
                                    } catch (RemoteException e) {
                                        e.printStackTrace();
                                    }
                                    mIsStartingUpdate = true;
                                }
                                break;
                            case CheckUpdate:
                            case DownloadGame:
                                ((MaterialTextView) findViewById(R.id.update_state)).setText("正在更新游戏…");
                                if (msg.getData().getBoolean("withProgress", false)) {
                                    long total = msg.getData().getLong("total", 0);
                                    long current = msg.getData().getLong("current", 0);
                                    ProgressBar progressBar = findViewById(R.id.progressBar);
                                    progressBar.setIndeterminate(false);
                                    progressBar.setMax((int) (total / 1000));
                                    progressBar.setProgress((int) (current / 1000));
                                    ((MaterialTextView) findViewById(R.id.progress)).setText((((long) 100) * current) / (total + 1) + "%");
                                    ((MaterialTextView) findViewById(R.id.filename)).setText("");
                                    ((MaterialTextView) findViewById(R.id.size)).setText("");
                                }
                                break;
                            case DownloadGameFiles:
                                ((MaterialTextView) findViewById(R.id.update_state)).setText("正在更新游戏文件…");
                                if (msg.getData().getBoolean("withProgress", false)) {
                                    long total = msg.getData().getLong("total", 0);
                                    long current = msg.getData().getLong("current", 0);
                                    String filename = msg.getData().getString("filename", "");
                                    filename = filename.replace("8.com.sampmobile.game.", "");
                                    long totalfiles = msg.getData().getLong("totalfiles", 0);
                                    long currentfile = msg.getData().getLong("currentfile", 0);
                                    ProgressBar progressBar = findViewById(R.id.progressBar);
                                    progressBar.setIndeterminate(false);
                                    progressBar.setMax((int) (total / 1000));
                                    progressBar.setProgress((int) (current / 1000));
                                    ((MaterialTextView) findViewById(R.id.progress)).setText((((long) 100) * current) / (total + 1) + "%");
                                    ((MaterialTextView) findViewById(R.id.filename)).setText(filename);
                                    ((MaterialTextView) findViewById(R.id.size)).setText(Utils.convertBytesToString(current) + " / " + Utils.convertBytesToString(total));

                                    Log.i("UpdateActivity", '[' + filename + "] total = " + total + " | current = " + current + " + " + (((long) 100) * current) / (total + 1) + "%");
                                }
                                break;
                        }
                    }
                    break;
                case 9:
                    Log.e("UpdateActivity", "UpdateActivity -> UpdateService.UPDATE_STATUS_ERROR");
                    try {
                        new AlertDialog.Builder(UpdateActivity.this)
                                .setTitle("更新：")
                                .setMessage("下载更新失败！")
                                .setPositiveButton("确定", (dialog, which) -> {
                                    startActivity(new Intent(UpdateActivity.this, SplashActivity.class));
                                    finish();
                                }).setCancelable(false).show();
                    } catch (Exception e) {
                        e.printStackTrace();
                    }
                    break;
            }
        }
    }

    protected void onDestroy() {
        super.onDestroy();
        if (mBound) {
            mBound = false;
            try {
                unbindService(mConnection);
            } catch (IllegalArgumentException ignored) {
            }
        }
    }

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestWindowFeature(1);
        setContentView(R.layout.activity_update);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        resetProgress(false, 0, 0);
        ((MaterialTextView) findViewById(R.id.update_state)).setText("准备中…");

        Intent intent = getIntent();
        String modeName = intent != null ? intent.getStringExtra("mode") : null;
        try {
            mUpdateMode = modeName == null ? UpdateMode.Undefined : UpdateMode.valueOf(modeName);
        } catch (IllegalArgumentException e) {
            mUpdateMode = UpdateMode.Undefined;
        }
        if (mService == null)
            mBound = bindService(new Intent(this, UpdateService.class), mConnection, Context.BIND_AUTO_CREATE);
        else if (mUpdateMode == UpdateMode.GameUpdate) {
            Message outMsg = Message.obtain(null, 7);
            outMsg.replyTo = mMessenger;
            try {
                mService.send(outMsg);
            } catch (RemoteException e) {
                e.printStackTrace();
            }
        }
    }

    private void resetProgress(boolean withProgress, long total, long current) {
        ProgressBar progressBar = findViewById(R.id.progressBar);
        ((MaterialTextView) findViewById(R.id.progress)).setText("");
        ((MaterialTextView) findViewById(R.id.filename)).setText("");
        ((MaterialTextView) findViewById(R.id.size)).setText("");
        progressBar.setIndeterminate(true);
        if (withProgress) {
            progressBar.setMax((int) total);
            progressBar.setProgress((int) current);
        }
    }

    @Override
    public void onBackPressed() {
        quitApp();
    }

    private long quit_time = 0;

    public void quitApp() {
        if ((System.currentTimeMillis() - quit_time) > 2000) {
            Toast.makeText(this, "再按一次退出", Toast.LENGTH_LONG).show();
            quit_time = System.currentTimeMillis();
        } else {
            finish();
        }
    }
}