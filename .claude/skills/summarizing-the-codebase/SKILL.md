---
name: "regenerating a technical summary of stellar-core"
description: "Instructions for regenerating the full set of subsystem and whole-system technical summary skill documents for stellar-core"
---

# Regenerating Technical Summaries of stellar-core

## Overview

This skill describes the process of generating (or regenerating) the full set of technical summary skill documents for stellar-core. These summaries provide AI agents with compact architectural context about each subsystem and the system as a whole.

## Output Artifacts

The process produces:
- 21 subsystem summary files in `.claude/skills/subsystem-summary-of-$X/SKILL.md`
- 1 whole-system summary file in `.claude/skills/stellar-core-summary/SKILL.md`

## Subsystem List

The following subsystem directories under `src/` are each summarized individually:

| Subsystem | Source Directory | Notes |
|-----------|-----------------|-------|
| bucket | `src/bucket/` | Bucket list and merge machinery |
| catchup | `src/catchup/` | Ledger catchup / sync logic |
| crypto | `src/crypto/` | Hashing, signing, key management |
| database | `src/database/` | SQL database abstraction |
| herder | `src/herder/` | Consensus coordination, tx queue |
| history | `src/history/` | History archive management |
| historywork | `src/historywork/` | Work tasks for history operations |
| invariant | `src/invariant/` | Runtime invariant checking |
| ledger | `src/ledger/` | Ledger state management, LedgerTxn |
| main | `src/main/` | Application, Config, event loop |
| overlay | `src/overlay/` | P2P network layer |
| process | `src/process/` | Child process management |
| protocol-curr | `src/protocol-curr/` | Current protocol XDR definitions |
| rust | `src/rust/` (excl. soroban/) | C++/Rust bridge, non-soroban Rust |
| soroban-env | `src/rust/soroban/p26/soroban-env-{common,host}/` | One protocol version of soroban env |
| scp | `src/scp/` | SCP consensus protocol |
| simulation | `src/simulation/` | Network simulation, load generation |
| test | `src/test/` | Test utilities and infrastructure |
| transactions | `src/transactions/` | Transaction/operation processing |
| util | `src/util/` | Utility classes and helpers |
| work | `src/work/` | Async work scheduling framework |

## Procedure

### Phase 1: Generate Subsystem Summaries (Parallelizable)

For each subsystem directory listed above, launch a parallel sub-agent with the following instructions:

1. **Read the entire source** of all `.h`, `.cpp`, and/or `.rs` files in that subsystem directory, **excluding** any test files (files with "test" or "Test" in the name, or files ending in `Tests.cpp`, `Test.cpp`, `test.cpp`).

2. **Create a skill document** at `.claude/skills/subsystem-summary-of-$X/SKILL.md`

3. **Write the document** using this template:

```markdown
---
name: "subsystem-summary-of-$X"
description: "read this skill for a token-efficient summary of the $X subsystem"
---

# Subsystem: $X

## Key Classes and Data Structures
...

## Key Modules
...

## Key Functions
...

## Control Loops, Threads, and Tasks
...

## Ownership Relationships
...

## Key Data Flows
...
```

The summary should be approximately **10KB** and focus on:
- Key classes / data structures with brief descriptions
- Key modules and their responsibilities
- Key functions within major classes
- Summary of key control loops, threads and/or tasks
- Ownership relationships between data structures
- Key data flows between components

### Special Cases

- **protocol-curr**: Focus on XDR type definitions, enumerations, unions, and type relationships.
- **rust** (non-soroban): Include the C++/Rust bridge mechanism (CXX bridge), files in `src/rust/src/` and `src/rust/*.h`/`*.cpp`, but exclude `src/rust/soroban/`.
- **soroban-env**: Read from one protocol version directory (currently `p26`), specifically `soroban-env-common/src/` and `soroban-env-host/src/`. Cover Val types, Env traits, Host internals, budget/metering, storage model, and VM dispatch.
- **test**: This contains test *utilities* and *infrastructure*, not tests themselves. Read everything but still skip files that are purely individual test cases (`*Tests.cpp`).

### Phase 2: Generate Whole-System Summary

After all subsystem summaries are complete, launch a sub-agent that:

1. **Reads ALL 21 subsystem summary files** together
2. **Creates** `.claude/skills/stellar-core-summary/SKILL.md`
3. **Writes a ~30KB summary** covering:
   - System overview (what stellar-core is)
   - Architecture overview (component relationships)
   - Core subsystems (concise paragraph per subsystem)
   - Threading model (all thread types across subsystems)
   - Key data flows (transaction lifecycle, ledger close, catchup, history publication, overlay messaging)
   - Ownership hierarchy (Application → managers)
   - Cross-cutting concerns (protocol versioning, invariants, metrics, logging, process management, work scheduling)
   - Soroban integration (Rust/C++ FFI, Host runtime)
   - Testing infrastructure
   - Key design patterns (recurring patterns across the codebase)

The description should indicate this is good initial context for any broad-scope task on stellar-core.

## When to Regenerate

Regenerate summaries when:
- Major architectural changes are made to a subsystem
- New subsystems are added
- Significant refactoring occurs across multiple subsystems
- The soroban protocol version advances (update the `p26` reference to the latest)

## Notes

- The soroban protocol version directory (currently `p26`) should be updated to the latest version when regenerating.
- Each subsystem summary targets ~10KB; the whole-system summary targets ~30KB.
- All summaries use YAML frontmatter with `name:` and `description:` fields.
- The goal is to provide enough context for an AI agent to understand the architecture without reading all the source code.
