@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem Build libneobae.dll for the foobar2000 plugin (Windows / MSVC via CMake).
rem Output is copied into neobae\foobar2000\lib\ for the MSVC foo_neobae project.
rem
rem Architecture (must match foo_neobae + your foobar2000.exe):
rem   build_libneobae.bat [Win32|x64|x86]
rem   cmd:  set ARCH=Win32 ^| x64
rem   PowerShell:  $env:ARCH='x64'; .\build_libneobae.bat
rem                ^(plain "set ARCH=x64" does NOT work in PowerShell^)

set "SCRIPT_DIR=%~dp0"
rem Trailing backslash from %~dp0
set "ROOT=%SCRIPT_DIR%..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "LIB_OUT=%SCRIPT_DIR%lib"

if not "%~1"=="" (
  if /I "%~1"=="Win32" set "ARCH=%~1"
  if /I "%~1"=="x86"   set "ARCH=Win32"
  if /I "%~1"=="x64"   set "ARCH=%~1"
)

if not defined ARCH set "ARCH=Win32"
if /I "%ARCH%"=="x86" set "ARCH=Win32"
if /I "%ARCH%"=="Win32" (
  set "ARCH_TAG=win32"
) else if /I "%ARCH%"=="x64" (
  set "ARCH_TAG=x64"
) else (
  echo ERROR: ARCH must be Win32 or x64 ^(got "%ARCH%"^)
  exit /b 1
)

rem Separate build trees per arch — CMake cannot retarget -A in-place.
if not defined BUILD_DIR set "BUILD_DIR=%ROOT%\build-foobar2000-%ARCH_TAG%"

echo ROOT=%ROOT%
echo ARCH=%ARCH%
echo BUILD_DIR=%BUILD_DIR%
echo LIB_OUT=%LIB_OUT%
echo.
echo Building 32-bit? Use:  ARCH=Win32  and msbuild ... /p:Platform=x86
echo Building 64-bit? Use:  ARCH=x64    and msbuild ... /p:Platform=x64
echo.

rem Keep /MD (default) so CRT matches foo_neobae + foobar SDK. /MT is not
rem viable for the plugin because libPPUI.lib is built MD_DynamicRelease.
cmake -S "%ROOT%" -B "%BUILD_DIR%" -A %ARCH% ^
  -DBAE_PLATFORM=foobar2000 ^
  -DNEOBAE_STATIC=ON ^
  -DNEOBAE_SHARED_LIBNEOBAE=ON ^
  -DNEOBAE_MSVC_STATIC_RUNTIME=OFF ^
  -DNEOBAE_BUILD_VCLIB=ON ^
  -DBUILD_PLAYBAE=OFF ^
  -DBUILD_ZEFIDI=OFF ^
  -DBUILD_CLITOOLS=OFF ^
  -DBUILD_NBEDITOR=OFF ^
  -DBUILD_NBSTUDIO=OFF ^
  -DBAE_DISABLE_MP3_ENCODER=ON ^
  -DBAE_DISABLE_VORBIS_ENCODER=ON ^
  -DBAE_DISABLE_OPUS_ENCODER=ON ^
  -DBAE_DISABLE_FLAC_ENCODER=ON ^
  %*
if errorlevel 1 exit /b 1

cmake --build "%BUILD_DIR%" --target neobae --config Release -j %NUMBER_OF_PROCESSORS%
if errorlevel 1 exit /b 1

if not exist "%LIB_OUT%" mkdir "%LIB_OUT%"

set "COPIED_DLL=0"
set "COPIED_LIB=0"

rem Preferred name (OUTPUT_NAME=libneobae) and legacy MSVC name (neobae.*)
for %%D in (
  "%BUILD_DIR%\bin\Release"
  "%BUILD_DIR%\Release"
  "%BUILD_DIR%\bin"
  "%BUILD_DIR%"
) do (
  if exist "%%~D\libneobae.dll" (
    copy /Y "%%~D\libneobae.dll" "%LIB_OUT%\libneobae.dll" >nul
    echo Copied %%~D\libneobae.dll -^> %LIB_OUT%\libneobae.dll
    set "COPIED_DLL=1"
  ) else if exist "%%~D\neobae.dll" (
    copy /Y "%%~D\neobae.dll" "%LIB_OUT%\libneobae.dll" >nul
    echo Copied %%~D\neobae.dll -^> %LIB_OUT%\libneobae.dll
    set "COPIED_DLL=1"
  )

  if exist "%%~D\libneobae.lib" (
    copy /Y "%%~D\libneobae.lib" "%LIB_OUT%\libneobae.lib" >nul
    echo Copied %%~D\libneobae.lib -^> %LIB_OUT%\libneobae.lib
    set "COPIED_LIB=1"
  ) else if exist "%%~D\neobae.lib" (
    copy /Y "%%~D\neobae.lib" "%LIB_OUT%\libneobae.lib" >nul
    echo Copied %%~D\neobae.lib -^> %LIB_OUT%\libneobae.lib
    set "COPIED_LIB=1"
  )
)

if "%COPIED_DLL%"=="0" (
  echo ERROR: neobae/libneobae.dll not found under "%BUILD_DIR%"
  exit /b 1
)
if "%COPIED_LIB%"=="0" (
  echo ERROR: neobae/libneobae.lib not found under "%BUILD_DIR%"
  echo The import library is required to link foo_neobae against libneobae.dll.
  exit /b 1
)

echo.
echo Contents of %LIB_OUT%:
dir /b "%LIB_OUT%"
echo.
echo Next: build foo_neobae as Release ^| %ARCH% / x86 to match this DLL.

endlocal
exit /b 0
