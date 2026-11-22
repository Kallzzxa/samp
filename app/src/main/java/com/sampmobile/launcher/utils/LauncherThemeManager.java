package com.sampmobile.launcher.utils;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.graphics.drawable.StateListDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageView;
import android.widget.Switch;
import android.widget.TextView;

import com.google.android.material.button.MaterialButton;
import com.google.android.material.textfield.TextInputLayout;
import com.sampmobile.launcher.R;
import com.sampmobile.launcher.ui.widget.GtaSaSliderView;

import java.util.ArrayList;
import java.util.List;

public class LauncherThemeManager {
    public static final String PREF_HUE = "launcher_theme_hue";
    public static final int DEFAULT_HUE = 38; // Classic GTA/SAMP Gold (#F2B32C)
    public static final int XML_DEFAULT_ACCENT = 0xFFF2B32C;

    private static final int TAG_THEME_ROLE = R.id.tag_theme_role;
    private static final String ROLE_ACCENT_TEXT = "role_accent_text";
    private static final String ROLE_ACCENT_BG_SOLID = "role_accent_bg_solid";
    private static final String ROLE_ACCENT_BTN_OUTLINE = "role_accent_btn_outline";
    private static final String ROLE_ACCENT_IMAGE_TINT = "role_accent_image_tint";

    private static final List<ThemeListener> sListeners = new ArrayList<>();

    public interface ThemeListener {
        void onThemeColorChanged(int accentColor);
    }

    public static void addListener(ThemeListener l) {
        if (!sListeners.contains(l)) {
            sListeners.add(l);
        }
    }

    public static void removeListener(ThemeListener l) {
        sListeners.remove(l);
    }

    public static int getHue(Context context) {
        SharedPreferences prefs = context.getSharedPreferences("samp_settings", Context.MODE_PRIVATE);
        return prefs.getInt(PREF_HUE, DEFAULT_HUE);
    }

    public static void setHue(Context context, int hue) {
        int clamped = Math.max(0, Math.min(360, hue));
        context.getSharedPreferences("samp_settings", Context.MODE_PRIVATE)
                .edit()
                .putInt(PREF_HUE, clamped)
                .apply();
        int accent = getAccentColor(context);
        for (ThemeListener l : new ArrayList<>(sListeners)) {
            l.onThemeColorChanged(accent);
        }
    }

    public static int hueToAccentColor(int hue) {
        float[] hsv = new float[]{hue % 360, 0.84f, 0.96f};
        return Color.HSVToColor(hsv);
    }

    public static int getAccentColor(Context context) {
        return hueToAccentColor(getHue(context));
    }

    public static int getAccentColorAlpha(Context context, int alpha) {
        int c = getAccentColor(context);
        return Color.argb(alpha, Color.red(c), Color.green(c), Color.blue(c));
    }

    private static boolean isDefaultGoldColor(int color) {
        if (color == XML_DEFAULT_ACCENT || color == hueToAccentColor(DEFAULT_HUE)) {
            return true;
        }
        int r = Color.red(color);
        int g = Color.green(color);
        int b = Color.blue(color);
        return (Math.abs(r - 0xF2) <= 10 && Math.abs(g - 0xB3) <= 18 && Math.abs(b - 0x2C) <= 18);
    }

    /**
     * Dynamically creates the selected row/tab background (replace R.drawable.launcher_row_selected)
     * with translucent accent fill + 3dp solid accent left stripe + 1dp bottom border.
     */
    public static Drawable createSelectedRowDrawable(Context ctx, int accentColor) {
        float dp = ctx.getResources().getDisplayMetrics().density;
        int dimFill = Color.argb(0x29, Color.red(accentColor), Color.green(accentColor), Color.blue(accentColor));

        GradientDrawable fill = new GradientDrawable();
        fill.setShape(GradientDrawable.RECTANGLE);
        fill.setColor(dimFill);

        GradientDrawable leftBar = new GradientDrawable();
        leftBar.setShape(GradientDrawable.RECTANGLE);
        leftBar.setColor(accentColor);
        int barWidth = Math.max(2, Math.round(3 * dp));
        leftBar.setSize(barWidth, -1);

        GradientDrawable bottomLine = new GradientDrawable();
        bottomLine.setShape(GradientDrawable.RECTANGLE);
        bottomLine.setColor(0xFF282D37);
        int lineHeight = Math.max(1, Math.round(1 * dp));
        bottomLine.setSize(-1, lineHeight);

        LayerDrawable layer = new LayerDrawable(new Drawable[]{fill, leftBar, bottomLine});
        layer.setLayerGravity(1, Gravity.LEFT | Gravity.FILL_VERTICAL);
        layer.setLayerWidth(1, barWidth);
        layer.setLayerGravity(2, Gravity.BOTTOM | Gravity.FILL_HORIZONTAL);
        layer.setLayerHeight(2, lineHeight);
        return layer;
    }

    /**
     * Dynamically builds a themed Switch track drawable with the "ON" / "OFF" pill matching ic_switch_on / ic_switch_off,
     * colored with the current user accentColor when checked.
     */
    public static void applySwitchTheme(Switch sw, int accentColor) {
        if (sw == null) return;
        Context ctx = sw.getContext();
        float dp = ctx.getResources().getDisplayMetrics().density;
        int w = Math.max(48, Math.round(48 * dp));
        int h = Math.max(24, Math.round(24 * dp));

        Drawable onDrawable = buildSwitchTrackBitmap(ctx, w, h, dp, accentColor, true);
        Drawable offDrawable = buildSwitchTrackBitmap(ctx, w, h, dp, 0xFF8A8A8A, false);

        StateListDrawable track = new StateListDrawable();
        track.addState(new int[]{android.R.attr.state_checked}, onDrawable);
        track.addState(new int[]{}, offDrawable);
        sw.setTrackDrawable(track);
    }

