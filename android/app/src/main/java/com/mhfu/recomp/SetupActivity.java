package com.mhfu.recomp;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.UriPermission;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

// The app's first screen. It unpacks the launcher's fonts and logo, has the player choose their
// ISO (once: the choice is kept), and starts the game (GameActivity, in a process of its own).
// When something stopped the game starting (the launcher writes launch-problem.txt: the wrong
// ISO, say), it says why and offers to choose another; when the game stopped unexpectedly, it
// shows the end of the game's log (log.txt) and offers to start again.
public class SetupActivity extends Activity {
    private static final int CHOOSE_ISO = 1;
    private static final int GAME = 2;

    private SharedPreferences preferences;
    private TextView message;
    private Button again;
    private Button choose;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        preferences = getSharedPreferences("setup", MODE_PRIVATE);
        unpackResources();
        buildScreen();
        if (saved != null) return;
        String iso = preferences.getString("iso", null);
        if (iso != null && canRead(Uri.parse(iso))) startGame(Uri.parse(iso));
        else showChoice(getString(iso == null ? R.string.first_start : R.string.cannot_open));
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == CHOOSE_ISO) {
            if (result != RESULT_OK || data == null || data.getData() == null) return;
            Uri iso = data.getData();
            getContentResolver().takePersistableUriPermission(iso, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            preferences.edit().putString("iso", iso.toString()).apply();
            startGame(iso);
        } else if (request == GAME) {
            File problem = new File(getFilesDir(), "launch-problem.txt");
            if (!problem.exists()) {
                if (result == RESULT_OK) finish();
                else showCrash();
                return;
            }
            String text;
            try {
                text = new String(Files.readAllBytes(problem.toPath()), StandardCharsets.UTF_8).trim();
            } catch (IOException e) {
                text = e.toString();
            }
            problem.delete();
            preferences.edit().remove("iso").apply();
            showChoice(text);
        }
    }

    // A controller's A button presses the button chosen, as a tap would.
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        View focused = getCurrentFocus();
        if (event.getKeyCode() == KeyEvent.KEYCODE_BUTTON_A && focused instanceof Button) {
            if (event.getAction() == KeyEvent.ACTION_UP) focused.performClick();
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    private void buildScreen() {
        LinearLayout screen = new LinearLayout(this);
        screen.setOrientation(LinearLayout.VERTICAL);
        screen.setGravity(Gravity.CENTER);
        screen.setBackgroundColor(Color.rgb(20, 16, 12));
        int padding = dp(32);
        screen.setPadding(padding, padding, padding, padding);

        TextView title = new TextView(this);
        title.setText(R.string.title);
        title.setTextColor(Color.rgb(230, 196, 120));
        title.setTextSize(26);
        title.setGravity(Gravity.CENTER);
        File cinzel = new File(getFilesDir(), "Resources/fonts/Cinzel-Bold.ttf");
        if (cinzel.exists()) title.setTypeface(Typeface.createFromFile(cinzel));
        screen.addView(title);

        message = new TextView(this);
        message.setTextColor(Color.rgb(220, 214, 200));
        message.setTextSize(17);
        message.setGravity(Gravity.CENTER);
        message.setPadding(0, dp(24), 0, dp(24));
        screen.addView(message);

        again = new Button(this);
        again.setText(R.string.start_again);
        again.setOnClickListener(view -> startGame(Uri.parse(preferences.getString("iso", ""))));
        again.setVisibility(View.GONE);
        screen.addView(again, new LinearLayout.LayoutParams(dp(220), LinearLayout.LayoutParams.WRAP_CONTENT));

        choose = new Button(this);
        choose.setText(R.string.choose_iso);
        choose.setOnClickListener(view -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("*/*");
            startActivityForResult(pick, CHOOSE_ISO);
        });
        choose.setVisibility(View.GONE);
        screen.addView(choose, new LinearLayout.LayoutParams(dp(220), LinearLayout.LayoutParams.WRAP_CONTENT));
        setContentView(screen);
    }

    private void showChoice(String text) {
        message.setText(text);
        again.setVisibility(View.GONE);
        choose.setVisibility(View.VISIBLE);
        choose.requestFocus();
    }

    // The game stopped without closing itself: the end of its log says where.
    private void showCrash() {
        StringBuilder text = new StringBuilder(getString(R.string.crashed));
        try {
            java.util.List<String> lines = Files.readAllLines(new File(getFilesDir(), "log.txt").toPath(), StandardCharsets.UTF_8);
            for (String line : lines.subList(Math.max(0, lines.size() - 15), lines.size())) text.append('\n').append(line);
        } catch (IOException e) {
            text.append('\n').append(e);
        }
        message.setText(text);
        message.setTextSize(12);
        again.setVisibility(View.VISIBLE);
        choose.setVisibility(View.VISIBLE);
        again.requestFocus();
    }

    private void startGame(Uri iso) {
        message.setText(R.string.starting);
        message.setTextSize(17);
        again.setVisibility(View.GONE);
        choose.setVisibility(View.GONE);
        Intent game = new Intent(this, GameActivity.class);
        game.setData(iso);
        game.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivityForResult(game, GAME);
    }

    private boolean canRead(Uri iso) {
        for (UriPermission permission : getContentResolver().getPersistedUriPermissions())
            if (permission.getUri().equals(iso) && permission.isReadPermission()) return true;
        return false;
    }

    // The fonts and logo the launcher draws with go to Resources in the app's files, again after
    // each update of the app.
    private void unpackResources() {
        long installed;
        try {
            installed = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
        } catch (PackageManager.NameNotFoundException e) {
            installed = 0;
        }
        if (preferences.getLong("resources", -1) == installed) return;
        try {
            copyAssets("Resources", new File(getFilesDir(), "Resources"));
            preferences.edit().putLong("resources", installed).apply();
        } catch (IOException e) {
            // The launcher draws without them; the next start tries again.
        }
    }

    private void copyAssets(String path, File target) throws IOException {
        String[] children = getAssets().list(path);
        if (children != null && children.length > 0) {
            target.mkdirs();
            for (String child : children) copyAssets(path + "/" + child, new File(target, child));
            return;
        }
        try (InputStream in = getAssets().open(path); OutputStream out = new FileOutputStream(target)) {
            byte[] buffer = new byte[1 << 16];
            for (int read; (read = in.read(buffer)) > 0;) out.write(buffer, 0, read);
        }
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
