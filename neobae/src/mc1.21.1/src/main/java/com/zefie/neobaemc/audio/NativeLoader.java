package com.zefie.neobaemc.audio;

import com.zefie.neobaemc.NeoBAEMC;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.Locale;

/**
 * Extracts the native NeoBAE shared library out of the mod jar's
 * {@code natives/<os>/<arch>/} folder into a temp directory and calls
 * {@link System#load(String)} on it.
 *
 * <p>The shared library exposes the {@code Java_com_zefie_neobaemc_audio_*}
 * JNI symbols used by {@link Mixer}, {@link Song} and {@link Sound}.
 */
public final class NativeLoader {
    private static volatile boolean loaded = false;
    private static volatile Throwable loadError = null;

    private NativeLoader() {}

    public static synchronized boolean ensureLoaded() {
        if (loaded) return true;
        if (loadError != null) return false;
        try {
            String os = detectOs();
            String arch = detectArch();
            String libName = libraryName(os);
            String resourcePath = "/natives/" + os + "/" + arch + "/" + libName;

            try (InputStream in = NativeLoader.class.getResourceAsStream(resourcePath)) {
                if (in == null) {
                    throw new IOException("Missing native library resource: " + resourcePath
                            + " (run `./build_native.sh` in the mod source tree to build it)");
                }
                Path tmpDir = Files.createTempDirectory("neobaemc-native-");
                tmpDir.toFile().deleteOnExit();
                Path tmpFile = tmpDir.resolve(libName);
                Files.copy(in, tmpFile, StandardCopyOption.REPLACE_EXISTING);
                tmpFile.toFile().deleteOnExit();
                System.load(tmpFile.toAbsolutePath().toString());
            }
            loaded = true;
            NeoBAEMC.LOGGER.info("Loaded NeoBAE native library");
            return true;
        } catch (Throwable t) {
            loadError = t;
            NeoBAEMC.LOGGER.error("Failed to load NeoBAE native library", t);
            return false;
        }
    }

    public static Throwable getLoadError() { return loadError; }

    private static String detectOs() {
        String n = System.getProperty("os.name", "").toLowerCase(Locale.ROOT);
        if (n.contains("win")) return "windows";
        if (n.contains("mac") || n.contains("darwin")) return "macos";
        return "linux";
    }

    private static String detectArch() {
        String a = System.getProperty("os.arch", "").toLowerCase(Locale.ROOT);
        if (a.equals("amd64") || a.equals("x86_64")) return "x86_64";
        if (a.equals("aarch64") || a.equals("arm64")) return "aarch64";
        if (a.equals("x86") || a.equals("i386") || a.equals("i686")) return "x86";
        return a;
    }

    private static String libraryName(String os) {
        return switch (os) {
            case "windows" -> "NeoBAE.dll";
            case "macos"   -> "libNeoBAE.dylib";
            default        -> "libNeoBAE.so";
        };
    }
}
