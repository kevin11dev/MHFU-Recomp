package com.mhfu.recomp;

import android.app.Activity;
import android.app.AlertDialog;
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
//
// Holding the app's icon offers two shortcuts that come here too: Export save writes the game's
// save (MHP2NDG.BIN) to a file the player chooses, and Import save replaces it with one they
// choose, to move a hunter between devices or keep a copy.
public class SetupActivity extends Activity {
    private static final int CHOOSE_ISO = 1;
    private static final int GAME = 2;
    private static final int IMPORT_SAVE = 3;
    private static final int EXPORT_SAVE = 4;
    private static final String ACTION_IMPORT_SAVE = "com.mhfu.recomp.IMPORT_SAVE";
    private static final String ACTION_EXPORT_SAVE = "com.mhfu.recomp.EXPORT_SAVE";
    private static final int SAVE_SIZE = 0x16a100;  // what the game writes, always

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
        String action = getIntent().getAction();
        if (ACTION_IMPORT_SAVE.equals(action)) chooseSave();
        else if (ACTION_EXPORT_SAVE.equals(action)) exportSave();
        else start();
    }

    private void start() {
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
        } else if (request == IMPORT_SAVE) {
            if (result == RESULT_OK && data != null && data.getData() != null) confirmImport(data.getData());
            else finish();
        } else if (request == EXPORT_SAVE) {
            if (result == RESULT_OK && data != null && data.getData() != null) writeSave(data.getData());
            else finish();
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
        again.setOnClickListener(view -> start());
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
        again.setText(R.string.start_again);
        again.setVisibility(View.VISIBLE);
        choose.setVisibility(View.VISIBLE);
        again.requestFocus();
    }

    private File saveFile() {
        return new File(getFilesDir(), "ms0/PSP/SAVEDATA/ULES01213/MHP2NDG.BIN");
    }

    private void chooseSave() {
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        startActivityForResult(pick, IMPORT_SAVE);
    }

    private void confirmImport(Uri source) {
        byte[] save = new byte[SAVE_SIZE];
        int length = 0;
        try (InputStream in = getContentResolver().openInputStream(source)) {
            for (int read; length < SAVE_SIZE && (read = in.read(save, length, SAVE_SIZE - length)) > 0;) length += read;
            if (length == SAVE_SIZE && in.read() != -1) length = -1;
        } catch (IOException | NullPointerException e) {
            length = -1;
        }
        if (length != SAVE_SIZE) {
            showSaveResult(getString(R.string.not_a_save));
            return;
        }
        if (!saveFile().exists()) {
            importSave(save);
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle(R.string.replace_save_title)
                .setMessage(R.string.replace_save)
                .setPositiveButton(R.string.replace, (dialog, which) -> importSave(save))
                .setNegativeButton(android.R.string.cancel, (dialog, which) -> finish())
                .setOnCancelListener(dialog -> finish())
                .show();
    }

    private void importSave(byte[] save) {
        File file = saveFile();
        file.getParentFile().mkdirs();
        try (OutputStream out = new FileOutputStream(file)) {
            out.write(save);
            showSaveResult(getString(R.string.save_imported));
        } catch (IOException e) {
            showSaveResult(e.toString());
        }
    }

    private void exportSave() {
        if (!saveFile().exists()) {
            showSaveResult(getString(R.string.no_save));
            return;
        }
        Intent create = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        create.addCategory(Intent.CATEGORY_OPENABLE);
        create.setType("application/octet-stream");
        create.putExtra(Intent.EXTRA_TITLE, "MHP2NDG.BIN");
        startActivityForResult(create, EXPORT_SAVE);
    }

    private void writeSave(Uri target) {
        try (OutputStream out = getContentResolver().openOutputStream(target, "wt")) {
            out.write(Files.readAllBytes(saveFile().toPath()));
            showSaveResult(getString(R.string.save_exported));
        } catch (IOException | NullPointerException e) {
            showSaveResult(e.toString());
        }
    }

    private void showSaveResult(String text) {
        message.setText(text);
        message.setTextSize(17);
        again.setText(R.string.start);
        again.setVisibility(View.VISIBLE);
        choose.setVisibility(View.GONE);
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
