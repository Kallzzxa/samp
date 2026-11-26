package com.sampmobile.game.ui.cef;

/**
 * Offline assertions for the pure logic of the CEF input path (no phone, no game, no gradle needed).
 *
 * Run with: bash tests/cef/logic_tests/run_logic_tests.sh
 *
 * Why it exists: logd on this ROM keeps restarting, so on-device the only proof that a given
 * path was taken is an "uplink packet" — which cannot prove every branch is right. All expected values below are **independently hardcoded** (they do not reference
 * the tables in CefKeys), so they can catch typos on the product side; and the CefLogic checks are written
 * directly against the semantics of the PC source.
 *
 * The KeyEvent.KEYCODE_* / META_* values in the assertions are all static final constants, so javac inlines them;
 * therefore the runtime classpath **does not contain android.jar** — if someone later imports an Android runtime type into CefKeys/CefLogic,
 * this test will fail on the spot with NoClassDefFoundError, acting as the guard for
 * keeping "pure logic" uncontaminated.
 */
public final class LogicTests {

    private static int passed = 0;
    private static int failed = 0;

    public static void main(String[] args) {
        section("CefKeys: A..Z -> VK / Set-1");
        letters();

        section("CefKeys: digits and punctuation");
        digitsAndPunctuation();

        section("CefKeys: control / navigation / function keys");
        controlsAndFunctionKeys();

        section("CefKeys: table invariants");
        invariants();

        section("CefKeys: unknown keys are never faked");
        unknowns();

        section("CefLogic.shouldSendKey (PressKey gate)");
        pressKeyGate();

        section("CefLogic.imeInsetOf (soft keyboard height)");
        imeInset();

        section("CefLogic.shouldBlockChat (ShouldBlockChat)");
        blockChat();

        System.out.println();
        System.out.println(passed + " passed, " + failed + " failed");
        if (failed == 0) {
            System.out.println("LOGIC TESTS OK");
        }
        System.exit(failed == 0 ? 0 : 1);
    }

    // ---------------------------------------------------------------- CefKeys

    /**
     * Set-1 scan codes, written out independently in alphabetical order (PC/AT keyboard spec:
     * Q=0x10 W=0x11 E=0x12 R=0x13 T=0x14 Y=0x15 U=0x16 I=0x17 O=0x18 P=0x19
     * A=0x1E S=0x1F D=0x20 F=0x21 G=0x22 H=0x23 J=0x24 K=0x25 L=0x26
     * Z=0x2C X=0x2D C=0x2E V=0x2F B=0x30 N=0x31 M=0x32).
     */
    private static final int[] EXPECT_SCAN = {
            0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
            0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C
    };

    private static void letters() {
        for (int i = 0; i < 26; i++) {
            final int keyCode = android.view.KeyEvent.KEYCODE_A + i;   // 29..54
            final char letter = (char) ('A' + i);
            eq("KEYCODE_" + letter + " -> VK 0x" + hex(0x41 + i), 0x41 + i, CefKeys.vkOf(keyCode));
            eq("KEYCODE_" + letter + " -> scan 0x" + hex(EXPECT_SCAN[i]), EXPECT_SCAN[i],
                    CefKeys.scanOf(keyCode));
        }
        // A concrete example of what "the server will see": input keyevent 29 on the device lands as VK 0x41.
        eq("injected KEYCODE_A(29) becomes Windows VK 0x41", 0x41,
                CefKeys.vkOf(android.view.KeyEvent.KEYCODE_A));
    }

    private static void digitsAndPunctuation() {
        final int[] digitScan = {0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};
        for (int i = 0; i < 10; i++) {
            final int keyCode = android.view.KeyEvent.KEYCODE_0 + i;
            eq("KEYCODE_" + i + " -> VK 0x" + hex(0x30 + i), 0x30 + i, CefKeys.vkOf(keyCode));
            eq("KEYCODE_" + i + " -> scan 0x" + hex(digitScan[i]), digitScan[i],
                    CefKeys.scanOf(keyCode));
        }
        // Punctuation VKs use US-layout OEM codes; scan codes are Set-1 physical positions.
        eq("GRAVE  -> VK_OEM_3 0xC0 / scan 0x29", 0xC0, CefKeys.vkOf(android.view.KeyEvent.KEYCODE_GRAVE));
        eq("GRAVE  -> scan 0x29", 0x29, CefKeys.scanOf(android.view.KeyEvent.KEYCODE_GRAVE));
        eq("MINUS  -> VK_OEM_MINUS 0xBD", 0xBD, CefKeys.vkOf(android.view.KeyEvent.KEYCODE_MINUS));
        eq("EQUALS -> VK_OEM_PLUS 0xBB", 0xBB, CefKeys.vkOf(android.view.KeyEvent.KEYCODE_EQUALS));
        eq("COMMA  -> VK_OEM_COMMA 0xBC / scan 0x33", 0xBC,
                CefKeys.vkOf(android.view.KeyEvent.KEYCODE_COMMA));
        eq("PERIOD -> scan 0x34", 0x34, CefKeys.scanOf(android.view.KeyEvent.KEYCODE_PERIOD));
        eq("SLASH  -> VK_OEM_2 0xBF / scan 0x35", 0xBF,
                CefKeys.vkOf(android.view.KeyEvent.KEYCODE_SLASH));
    }

