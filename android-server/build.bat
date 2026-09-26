@echo off
rem ===========================================================================
rem  MOBILADOR - android-server\build.bat
rem  Builds the on-device server module (mobilador.dex) on Windows.
rem
rem  Requirements (either path works):
rem    * a JDK (javac)          - winget install EclipseAdoptium.Temurin.17.JDK
rem    * d8: the copy of tools\d8.jar that ships with Mobilador, or the
rem      d8.bat from Android build-tools in PATH
rem
rem  Usage:
rem     build.bat                    ^<- auto-detect everything
rem     build.bat C:\path\android.jar
rem
rem  Output: dist\mobilador.dex  (Mobilador picks it up from "server\" next to
rem  Mobilador.exe, or uploads it through DIAGNOSTICO > INSTALAR)
rem ===========================================================================
setlocal enabledelayedexpansion

set HERE=%~dp0
set ROOT=%HERE%..
set OUT=%ROOT%\build\android
set DIST=%ROOT%\dist
set SRC=%HERE%src

rem ---- javac -----------------------------------------------------------------
set JAVAC=%JAVAC%
if "%JAVAC%"=="" for %%I in (javac.exe) do set JAVAC=%%~$PATH:I
if "%JAVAC%"=="" (
  echo ERROR: javac not found.
  echo        Install a JDK:  winget install EclipseAdoptium.Temurin.17.JDK
  exit /b 2
)

rem ---- android.jar -----------------------------------------------------------
set ANDROID_JAR=%1
if "%ANDROID_JAR%"=="" (
  if exist "%ROOT%\tools\android-stubs\android-33.jar" set ANDROID_JAR=%ROOT%\tools\android-stubs\android-33.jar
)
if "%ANDROID_JAR%"=="" (
  if exist "%ANDROID_HOME%\platforms\android-33\android.jar" set ANDROID_JAR=%ANDROID_HOME%\platforms\android-33\android.jar
)
if "%ANDROID_JAR%"=="" (
  echo ERROR: android.jar not found.
  echo        Pass it as the first argument, or install Android SDK platform 33.
  exit /b 3
)

rem ---- java (needed to run d8.jar) -------------------------------------------
set JAVA=%JAVA%
if "%JAVA%"=="" for %%I in (java.exe) do set JAVA=%%~$PATH:I
if "%JAVA%"=="" (
  echo ERROR: java.exe not found ^(it comes with the same JDK as javac^).
  exit /b 4
)

echo   javac       : %JAVAC%
echo   android.jar : %ANDROID_JAR%

if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%\classes" 2>nul
mkdir "%DIST%" 2>nul

echo   compiling...
dir /b /s "%SRC%\*.java" > "%OUT%\sources.txt"
"%JAVAC%" -source 8 -target 8 -nowarn -Xlint:none -bootclasspath "%ANDROID_JAR%" -classpath "%ANDROID_JAR%" -d "%OUT%\classes" @"%OUT%\sources.txt"
if errorlevel 1 (
  echo ERROR: javac failed.
  exit /b 5
)

echo   dexing...
rem d8 takes class files as separate arguments: build the list explicitly,
rem because cmd.exe does not expand wildcards for external programs.
set D8_FILES=
for /r "%OUT%\classes" %%f in (*.class) do set D8_FILES=!D8_FILES! "%%f"
if "!D8_FILES!"=="" (
  echo ERROR: no compiled classes found.
  exit /b 6
)

set D8_JAR=%ROOT%\tools\d8.jar
if exist "%D8_JAR%" (
  "%JAVA%" -jar "%D8_JAR%" --release --min-api 21 --lib "%ANDROID_JAR%" --output "%OUT%" !D8_FILES!
) else (
  where d8 >nul 2>nul || (
    echo ERROR: neither tools\d8.jar nor d8 in PATH was found.
    exit /b 7
  )
  d8 --release --min-api 21 --lib "%ANDROID_JAR%" --output "%OUT%" !D8_FILES!
)

if not exist "%OUT%\classes.dex" (
  echo ERROR: dexing produced no classes.dex
  exit /b 7
)

copy /y "%OUT%\classes.dex" "%DIST%\mobilador.dex" >nul
if not exist "%DIST%\server" mkdir "%DIST%\server"
copy /y "%OUT%\classes.dex" "%DIST%\server\mobilador.dex" >nul

echo.
echo   -^> dist\mobilador.dex
echo.
echo   to install it on a connected phone:
echo      adb push dist\mobilador.dex /data/local/tmp/mobilador.dex
echo   or simply run Mobilador.exe and press INSTALAR in DIAGNOSTICO.
endlocal
