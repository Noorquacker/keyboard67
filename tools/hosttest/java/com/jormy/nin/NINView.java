package com.jormy.nin;

@Api
public class NINView {
    @Api
    public static float getDevicePPI() {
        return 400f;
    }

    @Api
    public static float getDevicePortraitWidth() {
        return 1080f;
    }

    @Api
    public static void adjustWantedScaling(float scaling) {
        Utils.log("adjustWantedScaling " + scaling);
    }

    @Api
    public static void onRoenSignalDirty() {
        HostTest.dirty++;
    }

    @Api
    public static void onRoenFrozennessChange(boolean truth) {
        Utils.log("onRoenFrozennessChange " + truth);
    }

    @Api
    public static void adjustKeyboardDimensions(float wantedRoenHeight, boolean fullscreen) {
        Utils.log("adjustKeyboardDimensions " + wantedRoenHeight + " " + fullscreen);
    }
}
