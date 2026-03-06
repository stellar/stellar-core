```skill
---
name: "pruning-headers"
description: "How to prune unnecessary #include directives from C++ headers and source files in stellar-core, using clangd diagnostics."
---

# Pruning Headers in stellar-core

## Overview

Unnecessary `#include` directives increase compile times and create hidden
coupling between translation units. This skill describes how to systematically
remove them using clangd's Include Cleaner diagnostics, forward declarations,
and moving implementations out of headers.

## Prerequisites

1. **clangd-lsp plugin**: If you are running as Claude Code You MUST have the
   `clangd-lsp` Claude Code plugin installed. If it is not installed, prompt
   the user to install it:
   ```
   /install-plugin clangd-lsp@claude-plugins-official
   ```
   Do NOT attempt to invoke clangd manually via subprocess or script. The plugin
   provides diagnostics automatically after each Edit tool call. Similarly if
   you are running as copilot in VS Code, you must have the clangd extension
   installed and configured and the `graydon/lsp-lm-tools` extension to expose
   diagnostics in the editor.

2. **Homebrew clangd**: The plugin needs clangd on `$PATH`. The recommended
   version is Homebrew's (`/opt/homebrew/opt/llvm/bin/clangd`, version 21+)
   which has Include Cleaner support. Apple's system clangd may be too old.

3. **`compile_commands.json`**: clangd needs this in the project root to
   understand build flags. Generate it with:
   ```
   bear -- make -j $(nproc)
   ```
   (`bear` is at `/opt/homebrew/bin/bear`). Ask the user to generate this if
   it does not exist.

## How the Plugin Works

The clangd-lsp plugin reports diagnostics as `<new-diagnostics>` blocks in
system reminders **after Edit tool calls**. Diagnostics are NOT produced on
Read tool calls. To trigger diagnostics for a file you must make an edit to it.

The relevant diagnostic code is `[unused-includes]` from clangd. Example:

```
⚠ [Line 6:1] Included header Hex.h is not used directly (fix available) [unused-includes] (clangd)
```

**Important**: diagnostics sometimes lag by one edit. The diagnostics you see
after edit N may actually correspond to the state after edit N-1. Always check
whether reported line numbers and filenames match your current edit.

## Approach: Three Passes

### Pass 1 — Remove Unused Includes

For each `.cpp` and `.h` file in the target directory:

1. Read the file to understand its includes.
2. Make a trivial edit (e.g., remove then re-add a blank line after the
   copyright header) to trigger clangd diagnostics.
3. Read the `[unused-includes]` diagnostics from the system reminder.
4. Remove the flagged includes.
5. Revert the trivial formatting change (restore the blank line).

After processing all files, do a full build from the **top-level repo
directory**:
```
make -j $(nproc)
```

If a file fails to compile, the removal was a false positive. Add the include
back and rebuild.

### Pass 2 — Move Implementations from Headers to Source Files

Headers should ideally contain only declarations, not implementations. Moving
function bodies, static data, and complex inline code out of `.h` files and
into `.cpp` files is one of the most effective ways to reduce transitive
include load — it often makes entire heavy includes unnecessary in the header.

**What to move:**

- Non-template function bodies (move to `.cpp`, leave declaration in `.h`)
- Static/global variable definitions (move to `.cpp`, leave `extern` decl or
  accessor function in `.h`)
- Complex inline functions that pull in heavy headers (move to `.cpp`)
- Private implementation details that callers don't need to see

**What must stay in headers:**

- Template definitions — UNLESS you use explicit instantiation in the `.cpp`
  file, which is encouraged when the set of template arguments is known and
  finite (e.g., `template class BinaryFuseFilter<uint8_t>;`)
- `constexpr` / `consteval` functions
- Truly trivial inline accessors (e.g., `int size() const { return mSize; }`)
- Type definitions, enums, constants needed by callers

**Creating new `.cpp` files:** If a header has no corresponding `.cpp` file,
create one. Add it to git (`git add`), then run `./make-mks` from the
top-level repo directory — this script populates the source list variables
used by automake. Do NOT modify `Makefile.am` or other automake files
yourself.