    private static void controlsAndFunctionKeys() {
        eq("ENTER -> VK_RETURN 0x0D", 0x0D, CefKeys.vkOf(android.view.KeyEvent.KEYCODE_ENTER));
        eq("ENTER -> scan 0x1C", 0x1C, CefKeys.scanOf(android.view.KeyEvent.KEYCODE_ENTER));
        eq("TAB   -> VK_TAB 0x09 / scan 0x0F", 0x09, CefKeys.vkOf(android.view.KeyEvent.KEYCODE_TAB));
        eq("TAB   -> scan 0x0F", 0x0F, CefKeys.scanOf(android.view.KeyEvent.KEYCODE_TAB));
        eq("DEL   -> VK_BACK 0x08 / scan 0x0E (backspace)", 0x08,
                CefKeys.vkOf(android.view.KeyEvent.KEYCODE_DEL));
        eq("DEL   -> scan 0x0E", 0x0E, CefKeys.scanOf(android.view.KeyEvent.KEYCODE_DEL));
        eq("ESCAPE-> VK_ESCAPE 0x1B / scan 0x01", 0x1B,
                CefKeys.vkOf(android.view.KeyEvent.KEYCODE_ESCAPE));
        eq("SPACE -> VK_SPACE 0x20 / scan 0x39", 0x20,
                CefKeys.vkOf(android.view.KeyEvent.KEYCODE_SPACE));

        eq("SHIFT_LEFT  -> VK_SHIFT 0x10 / scan 0x2A", 0x10,
                CefKeys.vkOf(android.view.KeyEvent.KEYCODE_SHIFT_LEFT));
        eq("SHIFT_RIGHT -> VK_SHIFT 0x10 / scan 0x36", 0x36,
                CefKeys.scanOf(android.view.KeyEvent.KEYCODE_SHIFT_RIGHT));
        eq("CTRL_LEFT   -> VK_CONTROL 0x11 / scan 0x1D", 0x11,
                CefKeys.vkOf(android.view.KeyEvent.KEYCODE_CTRL_LEFT));
        eq("ALT_LEFT    -> VK_MENU 0x12 / scan 0x38", 0x12,
                CefKeys.vkOf(android.view.KeyEvent.KEYCODE_ALT_LEFT));

        eq("DPAD_UP    -> VK_UP 0x26 / scan 0x48", 0x26,
                CefKeys.vkOf(android.view.KeyEvent.KEYCODE_DPAD_UP));
        eq("DPAD_UP    -> scan 0x48", 0x48, CefKeys.scanOf(android.view.KeyEvent.KEYCODE_DPAD_UP));
        eq("DPAD_DOWN  -> VK_DOWN 0x28 / scan 0x50", 0x50,
                CefKeys.scanOf(android.view.KeyEvent.KEYCODE_DPAD_DOWN));
        eq("DPAD_LEFT  -> VK_LEFT 0x25 / scan 0x4B", 0x4B,
                CefKeys.scanOf(android.view.KeyEvent.KEYCODE_DPAD_LEFT));
        eq("DPAD_RIGHT -> VK_RIGHT 0x27 / scan 0x4D", 0x4D,
                CefKeys.scanOf(android.view.KeyEvent.KEYCODE_DPAD_RIGHT));

        eq("F1  -> VK_F1 0x70 / scan 0x3B", 0x70, CefKeys.vkOf(android.view.KeyEvent.KEYCODE_F1));
        eq("F1  -> scan 0x3B", 0x3B, CefKeys.scanOf(android.view.KeyEvent.KEYCODE_F1));
        eq("F10 -> VK_F10 0x79 / scan 0x44", 0x44,
                CefKeys.scanOf(android.view.KeyEvent.KEYCODE_F10));
        eq("F11 -> scan 0x57 (Set-1 F11 is not contiguous)", 0x57,
                CefKeys.scanOf(android.view.KeyEvent.KEYCODE_F11));
        eq("F12 -> VK_F12 0x7B / scan 0x58", 0x58,
                CefKeys.scanOf(android.view.KeyEvent.KEYCODE_F12));

        eq("NUMPAD_0 -> VK_NUMPAD0 0x60 / scan 0x52", 0x52,
                CefKeys.scanOf(android.view.KeyEvent.KEYCODE_NUMPAD_0));
        eq("NUMPAD_5 -> VK_NUMPAD5 0x65 / scan 0x4C", 0x4C,
                CefKeys.scanOf(android.view.KeyEvent.KEYCODE_NUMPAD_5));
        eq("NUMPAD_ENTER -> VK_RETURN 0x0D", 0x0D,
                CefKeys.vkOf(android.view.KeyEvent.KEYCODE_NUMPAD_ENTER));
    }

