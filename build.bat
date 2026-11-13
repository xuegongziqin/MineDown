@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem ======================================================================
rem  MineDown build script  (Qt6 + MSYS2 MinGW64 + GNU g++, no CMake)
rem
rem  Usage:
rem    build.bat            incremental build (default)
rem    build.bat fast       -O0 -g, incremental
rem    build.bat clean      wipe build\ then full build
rem    build.bat run        build then launch
rem
rem  Any new .cpp under src\ is compiled automatically.
rem  Every .h under src\ is run through moc automatically, so new
rem  Q_OBJECT classes also need no script change.
rem
rem  Incremental logic uses byte comparison against copies kept in
rem  build\mark\ (locale independent, unlike timestamp parsing).
rem  A changed/added .h triggers a full recompile reference-wise.
rem
rem  NOTE: keep this file pure ASCII; cmd.exe cannot parse UTF-8 here.
rem ======================================================================

set "MINGW=D:\msys64\mingw64"
set "CXX=%MINGW%\bin\g++.exe"
set "MOC=%MINGW%\share\qt6\bin\moc.exe"
set "RCC=%MINGW%\share\qt6\bin\rcc.exe"
set "WINDRES=%MINGW%\bin\windres.exe"
set "QTINC=%MINGW%\include\qt6"
set "QTLIB=%MINGW%\lib"

set "ROOT=%~dp0"
set "SRC=%ROOT%src"
set "BUILD=%ROOT%build"
set "OBJ=%BUILD%\obj"
set "MARK=%BUILD%\mark"
set "EXE=%BUILD%\MineDown.exe"

if not exist "%CXX%" (
  echo [ERR] g++ not found: %CXX%
  exit /b 1
)
if not exist "%MOC%" (
  echo [ERR] moc not found: %MOC%
  exit /b 1
)
if not exist "%RCC%" (
  echo [ERR] rcc not found: %RCC%
  exit /b 1
)
if not exist "%WINDRES%" (
  echo [ERR] windres not found: %WINDRES%
  exit /b 1
)

rem this script uses unquoted -I paths, so spaces are not supported
set "CHECK=%ROOT%%MINGW%"
if not "%CHECK: =%"=="%CHECK%" (
  echo [ERR] project or MINGW path contains a space; unquoted -I would break:
  echo       ROOT=%ROOT%
  echo       MINGW=%MINGW%
  exit /b 1
)

set "MODE=%~1"
if /i "%MODE%"=="clean" (
  if exist "%BUILD%" rmdir /s /q "%BUILD%"
  set "MODE=release"
)
if /i "%MODE%"=="fast" (set "OPT=-O0 -g") else (set "OPT=-O2")

set "CXXFLAGS=-std=c++20 %OPT% -Wall -Wextra -Wno-sfinae-incomplete -DUNICODE -D_UNICODE -I%SRC% -I%SRC%\core -I%SRC%\ui -I%QTINC% -I%QTINC%\QtCore -I%QTINC%\QtGui -I%QTINC%\QtWidgets -I%QTINC%\QtNetwork -I%QTINC%\QtConcurrent"
rem strip symbols (-s): smaller exe, no debug info in release
set "LDFLAGS=-mwindows -s"
rem ld resolves archives left to right, so Qt import libs MUST come after
rem the object files or every Qt symbol stays undefined.
set "LDLIBS=-L%QTLIB% -lQt6EntryPoint -lQt6Widgets -lQt6Gui -lQt6Network -lQt6Concurrent -lQt6Core -lz -static-libgcc -static-libstdc++"

if not exist "%BUILD%" mkdir "%BUILD%" >nul 2>&1
if not exist "%OBJ%"   mkdir "%OBJ%"   >nul 2>&1
if not exist "%MARK%"  mkdir "%MARK%"  >nul 2>&1

set "T0=%TIME%"
echo ============================================================
echo  MineDown build
echo    mode     : %MODE%     optimize: %OPT%
echo    compiler : %CXX%
echo    Qt       : %QTINC%
echo    started  : %TIME%
echo ============================================================

