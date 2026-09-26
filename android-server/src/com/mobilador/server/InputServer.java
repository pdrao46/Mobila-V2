/*
 * ============================================================================
 *  MOBILADOR - android-server  src/com/mobilador/server/InputServer.java
 *
 *  Receives pointer / keyboard events from the PC and injects them into the
 *  phone with the lowest achievable delay.
 *
 *  TWO INJECTION BACKENDS, CHOSEN AUTOMATICALLY
 *  --------------------------------------------
 *  1. KERNEL (uinput) - the events become real HID events in the kernel, so
 *     they are indistinguishable from a physically attached mouse and keyboard
 *     and they cost a single write() with no binder transaction.  Relative
 *     mouse motion stays relative, which is exactly what an FPS camera wants:
 *     the game (or GG Mouse Pro 3) performs the mapping itself.
 *  2. FRAMEWORK - InputManager.injectInputEvent, the same call the 'input'
 *     shell command uses.  Slightly slower (a binder round trip) but available
 *     on any ROM.  Absolute coordinates are tracked here because the framework
 *     has no notion of a relative mouse pointer.
 *
 *  Both backends run on a single dedicated thread that does nothing else, so
 *  an input burst never waits behind video work.
 * ============================================================================
 */
package com.mobilador.server;

import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;

final class InputServer {

    /* ---------------------------------------------------------------- state */
    private ServerSocket mServer;
    private Socket mClient;
    private Thread mThread;
    private volatile boolean mRunning;
    private volatile int mScreenW = 1080, mScreenH = 1920;
    private volatile float mCursorX, mCursorY;
    private volatile boolean mUseRelativeMouse;
    private long mEventsInjected;
    private Injector mInjector;
    private final Object mLock = new Object();

    interface Listener {
        void onInputError(String message);
        void onInjectionBackend(String backend);
    }

    private Listener mListener;

    boolean listen(int port) {
        try {
            mServer = new ServerSocket();
            mServer.setReuseAddress(true);
            mServer.bind(new InetSocketAddress("127.0.0.1", port), 1);
            Log.info("input channel on 127.0.0.1:" + port);
            return true;
        } catch (IOException e) {
            Log.error("input bind failed: " + e);
            return false;
        }
    }

    void setScreenSize(int w, int h) {
        mScreenW = w;
        mScreenH = h;
        mCursorX = w * 0.5f;
        mCursorY = h * 0.5f;
    }

    void start(Listener listener, boolean preferKernel) {
        mListener = listener;
        mInjector = preferKernel ? UinputInjector.tryCreate() : null;
        if (mInjector == null) mInjector = new ManagerInjector();
        if (listener != null) listener.onInjectionBackend(mInjector.name());
        Log.info("input injection backend: " + mInjector.name());
        mRunning = true;
        mThread = new Thread(new Runnable() {
            @Override public void run() { runLoop(); }
        }, "mobilador-input");
        // Time critical: a late input event is worse than a late video frame.
        mThread.setPriority(Thread.MAX_PRIORITY);
        mThread.setDaemon(true);
        mThread.start();
    }

    void stop() {
        mRunning = false;
        synchronized (mLock) {
            try { if (mInjector != null) mInjector.close(); } catch (Throwable ignored) {}
            mInjector = null;
        }
        try { if (mClient != null) mClient.close(); } catch (Throwable ignored) {}
        try { if (mServer != null) mServer.close(); } catch (Throwable ignored) {}
    }

    long eventsInjected() { return mEventsInjected; }