    private static void invariants() {
        boolean vkWithoutScan = false;
        boolean scanWithoutVk = false;
        boolean vkOutOfByte = false;
        String firstBad = "";
        for (int keyCode = 0; keyCode < 300; keyCode++) {
            final int vk = CefKeys.vkOf(keyCode);
            final int scan = CefKeys.scanOf(keyCode);
            if (vk != 0 && scan == 0) {
                vkWithoutScan = true;
                if (firstBad.isEmpty()) firstBad = "keyCode " + keyCode + " vk=" + vk;
            }
            if (vk == 0 && scan != 0) {
                scanWithoutVk = true;
                if (firstBad.isEmpty()) firstBad = "keyCode " + keyCode + " scan=" + scan;
            }
            // The server-side EnableKey whitelist has 256 entries; a VK above 255 can never be sent.
            if (vk > 255) vkOutOfByte = true;
        }
        check("every mapped key has both VK and scan", !vkWithoutScan, firstBad);
        check("no scan without VK", !scanWithoutVk, firstBad);
        check("all VKs fit the 256-entry EnableKey whitelist", !vkOutOfByte, null);

        // At least 90 keys must be covered: fewer means some put loop was deleted.
        int mapped = 0;
        for (int k = 0; k < 300; k++) if (CefKeys.vkOf(k) != 0) mapped++;
        check("table covers at least 90 keys (got " + mapped + ")", mapped >= 90, null);
    }

    private static void unknowns() {
        // These are Android-only keys with no Windows VK equivalent, so they must return 0 rather than
        // picking a "close enough" code — the server would treat it as a real key press.
        eq("KEYCODE_BACK  -> 0 (no VK equivalent, never faked)", 0,
                CefKeys.vkOf(android.view.KeyEvent.KEYCODE_BACK));
        eq("KEYCODE_HOME  -> 0 (only MOVE_HOME is mapped)", 0,
                CefKeys.vkOf(android.view.KeyEvent.KEYCODE_HOME));
        eq("KEYCODE_MENU  -> 0", 0, CefKeys.vkOf(android.view.KeyEvent.KEYCODE_MENU));
        eq("KEYCODE_VOLUME_UP -> 0", 0,
                CefKeys.vkOf(android.view.KeyEvent.KEYCODE_VOLUME_UP));
        eq("negative keyCode -> 0", 0, CefKeys.vkOf(-1));
        eq("keyCode past table -> 0", 0, CefKeys.vkOf(299));
        eq("keyCode far out of range -> 0", 0, CefKeys.vkOf(9999));
    }

    // --------------------------------------------------------------- CefLogic

    private static boolean[] whitelist(int... vks) {
        boolean[] allowed = new boolean[256];
        for (int vk : vks) allowed[vk] = true;
        return allowed;
    }

    private static void pressKeyGate() {
        final boolean[] allowed = whitelist(0x41, 0x0D);   // A, ENTER

        check("capture disabled -> nothing sent, even for whitelisted VK",
                !CefLogic.shouldSendKey(false, allowed, 0x41, true, false), null);
        check("capture enabled + whitelisted + keydown -> sent",
                CefLogic.shouldSendKey(true, allowed, 0x41, true, false), null);
        check("capture enabled + whitelisted + keyup -> sent",
                CefLogic.shouldSendKey(true, allowed, 0x41, false, false), null);
        check("VK not in EnableKey whitelist -> not sent (0x5A = Z)",
                !CefLogic.shouldSendKey(true, allowed, 0x5A, true, false), null);
        check("keydown auto-repeat -> dropped (PC: if (!(down && repeat)))",
                !CefLogic.shouldSendKey(true, allowed, 0x41, true, true), null);
        // Key-up must still be sent even with repeat set, otherwise the server sees "pressed, never released" and the page never gets keyup.
        check("keyup carrying repeat -> still sent",
                CefLogic.shouldSendKey(true, allowed, 0x41, false, true), null);
        check("vk 0 (unmapped Android key) -> not sent",
                !CefLogic.shouldSendKey(true, whitelist(0), 0, true, false), null);
        check("vk 256 (beyond whitelist array) -> not sent",
                !CefLogic.shouldSendKey(true, allowed, 256, true, false), null);
        check("negative vk -> not sent",
                !CefLogic.shouldSendKey(true, allowed, -5, true, false), null);
        check("null whitelist -> not sent",
                !CefLogic.shouldSendKey(true, null, 0x41, true, false), null);
        check("empty whitelist -> not sent",
                !CefLogic.shouldSendKey(true, new boolean[256], 0x41, true, false), null);

        // The on-device script has sent ENTER (0x0D) and A (0x41); the server received both.
        check("ENTER stays deliverable while capture is on",
                CefLogic.shouldSendKey(true, allowed, 0x0D, true, false), null);
    }

