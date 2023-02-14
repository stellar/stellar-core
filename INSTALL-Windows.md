# Windows Installation Instructions

## Make sure you have a 64 bit version of Windows
* try Settings -> System -> About and look under System Type
* When in doubt, see http://www.tenforums.com/tutorials/4399-system-type-32-bit-x86-64-bit-x64-windows-10-a.html

## Download and install `Visual Studio 2022` (for Visual C++) ; the Community Edition is free and fully functional.
* See https://www.visualstudio.com/downloads/
* When installing, you will need to select:
    * Desktop Development with C++
    * C++ Profiling (optional)
    * Windows 10 SDK
    * C++/CLI Support

# Configure Git

Some functionality depends on Linux file endings, as such you need to configure git with:
```
[core]
        eol = lf
        autocrlf = input
```
NB: if you change this setting in an existing working folder, you need to reset it to have the proper line ending.
This can be done with the following sequence:
```
git checkout-index --force --all
git rm --cached -r .
git reset --hard
```

## Download and install rust

Easiest is to use rustup, found on [rust-lang.org](https://www.rust-lang.org/tools/install).

Install the x64 MSVC toolchain (`stable-x86_64-pc-windows-msvc`), and make sure that `rustc.exe` is in your `PATH`.

## Download and install PostgreSQL

Note: if you do not want to use postgres you can select `DebugNoPostgres` as the build target.

Get version 15 from https://www.enterprisedb.com/download-postgresql-binaries

The default project file defines USE_POSTGRES and links against it.
* Pick a directory for the database
* Set an admin password for the database
* Accept the default port (5432)
* Accept `default` for the locale (not clear if anything depends on this. The `default` locale will
presumably depend on your operating system's setting might cause inconsistencies)
* Add `c:\Program Files\PostgreSQL\15\lib` to your PATH (else the binary will fail to start,
    not finding `libpq.dll`)
* If you install postgres in a different folder, you will have to update the project file in two places:
    * "additional include locations" and
    * "Linker input"

> If the installation fails, look into `%TEMP%\install-postgresql.log` for hints.

## Building xdrc
 In order to compile xdrc and run the binary you will need to either
* Download and install MinGW from http://sourceforge.net/projects/mingw/files/
    * In the MinGW Installation Manager in `MSYS/MinGW Developer Toolkit` select the following packages:
      * `Flex`
      * `Bison`
      * `gcc`
      * `sed`
      * `perl`
      * `curl`
    * Add `C:\MinGW\msys\1.0\bin;C:\MinGW\bin` to the end of `%PATH%`
* Download and install cygwin 64 bit build from https://cygwin.com/install.html
    * Get cygwin setup to install
        * `Flex`
        * `Bison`
        * `curl` (command line)
        * `gcc-core`
        * `sed`
        * `perl`
    * Add `c:\cygwin64\bin` to the end of `%PATH%` (at least for Visual Studio)

    > Note: if you're going to use 'cp'and 'mkdir' from cygwin (tests do),
        make sure that your install is correct by trying to copy a
        file from a `cmd.exe` console (not from a cygwin terminal).
        `cp in.txt out.txt` and then try to open *out.txt* with
        notepad. You should not get a permission denied error.

    > Note: both MinGW and CygWin may run into virtual memory address space
    conflicts on modern versions of Windows. You will run into errors like
    `Couldn't reserve space for mingw's heap, Win32 error 0`.
    A workaround is to reboot until you can run `bison.exe` from a cmd.exe
    prompt.

## Curl
If you do not have cURL installed

* Download and install/extract cURL from https://curl.haxx.se/download.html#Win64
* Add installation/extraction directory (e.g. `C:\Program Files\curl_7_47_1_x64`) to the end of `%PATH%`

## clang-format

For making changes to the code, you should install the clang-format tool and Visual Studio extension, you can find both at http://llvm.org/builds/
* note that the version of clang-format used currently is 12.0 (other versions may not format the same way).
* we recommend downloading 12.0 from http://releases.llvm.org/download.html

# Build on Windows using the Windows Subsystem for Linux
To setup the subsystem, go to https://msdn.microsoft.com/en-us/commandline/wsl/install_guide

Then, you can simply follow the [Linux instructions](./README.md)

Note that you can (and should) install the Windows version of postgres even when running stellar-core from within WSL.


# Basic Installation

- `git clone PATH_TO_STELLAR_CORE`
- `git submodule init`
- `git submodule update`
- Open the solution `Builds\VisualStudio\stellar-core.sln`
- Pick the target architecture and flavor (e.g. x64, Release)
- Hit "Build Solution (F7)"
