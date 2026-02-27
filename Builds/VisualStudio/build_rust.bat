:: Builds the Rust libraries for all the host versions.
:: Expects the first argument to be the path to the Visual Studio's `$OutDir` varible, the second argment to be `debug`/`release` for the respective build modes, and the third argument may be `curr`/`next` for vcurr/vnext builds of the host.
@echo off

set "project_dir=%~dp0..\.."
set "toolchain_file=%project_dir%\rust-toolchain.toml"
set "out_dir=%1\rust"
set features=
set release_profile=
set "set_linker_flags=cd ."
if "%2"=="debug" set "set_linker_flags=(set CFLAGS=-MDd) & (set CXXFLAGS=-MDd)"
if "%2"=="release" set "release_profile=--release"
if "%3"=="next" set "features=--features next"

if not exist "%toolchain_file%" (
    echo Error: "%toolchain_file%" not found.
    exit /b 1
)

:: Read the channel version from the file
for /f "tokens=2 delims==" %%A in ('findstr "channel" "%toolchain_file%"') do (
    set "version=%%~A"
)
:: Remove quotes from the version string
set "version=%version:~2,-1%"

if "%version%"=="" (
    echo Error: Failed to extract the toolchain channel version.
    exit /b 1
)

setlocal EnableDelayedExpansion

rem -- range to use for stable host envs
set MIN_P=21
set MAX_P=25
rem -- version of the latest WIP protocol
set LATEST_P=26

rem ---- Accumulators for final rustc link flags ----
set "EXTERNS="
set "LPATHS="

rem ---- Build historical protocols MIN_P..MAX_P (no %features%) ----
for /l %%P in (%MIN_P%,1,%MAX_P%) do (
  %set_linker_flags% & pushd "%project_dir%\src\rust\soroban\p%%P" & (set RUSTFLAGS=-Cmetadata=p%%P) & cargo +%version% build %release_profile% --package soroban-env-host --locked --target-dir "%out_dir%\soroban-p%%P-target" & popd

  set "EXTERNS=!EXTERNS! --extern soroban_env_host_p%%P=%out_dir%\soroban-p%%P-target\%2\libsoroban_env_host.rlib"
  set "LPATHS=!LPATHS! -L dependency=%out_dir%\soroban-p%%P-target\%2\deps"
)

echo rem ---- Build latest protocol (passes --features) ----
%set_linker_flags% & pushd %project_dir%\src\rust\soroban\p%LATEST_P% & (set RUSTFLAGS=-Cmetadata=p%LATEST_P%) & cargo +%version% build %release_profile% --package soroban-env-host --locked %features% --target-dir %out_dir%\soroban-p%LATEST_P%-target & popd

set "EXTERNS=%EXTERNS% --extern soroban_env_host_p%LATEST_P%=%out_dir%\soroban-p%LATEST_P%-target\%2\libsoroban_env_host.rlib"
set "LPATHS=%LPATHS% -L dependency=%out_dir%\soroban-p%LATEST_P%-target\%2\deps"

rem ---- Final stellar-core compile linking all protocol libs ----
cd /d "%project_dir%" & cargo +%version% rustc %release_profile% --package stellar-core --locked %features% --target-dir "%out_dir%\target" -- %EXTERNS% %LPATHS%

endlocal