rem ----------------------------------------------------------------------
rem  0) header change detection -> full recompile
rem ----------------------------------------------------------------------
echo [HDR] checking headers for changes ...
set "REBUILD_ALL=0"
set /a HDR_TOTAL=0
for /r "%SRC%" %%h in (*.h) do (
  set /a HDR_TOTAL+=1
  set "REL=%%h"
  set "REL=!REL:%SRC%\=!"
  set "REL=!REL:\=_!"
  set "MF=%MARK%\hdr_!REL!"
  if not exist "!MF!" (
    set "REBUILD_ALL=1"
  ) else (
    fc /b "%%h" "!MF!" >nul 2>&1 || set "REBUILD_ALL=1"
  )
  copy /y "%%h" "!MF!" >nul
)
echo [HDR] !HDR_TOTAL! headers tracked
if "!REBUILD_ALL!"=="1" (
  echo [HDR] header set changed - full recompile of every source
  del /q "%OBJ%\*.o" >nul 2>&1
)

rem ----------------------------------------------------------------------
rem  1) moc: regenerate every header (cheap), compile only if output changed
rem ----------------------------------------------------------------------
echo [MOC] running moc on every header ...
del /q "%OBJ%\moc_*.cpp" >nul 2>&1
set "MOCS="
set /a MOC_FAIL=0
for /r "%SRC%" %%h in (*.h) do (
  set "REL=%%h"
  set "REL=!REL:%SRC%\=!"
  set "REL=!REL:\=_!"
  set "MOCOUT=%OBJ%\moc_!REL:.h=.cpp!"
  rem headers without Q_OBJECT make moc print "No relevant classes found"
  rem on stderr; failures are caught through errorlevel instead.
  "%MOC%" -I%SRC% -I%SRC%\core -I%SRC%\ui -I%QTINC% "%%h" -o "!MOCOUT!" 2>nul
  if errorlevel 1 (
    set /a MOC_FAIL+=1
    echo [ERR] moc failed: %%h
    goto :fail
  )
  set "MOCS=!MOCS! !MOCOUT!"
)
echo [MOC] !HDR_TOTAL! headers processed

rem ----------------------------------------------------------------------
rem  2) compile sources + moc output
rem ----------------------------------------------------------------------
echo [SRC] scanning sources ...
set /a TOTAL_SRC=0
for /r "%SRC%" %%f in (*.cpp) do set /a TOTAL_SRC+=1
echo [SRC] !TOTAL_SRC! source files

set "OBJS="
set "SKIPLIST="
set /a COUNT=0
set /a SKIPPED=0
set /a FILE_NO=0
for /r "%SRC%" %%f in (*.cpp) do (
  set /a FILE_NO+=1
  set "SRC_FILE=%%f"
  set "SRC_REL=%%f"
  set "SRC_REL=!SRC_REL:%SRC%\=!"
  set "REL=!SRC_REL:\=_!"
  set "OUT=%OBJ%\!REL:.cpp=.o!"
  call :maybe_cc "!SRC_FILE!" "!OUT!" "src_!REL!" "!SRC_REL!" !FILE_NO!
  if errorlevel 1 goto :fail
  set "OBJS=!OBJS! !OUT!"
)
if defined MOCS (
  for %%f in (!MOCS!) do (
    set "OUT=%%~dpnf.o"
    call :maybe_cc "%%f" "!OUT!" "moc_%%~nxf" ".moc\%%~nxf" 0
    if errorlevel 1 goto :fail
    set "OBJS=!OBJS! !OUT!"
  )
)

rem ----------------------------------------------------------------------
rem  2b) resources: Qt resource (window icon) + Win32 resource (exe icon)
rem ----------------------------------------------------------------------
echo [RCC] app.qrc  ^(window icon^) ...
"%RCC%" -name app "%ROOT%res\app.qrc" -o "%OBJ%\qrc_app.cpp"
if errorlevel 1 goto :fail
call :maybe_cc "%OBJ%\qrc_app.cpp" "%OBJ%\qrc_app.o" "qrc_app.cpp" ".qrc\app.qrc" 0
if errorlevel 1 goto :fail
set "OBJS=!OBJS! %OBJ%\qrc_app.o"

