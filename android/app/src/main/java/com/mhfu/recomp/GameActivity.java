package com.mhfu.recomp;

import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.system.ErrnoException;
import android.system.Os;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

// The game: the launcher (recomp/src/launcher.cpp, SDL's main) in SDL's activity. The ISO the
// player chose is handed to it as an open file, /proc/self/fd/N: Android doesn't let the app open
// it by path. Its files, settings and saves are kept in the app's files.
public class GameActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle saved) {
        try {
            Os.setenv("MHFU_DATA_DIR", getFilesDir().getAbsolutePath(), true);
            Os.setenv("MHFU_RESOURCES", new File(getFilesDir(), "Resources").getAbsolutePath(), true);
        } catch (ErrnoException e) {
            // The launcher then uses its defaults.
        }
        super.onCreate(saved);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override
    protected String[] getArguments() {
        Uri iso = getIntent().getData();
        if (iso != null) {
            try {
                ParcelFileDescriptor file = getContentResolver().openFileDescriptor(iso, "r");
                if (file != null) return new String[] {"/proc/self/fd/" + file.detachFd()};
            } catch (IOException | SecurityException e) {
                // Moved, deleted, or the permission was taken back: said below.
            }
        }
        try (FileOutputStream problem = new FileOutputStream(new File(getFilesDir(), "launch-problem.txt"))) {
            problem.write(getString(R.string.cannot_open).getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            // SetupActivity then just closes.
        }
        return new String[0];
    }

    // Closed by the game itself (it quit, or couldn't start): not a crash, for SetupActivity.
    @Override
    public void finish() {
        setResult(RESULT_OK);
        super.finish();
    }

    // The game's process ends with it, so that the next start begins afresh.
    @Override
    protected void onDestroy() {
        super.onDestroy();
        System.exit(0);
    }
}
