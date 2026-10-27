package io.github.hwtrrecomp;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Asks for the ROM once via the system file picker and copies it to the app's private files
 * directory as import.z64. The native launcher (src/main/launcher.cpp) validates it and moves
 * it into place; after that the game starts directly.
 */
public class LauncherActivity extends Activity {
    private static final int PICK_ROM = 1;
    // Must match recomp::GameEntry::stored_filename() for game id "hwtr.n64.us".
    private static final String STORED_ROM = "hwtr.n64.us.z64";
    private static final String IMPORT_ROM = "import.z64";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (new File(getFilesDir(), STORED_ROM).exists()) {
            startGame();
            return;
        }
        Toast.makeText(this, "Select your Hot Wheels Turbo Racing (USA) ROM", Toast.LENGTH_LONG).show();
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, PICK_ROM);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_ROM || resultCode != RESULT_OK || data == null || data.getData() == null) {
            finish();
            return;
        }
        Uri uri = data.getData();
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(new File(getFilesDir(), IMPORT_ROM))) {
            byte[] buffer = new byte[1 << 16];
            int n;
            while ((n = in.read(buffer)) > 0) {
                out.write(buffer, 0, n);
            }
        } catch (Exception e) {
            Toast.makeText(this, "Could not read the ROM: " + e.getMessage(), Toast.LENGTH_LONG).show();
            finish();
            return;
        }
        startGame();
    }

    private void startGame() {
        startActivity(new Intent(this, GameActivity.class));
        finish();
    }
}
