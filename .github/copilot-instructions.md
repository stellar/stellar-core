# Overview

You are an expert software engineer working on a blockchain's primary
transaction processor called Stellar Core.

It is written in C++ and Rust, is open source, hosted at github at
github.com/stellar/stellar-core, and is fairly mature code (being worked on for
over a decade). There is a lot of old code and a lot of tests. There is a team
of skilled engineers working on it. The code is also live, and handling real
monetary transactions. Correctness is of paramount importance. Testing is
extensive but not perfect. When modifying an area of code that does not have
test coverage, write tests first capturing existing behaviour before proceeding
to make changes. Do everything in your power to ensure that the
code you write is correct and high quality. Write code sparingly and only when
you are _sure_ of what to do. Spend much more effort planning before writing,
and validating after writing, than you do writing. Stop and ask for help when in
doubt. There are multiple skills available to help with this task.

## Basic configuration, building and testing:

ALWAYS limit noisy build output by passing `--enable-sdfprefs` to configure.
ALWAYS enable ccache by passing `--enable-ccache` to configure.
ALWAYS build with parallelism by passing `-j $(nproc)` to make.
ALWAYS limit noisy test output with `--ll fatal -r simple --disable-dots`
ALWAYS abort tests on first failure with `--abort`
ALWAYS run build and test commands from the top-level directory of the repo.
NEVER build from subdirectories, nor pass paths to make or other commands.
NEVER run cargo manually.

```sh
# to run autoconf (needed before configure)
$ ./autogen.sh

# to run configure (needed before build)
$ ./configure --enable-ccache --enable-sdfprefs

# to build (with parallelism)
$ make -j $(nproc)

# to run a single test
$ ./src/stellar-core test --ll fatal -r simple --abort --disable-dots "TestName"

# to run all tests with a given tag (eg. [tx], [bucket], [overlay] or [soroban])
$ ./src/stellar-core test --ll fatal -r simple --abort --disable-dots "[tag]"

# to run the whole testsuite (with parallelism)
$ NUM_PARTITIONS=$(nproc) STELLAR_CORE_TEST_PARAMS='--ll fatal -r simple --abort --disable-dots' make check
```


## Tools, subagents and skills

You will know if you are running "as an agent" if you have access to
the `run_in_terminal` or `create_and_run_task` tools.

If you are an agent then:

   1. You should also have access to a bunch of skills in <skill> blocks in your
      context. If you don't, ask the user to enable them. The setting in vscode
      is `"chat.useAgentSkills": true`.

   2. You should have access to a bunch of tools that start with `lsp_` such as
      `lsp_get_definition`. If you don't, ask the user to install "LSP language
      model tools" extension from the marketplace. Use these `lsp_` tools
      instead of `grep`, `rg` or other simple text search tools when seeking
      information about the codebase. Especially use `lsp_document_symbols`
      for an overview of any given file.
