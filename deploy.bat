@echo off
rem ============================================================
rem  MineDown deploy script  -  build a self-contained folder
rem  Usage:  deploy.bat
rem  Output: dist\  (MineDown.exe + all needed DLLs + Qt plugins)
rem  NOTE: keep this file pure ASCII (cmd.exe uses the OEM code page).
rem ============================================================
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
set "MINGW=D:\msys64\mingw64"
set "DIST=%ROOT%dist"
set "OBJDUMP=%MINGW%\bin\objdump.exe"
set "WINDEPLOY=%MINGW%\bin\windeployqt6.exe"

echo ============================================================
echo  MineDown deploy
echo ============================================================

if not exist "%OBJDUMP%" (
    echo [ERROR] objdump not found: %OBJDUMP%
    exit /b 1
)
if not exist "%WINDEPLOY%" (
    echo [ERROR] windeployqt6 not found: %WINDEPLOY%
    exit /b 1
)

echo [1/4] building release ...
call "%ROOT%build.bat" release
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)

echo [2/4] preparing dist ...
if exist "%DIST%" rd /s /q "%DIST%"
mkdir "%DIST%" 2>nul
copy /y "%ROOT%build\MineDown.exe" "%DIST%\MineDown.exe" >nul
if errorlevel 1 (
    echo [ERROR] cannot copy MineDown.exe
    exit /b 1
)

echo [3/4] running windeployqt ...
"%WINDEPLOY%" --release --compiler-runtime --no-translations --dir "%DIST%" "%DIST%\MineDown.exe"
if errorlevel 1 (
    echo [WARN] windeployqt reported an error, continuing anyway
)

echo [4/4] collecting remaining MinGW dependencies ...
rem NOTE: objdump output goes through a temp file on purpose. Piping it inside
rem `for /f 'cmd'` breaks because cmd strips the first/last quote of the command.
set "DEPLIST=%TEMP%\minedown_deps.txt"
set /a ROUND=0
:deps_loop
set "CHANGED=0"
set /a ROUND+=1
for /r "%DIST%" %%F in (*.exe *.dll) do (
    "%OBJDUMP%" -p "%%F" 2>nul | findstr /i "DLL Name:" > "%DEPLIST%"
    for /f "usebackq tokens=3" %%N in ("%DEPLIST%") do (
        if not exist "%DIST%\%%N" (
            if exist "%MINGW%\bin\%%N" (
                copy /y "%MINGW%\bin\%%N" "%DIST%\%%N" >nul
                set "CHANGED=1"
            )
        )
    )
)
if exist "%DEPLIST%" del /q "%DEPLIST%"
if "%CHANGED%"=="1" goto deps_loop

set /a COUNT=0
set /a TOTAL=0
for /r "%DIST%" %%F in (*) do (
    set /a COUNT+=1
    for %%S in ("%%F") do set /a TOTAL+=%%~zS
)
echo.
echo [OK]  dist ready: %DIST%
echo       files: %COUNT%   size: %TOTAL% bytes   dep rounds: %ROUND%
echo.
echo Run it with a clean PATH to verify:
echo   set PATH=C:\Windows\system32;C:\Windows
echo   dist\MineDown.exe
echo.
endlocal
exit /b 0
