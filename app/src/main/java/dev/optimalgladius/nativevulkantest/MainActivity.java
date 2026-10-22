package dev.optimalgladius.nativevulkantest;

import android.app.Activity;
import android.os.Bundle;
import android.graphics.Typeface;
import android.view.Gravity;
import android.widget.ScrollView;
import android.widget.TextView;

public class MainActivity extends Activity {
    static {
        System.loadLibrary("vulkan_probe");
    }

    private native String getVulkanReport();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        TextView output = new TextView(this);
        output.setText(getVulkanReport());
        output.setTextSize(14);
        output.setTypeface(Typeface.MONOSPACE);
        output.setPadding(24, 24, 24, 24);
        output.setTextIsSelectable(true);
        output.setGravity(Gravity.START);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(output);
        setContentView(scroll);
    }
}
