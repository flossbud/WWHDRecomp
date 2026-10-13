// The app's activity (m-android A2): SDL's, with SDL linked into libmain.so (one library), and the settings play.sh
// gives the desktop build as environment variables. The app's files are its external files directory
// (Android/data/org.windsong.app/files): game/wwhd.wua (the player's copy, adb-pushed until Setup has the system
// picker: t-startscreen S3), cemu/ (Cemu's resources), portable/ (saves, settings, caches).
package org.windsong.app;

import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import java.io.File;
import org.libsdl.app.SDLActivity;

public class WindSongActivity extends SDLActivity {
    private File files() {
        File f = getExternalFilesDir(null);
        return f != null ? f : getFilesDir();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        String dir = files().getAbsolutePath();
        try {
            Os.setenv("WWHD_HOME", dir, true);              // the frontend's working directory (portable/ there)
            Os.setenv("WWHD_NATIVE", "on", false);
            Os.setenv("WWHD_RENDER", "vk", false);
            Os.setenv("WWHD_WINDOW", "1", false);
            Os.setenv("WWHD_CEMU_DATA", dir + "/cemu", false);
            Os.setenv("CEMU_NO_GAMEPAD", "1", false);
        } catch (ErrnoException e) {
            throw new RuntimeException(e);
        }
        super.onCreate(savedInstanceState);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    @Override
    protected String[] getArguments() {
        return new String[] { "-g", files().getAbsolutePath() + "/game/wwhd.wua" };
    }
}