    private static void imeInset() {
        // Measured on a real device (V2312A, 1080 tall): with the keyboard up, the visible area bottom = 372.
        eq("1080px window, visible bottom 372 -> 708px keyboard", 708,
                CefLogic.imeInsetOf(1080, 372));
        eq("keyboard hidden (visible bottom == window height) -> 0", 0,
                CefLogic.imeInsetOf(1080, 1080));
        eq("48px navigation bar is not a keyboard -> 0", 0,
                CefLogic.imeInsetOf(1080, 1032));
        // The threshold sits exactly on the 1/4 boundary: 270 is not less than 270, so it counts as a keyboard.
        eq("exactly 1/4 of the window is treated as keyboard", 270,
                CefLogic.imeInsetOf(1080, 810));
        eq("one pixel below 1/4 is treated as chrome, not keyboard -> 0", 0,
                CefLogic.imeInsetOf(1080, 811));
        eq("visible bottom past the window bottom clamps to 0", 0,
                CefLogic.imeInsetOf(1080, 1200));
        eq("zero-height window -> 0 (avoids divide-by-four noise)", 0,
                CefLogic.imeInsetOf(0, 0));
        eq("negative height -> 0", 0, CefLogic.imeInsetOf(-100, -50));
        eq("small window keeps proportional behaviour", 40,
                CefLogic.imeInsetOf(100, 60));

        // ---- imeMarginOf: when adjustResize has already made room, do not reduce bottomMargin again ----
        // Keyboard up, window not resized (adjustPan): give way yourself
        eq("pan mode, keyboard 708px -> margin 708", 708,
                CefLogic.imeMarginOf(1080, 372, 1080));
        // Keyboard up, window already resized to 372: the system has already made room, so margin must be 0
        eq("resize mode, window already shrunk to 372 -> margin 0 (no double give-back)", 0,
                CefLogic.imeMarginOf(372, 372, 1080));
        // Exact reproduction of the feedback loop: window 372 tall, visible bottom also 372
        eq("feedback loop guard: decor 372 == visible 372 -> 0", 0,
                CefLogic.imeMarginOf(372, 372, 1080));
        // Keyboard dismissed and window back to fullscreen -> 0
        eq("keyboard hidden, full-height window -> 0", 0,
                CefLogic.imeMarginOf(1080, 1080, 1080));
        // When it cannot be measured, use 0 and wait for the next layout; do not subtract blindly
        eq("zero decor height -> 0 (wait for next layout)", 0,
                CefLogic.imeMarginOf(0, 372, 1080));
        eq("unknown screen height -> 0 (fail safe)", 0,
                CefLogic.imeMarginOf(1080, 372, 0));
    }

    private static void blockChat() {
        check("server disabled chat input -> blocked regardless of the page",
                CefLogic.shouldBlockChat(false, -1, false), null);
        check("no page input focused -> game chat stays available",
                !CefLogic.shouldBlockChat(true, -1, true), null);
        check("page owns chat input -> blocked (ShouldBlockChat)",
                CefLogic.shouldBlockChat(true, 1, true), null);
        check("page with controls_chat_input=false -> game chat stays available",
                !CefLogic.shouldBlockChat(true, 1, false), null);
        check("server disabled chat input still blocks a controls_chat_input=false page",
                CefLogic.shouldBlockChat(false, 1, false), null);
    }

    // ---------------------------------------------------------------- helpers

    private static void section(String title) {
        System.out.println();
        System.out.println("== " + title);
    }

    private static void eq(String what, long want, long got) {
        check(what, want == got, "want=" + want + " got=" + got);
    }

    private static void check(String what, boolean ok, String detail) {
        if (ok) {
            passed++;
            System.out.println("  PASS  " + what);
        } else {
            failed++;
            System.out.println("  FAIL  " + what + (detail == null ? "" : "   [" + detail + "]"));
        }
    }

    private static String hex(int v) {
        return Integer.toHexString(v).toUpperCase();
    }

    private LogicTests() {
    }
}
