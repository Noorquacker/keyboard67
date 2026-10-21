package com.jormy.nin;

import android.content.res.AssetManager;

import java.io.File;

// Desktop stand-in for the Android implementation; same signatures, logs every call.
@Api
public class Utils {
    static void log(String s) {
        System.out.println("[java] " + s);
    }

    @Api
    public static String getIPAddressIPV4() {
        return "127.0.0.1";
    }

    @Api
    public static void openNinPlayStoreLink() {
        log("openNinPlayStoreLink");
    }

    @Api
    public static void testLength(String lestr) {
        log("testLength " + lestr.length());
    }

    @Api
    public static void vibrate(int millisecs) {
        log("vibrate " + millisecs);
    }

    @Api
    public static void tracedims(String desc, float xdim, float ydim) {
        log("tracedims " + desc + " " + xdim + " " + ydim);
    }

    @Api
    public static void prin(String lestr) {
        log("prin: " + lestr);
    }

    @Api
    public static String homeDirectory() {
        String d = System.getProperty("nin.home");
        new File(d).mkdirs();
        return d;
    }

    @Api
    public static String cacheDirectory() {
        String d = System.getProperty("nin.home") + "/cache";
        new File(d).mkdirs();
        return d;
    }

    private static final AssetManager am = new AssetManager();

    @Api
    public static AssetManager assetManager() {
        return am;
    }

    @Api
    public static void initRoenSoundPool() {
        log("initRoenSoundPool");
    }

    @Api
    public static Integer roenObtainSound(String soundname) {
        return 0;
    }

    @Api
    public static void roenPreloadSound(String soundname) {
        log("roenPreloadSound " + soundname);
    }

    @Api
    public static void roenCallPlaySound(String soundname, float pitch, float volume) {
        log("roenCallPlaySound " + soundname + " " + pitch + " " + volume);
    }

    @Api
    public static boolean androidScreenLocked() {
        return false;
    }

    @Api
    public static boolean listAssetFiles(String path) {
        File f = new File(System.getenv("NIN_ASSETS"), path);
        boolean r = f.isDirectory() && f.list().length > 0;
        log("listAssetFiles " + path + " -> " + r);
        return r;
    }

    static String clip = "";

    @Api
    public static void copyToClipboard(String value) {
        log("copyToClipboard " + value);
        clip = value;
    }

    @Api
    public static String getStringFromClipboard() {
        return clip;
    }
}