    /* ----------------------------------------------------------- event loop */
    private void runLoop() {
        try {
            mServer.setSoTimeout(8000);
            mClient = mServer.accept();
            mClient.setTcpNoDelay(true);
            mClient.setReceiveBufferSize(64 * 1024);
            InputStream in = mClient.getInputStream();
            byte[] header = new byte[64];
            ByteBuffer hb = ByteBuffer.wrap(header);
            while (mRunning) {
                if (!CaptureSocket.readFully(in, header, CodecMath.HEADER_SIZE)) break;
                long magic = 0;
                for (int i = 0; i < 8; i++) magic = (magic << 8) | (header[i] & 0xFF);
                if (magic != CaptureSocket.MAGIC) { Log.error("input desync"); break; }
                int type = header[8] & 0xFF;
                long len = CodecMath.getU32(hb, 14);
                if (len > 4096) { Log.error("bad input packet"); break; }
                if (!CaptureSocket.readFully(in, header, CodecMath.HEADER_SIZE + (int) len)) break;
                handlePacket(type, hb, (int) len);
            }
        } catch (Throwable t) {
            if (mRunning) Log.warn("input channel ended: " + t);
        }
        mRunning = false;
    }

    /** Payload offsets start right after the 18 byte common header. */
    private void handlePacket(int type, ByteBuffer b, int len) {
        Injector inj;
        synchronized (mLock) { inj = mInjector; }
        if (inj == null) return;
        final int P = CodecMath.HEADER_SIZE;
        try {
            switch (type) {
                case CodecMath.PACKET_TOUCH: {
                    float x = CodecMath.getU32(b, P + 0);
                    float y = CodecMath.getU32(b, P + 4);
                    int action = b.get(P + 8) & 0xFF;
                    int pointer = b.get(P + 9) & 0xFF;
                    inj.touch(x, y, action, pointer, 1.0f);
                    break;
                }
                case CodecMath.PACKET_MOUSE_MOVE: {
                    int dx = (int) CodecMath.getU32(b, P + 0);
                    int dy = (int) CodecMath.getU32(b, P + 4);
                    int mode = b.get(P + 8) & 0xFF;
                    if (mode == 1) {
                        float ax = CodecMath.getU32(b, P + 9);
                        float ay = CodecMath.getU32(b, P + 13);
                        mCursorX = ax; mCursorY = ay;
                        inj.mouseAbsolute(ax, ay);
                    } else {
                        mCursorX = clamp(mCursorX + dx, 0, mScreenW - 1);
                        mCursorY = clamp(mCursorY + dy, 0, mScreenH - 1);
                        inj.mouseRelative(dx, dy, mCursorX, mCursorY);
                    }
                    break;
                }
                case CodecMath.PACKET_MOUSE_BUTTON: {
                    int button = b.get(P + 0) & 0xFF;
                    int down = b.get(P + 1) & 0xFF;
                    float x = CodecMath.getU32(b, P + 4);
                    float y = CodecMath.getU32(b, P + 8);
                    if (x > 0 || y > 0) { mCursorX = x; mCursorY = y; }
                    inj.mouseButton(button, down != 0, mCursorX, mCursorY);
                    break;
                }
                case CodecMath.PACKET_SCROLL: {
                    int dx = (int) CodecMath.getU32(b, P + 0);
                    int dy = (int) CodecMath.getU32(b, P + 4);
                    inj.scroll(dx, dy, mCursorX, mCursorY);
                    break;
                }
                case CodecMath.PACKET_KEY: {
                    int keyCode = CodecMath.getU16(b, P + 0);
                    int down = b.get(P + 2) & 0xFF;
                    int meta = b.get(P + 3) & 0xFF;
                    inj.key(keyCode, down != 0, meta);
                    break;
                }
                case CodecMath.PACKET_PING: {
                    // RTT probe: answered on the same socket, so what the PC
                    // measures is exactly the input round trip.
                    int probeId = (int) CodecMath.getU32(b, P + 0);
                    byte[] out = new byte[CodecMath.HEADER_SIZE + 16];
                    CodecMath.putU64(out, 0, CaptureSocket.MAGIC);
                    out[8] = (byte) CodecMath.PACKET_PONG;
                    CodecMath.putU32(out, 10, probeId);
                    CodecMath.putU32(out, 14, 16);
                    CodecMath.putU32(out, 18, probeId);
                    CodecMath.putU32(out, 22, 0);
                    CodecMath.putU64(out, 26, System.nanoTime() / 1000L);
                    synchronized (mClient) {
                        mClient.getOutputStream().write(out);
                        mClient.getOutputStream().flush();
                    }
                    break;
                }
                default: break;
            }
            mEventsInjected++;
        } catch (Throwable t) {
            if (mListener != null) mListener.onInputError("inject failed: " + t);
        }
    }

