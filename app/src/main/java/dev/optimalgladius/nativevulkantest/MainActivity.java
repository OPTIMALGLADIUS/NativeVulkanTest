package dev.optimalgladius.nativevulkantest;

import android.app.Activity;
import android.os.Bundle;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.view.Gravity;

public class MainActivity extends Activity implements SurfaceHolder.Callback {
    static {
        System.loadLibrary("vulkan_probe");
    }

    private native String startRendering(Surface surface);
    private native void stopRendering();

    private TextView status;
    private boolean surfaceReady = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        FrameLayout root = new FrameLayout(this);
        SurfaceView surfaceView = new SurfaceView(this);
        status = new TextView(this);
        status.setText("Waiting for Android surface...");
        status.setTextColor(0xFFFFFFFF);
        status.setTextSize(14);
        status.setGravity(Gravity.CENTER);
        status.setBackgroundColor(0xCC101820);

        root.addView(surfaceView, new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT
        ));

        FrameLayout.LayoutParams statusParams = new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.TOP
        );
        root.addView(status, statusParams);

        setContentView(root);
        surfaceView.getHolder().addCallback(this);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        status.setText("Starting native Vulkan renderer...");
        String result = startRendering(holder.getSurface());
        status.setText(result);
        surfaceReady = true;
    }

    @Override
    public void surfaceChanged(
        SurfaceHolder holder, int format, int width, int height
    ) {
        // The first implementation uses the surface's current extent.
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        stopRendering();
        status.setText("Surface destroyed.");
    }

    @Override
    protected void onDestroy() {
        if (surfaceReady) {
            stopRendering();
            surfaceReady = false;
        }
        super.onDestroy();
    }
}