**Workflow:**

1. Read the header and identify function bodies that can move to `.cpp`.
2. Move the bodies, leaving only declarations in the header.
3. Remove any includes from the header that were only needed for the moved
   implementations.
4. Add those includes to the `.cpp` file instead.
5. Rebuild and fix any transitive-inclusion breakage in other files.

**Example — before:**
```cpp
// Foo.h
#include "Bar.h"      // heavy header
#include <algorithm>

class Foo {
  public:
    void doWork() {
        Bar b;
        std::sort(b.begin(), b.end());  // needs Bar.h + algorithm
    }
};
```

**Example — after:**
```cpp
// Foo.h
class Bar;  // forward declaration suffices now
class Foo {
  public:
    void doWork();
};

// Foo.cpp
#include "Foo.h"
#include "Bar.h"
#include <algorithm>

void Foo::doWork() {
    Bar b;
    std::sort(b.begin(), b.end());
}
```

### Pass 3 — Replace Remaining Header Includes with Forward Declarations

After moving implementations out, examine what remains in each header. For any
included type that is now only used in a pointer, reference, or function
declaration context, replace the `#include` with a forward declaration.

**When to forward-declare vs. include:**

- **Forward-declare** when the header only uses a type as:
  - A pointer or reference (`Foo*`, `Foo&`, `std::unique_ptr<Foo>`)
  - A function parameter or return type (by pointer/reference)
  - A template parameter where the template doesn't need the full definition
  - A friend declaration

- **Must include** when the header:
  - Inherits from the type (`class Bar : public Foo`)
  - Has a member of that type by value (`Foo mFoo;`)
  - Calls methods on the type inline
  - Uses `sizeof(Foo)` or accesses members
  - Uses the type as a template argument where the template needs the full
    definition (e.g., `std::vector<Foo>` needs the full type)

**Workflow:**

1. Replace the `#include` with a forward declaration of the needed type.
2. Add the removed `#include` to every `.cpp` file that was relying on the
   transitive inclusion (these will show up as compilation errors).
3. Rebuild to verify.

**Prefer changing return types over keeping includes.** If a function in a
header returns a type by value and that's the only reason for a heavy include,
consider whether the return type can be changed to a standard type. For
example, if `VirtualClock::duration` is really just
`std::chrono::steady_clock::duration`, use the latter directly to avoid
pulling in `Timer.h`.

**Common forward-declaration patterns in stellar-core:**

```cpp
// Forward-declare XDR types
struct LedgerEntry;
struct LedgerKey;
struct TransactionMeta;

// Forward-declare project classes
class Application;
class Config;
class VirtualClock;

// Forward-declare in namespaces
namespace medida { class MetricsRegistry; }
```

## Known False Positives

clangd's Include Cleaner sometimes flags includes that are actually needed:

- **`util/asio.h`** in headers that use `asio::` types — clangd may not see
  the usage if it's in platform-specific `#ifdef` blocks.
- **`util/SpdlogTweaks.h`** in `Logging.h` — this defines macros that must
  be set before including spdlog headers. clangd doesn't track macro deps.
- **XDR headers** like `Stellar-ledger.h` — types may be used through
  transitive XDR includes that clangd doesn't fully resolve.
- **`medida/metrics_registry.h`** — may provide base classes that clangd
  doesn't always trace through inheritance.

When in doubt, remove the include, rebuild, and re-add if it fails.

## Build Verification

Always build from the **top-level of the repo** (not a subdirectory):

```
make -j $(nproc)
```

Do NOT use bare `make -j` (no job count) as it may spawn too many processes.

If the build fails due to a missing type after a header include was removed,
this means some other file was relying on transitive inclusion. Fix it by
adding the needed `#include` directly to the file that uses the type.

## Scope Conventions

- Process one directory at a time (e.g., `src/util/`, `src/crypto/`).
- Skip `test/` subdirectories in the initial pass.
- Commit after each directory is clean.
```
