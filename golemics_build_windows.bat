@echo off
REM Build the Golemics Blender fork natively on Windows (x64, Visual Studio).
REM
REM Usage:  golemics_build_windows.bat [make.bat arguments...]
REM   Arguments go straight to Blender's make.bat, e.g. "ninja", "2022", "debug".
REM   With no arguments, make.bat auto-detects Visual Studio and builds Release.
REM
REM Environment overrides:
REM   GOLEMICS_SRC     Source checkout directory (default C:\src\blender; no spaces).
REM   GOLEMICS_REPO    Git URL or path to clone from (default: the GitHub fork).
REM                    To clone straight from WSL instead:
REM                      set GOLEMICS_REPO=\\wsl.localhost\Ubuntu\home\tasinari\my_repos\blender
REM   GOLEMICS_BRANCH  Branch to build (default golemics).
REM
REM Prerequisites: Visual Studio 2022 with "Desktop development with C++",
REM Git for Windows, and Git LFS ("git lfs install" run once).
REM The first run downloads several GB of prebuilt libraries and takes hours.

setlocal EnableExtensions
if not defined GOLEMICS_SRC set "GOLEMICS_SRC=C:\src\blender"
if not defined GOLEMICS_REPO set "GOLEMICS_REPO=https://github.com/Teo-Asinari/blender.git"
if not defined GOLEMICS_BRANCH set "GOLEMICS_BRANCH=golemics"
set "SRC=%GOLEMICS_SRC%"
REM A repository on the WSL share is owned by another user; allow Git to read it.
set "GITC=git -c safe.directory=* -c core.longpaths=true"

echo [1/5] Checking prerequisites
where git >nul 2>&1 || (echo ERROR: git not found. Install Git for Windows. & exit /b 1)
git lfs version >nul 2>&1 || (echo ERROR: Git LFS not found. Install it from https://git-lfs.com, then run "git lfs install". & exit /b 1)
if not "%SRC%"=="%SRC: =%" (echo ERROR: GOLEMICS_SRC must not contain spaces; Blender's build scripts reject them. & exit /b 1)

echo [2/5] Getting %GOLEMICS_BRANCH% from %GOLEMICS_REPO% into %SRC%
if exist "%SRC%\.git" (
  %GITC% -C "%SRC%" fetch origin %GOLEMICS_BRANCH% || exit /b 1
  %GITC% -C "%SRC%" checkout %GOLEMICS_BRANCH% || exit /b 1
  %GITC% -C "%SRC%" merge --ff-only FETCH_HEAD || exit /b 1
) else (
  %GITC% clone --branch %GOLEMICS_BRANCH% "%GOLEMICS_REPO%" "%SRC%" || exit /b 1
  git -C "%SRC%" config core.longpaths true
)
cd /d "%SRC%" || exit /b 1

echo [3/5] Fetching prebuilt Windows libraries (lib\windows_x64)
REM Same steps as build_files\windows\check_libraries.cmd, without its y/n prompt.
if not exist "lib\windows_x64\.git" (
  git config --local submodule.lib/windows_x64.update checkout
  set GIT_LFS_SKIP_SMUDGE=1
  git submodule update --progress --init lib/windows_x64 || exit /b 1
  set GIT_LFS_SKIP_SMUDGE=
  git -C lib/windows_x64 lfs pull || exit /b 1
)

echo [4/5] Running make update
call make.bat update || exit /b 1

echo [5/5] Building: make.bat %*
call make.bat %* || exit /b 1

set "BLENDER_EXE="
for /d %%D in ("%SRC%\..\build_windows_*") do (
  for %%C in (Release RelWithDebInfo Debug .) do (
    if exist "%%~fD\bin\%%C\blender.exe" set "BLENDER_EXE=%%~fD\bin\%%C\blender.exe"
  )
)
if defined BLENDER_EXE (
  echo.
  echo Built: %BLENDER_EXE%
) else (
  echo.
  echo Build finished, but blender.exe was not found under %SRC%\..\build_windows_*
)
endlocal
