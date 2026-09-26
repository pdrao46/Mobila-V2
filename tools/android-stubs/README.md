# Android platform stubs

`android-33.jar` is the *stub* platform library (the same artifact the Android SDK
ships as `platforms/android-33/android.jar`). It contains only public API
signatures and is used exclusively at compile time - it never runs and no code
from it is redistributed in the application.

It is bundled so that building the on-device server module needs nothing but a
JDK:

    javac -bootclasspath tools/android-stubs/android-33.jar -d out android-server/src/**/*.java
    java  -jar tools/d8.jar --lib tools/android-stubs/android-33.jar --output out out/**/*.class

If you prefer to use your own Android SDK, point `ANDROID_JAR` at
`$ANDROID_HOME/platforms/android-33/android.jar` instead; the result is identical.

Source: Android Open Source Platform stubs, Apache License 2.0.