echo [RC]  app.rc   ^(exe icon + manifest^) ...
rem A malformed manifest does not break the link: the exe links fine and then
rem Windows refuses to start it with "side-by-side configuration is incorrect"
rem (error 14001). Catch that here instead of at run time.
powershell -NoProfile -Command "try { $null = [xml](Get-Content -Raw '%ROOT%res\app.manifest'); exit 0 } catch { Write-Host $_.Exception.Message; exit 1 }"
if errorlevel 1 (
  echo [ERR] res\app.manifest is not well-formed XML
  goto :fail
)
"%WINDRES%" -I "%ROOT%res" -i "%ROOT%res\app.rc" -o "%OBJ%\app_res.o"
if errorlevel 1 (
  echo [ERR] windres failed: %ROOT%res\app.rc
  goto :fail
)
set "OBJS=!OBJS! %OBJ%\app_res.o"

rem ----------------------------------------------------------------------
rem  3) link
rem ----------------------------------------------------------------------
set /a OBJ_COUNT=0
for %%o in (!OBJS!) do set /a OBJ_COUNT+=1
echo [LD]  linking MineDown.exe  ^(!OBJ_COUNT! objects: !COUNT! compiled, !SKIPPED! reused^)
"%CXX%" %LDFLAGS% -o "%EXE%" !OBJS! %LDLIBS%
if errorlevel 1 (
  echo [ERR] link failed
  echo       %CXX% %LDFLAGS% -o "%EXE%" ^<objects^> %LDLIBS%
  goto :fail
)

rem ----------------------------------------------------------------------
rem  4) summary
rem ----------------------------------------------------------------------
set "T1=%TIME%"
call :elapsed "%T0%" "%T1%"
echo ------------------------------------------------------------
echo  compiled : !COUNT! file^(s^)
echo  reused   : !SKIPPED! object^(s^) up to date
if defined SKIPLIST (
  echo  up to date:!SKIPLIST!
)
for %%s in ("%EXE%") do echo  exe      : %%~fs  ^(%%~zs bytes^)
echo  elapsed  : !ELAPSED!s
echo ------------------------------------------------------------
echo [OK]  build succeeded

if /i "%MODE%"=="run" start "" "%EXE%"
endlocal
exit /b 0

rem ----------------------------------------------------------------------
rem  :maybe_cc  <source> <object> <mark name> <display name> <index>
rem
rem  Compiles only when the source differs from its stored copy, or the
rem  object is missing (or a full recompile was requested).
rem ----------------------------------------------------------------------
:maybe_cc
set "SF=%~1"
set "OF=%~2"
set "MF=%MARK%\%~3"
set "DISP=%~4"
set "IDX=%~5"
set "NEED=0"
if not exist "!OF!" set "NEED=1"
if not exist "!MF!" set "NEED=1"
if "!NEED!"=="0" (
  fc /b "!SF!" "!MF!" >nul 2>&1 || set "NEED=1"
)
if "!NEED!"=="1" (
  set /a COUNT+=1
  if "!IDX!"=="0" (
    echo [CC]  !COUNT!  !DISP!
  ) else (
    echo [CC]  !COUNT!  [src !IDX!/%TOTAL_SRC%]  !DISP!
  )
  "%CXX%" %CXXFLAGS% -c "!SF!" -o "!OF!"
  if errorlevel 1 (
    echo.
    echo [ERR] compile failed: !DISP!
    echo       command: "%CXX%" %CXXFLAGS% -c "!SF!" -o "!OF!"
    exit /b 1
  )
) else (
  set /a SKIPPED+=1
  if !SKIPPED! leq 12 set "SKIPLIST=!SKIPLIST! !DISP!"
)
copy /y "!SF!" "!MF!" >nul
exit /b 0

rem ----------------------------------------------------------------------
rem  :elapsed  <start time> <end time>  ->  ELAPSED (seconds)
rem  Times look like 10:04:25.81; the 1xx-100 trick parses 08/09 safely.
rem ----------------------------------------------------------------------
:elapsed
setlocal EnableDelayedExpansion
set /a E1=0
set /a E2=0
for /f "tokens=1-3 delims=:.," %%a in ("%~1") do set /a E1=(1%%a-100)*3600+(1%%b-100)*60+(1%%c-100)
for /f "tokens=1-3 delims=:.," %%a in ("%~2") do set /a E2=(1%%a-100)*3600+(1%%b-100)*60+(1%%c-100)
set /a D=E2-E1
if !D! lss 0 set /a D+=86400
endlocal & set "ELAPSED=%D%"
exit /b 0

:fail
echo.
echo *** BUILD FAILED ***
call :elapsed "%T0%" "%TIME%"
echo  elapsed  : !ELAPSED!s
endlocal
exit /b 1
