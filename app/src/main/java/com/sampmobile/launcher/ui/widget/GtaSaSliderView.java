package com.sampmobile.launcher.ui.widget;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Shader;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewParent;

import com.sampmobile.launcher.utils.LauncherThemeManager;

import java.util.ArrayList;
import java.util.List;

/**
 * GTA:SA single-player style segmented bar gauge slider.
 * Lightweight custom Canvas view that replaces heavy Material Slider (20x faster inflation).
 */
public class GtaSaSliderView extends View {

    public interface OnChangeListener {
        void onValueChange(GtaSaSliderView slider, float value, boolean fromUser);
    }

    private static final String ANDROID_NS = "http://schemas.android.com/apk/res/android";
    private static final int SEGMENT_COUNT = 16;

    private float valueFrom = 0f;
    private float valueTo = 100f;
    private float stepSize = 1f;
    private float value = 50f;

    private int accentColor = 0xFFF5A623;
    private boolean rainbowMode = false;
    private boolean isDragging = false;

    private final Paint framePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint borderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint emptyBarPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint filledBarPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint highlightPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint thumbPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private final RectF outerRect = new RectF();
    private final RectF segRect = new RectF();
    private final RectF topHighlightRect = new RectF();

    private final List<OnChangeListener> listeners = new ArrayList<>();

    public GtaSaSliderView(Context context) {
        super(context);
        init(context, null);
    }