    private static float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    /* -------------------------------------------------------------- backends */
    interface Injector {
        String name();
        void touch(float x, float y, int action, int pointerId, float pressure);
        void mouseRelative(int dx, int dy, float absX, float absY);
        void mouseAbsolute(float x, float y);
        void mouseButton(int button, boolean down, float x, float y);
        void scroll(int dx, int dy, float x, float y);
        void key(int keyCode, boolean down, int meta);
        void close();
    }

    /* ------------------------------------------------ framework injection */
    static final class ManagerInjector implements Injector {
        private Object mInputManager;
        private Method mInject;
        private static final int MODE_ASYNC = 0;
        private long mDownTime = 0;
        private long mLastEventTime = 0;

        ManagerInjector() {
            try {
                Class<?> imClass = Class.forName("android.hardware.input.InputManager");
                Method getInstance = imClass.getMethod("getInstance");
                mInputManager = getInstance.invoke(null);
                mInject = imClass.getMethod("injectInputEvent", android.view.InputEvent.class, int.class);
            } catch (Throwable t) {
                Log.error("InputManager reflection unavailable: " + t);
            }
        }

        @Override public String name() { return "framework (InputManager.injectInputEvent)"; }

        /**
         * Event times are expressed in the phone's own uptime base.  The delta
         * that the PC measured for its own event (a few hundred microseconds
         * old by the time it arrives) is subtracted, so the injected event
         * carries a truthful timestamp instead of "now" - this matters for
         * gesture velocity and for the game's own input prediction.
         */
        private long eventTime(long deltaUs) {
            long now = SystemClock.uptimeMillis();
            long t = now - (deltaUs / 1000L);
            if (t <= mLastEventTime) t = mLastEventTime + 1;
            mLastEventTime = t;
            return t;
        }

        private void inject(android.view.InputEvent ev) {
            if (mInject == null || mInputManager == null) return;
            try { mInject.invoke(mInputManager, ev, MODE_ASYNC); } catch (Throwable ignored) {}
        }

        @Override public void touch(float x, float y, int action, int pointerId, float pressure) {
            long t = eventTime(0);
            int a;
            switch (action) {
                case 0:  a = MotionEvent.ACTION_DOWN;  mDownTime = t; break;
                case 1:  a = MotionEvent.ACTION_UP;    break;
                case 2:  a = MotionEvent.ACTION_MOVE;  break;
                default: a = MotionEvent.ACTION_MOVE;  break;
            }
            if (a == MotionEvent.ACTION_UP && mDownTime == 0) mDownTime = t;
            MotionEvent ev = MotionEvent.obtain(mDownTime, t, a, x, y, pressure, 1.0f, 0, 1.0f, 1.0f, 0, 0);
            ev.setSource(InputDevice.SOURCE_TOUCHSCREEN);
            inject(ev);
            ev.recycle();
        }

        @Override public void mouseRelative(int dx, int dy, float absX, float absY) {
            // The framework has no relative pointer, so the client-side cursor
            // is tracked here and injected at its absolute position.  Scaled so
            // that the motion feels identical to the same distance of real
            // mouse travel.
            mouseAbsolute(absX, absY);
        }

        @Override public void mouseAbsolute(float x, float y) {
            long t = eventTime(0);
            if (mDownTime == 0) mDownTime = t;
            MotionEvent ev = MotionEvent.obtain(mDownTime, t, MotionEvent.ACTION_MOVE, x, y, 1.0f, 1.0f, 0,
                                                1.0f, 1.0f, 0, 0);
            ev.setSource(InputDevice.SOURCE_MOUSE);
            inject(ev);
            ev.recycle();
        }

        @Override public void mouseButton(int button, boolean down, float x, float y) {
            long t = eventTime(0);
            if (down) { mDownTime = t; }
            int a = down ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_UP;
            MotionEvent ev = MotionEvent.obtain(mDownTime, t, a, x, y, 1.0f, 1.0f, 0, 1.0f, 1.0f, 0, 0);
            ev.setSource(InputDevice.SOURCE_MOUSE);
            inject(ev);
            ev.recycle();
            if (!down) mDownTime = 0;
        }

