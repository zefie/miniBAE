@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem =============================================================================
rem build_release.bat — production build of libneobae + foo_neobae
rem
rem Defaults match 32-bit foobar2000 (Win32 / x86).
rem
rem Optional env overrides:
rem   ARCH=Win32|x64              (default Win32)
rem   TOOLSET=v145                (default v145 = VS 2026; use v143 for VS 2022)
rem   CONFIG=Release              (default Release)
rem   BUILD_DIR=...               (optional; otherwise per-arch cmake tree)
rem
rem Examples:
rem   build_release.bat
rem   set ARCH=x64 && build_release.bat
rem   set TOOLSET=v143 && build_release.bat
rem =============================================================================

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%" >nul

if not defined ARCH set "ARCH=Win32"
if /I "%ARCH%"=="x86" set "ARCH=Win32"
if not defined CONFIG set "CONFIG=Release"
rem v143 (VS 2022) matches typical foobar CRT age better than bleeding-edge v145.
if not defined TOOLSET set "TOOLSET=v143"

if /I "%ARCH%"=="Win32" (
  set "ARCH_TAG=win32"
  set "MSBUILD_PLATFORM=x86"
  set "OUT_PLATFORM=Win32"
) else if /I "%ARCH%"=="x64" (
  set "ARCH_TAG=x64"
  set "MSBUILD_PLATFORM=x64"
  set "OUT_PLATFORM=x64"
) else (
  echo ERROR: ARCH must be Win32 or x64 ^(got "%ARCH%"^)
  popd >nul
  exit /b 1
)

set "RELEASE_DIR=%SCRIPT_DIR%release\%ARCH_TAG%"
set "PLUGIN_OUT=%SCRIPT_DIR%out\%OUT_PLATFORM%\%CONFIG%"

echo ============================================================
echo NeoBAE foobar2000 release build
echo   ARCH=%ARCH%  ^(msbuild Platform=%MSBUILD_PLATFORM%^)
echo   CONFIG=%CONFIG%
echo   TOOLSET=%TOOLSET%
echo   RELEASE_DIR=%RELEASE_DIR%
echo ============================================================
echo.

rem ----- 1) libneobae via CMake -----
echo [1/3] Building libneobae ^(%ARCH%^)...
call "%SCRIPT_DIR%build_libneobae.bat" %*
if errorlevel 1 (
  echo ERROR: libneobae build failed.
  popd >nul
  exit /b 1
)
echo.

rem ----- 2) Locate MSBuild -----
set "MSBUILD="
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
  for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
    set "MSBUILD=%%I"
    goto :msbuild_found
  )
)
:msbuild_found
if not defined MSBUILD (
  where msbuild >nul 2>&1
  if not errorlevel 1 (
    for /f "delims=" %%I in ('where msbuild') do (
      set "MSBUILD=%%I"
      goto :msbuild_ready
    )
  )
)
:msbuild_ready
if not defined MSBUILD (
  echo ERROR: MSBuild not found. Open a "Developer Command Prompt for VS" or install VS C++ workload.
  popd >nul
  exit /b 1
)
echo Using MSBuild: %MSBUILD%
echo.

rem ----- 3) foo_neobae -----
echo [2/3] Building foo_neobae ^(%CONFIG% ^| %MSBUILD_PLATFORM%, toolset %TOOLSET%^)...
"%MSBUILD%" "%SCRIPT_DIR%foo_neobae.sln" ^
  /m ^
  /nologo ^
  /v:m ^
  /p:Configuration=%CONFIG% ^
  /p:Platform=%MSBUILD_PLATFORM% ^
  /p:PlatformToolset=%TOOLSET% ^
  /p:NeoBAE_PlatformToolset=%TOOLSET%
if errorlevel 1 (
  echo ERROR: foo_neobae build failed.
  popd >nul
  exit /b 1
)
echo.

rem ----- 4) Stage release package -----
echo [3/3] Staging release package...
if not exist "%RELEASE_DIR%" mkdir "%RELEASE_DIR%"

if not exist "%PLUGIN_OUT%\foo_neobae.dll" (
  echo ERROR: missing "%PLUGIN_OUT%\foo_neobae.dll"
  popd >nul
  exit /b 1
)
if not exist "%SCRIPT_DIR%lib\libneobae.dll" (
  echo ERROR: missing "%SCRIPT_DIR%lib\libneobae.dll"
  popd >nul
  exit /b 1
)

copy /Y "%PLUGIN_OUT%\foo_neobae.dll" "%RELEASE_DIR%\" >nul
copy /Y "%SCRIPT_DIR%lib\libneobae.dll" "%RELEASE_DIR%\" >nul
rem Post-build on the vcxproj also copies libneobae next to the plugin; keep staged set minimal.
if exist "%PLUGIN_OUT%\foo_neobae.pdb" copy /Y "%PLUGIN_OUT%\foo_neobae.pdb" "%RELEASE_DIR%\" >nul

echo.
echo ============================================================
echo Release ready: %RELEASE_DIR%
dir /b "%RELEASE_DIR%"
echo.
echo Install BOTH DLLs into your foobar2000 "components" folder:
echo   foo_neobae.dll
echo   libneobae.dll
echo.
echo Note: foobar SDK forces /MD; users who can run foobar already have the CRT.
echo Prefer TOOLSET=v143 if targeting older machines than your VS 2026 host.
echo ============================================================

popd >nul
endlocal
exit /b 0
