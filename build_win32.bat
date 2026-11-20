@echo off
setlocal EnableExtensions
set "EXIT_CODE=1"
title Refunct Utility - Windows x86 Build (Auto-Fix CMake Cache)
rem Always run from the project directory, regardless of how the BAT was launched.
set "PROJECT_DIR=%~dp0"
if not exist "%PROJECT_DIR%CMakeLists.txt" (
    echo ERROR: Cannot find CMakeLists.txt beside this script.
    echo Extract the ENTIRE ZIP first, then run build_win32.bat.
    goto :finish
)
set "BUILD_DIR=%PROJECT_DIR%build-win32"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if not exist "%BUILD_DIR%" (
    echo ERROR: Could not create the build folder. Check write permissions.
    goto :finish
)
set "CONFIG_LOG=%BUILD_DIR%\configure.log"
set "COMPILE_LOG=%BUILD_DIR%\compile.log"
call :find_cmake
if not defined CMAKE_EXE (
    echo ERROR: CMake is not installed or could not be found.
    echo Install the Desktop development with C++ workload in Visual Studio 2022.
    echo Make sure the C++ CMake tools for Windows component is selected.
    goto :finish
)
rem MinHook is downloaded during configuration unless a local checkout is supplied.
call :find_git
if not defined GIT_AVAILABLE if not defined REFUNCT_MINHOOK_SOURCE if not exist "%PROJECT_DIR%third_party\minhook\CMakeLists.txt" (
    echo ERROR: Git is not available. CMake needs Git to download MinHook.
    echo Install Git for Windows, or configure a local MinHook source checkout.
    goto :finish
)
rem A copied/moved project may contain an old CMakeCache.txt with absolute paths.
rem Detect this and remove ONLY the stale build directory before configuring.
call :clear_moved_cmake_cache
if errorlevel 1 goto :finish

echo.
echo ============================================================
echo   REFUNCT UTILITY v1.0.0 - Win32 Release build
echo ============================================================
echo Project: "%PROJECT_DIR%"
echo CMake: "%CMAKE_EXE%"
echo Logs: "%BUILD_DIR%"
echo.
echo [1/2] Configuring project - baseline disabled, may download MinHook...
rem Do not pass a quoted directory ending in a backslash: Windows argv parsing
rem treats that backslash as escaping the closing quote and swallows -B/-G.
rem Adding a final dot yields the same directory without a trailing backslash.
"%CMAKE_EXE%" -S "%PROJECT_DIR%." -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A Win32 -DREFUNCT_BUILD_BASELINE:BOOL=OFF -DBUILD_SHARED_LIBS:BOOL=OFF > "%CONFIG_LOG%" 2>&1
set "STEP_RESULT=%errorlevel%"
type "%CONFIG_LOG%"
if not "%STEP_RESULT%"=="0" (
    echo.
    echo ERROR: Project configuration failed with exit code %STEP_RESULT%.
    echo See "%CONFIG_LOG%"
    goto :finish
)
echo.
echo [2/2] CLEAN REBUILD: compiling the modular ASI from scratch...
rem --clean-first removes stale compiled outputs before building.
rem This avoids accidentally reusing an older ASI when files or timestamps changed.
"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release --target RefunctUtility --clean-first > "%COMPILE_LOG%" 2>&1
set "STEP_RESULT=%errorlevel%"
type "%COMPILE_LOG%"
if not "%STEP_RESULT%"=="0" (
    echo.
    echo ERROR: Compilation failed with exit code %STEP_RESULT%.
    echo See "%COMPILE_LOG%"
    goto :finish
)
if not exist "%BUILD_DIR%\Release\RefunctUtility.asi" (
    echo.
    echo ERROR: Build reported success but the expected .asi was not found.
    echo Check "%COMPILE_LOG%" for its actual output location.
    goto :finish
)
set "EXIT_CODE=0"
echo.
echo SUCCESS! Refunct Utility ASI built at:
echo "%BUILD_DIR%\Release\RefunctUtility.asi"
goto :finish
:clear_moved_cmake_cache
set "CACHE_FILE=%BUILD_DIR%\CMakeCache.txt"
if not exist "%CACHE_FILE%" exit /b 0