    private static Drawable buildSwitchTrackBitmap(Context ctx, int w, int h, float dp, int pillColor, boolean isOn) {
        Bitmap bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bmp);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

        float left = (isOn ? 5f : 3f) * dp;
        float right = (isOn ? 45f : 44f) * dp;
        float top = 4f * dp;
        float bottom = 20f * dp;
        float radius = 8f * dp;

        paint.setStyle(Paint.Style.FILL);
        paint.setColor(pillColor);
        canvas.drawRoundRect(new RectF(left, top, right, bottom), radius, radius, paint);

        paint.setColor(0xFF0A0C10);
        paint.setTypeface(Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD));
        paint.setTextSize(7.5f * dp);
        paint.setTextAlign(Paint.Align.CENTER);
        Paint.FontMetrics fm = paint.getFontMetrics();
        float textY = (top + bottom) / 2f - (fm.ascent + fm.descent) / 2f;
        float textX = (isOn ? 19.5f : 29f) * dp;
        canvas.drawText(isOn ? "ON" : "OFF", textX, textY, paint);

        return new BitmapDrawable(ctx.getResources(), bmp);
    }

    /**
     * Recursively applies the user's accent color across an entire view hierarchy:
     * - GtaSaSliderView (track & thumb)
     * - Switch (ON track pill color)
     * - TextInputLayout (dropdown arrow tint & active box stroke)
     * - MaterialButton (outline stroke & text if originally gold)
     * - TextView (section headers, card titles, slider percentage labels, badges, links originally gold)
     * - ImageView (icons originally tinted gold)
     * - View (2dp accent underline bars originally gold)
     */
    public static void applyThemeToTree(View root, int accentColor) {
        if (root == null) return;

        if (root instanceof GtaSaSliderView) {
            ((GtaSaSliderView) root).setAccentColor(accentColor);
            return;
        }

        if (root instanceof Switch) {
            applySwitchTheme((Switch) root, accentColor);
            return;
        }

        if (root instanceof TextInputLayout) {
            TextInputLayout til = (TextInputLayout) root;
            til.setEndIconTintList(ColorStateList.valueOf(accentColor));
            til.setBoxStrokeColor(accentColor);
            til.setHintTextColor(ColorStateList.valueOf(accentColor));
        }

        Object tag = root.getTag();
        if (tag instanceof String) {
            String t = (String) tag;
            if ("theme_accent_text".equals(t) && root instanceof TextView) {
                ((TextView) root).setTextColor(accentColor);
            } else if ("theme_accent_bg".equals(t)) {
                root.setBackgroundTintList(ColorStateList.valueOf(accentColor));
            } else if ("theme_accent_btn".equals(t) && root instanceof MaterialButton) {
                MaterialButton mb = (MaterialButton) root;
                mb.setTextColor(accentColor);
                mb.setStrokeColor(ColorStateList.valueOf(accentColor));
            }
        }

        Object roleObj = root.getTag(TAG_THEME_ROLE);
        String role = (roleObj instanceof String) ? (String) roleObj : null;

        if (root instanceof MaterialButton) {
            MaterialButton mb = (MaterialButton) root;
            if (role == null) {
                if (isDefaultGoldColor(mb.getCurrentTextColor())) {
                    role = ROLE_ACCENT_BTN_OUTLINE;
                    mb.setTag(TAG_THEME_ROLE, role);
                }
            }
            if (ROLE_ACCENT_BTN_OUTLINE.equals(role)) {
                mb.setTextColor(accentColor);
                mb.setStrokeColor(ColorStateList.valueOf(accentColor));
            }
        } else if (root instanceof TextView) {
            TextView tv = (TextView) root;
            if (role == null) {
                if (isDefaultGoldColor(tv.getCurrentTextColor())) {
                    role = ROLE_ACCENT_TEXT;
                    tv.setTag(TAG_THEME_ROLE, role);
                }
            }
            if (ROLE_ACCENT_TEXT.equals(role)) {
                tv.setTextColor(accentColor);
            }
        } else if (root instanceof ImageView) {
            ImageView iv = (ImageView) root;
            if (role == null) {
                ColorStateList tint = iv.getImageTintList();
                if (tint != null && isDefaultGoldColor(tint.getDefaultColor())) {
                    role = ROLE_ACCENT_IMAGE_TINT;
                    iv.setTag(TAG_THEME_ROLE, role);
                }
            }
            if (ROLE_ACCENT_IMAGE_TINT.equals(role)) {
                iv.setImageTintList(ColorStateList.valueOf(accentColor));
            }
        } else {
            if (role == null) {
                Drawable bg = root.getBackground();
                if (bg instanceof ColorDrawable && isDefaultGoldColor(((ColorDrawable) bg).getColor())) {
                    role = ROLE_ACCENT_BG_SOLID;
                    root.setTag(TAG_THEME_ROLE, role);
                }
            }
            if (ROLE_ACCENT_BG_SOLID.equals(role)) {
                root.setBackgroundColor(accentColor);
            }
        }

        if (root instanceof ViewGroup) {
            ViewGroup vg = (ViewGroup) root;
            for (int i = 0; i < vg.getChildCount(); i++) {
                applyThemeToTree(vg.getChildAt(i), accentColor);
            }
        }
    }
}