        @Override public void scroll(int dx, int dy, float x, float y) {
            long t = eventTime(0);
            MotionEvent ev = MotionEvent.obtain(mDownTime == 0 ? t : mDownTime, t,
                    MotionEvent.ACTION_SCROLL, x, y, 1.0f, 1.0f, 0, 1.0f, 1.0f, 0, 0);
            ev.setSource(InputDevice.SOURCE_MOUSE);
            inject(ev);
            ev.recycle();
        }

        @Override public void key(int keyCode, boolean down, int meta) {
            KeyEvent ev = new KeyEvent(down ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP, keyCode);
            inject(ev);
        }

        @Override public void close() {}
    }

    /* ------------------------------------------------------- kernel (uinput) */
    static final class UinputInjector implements Injector {
        private static final int UINPUT_IOCTL_BASE = 85;
        private static final long UI_DEV_CREATE   = 0x5501L;
        private static final long UI_DEV_DESTROY  = 0x5502L;
        private static final long UI_SET_EVBIT    = 0x40045564L;
        private static final long UI_SET_KEYBIT   = 0x40045565L;
        private static final long UI_SET_RELBIT   = 0x40045566L;
        private static final long UI_SET_ABSBIT   = 0x40045567L;
        private static final long UI_SET_PROPBIT  = 0x4004556EL;

        private static final int EV_SYN = 0x00, EV_KEY = 0x01, EV_REL = 0x02, EV_ABS = 0x03;
        private static final int REL_X = 0x00, REL_Y = 0x01, REL_WHEEL = 0x08, REL_HWHEEL = 0x06;
        private static final int ABS_MT_SLOT = 0x2F, ABS_MT_POSITION_X = 0x35, ABS_MT_POSITION_Y = 0x36,
                                 ABS_MT_TRACKING_ID = 0x39, ABS_MT_PRESSURE = 0x3A;
        private static final int BTN_LEFT = 0x110, BTN_RIGHT = 0x111, BTN_MIDDLE = 0x112;
        private static final int BTN_TOUCH = 0x14A;
        private static final int SYN_REPORT = 0;
        private static final int INPUT_PROP_POINTER = 0x00, INPUT_PROP_DIRECT = 0x01;

        private FileOutputStream mTouch, mMouse, mKeyboard;
        private FileDescriptor mTouchFd, mMouseFd, mKeyboardFd;
        private final int mEventSize;
        private Method mIoctlInt;
        private Object mIoctlTarget;
        private int mAbsMaxX, mAbsMaxY;

        static UinputInjector tryCreate() {
            try {
                UinputInjector u = new UinputInjector();
                if (u.setup()) return u;
                u.close();
            } catch (Throwable t) {
                Log.warn("uinput unavailable: " + t);
            }
            return null;
        }

        private UinputInjector() {
            mEventSize = System.getProperty("os.arch", "aarch64").contains("64") ? 24 : 16;
        }

        private boolean setup() throws Exception {
            if (!resolveIoctl()) return false;
            java.io.File dev = new java.io.File("/dev/uinput");
            if (!dev.exists() || !dev.canWrite()) {
                Log.warn("no write access to /dev/uinput");
                return false;
            }
            mTouch = open("/dev/uinput");
            if (mTouch == null) return false;
            mTouchFd = fdOf(mTouch);

            setupTouchDevice(mTouch, mTouchFd);
            mMouse = open("/dev/uinput");
            if (mMouse != null) { mMouseFd = fdOf(mMouse); setupMouseDevice(mMouse, mMouseFd); }
            mKeyboard = open("/dev/uinput");
            if (mKeyboard != null) { mKeyboardFd = fdOf(mKeyboard); setupKeyboardDevice(mKeyboard, mKeyboardFd); }
            Log.info("uinput devices created (" + mEventSize + " byte events)");
            return mTouch != null;
        }