rem CMake writes these as absolute paths, normally using forward slashes.
rem Read the cache as a file rather than running findstr via a command shell:
rem this also handles spaces and parentheses in the project directory.
set "CACHED_SOURCE="
set "CACHED_BUILD="
for /f "usebackq tokens=1,* delims==" %%A in ("%CACHE_FILE%") do (
    if /I "%%A"=="CMAKE_HOME_DIRECTORY:INTERNAL" set "CACHED_SOURCE=%%B"
    if /I "%%A"=="CMAKE_CACHEFILE_DIR:INTERNAL" set "CACHED_BUILD=%%B"
)

rem Normalize Windows backslashes and the source's trailing slash.
set "CURRENT_SOURCE=%PROJECT_DIR:\=/%"
if "%CURRENT_SOURCE:~-1%"=="/" set "CURRENT_SOURCE=%CURRENT_SOURCE:~0,-1%"
set "CURRENT_BUILD=%BUILD_DIR:\=/%"
set "CACHED_SOURCE=%CACHED_SOURCE:\=/%"
set "CACHED_BUILD=%CACHED_BUILD:\=/%"

set "RESET_CACHE=0"
if not defined CACHED_SOURCE set "RESET_CACHE=1"
if /I not "%CACHED_SOURCE%"=="%CURRENT_SOURCE%" set "RESET_CACHE=1"
if defined CACHED_BUILD if /I not "%CACHED_BUILD%"=="%CURRENT_BUILD%" set "RESET_CACHE=1"
if "%RESET_CACHE%"=="0" exit /b 0

echo.
echo Detected an outdated CMake cache from another project location.
echo Automatically removing stale build files. Source files are untouched.
rd /s /q "%BUILD_DIR%" 2>nul
if exist "%BUILD_DIR%" (
    echo ERROR: Could not remove the outdated build folder.
    echo Close Visual Studio, CMake, and any tools using "%BUILD_DIR%".
    exit /b 1
)
md "%BUILD_DIR%" 2>nul
if not exist "%BUILD_DIR%" (
    echo ERROR: Could not recreate the build directory.
    exit /b 1
)
echo CMake cache cleared. Continuing with a fresh configuration...
echo.
exit /b 0

:find_cmake
set "CMAKE_EXE="
where cmake.exe >nul 2>&1
if not errorlevel 1 (
    set "CMAKE_EXE=cmake.exe"
    exit /b 0
)
rem Visual Studio Build Tools often installs CMake without adding it to PATH.
for %%E in (BuildTools Community Professional Enterprise) do (
    if not defined CMAKE_EXE if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
        set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
)
for %%E in (BuildTools Community Professional Enterprise) do (
    if not defined CMAKE_EXE if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
        set "CMAKE_EXE=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
)
exit /b 0
:find_git
set "GIT_AVAILABLE="
where git.exe >nul 2>&1
if not errorlevel 1 (
    set "GIT_AVAILABLE=1"
    exit /b 0
)
if exist "%ProgramFiles%\Git\cmd\git.exe" (
    set "PATH=%ProgramFiles%\Git\cmd;%PATH%"
    set "GIT_AVAILABLE=1"
    exit /b 0
)
if exist "%ProgramFiles(x86)%\Git\cmd\git.exe" (
    set "PATH=%ProgramFiles(x86)%\Git\cmd;%PATH%"
    set "GIT_AVAILABLE=1"
)
exit /b 0
:finish
echo.
if "%EXIT_CODE%"=="0" (
    echo BUILD SUCCEEDED.
) else (
    echo BUILD DID NOT COMPLETE. Read the error above.
)
echo.
if /I not "%~1"=="--no-pause" (
    echo Press any key to close this window.
    pause >nul
)
exit /b %EXIT_CODE%
