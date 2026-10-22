package dev.optimalgladius.nativevulkantest;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.graphics.Color;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.FrameLayout;
import android.widget.TextView;

public class MainActivity extends Activity
        implements SurfaceHolder.Callback {

    static {
        System.loadLibrary("vulkan_probe");
    }

    private native String startRendering(Surface surface);
    private native void stopRendering();
    private native String getRendererStatus();

    private final Handler handler = new Handler(Looper.getMainLooper());

    private SurfaceView surfaceView;
    private TextView statusView;
    private boolean surfaceReady = false;

    private final Runnable statusUpdater = new Runnable() {
        @Override
        public void run() {
            if (!surfaceReady || statusView == null) {
                return;
            }

            try {
                statusView.setText(getRendererStatus());
            } catch (Exception e) {
                statusView.setText("Unable to read renderer status:\n"
                        + e.getMessage());
            }

            handler.postDelayed(this, 500);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        FrameLayout root = new FrameLayout(this);

        surfaceView = new SurfaceView(this);
        statusView = new TextView(this);

        statusView.setText("Waiting for Android surface...");
        statusView.setTextColor(Color.WHITE);
        statusView.setTextSize(13);
        statusView.setTypeface(Typeface.MONOSPACE);
        statusView.setPadding(16, 14, 16, 14);
        statusView.setGravity(Gravity.START);
        statusView.setBackgroundColor(0xDD101820);
        statusView.setTextIsSelectable(true);

        root.addView(surfaceView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        FrameLayout.LayoutParams statusParams =
                new FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.MATCH_PARENT,
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        Gravity.TOP);

        root.addView(statusView, statusParams);

        setContentView(root);
        surfaceView.getHolder().addCallback(this);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        statusView.setText("Starting native Vulkan renderer...");

        String result = startRendering(holder.getSurface());
        statusView.setText(result);

        surfaceReady = true;
        handler.removeCallbacks(statusUpdater);
        handler.post(statusUpdater);
    }

    @Override
    public void surfaceChanged(
            SurfaceHolder holder,
            int format,
            int width,
            int height) {
        // Swapchain recreation will be added in a later milestone.
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        handler.removeCallbacks(statusUpdater);

        stopRendering();

        if (statusView != null) {
            statusView.setText("Surface destroyed. Renderer stopped.");
        }
    }

    @Override
    protected void onDestroy() {
        surfaceReady = false;
        handler.removeCallbacks(statusUpdater);

        stopRendering();

        super.onDestroy();
    }
}
