:: Generates rust bridge header and source files in the src directory.
:: Expects the first argument to be the command line for `cxxbridge.cpp`
@echo off

set "out_dir=src\generated\rust"
set "temp_header=%out_dir%\temp\RustBridge.h"
set "temp_source=%out_dir%\temp\RustBridge.cpp"
set "final_header=%out_dir%\RustBridge.h"
set "final_source=%out_dir%\RustBridge.cpp"

mkdir "%out_dir%\temp" >nul 2>nul

%1 ..\..\src\rust\src\lib.rs --cfg test=false --header --output "%temp_header%"
if %errorlevel% neq 0 (
    echo Error generating temporary header file.
    exit /b %errorlevel%
)

%1 ..\..\src\rust\src\lib.rs --cfg test=false --output "%temp_source%"
if %errorlevel% neq 0 (
    echo Error generating temporary source file.
    exit /b %errorlevel%
)

fc /b "%temp_header%" "%final_header%" >nul 2>nul
if %errorlevel% neq 0 (
    copy /y "%temp_header%" "%final_header%" >nul
)

fc /b "%temp_source%" "%final_source%" >nul 2>nul
if %errorlevel% neq 0 (
    copy /y "%temp_source%" "%final_source%" >nul
)

del "%temp_header%" >nul 2>nul
del "%temp_source%" >nul 2>nul

:: Make sure we exit without an error
exit 0