// The app's activity (m-android A2): SDL's, with SDL linked into libmain.so (one library), and the settings play.sh
// gives the desktop build as environment variables. The app's files are its external files directory
// (Android/data/org.windsong.app/files): game/wwhd.wua (the player's copy, which the start screen's Setup copies in
// from the system's picker: t-startscreen S3), app/ (Cemu's resources, the menus' assets: from the APK, extractAssets),
// portable/ (saves, settings, caches).
package org.windsong.app;

import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Process;
import android.provider.OpenableColumns;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.libsdl.app.SDLActivity;

public class WindSongActivity extends SDLActivity {
    private File files() {
        File f = getExternalFilesDir(null);
        return f != null ? f : getFilesDir();
    }

    // The app's own files from the APK's assets (t-startscreen S3; tools/android/apk.sh lays them out): app/ is ours, in
    // the files dir's app/ and replaced (Cemu's resources and game profiles, the shader list, the pause menu's assets);
    // once/ is the player's once there (settings.xml, the controller profile), copied only where missing. Done when the
    // APK's trees (their hash, wwhd-assets.txt) aren't the ones last extracted (files/.assets): a first start, an update.
    // A file that can't be written is logged and left (files a test pushed with adb may not be the app's to replace);
    // the stamp waits until all are.
    private void extractAssets(File dir) {
        String want = readAsset("wwhd-assets.txt");
        File stamp = new File(dir, ".assets");
        if (want == null || want.equals(readFile(stamp)))
            return;
        long start = System.nanoTime();
        int[] failed = { 0 };
        int n = copyTree("app", "", new File(dir, "app"), true, failed) + copyTree("once", "", dir, false, failed);
        if (failed[0] == 0)
            try {
                writeFile(stamp, want);
            } catch (IOException e) {
                failed[0]++;
            }
        Log.i("wwhd", "assets: " + n + " files extracted (" + want.trim() + "), " + failed[0] + " failed, in "
            + (System.nanoTime() - start) / 1000000 + " ms");
    }

    // the files under an assets folder into dir (rel: the path below the folder); replace: over the ones there
    private int copyTree(String top, String rel, File dir, boolean replace, int[] failed) {
        String path = rel.isEmpty() ? top : top + "/" + rel;
        String[] names;
        try {
            names = getAssets().list(path);
        } catch (IOException e) {
            names = null;
        }
        if (names == null || names.length == 0) {      // a file (AssetManager lists a file as an empty folder)
            File out = new File(dir, rel);
            if (!replace && out.exists())
                return 0;
            File part = new File(out.getPath() + ".part");
            try {
                out.getParentFile().mkdirs();
                try (InputStream in = getAssets().open(path); OutputStream o = new FileOutputStream(part)) {
                    byte[] buf = new byte[1 << 16];
                    for (int k; (k = in.read(buf)) > 0;)
                        o.write(buf, 0, k);
                }
                if (!part.renameTo(out))
                    throw new IOException("couldn't put it in place");
                return 1;
            } catch (IOException e) {
                part.delete();
                failed[0]++;
                Log.w("wwhd", "assets: " + out + ": " + e);
                return 0;
            }
        }
        int n = 0;
        for (String name : names)
            n += copyTree(top, rel.isEmpty() ? name : rel + "/" + name, dir, replace, failed);
        return n;
    }

    private String readAsset(String name) {
        try (InputStream in = getAssets().open(name)) {
            return new String(in.readAllBytes(), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return null;
        }
    }

    private static String readFile(File f) {
        try {
            return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return null;
        }
    }

    private static void writeFile(File f, String text) throws IOException {
        Files.write(f.toPath(), text.getBytes(StandardCharsets.UTF_8));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        extractAssets(files());
        String dir = files().getAbsolutePath();
        try {
            Os.setenv("WWHD_HOME", dir, true);              // the frontend's working directory (portable/ there)
            Os.setenv("WWHD_NATIVE", "on", false);
            Os.setenv("WWHD_RENDER", "vk", false);
            Os.setenv("WWHD_WINDOW", "1", false);
            Os.setenv("WWHD_CEMU_DATA", dir + "/app/cemu", false);       // the APK's (extractAssets)
            Os.setenv("WWHD_UI_ASSETS", dir + "/app/assets/pause_menu", false);
            Os.setenv("CEMU_NO_GAMEPAD", "1", false);
            // a4-drivers: libadrenotools' hooks are in the app's native library dir (extracted at install: the
            // manifest's extractNativeLibs); imported GPU drivers in the internal files, where dlopen may load from
            Os.setenv("WWHD_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir, true);
            Os.setenv("WWHD_DRIVERS_DIR", getFilesDir().getAbsolutePath() + "/gpu_drivers", false);
            // tests: am start ... --es env "NAME=VALUE;NAME=VALUE" (tools/android/device.sh run's ENV)
            String env = getIntent() != null ? getIntent().getStringExtra("env") : null;
            if (env != null)
                for (String kv : env.split(";")) {
                    int eq = kv.indexOf('=');
                    if (eq > 0)
                        Os.setenv(kv.substring(0, eq), kv.substring(eq + 1), true);
                }
        } catch (ErrnoException e) {
            throw new RuntimeException(e);
        }
        super.onCreate(savedInstanceState);
        hideSystemBars();
    }

    // The system bars hidden whenever the window is ours again. SDL hides them once, when the window goes fullscreen,
    // with the old system UI flags, which a moment without focus then (another activity over ours, a dialog) can
    // undo: the game was drawn between the bars, black bands at the top and bottom, on some launches and not others.
    private void hideSystemBars() {
        WindowInsetsController bars = getWindow().getInsetsController();
        if (bars == null)
            return;
        bars.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        bars.hide(WindowInsets.Type.systemBars());
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus)
            hideSystemBars();
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    // the player's copy once Setup has copied it in (or adb put it there); without it the start screen's Setup asks
    // (WWHD_NO_GAME=1, a test's env extra: none, so a test home with no copy registered shows Setup)
    @Override
    protected String[] getArguments() {
        File game = new File(files(), "game/wwhd.wua");
        if ("1".equals(Os.getenv("WWHD_NO_GAME")))
            return new String[0];
        return game.isFile() ? new String[] { "-g", game.getAbsolutePath() } : new String[0];
    }

    // the name a content:// URI's document goes by (the picker's choice, for the start screen's Setup: S3), or null
    public String displayName(String uri) {
        try (Cursor c = getContentResolver().query(Uri.parse(uri), new String[] { OpenableColumns.DISPLAY_NAME },
                null, null, null)) {
            if (c != null && c.moveToFirst())
                return c.getString(0);
        } catch (RuntimeException e) {
        }
        return null;
    }

    // Change Files (t-startscreen S3, from native code): the app again, on the copy now in game/wwhd.wua. The system
    // starts activities, so no exec: Restarter, in a process of its own, ends this one and starts the activity anew
    // (a test's environment, the "env" extra, kept)
    public void restart() {
        Intent again = new Intent(this, WindSongActivity.class);
        String env = getIntent() != null ? getIntent().getStringExtra("env") : null;
        if (env != null)
            again.putExtra("env", env);
        Intent r = new Intent(this, Restarter.class);
        r.putExtra(Restarter.PID, Process.myPid());
        r.putExtra(Restarter.AGAIN, again);
        r.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(r);
    }
}
