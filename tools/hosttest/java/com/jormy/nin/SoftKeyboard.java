package com.jormy.nin;

// Records the text operations the native keyboard asks for, so the harness can assert on them.
public class SoftKeyboard {
    public static final StringBuilder ops = new StringBuilder();

    static void op(String s) {
        Utils.log("textop " + s);
        ops.append(s).append('\n');
    }

    @Api
    public static void callMUCommand(String command, String arg1, String arg2, String arg3) {
        op("MU " + command + " [" + arg1 + "] [" + arg2 + "] [" + arg3 + "]");
    }

    @Api
    public static void callRequestSel() {
        op("RequestSel");
    }

    @Api
    public static void callSetSel(int start, int end, boolean fromStart, boolean dontSignal) {
        op("SetSel " + start + " " + end + " " + fromStart + " " + dontSignal);
    }

    @Api
    public static void callDragCursorUp(int releasedDirection) {
        op("DragCursorUp " + releasedDirection);
    }

    @Api
    public static void callDragCursorMove(int xMovement, int yMovement, boolean selectionMode) {
        op("DragCursorMove " + xMovement + " " + yMovement + " " + selectionMode);
    }

    @Api
    public static void callSimpleBackspace(boolean singleCharacterMode) {
        op("SimpleBackspace " + singleCharacterMode);
    }

    @Api
    public static void callBackReplacement(int backIndexFromCursorBytes, String oldString, String newString) {
        op("BackReplacement " + backIndexFromCursorBytes + " [" + oldString + "] [" + newString + "]");
    }

    @Api
    public static void callBackspaceModed(String string) {
        op("BackspaceModed [" + string + "]");
    }

    @Api
    public static void callMarkLiquid(String string) {
        op("MarkLiquid [" + string + "]");
    }

    @Api
    public static void callSolidify(String string) {
        op("Solidify [" + string + "]");
    }
}
