---
title: Debugging
---

This is a little howto on building stellar-core -- both its C++ and Rust components -- for debugging, and running it under an interactive debugger: specifically Visual Studio Code

## Build variants

  - The default build will create `stellar-core` with optimization _and_ debuginfo. The Rust soroban sub-crates may or may not have debuginfo enabled depending on whether, in their local `Cargo.toml` files, the `release` profile has `debug=true` in it. If not you may need to manually turn it on.

  - The default build can be made more debugging-friendly by turning _off_ optimization. This will allow the debugger to see more variables and functions, do less inlining and variable elimination.
    - For C++ code this involves overriding `CXXFLAGS` to contain `-O0` rather than `-O2` (and don't forget to keep `-g` for debugging!)
    - For Rust code this involves switching `RUST_PROFILE` to the `dev` profile.
    - So for both you will want something like: `make CXXFLAGS='-O0 -g' RUST_PROFILE=dev`

## Configuring VSCode for Debugging

  - Install the CodeLLDB extension. This will automatically download and install an appropriate `lldb` binary.

  - Run the menu command Run => Open Configurations (or otherwise edit the launch.json file) and set the configuration to the following:
    ```json
    {
        "version": "0.2.0",
        "configurations": [
            {
                "type": "lldb",
                "request": "launch",
                "name": "Debug",
                "program": "${workspaceFolder}/src/stellar-core",
                "args": ["test","--all-versions"],
                "env": {"ASAN_OPTIONS": "alloc_dealloc_mismatch=0"},
                "cwd": "${workspaceFolder}"
            }
        ]
    }
    ```

  - Adjust the `args` entry there to customize the core run you actually want to debug. Usually you will want to run it with a specific offline catchup configuration or a specific test.

## Configuring VSCode for automatic per-test execution and debugging

  - Install the "C++ TestMate" extension.

  - Add the following to your local settings.json file:
    ```json
    {
        "testMate.cpp.test.advancedExecutables": [
            {
                "pattern": "${workspaceFolder}/src/stellar-core",
                "catch2": {
                    "prependTestListingArgs": ["test"],
                    "prependTestRunningArgs": ["test"],
                    "ignoreTestEnumerationStdErr": true,
                    "testGrouping": {
                        "groupBySource": {}
                    }
                }
            }
        ]
    }
    ```

  - Click the testing tab on the far left hand panel of the editor (there is a little icon that looks like a beaker full of liquid: I think the idea is that this represents doing science in a chemistry lab?)

  - The primary sidebar should automatically populate with all the tests in `stellar-core` grouped by file, and if you hover over any of them there should be a little arrow button to run the test and an arrow with a little bug that will let you invoke the debugger on that test.

  - If you open one of core's unit test files (eg. `src/transactions/test/InvokeHostFunctionTests.cpp`) the individual tests should now also have a little arrow at the line declaring `TEST_CASE` and you can click that arrow to run it or right-click to get a menu of options including "debug test".