    public GtaSaSliderView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init(context, attrs);
    }

    public GtaSaSliderView(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        init(context, attrs);
    }

    private static float readAttrFloat(AttributeSet attrs, String name, float def) {
        if (attrs == null) return def;
        String raw = attrs.getAttributeValue(ANDROID_NS, name);
        if (raw == null || raw.isEmpty()) return def;
        try {
            return Float.parseFloat(raw);
        } catch (NumberFormatException e) {
            return def;
        }
    }

    private void init(Context context, AttributeSet attrs) {
        accentColor = LauncherThemeManager.getAccentColor(context);

        if (attrs != null) {
            valueFrom = readAttrFloat(attrs, "valueFrom", 0f);
            valueTo = readAttrFloat(attrs, "valueTo", 100f);
            stepSize = readAttrFloat(attrs, "stepSize", 1f);
            value = readAttrFloat(attrs, "value", valueFrom);
        }

        float density = context.getResources().getDisplayMetrics().density;

        framePaint.setStyle(Paint.Style.FILL);
        framePaint.setColor(0xFF101218);

        borderPaint.setStyle(Paint.Style.STROKE);
        borderPaint.setStrokeWidth(1.3f * density);
        borderPaint.setColor(0xFF323644);

        emptyBarPaint.setStyle(Paint.Style.FILL);
        emptyBarPaint.setColor(0xFF232631);

        filledBarPaint.setStyle(Paint.Style.FILL);
        filledBarPaint.setColor(accentColor);

        highlightPaint.setStyle(Paint.Style.FILL);
        highlightPaint.setColor(0x40FFFFFF);

        thumbPaint.setStyle(Paint.Style.FILL);
        thumbPaint.setColor(0xFFFFFFFF);

        setClickable(true);
    }

    public void setRainbowMode(boolean rainbow) {
        this.rainbowMode = rainbow;
        invalidate();
    }

    public void setAccentColor(int color) {
        this.accentColor = color;
        filledBarPaint.setColor(color);
        invalidate();
    }

    public float getValue() {
        return value;
    }

    public void setValue(float newValue) {
        setValueInternal(newValue, false);
    }

    public float getValueFrom() {
        return valueFrom;
    }

    public void setValueFrom(float valueFrom) {
        this.valueFrom = valueFrom;
        setValueInternal(this.value, false);
    }

    public float getValueTo() {
        return valueTo;
    }

    public void setValueTo(float valueTo) {
        this.valueTo = valueTo;
        setValueInternal(this.value, false);
    }

    public float getStepSize() {
        return stepSize;
    }

    public void setStepSize(float stepSize) {
        this.stepSize = stepSize;
    }

    public void addOnChangeListener(OnChangeListener listener) {
        if (listener != null && !listeners.contains(listener)) {
            listeners.add(listener);
        }
    }

    public void clearOnChangeListeners() {
        listeners.clear();
    }

    private void setValueInternal(float rawValue, boolean fromUser) {
        float clamped = Math.max(valueFrom, Math.min(valueTo, rawValue));
        if (stepSize > 0f) {
            int steps = Math.round((clamped - valueFrom) / stepSize);
            clamped = valueFrom + steps * stepSize;
            clamped = Math.max(valueFrom, Math.min(valueTo, clamped));
        }
        if (Math.abs(clamped - this.value) > 0.0001f || !fromUser) {
            boolean changed = Math.abs(clamped - this.value) > 0.0001f;
            this.value = clamped;
            invalidate();
            if (changed) {
                for (OnChangeListener l : listeners) {
                    l.onValueChange(this, this.value, fromUser);
                }
            }
        }
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        float density = getResources().getDisplayMetrics().density;
        int defaultW = (int) (220 * density);
        int defaultH = (int) (34 * density);
        int w = resolveSize(defaultW, widthMeasureSpec);
        int h = resolveSize(defaultH, heightMeasureSpec);
        setMeasuredDimension(w, h);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        float density = getResources().getDisplayMetrics().density;
        int w = getWidth();
        int h = getHeight();
        if (w <= 0 || h <= 0) return;

        float padH = 4f * density;
        float barHeight = Math.min(h - 10f * density, 20f * density);
        float top = (h - barHeight) * 0.5f;
        float bottom = top + barHeight;
        float corner = 3.5f * density;

        outerRect.set(padH, top, w - padH, bottom);
        canvas.drawRoundRect(outerRect, corner, corner, framePaint);

        float innerPad = 3.2f * density;
        float innerLeft = outerRect.left + innerPad;
        float innerRight = outerRect.right - innerPad;
        float innerTop = outerRect.top + innerPad;
        float innerBottom = outerRect.bottom - innerPad;
        float innerWidth = Math.max(1f, innerRight - innerLeft);

        float range = Math.max(0.0001f, valueTo - valueFrom);
        float fraction = Math.max(0f, Math.min(1f, (value - valueFrom) / range));

        if (rainbowMode) {
            int[] rainbowColors = new int[]{
                    Color.HSVToColor(new float[]{0f, 0.85f, 0.96f}),
                    Color.HSVToColor(new float[]{60f, 0.85f, 0.96f}),
                    Color.HSVToColor(new float[]{120f, 0.85f, 0.96f}),
                    Color.HSVToColor(new float[]{180f, 0.85f, 0.96f}),
                    Color.HSVToColor(new float[]{240f, 0.85f, 0.96f}),
                    Color.HSVToColor(new float[]{300f, 0.85f, 0.96f}),
                    Color.HSVToColor(new float[]{360f, 0.85f, 0.96f})
            };
            filledBarPaint.setShader(new LinearGradient(
                    innerLeft, innerTop, innerRight, innerTop,
                    rainbowColors, null, Shader.TileMode.CLAMP));
            segRect.set(innerLeft, innerTop, innerRight, innerBottom);
            canvas.drawRoundRect(segRect, 2f * density, 2f * density, filledBarPaint);
            filledBarPaint.setShader(null);

            float thumbX = innerLeft + fraction * innerWidth;
            float thumbW = 3.5f * density;
            segRect.set(thumbX - thumbW, outerRect.top - 1.5f * density, thumbX + thumbW, outerRect.bottom + 1.5f * density);
            canvas.drawRoundRect(segRect, 2f * density, 2f * density, thumbPaint);
        } else {
            // Classic GTA:SA segmented bar gauge (16 distinct bars)
            float gap = 2.4f * density;
            float totalGaps = gap * (SEGMENT_COUNT - 1);
            float segW = Math.max(2f, (innerWidth - totalGaps) / SEGMENT_COUNT);
            float exactFillSegments = fraction * SEGMENT_COUNT;

            filledBarPaint.setColor(accentColor);

            for (int i = 0; i < SEGMENT_COUNT; i++) {
                float sLeft = innerLeft + i * (segW + gap);
                float sRight = sLeft + segW;
                segRect.set(sLeft, innerTop, sRight, innerBottom);

                // First draw empty background slot
                canvas.drawRoundRect(segRect, 1.5f * density, 1.5f * density, emptyBarPaint);

                // Then draw filled portion (full or partial on active segment)
                if (exactFillSegments > i) {
                    float segFill = Math.min(1f, exactFillSegments - i);
                    float fRight = sLeft + segW * segFill;
                    segRect.set(sLeft, innerTop, fRight, innerBottom);
                    canvas.drawRoundRect(segRect, 1.5f * density, 1.5f * density, filledBarPaint);

                    // Classic GTA:SA top bevel sheen
                    topHighlightRect.set(sLeft, innerTop, fRight, innerTop + (innerBottom - innerTop) * 0.35f);
                    canvas.drawRoundRect(topHighlightRect, 1.5f * density, 1.5f * density, highlightPaint);
                }
            }

            // Crisp active thumb marker when dragging
            if (isDragging) {
                float thumbX = innerLeft + fraction * innerWidth;
                float thumbHalfW = 1.8f * density;
                segRect.set(thumbX - thumbHalfW, outerRect.top - 1f * density, thumbX + thumbHalfW, outerRect.bottom + 1f * density);
                canvas.drawRoundRect(segRect, 1.5f * density, 1.5f * density, thumbPaint);
            }
        }

        borderPaint.setColor(isDragging ? accentColor : 0xFF323644);
        canvas.drawRoundRect(outerRect, corner, corner, borderPaint);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (!isEnabled()) return false;
        int action = event.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
                ViewParent parent = getParent();
                if (parent != null) {
                    parent.requestDisallowInterceptTouchEvent(true);
                }
                isDragging = true;
                updateFromTouchX(event.getX());
                invalidate();
                return true;
            case MotionEvent.ACTION_MOVE:
                isDragging = true;
                updateFromTouchX(event.getX());
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                isDragging = false;
                updateFromTouchX(event.getX());
                performClick();
                invalidate();
                return true;
        }
        return super.onTouchEvent(event);
    }

    @Override
    public boolean performClick() {
        return super.performClick();
    }

    private void updateFromTouchX(float touchX) {
        float density = getResources().getDisplayMetrics().density;
        float pad = 7.2f * density;
        float left = pad;
        float right = Math.max(left + 1f, getWidth() - pad);
        float fraction = Math.max(0f, Math.min(1f, (touchX - left) / (right - left)));
        float raw = valueFrom + fraction * (valueTo - valueFrom);
        setValueInternal(raw, true);
    }
}