        /** /dev/uinput nodes are opened through a small helper so the failure
         *  reason is reported instead of silently swallowed. */
        private FileOutputStream open(String path) {
            try { return new FileOutputStream(path); } catch (Throwable t) {
                Log.warn("open " + path + " failed: " + t.getMessage());
                return null;
            }
        }

        private static FileDescriptor fdOf(FileOutputStream fos) throws Exception {
            Field f = FileOutputStream.class.getDeclaredField("fd");
            f.setAccessible(true);
            return (FileDescriptor) f.get(fos);
        }

        /** ioctl is not part of the public API surface, so it is resolved by
         *  reflection with several candidates to survive ROM differences. */
        private boolean resolveIoctl() {
            try {
                Class<?> libcore = Class.forName("libcore.io.Libcore");
                Field osField = libcore.getField("os");
                mIoctlTarget = osField.get(null);
            } catch (Throwable t) {
                Log.warn("libcore not available: " + t);
                return false;
            }
            for (Method m : mIoctlTarget.getClass().getMethods()) {
                if (!m.getName().equals("ioctlInt")) continue;
                Class<?>[] pt = m.getParameterTypes();
                if (pt.length == 3 && pt[0] == FileDescriptor.class && pt[1] == int.class) {
                    mIoctlInt = m;
                    break;
                }
            }
            if (mIoctlInt == null) { Log.warn("ioctlInt signature not found"); return false; }
            return true;
        }

        private void ioctl(FileDescriptor fd, long cmd, long arg) throws Exception {
            Class<?>[] pt = mIoctlInt.getParameterTypes();
            Object argObj;
            if (pt[2] == int.class) {
                argObj = Integer.valueOf((int) arg);
            } else {
                // MutableInt style argument
                Object mi = pt[2].getConstructor(int.class).newInstance((int) arg);
                argObj = mi;
            }
            mIoctlInt.invoke(mIoctlTarget, fd, Integer.valueOf((int) cmd), argObj);
        }

        private void setupTouchDevice(FileOutputStream fos, FileDescriptor fd) throws Exception {
            ioctl(fd, UI_SET_EVBIT, EV_ABS);
            ioctl(fd, UI_SET_EVBIT, EV_KEY);
            ioctl(fd, UI_SET_EVBIT, EV_SYN);
            ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
            for (int code : new int[] { ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y,
                                        ABS_MT_TRACKING_ID, ABS_MT_PRESSURE }) {
                ioctl(fd, UI_SET_ABSBIT, code);
            }
            ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);   // touchscreen, not a tablet
            mAbsMaxX = mScreenW - 1;
            mAbsMaxY = mScreenH - 1;
            writeDeviceDescriptor(fos, "Mobilador Touch (virtual)", 0x01, 0x05, 0x0001, true);
        }

