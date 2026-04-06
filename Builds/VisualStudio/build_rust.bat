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
set MAX_P=26
rem -- version of the latest WIP protocol
set LATEST_P=26

rem ---- Accumulators for final rustc link flags ----
set "EXTERNS="
set "LPATHS="
set "ALL_REVS="
set "SOURCE_STAMP=.source-rev"

rem ---- Build protocols MIN_P..MAX_P ----
rem When "next" is passed and LATEST_P falls within this range, skip it here
rem (it will be built once below with --features next).
for /l %%P in (%MIN_P%,1,%MAX_P%) do (
    set "proto_dir=%project_dir%\src\rust\soroban\p%%P"
    set "proto_target=%out_dir%\soroban-p%%P-target"

    rem -- Resolve current submodule rev --
    set "current_rev="
    for /f %%R in ('git -C "!proto_dir!" rev-parse HEAD 2^>nul') do set "current_rev=%%R"
    set "ALL_REVS=!ALL_REVS!p%%P-!current_rev:~0,12!_"

    rem -- Compare stamp to decide if cargo needs to run --
    set "stamp_ok="
    if not "!current_rev!"=="" (
        if exist "!proto_target!\%SOURCE_STAMP%" (
            set "saved_rev="
            set /p saved_rev=<"!proto_target!\%SOURCE_STAMP%"
            if /I "!saved_rev!"=="!current_rev!" set "stamp_ok=1"
        )
    )

    rem -- Decide whether to skip building this protocol in the loop --
    set "skip_build="
    if defined features if %%P==%LATEST_P% set "skip_build=1"

    if not defined skip_build (
        if defined stamp_ok (
            echo p%%P: up to date, skipping.
        ) else (
            echo p%%P: building soroban-env-host...
            %set_linker_flags% & pushd "!proto_dir!" & (set RUSTFLAGS=-Cmetadata=p%%P-!current_rev:~0,12!) & cargo +%version% build %release_profile% --package soroban-env-host --locked --target-dir "!proto_target!" & popd
            if errorlevel 1 exit /b 1
            if not "!current_rev!"=="" (
                if not exist "!proto_target!" mkdir "!proto_target!"
                >"!proto_target!\%SOURCE_STAMP%" echo(!current_rev!
            )
        )
    )

  if not defined skip_build (
    set "EXTERNS=!EXTERNS! --extern soroban_env_host_p%%P=%out_dir%\soroban-p%%P-target\%2\libsoroban_env_host.rlib"
    set "LPATHS=!LPATHS! -L dependency=%out_dir%\soroban-p%%P-target\%2\deps"
  )
)

rem ---- Build LATEST_P with features (only when "next" is passed) ----
if defined features (
    set "latest_proto_dir=%project_dir%\src\rust\soroban\p%LATEST_P%"
    set "latest_proto_target=%out_dir%\soroban-p%LATEST_P%-target"

    set "latest_rev="
    for /f %%R in ('git -C "!latest_proto_dir!" rev-parse HEAD 2^>nul') do set "latest_rev=%%R"

    set "latest_stamp_ok="
    if not "!latest_rev!"=="" (
        if exist "!latest_proto_target!\%SOURCE_STAMP%" (
            set "saved_latest_rev="
            set /p saved_latest_rev=<"!latest_proto_target!\%SOURCE_STAMP%"
            if /I "!saved_latest_rev!"=="!latest_rev!" set "latest_stamp_ok=1"
        )
    )

    if defined latest_stamp_ok (
        echo p%LATEST_P% ^(latest^): up to date, skipping.
    ) else (
        echo p%LATEST_P% ^(latest^): building soroban-env-host with %features%...
        %set_linker_flags% & pushd "!latest_proto_dir!" & (set RUSTFLAGS=-Cmetadata=p%LATEST_P%-!latest_rev:~0,12!) & cargo +%version% build %release_profile% --package soroban-env-host --locked %features% --target-dir "!latest_proto_target!" & popd
        if errorlevel 1 exit /b 1
        if not "!latest_rev!"=="" (
            if not exist "!latest_proto_target!" mkdir "!latest_proto_target!"
            >"!latest_proto_target!\%SOURCE_STAMP%" echo(!latest_rev!
        )
    )

    set "EXTERNS=!EXTERNS! --extern soroban_env_host_p%LATEST_P%=!latest_proto_target!\%2\libsoroban_env_host.rlib"
    set "LPATHS=!LPATHS! -L dependency=!latest_proto_target!\%2\deps"
)

rem ---- Final stellar-core compile ----
rem Clear RUSTFLAGS so that metadata from soroban-protocol builds above does
rem not leak into the stellar-core build and cause cargo to invalidate its
rem fingerprints on the next run (where the soroban builds may be skipped).
set "RUSTFLAGS="

rem Write submodule revisions to a file that build.rs watches via
rem cargo:rerun-if-changed.  Only update the file when content actually
rem changes so that the mtime (which cargo uses for freshness) stays
rem stable across no-op builds.
set "REVS_FILE=%project_dir%\src\rust\.soroban-revs"
set "revs_changed="
if exist "!REVS_FILE!" (
    set "saved_revs="
    set /p saved_revs=<"!REVS_FILE!"
    if not "!saved_revs!"=="!ALL_REVS!" set "revs_changed=1"
) else (
    set "revs_changed=1"
)
if defined revs_changed >"!REVS_FILE!" echo(!ALL_REVS!

rem Always invoke cargo here: cargo's own incremental-build tracking will
rem no-op quickly when nothing changed, and the .soroban-revs file
rem (tracked via build.rs) forces a rebuild when submodules change.
echo Building stellar-core Rust library...
%set_linker_flags% & cd /d "%project_dir%" & cargo +%version% rustc %release_profile% --package stellar-core --locked %features% --target-dir "%out_dir%\target" -- %EXTERNS% %LPATHS%

endlocal

