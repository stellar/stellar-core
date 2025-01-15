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

%set_linker_flags% & cd %project_dir%\src\rust\soroban\p21 & (set RUSTFLAGS=-Cmetadata=p21) & cargo +%version% build %release_profile% --package soroban-env-host --locked --target-dir %out_dir%\soroban-p21-target
%set_linker_flags% & cd %project_dir%\src\rust\soroban\p22 & (set RUSTFLAGS=-Cmetadata=p22) & cargo +%version% build %release_profile% --package soroban-env-host --locked --target-dir %out_dir%\soroban-p22-target
%set_linker_flags% & cd %project_dir%\src\rust\soroban\p23 & (set RUSTFLAGS=-Cmetadata=p23) & cargo +%version% build %release_profile% --package soroban-env-host --features next --locked --target-dir %out_dir%\soroban-p23-target
cd %project_dir% & cargo +%version% rustc %release_profile% --package stellar-core --locked %features% --target-dir %out_dir%\target -- --extern soroban_env_host_p21=%out_dir%\soroban-p21-target\%2\libsoroban_env_host.rlib --extern soroban_env_host_p22=%out_dir%\soroban-p22-target\%2\libsoroban_env_host.rlib --extern soroban_env_host_p23=%out_dir%\soroban-p23-target\%2\libsoroban_env_host.rlib -L dependency=%out_dir%\soroban-p21-target\%2\deps -L dependency=%out_dir%\soroban-p22-target\%2\deps -L dependency=%out_dir%\soroban-p23-target\%2\deps
