package io.github.hwtrrecomp;

import org.libsdl.app.SDLActivity;

/** Runs the native game (libmain.so, built from the root CMake project). */
public class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "c++_shared", "main" };
    }
}