        private void setupMouseDevice(FileOutputStream fos, FileDescriptor fd) throws Exception {
            ioctl(fd, UI_SET_EVBIT, EV_REL);
            ioctl(fd, UI_SET_EVBIT, EV_KEY);
            ioctl(fd, UI_SET_EVBIT, EV_SYN);
            ioctl(fd, UI_SET_RELBIT, REL_X);
            ioctl(fd, UI_SET_RELBIT, REL_Y);
            ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
            ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);
            ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
            ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
            ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
            for (int i = 0; i < 256; i++) ioctl(fd, UI_SET_KEYBIT, i);   // keyboard passthrough on the same node
            ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
            writeDeviceDescriptor(fos, "Mobilador Mouse (virtual)", 0x03, 0x02, 0x0100, false);
        }

        private void setupKeyboardDevice(FileOutputStream fos, FileDescriptor fd) throws Exception {
            ioctl(fd, UI_SET_EVBIT, EV_KEY);
            ioctl(fd, UI_SET_EVBIT, EV_SYN);
            for (int i = 0; i < 256; i++) ioctl(fd, UI_SET_KEYBIT, i);
            writeDeviceDescriptor(fos, "Mobilador Keyboard (virtual)", 0x03, 0x01, 0x0100, false);
        }

        /**
         * struct uinput_user_dev:
         *   char name[80]; struct input_id id; __u32 ff_effects_max;
         *   __s32 absmax[64]; __s32 absmin[64]; __s32 absfuzz[64]; __s32 absflat[64];
         */
        private void writeDeviceDescriptor(FileOutputStream fos, String name, int bustype,
                                           int vendor, int product, boolean touch) throws Exception {
            ByteBuffer b = ByteBuffer.allocate(80 + 16 + 4 + 64 * 4 * 4);
            byte[] nb = name.getBytes("UTF-8");
            b.put(nb, 0, Math.min(nb.length, 79));
            b.position(80);
            b.putShort((short) bustype);
            b.putShort((short) vendor);
            b.putShort((short) product);
            b.putShort((short) 1);          // version
            b.position(80 + 16);
            b.putInt(0);                    // ff_effects_max
            // absmax[]
            b.position(80 + 20);
            b.putInt(touch ? mAbsMaxX : 0);
            b.putInt(touch ? mAbsMaxY : 0);
            b.position(80 + 20 + 64 * 4 * 2);   // skip absmin[]
            b.position(80 + 20 + 64 * 4 * 3);   // skip absfuzz[] / absflat[]
            fos.write(b.array());
            fos.flush();
            ioctl(touch ? mTouchFd : (fos == mMouse ? mMouseFd : mKeyboardFd), UI_DEV_CREATE, 0);
        }

        private void emit(FileOutputStream fos, int type, int code, int value) throws IOException {
            if (fos == null) return;
            byte[] ev = new byte[mEventSize];
            // timeval is intentionally zero: a zero timestamp tells the kernel
            // to stamp the event at the moment it is delivered, which is the
            // most accurate option available.
            CodecMath.putU16(ev, mEventSize - 8, type & 0xFFFF);
            CodecMath.putU16(ev, mEventSize - 6, code & 0xFFFF);
            CodecMath.putU32(ev, mEventSize - 4, value & 0xFFFFFFFFL);
            fos.write(ev);
        }

        private void syn(FileOutputStream fos) throws IOException { emit(fos, EV_SYN, SYN_REPORT, 0); }

        @Override public String name() { return "kernel (uinput virtual HID)"; }

        @Override public void touch(float x, float y, int action, int pointerId, float pressure) {
            try {
                if (mTouch == null) return;
                int px = (int) clamp(x, 0, mAbsMaxX);
                int py = (int) clamp(y, 0, mAbsMaxY);
                emit(mTouch, EV_ABS, ABS_MT_SLOT, pointerId);
                if (action == 0) {                       // down
                    emit(mTouch, EV_ABS, ABS_MT_TRACKING_ID, pointerId + 1);
                    emit(mTouch, EV_KEY, BTN_TOUCH, 1);
                }
                emit(mTouch, EV_ABS, ABS_MT_POSITION_X, px);
                emit(mTouch, EV_ABS, ABS_MT_POSITION_Y, py);
                emit(mTouch, EV_ABS, ABS_MT_PRESSURE, (int) (pressure * 255));
                if (action == 1) {                       // up
                    emit(mTouch, EV_ABS, ABS_MT_TRACKING_ID, -1);
                    emit(mTouch, EV_KEY, BTN_TOUCH, 0);
                }
                syn(mTouch);
            } catch (Throwable ignored) {}
        }

        @Override public void mouseRelative(int dx, int dy, float absX, float absY) {
            try {
                if (mMouse == null) return;
                if (dx != 0) emit(mMouse, EV_REL, REL_X, dx);
                if (dy != 0) emit(mMouse, EV_REL, REL_Y, dy);
                syn(mMouse);
            } catch (Throwable ignored) {}
        }

        @Override public void mouseAbsolute(float x, float y) {
            // A pointer-class uinput device is relative by definition; absolute
            // positioning is realised by emitting the delta to the target.
            try {
                if (mMouse == null) return;
                emit(mMouse, EV_REL, REL_X, (int) (x - mLastAbsX));
                emit(mMouse, EV_REL, REL_Y, (int) (y - mLastAbsY));
                mLastAbsX = x; mLastAbsY = y;
                syn(mMouse);
            } catch (Throwable ignored) {}
        }
        private float mLastAbsX, mLastAbsY;

        @Override public void mouseButton(int button, boolean down, float x, float y) {
            try {
                FileOutputStream fos = mMouse != null ? mMouse : mKeyboard;
                if (fos == null) return;
                int code = button == 0 ? BTN_LEFT : (button == 1 ? BTN_RIGHT : BTN_MIDDLE);
                emit(fos, EV_KEY, code, down ? 1 : 0);
                syn(fos);
            } catch (Throwable ignored) {}
        }

        @Override public void scroll(int dx, int dy, float x, float y) {
            try {
                if (mMouse == null) return;
                if (dy != 0) emit(mMouse, EV_REL, REL_WHEEL, dy);
                if (dx != 0) emit(mMouse, EV_REL, REL_HWHEEL, dx);
                syn(mMouse);
            } catch (Throwable ignored) {}
        }

        @Override public void key(int keyCode, boolean down, int meta) {
            // KeyEvent codes are Android key codes; for the kernel the Linux
            // code is required.  A tiny table covers the keys a game actually
            // uses; anything else falls back to the framework injector.
            int linux = androidToLinuxKey(keyCode);
            if (linux == 0) {
                if (mFallback == null) mFallback = new ManagerInjector();
                mFallback.key(keyCode, down, meta);
                return;
            }
            try {
                FileOutputStream fos = mKeyboard != null ? mKeyboard : mMouse;
                if (fos == null) return;
                emit(fos, EV_KEY, linux, down ? 1 : 0);
                syn(fos);
            } catch (Throwable ignored) {}
        }
        private ManagerInjector mFallback;

        private static int androidToLinuxKey(int keyCode) {
            if (keyCode >= KeyEvent.KEYCODE_A && keyCode <= KeyEvent.KEYCODE_Z) {
                return 30 + (keyCode - KeyEvent.KEYCODE_A);        // KEY_A..KEY_Z
            }
            if (keyCode >= KeyEvent.KEYCODE_0 && keyCode <= KeyEvent.KEYCODE_9) {
                return 2 + (keyCode - KeyEvent.KEYCODE_1 + 9) % 10; // KEY_1..KEY_0
            }
            switch (keyCode) {
                case KeyEvent.KEYCODE_SPACE:  return 57;   // KEY_SPACE
                case KeyEvent.KEYCODE_ENTER:  return 28;
                case KeyEvent.KEYCODE_TAB:    return 15;
                case KeyEvent.KEYCODE_ESCAPE: return 1;
                case KeyEvent.KEYCODE_DEL:    return 14;
                case KeyEvent.KEYCODE_SHIFT_LEFT:  return 42;
                case KeyEvent.KEYCODE_SHIFT_RIGHT: return 54;
                case KeyEvent.KEYCODE_CTRL_LEFT:   return 29;
                case KeyEvent.KEYCODE_ALT_LEFT:    return 56;
                case KeyEvent.KEYCODE_DPAD_UP:     return 103;
                case KeyEvent.KEYCODE_DPAD_DOWN:   return 108;
                case KeyEvent.KEYCODE_DPAD_LEFT:   return 105;
                case KeyEvent.KEYCODE_DPAD_RIGHT:  return 106;
                default: return 0;
            }
        }

        @Override public void close() {
            try { if (mTouchFd != null) ioctl(mTouchFd, UI_DEV_DESTROY, 0); } catch (Throwable ignored) {}
            try { if (mMouseFd != null) ioctl(mMouseFd, UI_DEV_DESTROY, 0); } catch (Throwable ignored) {}
            try { if (mKeyboardFd != null) ioctl(mKeyboardFd, UI_DEV_DESTROY, 0); } catch (Throwable ignored) {}
            try { if (mTouch != null) mTouch.close(); } catch (Throwable ignored) {}
            try { if (mMouse != null) mMouse.close(); } catch (Throwable ignored) {}
            try { if (mKeyboard != null) mKeyboard.close(); } catch (Throwable ignored) {}
            mTouch = null; mMouse = null; mKeyboard = null;
        }
    }
}
