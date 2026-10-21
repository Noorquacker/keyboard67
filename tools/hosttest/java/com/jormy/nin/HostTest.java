package com.jormy.nin;

// Drives the native keyboard the way NINView.Renderer does, against an offscreen GLES context.
public class HostTest {
    static int dirty;

    static native boolean glInit(int w, int h);

    static native void glSave(String path, int w, int h);

    static void frames(int n, long[] clock) {
        for (int i = 0; i < n; i++) {
            NINLib.step();
            clock[0] += 16;
        }
    }

    static void tap(float x, float y, long[] clock) {
        NINLib.onTouchEvent(0, 0, x, y, 1f, 0.1f, clock[0]);
        frames(3, clock);
        NINLib.onTouchEvent(0, 2, x, y, 1f, 0.1f, clock[0] + 40);
        frames(6, clock);
    }

    public static void main(String[] args) throws Exception {
        int w = Integer.parseInt(System.getProperty("nin.w", "1080"));
        int h = Integer.parseInt(System.getProperty("nin.h", "760"));
        String out = System.getProperty("nin.out", ".");
        System.loadLibrary("hostgl");
        if (!glInit(w, h)) throw new RuntimeException("no GL context");

        long t = System.currentTimeMillis();
        long t0 = System.nanoTime();
        NINLib.syncTiming(t);  // first call loads libgl2jni (runtime + static constructors)
        System.out.printf("[host] load + syncTiming: %.1f ms%n", (System.nanoTime() - t0) / 1e6);

        t0 = System.nanoTime();
        NINLib.init(w, h, 1080, 2340);
        System.out.printf("[host] init: %.1f ms%n", (System.nanoTime() - t0) / 1e6);

        NINLib.onChangeAppOrTextbox("com.example.notes", "body", "text");
        NINLib.onTextSelection("Hello ", "", "", "");

        long[] clock = {System.currentTimeMillis()};
        t0 = System.nanoTime();
        frames(60, clock);
        double perFrame = (System.nanoTime() - t0) / 1e6 / 60;
        System.out.printf("[host] 60 warm-up frames, %.2f ms/frame%n", perFrame);
        glSave(out + "/frame0.ppm", w, h);

        // Tap a grid of points over the keyboard area.
        int n = 0;
        for (float fy = 0.35f; fy < 0.95f; fy += 0.15f) {
            for (float fx = 0.08f; fx < 1f; fx += 0.21f) {
                tap(fx * w, fy * h, clock);
                n++;
            }
        }
        glSave(out + "/frame1.ppm", w, h);

        // Key centres (fractions of the surface) measured from frame0 for the qwerty layout.
        java.util.Map<Character, float[]> keys = new java.util.HashMap<>();
        String[] rows = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
        float[][] rowGeom = {{0.050f, 0.0985f, 0.50f}, {0.089f, 0.0995f, 0.64f}, {0.198f, 0.0995f, 0.785f}};
        for (int r = 0; r < 3; r++)
            for (int i = 0; i < rows[r].length(); i++)
                keys.put(rows[r].charAt(i), new float[] {rowGeom[r][0] + i * rowGeom[r][1], rowGeom[r][2]});
        for (char ch : "howdy".toCharArray()) tap(keys.get(ch)[0] * w, keys.get(ch)[1] * h, clock);
        frames(30, clock);
        glSave(out + "/frame3.ppm", w, h);
        tap(0.05f * w, 0.345f * h, clock);  // close the tutorial
        frames(30, clock);
        for (char ch : "hello".toCharArray()) tap(keys.get(ch)[0] * w, keys.get(ch)[1] * h, clock);
        tap(0.5f * w, 0.93f * h, clock);  // space bar
        for (char ch : "world".toCharArray()) tap(keys.get(ch)[0] * w, keys.get(ch)[1] * h, clock);
        frames(30, clock);
        glSave(out + "/frame4.ppm", w, h);

        t0 = System.nanoTime();
        frames(120, clock);
        perFrame = (System.nanoTime() - t0) / 1e6 / 120;
        System.out.printf("[host] steady state %.2f ms/frame%n", perFrame);
        glSave(out + "/frame2.ppm", w, h);

        System.out.println("[host] backIndex test: " + NINLib.getUnicodeBackIndex("héllo", 3) + " "
                + NINLib.getUnicodeFrontIndex("héllo", 1));
        System.out.println("[host] taps=" + n + " dirty=" + dirty + "\n[host] text ops:\n" + SoftKeyboard.ops);
    }
}
