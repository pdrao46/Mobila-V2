/*
 * ============================================================================
 *  MOBILADOR - android-server  src/com/mobilador/server/Log.java
 *
 *  Deliberately tiny logging facade.
 *
 *  Everything written here ends up in the PC client's log pane (stderr is piped
 *  through 'adb shell' and read by the Mobilador console reader).  The encoder
 *  callback path must never log per frame, so nothing on the hot path calls
 *  into this class.
 * ============================================================================
 */
package com.mobilador.server;

import android.os.SystemClock;

final class Log {

    private static final boolean VERBOSE = true;
    private static long sStart = SystemClock.elapsedRealtime();

    private Log() {}

    private static void emit(String level, String message) {
        long t = SystemClock.elapsedRealtime() - sStart;
        System.err.println("[" + level + " " + (t / 1000) + "." + (t % 1000) + "] " + message);
        System.err.flush();
    }

    static void info(String message)  { emit("I", message); }
    static void warn(String message)  { emit("W", message); }
    static void error(String message) { emit("E", message); }
    static void debug(String message) { if (VERBOSE) emit("D", message); }
}
