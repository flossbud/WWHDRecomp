// Change Files' restart (t-startscreen S3): an activity in a process of its own (the manifest's :restart) that ends
// the app's process and starts its activity again, as an app can't exec itself. It shows nothing and is gone at once.
package org.windsong.app;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

public class Restarter extends Activity {
    static final String PID = "pid", AGAIN = "again";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        int pid = getIntent().getIntExtra(PID, -1);
        if (pid > 0)
            Process.killProcess(pid);
        Intent again = getIntent().getParcelableExtra(AGAIN, Intent.class);
        if (again != null) {
            again.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            startActivity(again);
        }
        finish();
        Runtime.getRuntime().exit(0);
    }
}
