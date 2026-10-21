package com.jormy.nin;

@Api
public class NINView {
    @Api
    public static float getDevicePPI() {
        return com.nqind.keyboard67.NINView.Companion.getDevicePPI();

    }
    @Api
    public static float getDevicePortraitWidth() {
        return com.nqind.keyboard67.NINView.Companion.getDevicePortraitWidth();
    }

    @Api
    public static void adjustWantedScaling(float scaling) {
        com.nqind.keyboard67.NINView.Companion.adjustWantedScaling(scaling);
    }

    @Api
    public static void onRoenSignalDirty() {
        com.nqind.keyboard67.NINView.Companion.onRoenSignalDirty();
    }

    @Api
    public static void onRoenFrozennessChange(boolean truth) {
        com.nqind.keyboard67.NINView.Companion.onRoenFrozennessChange(truth);
    }

    @Api
    public static void adjustKeyboardDimensions(float wantedRoenHeight, boolean fullscreen) {
        com.nqind.keyboard67.NINView.Companion.adjustKeyboardDimensions(wantedRoenHeight, fullscreen);
    }
}
