# Experiment 024: Tracy Zones for try_finish Breakdown

## Date
2026-02-21

## Hypothesis
The 5.4us `host try_finish` self-time in exp-023b is a mix of event
externalization and Host object destruction. Adding granular Tracy zones will
reveal the precise split and guide the next optimization.

## Change Summary
- Added Tracy zones inside `Host::try_finish`:
  - `externalize events` around the `externalize()` call
  - `drop host extract storage` around `Rc::try_unwrap` + HostImpl drop
- Added Tracy zones inside `InternalContractEvent::to_xdr`:
  - `convert event topics` around `vecobject_to_scval_vec`
  - `convert event data` around `from_host_val`
  - `convert contract id` around `hash_from_bytesobj_input`

## Results

### TPS
- Baseline (exp-023b): 14,656 TPS
- Post-change: 14,656 TPS
- Delta: 0 (diagnostic-only change)

### Tracy Analysis (per-TX self-time means)

#### try_finish breakdown (was 5.4us combined)
| Zone | Self (ns) | % of old total |
|------|-----------|----------------|
| drop host extract storage | **5,113** | **94%** |
| externalize events | 378 | 7% |
| host try_finish (overhead) | 183 | 3% |

#### Event externalization breakdown (was part of try_finish)
| Zone | Self (ns) |
|------|-----------|
| convert event topics | 90 |
| convert event data | 94 |
| convert contract id | 98 |
| (Val to ScVal children) | ~484 |
| Total externalize | ~1,144 |

#### Full Rust zone per-TX self-time ranking
| Zone | Self (ns) |
|------|-----------|
| drop host extract storage | 5,113 |
| Host::invoke_function | 4,548 |
| e2e_invoke::invoke_function | 4,583 |
| build storage map | 2,097 |
| host setup | 2,060 |
| invoke_host_function_or_maybe_panic | 1,808 |
| build footprint | 1,659 |
| get_ledger_changes | 1,240 |
| externalize events | 378 |
| encode_contract_events | 204 |

### Key Finding
**Host destruction dominates try_finish at 5.1us (94% of the zone).** Event
externalization is only 1.1us. The Host lifecycle (creation in `host setup` +
destruction in `drop host extract storage`) costs **7.2us per TX**. Caching
the Host in thread-local storage (similar to Budget caching in exp-022) could
save most of this.

### Cumulative Results (from exp-016e baseline)
- parallelApply: 130.8us -> 120.3us (-10.5us, -8.0%)
- No change from this diagnostic experiment

## Files Changed
- `src/rust/soroban/p25/soroban-env-host/src/host.rs` -- added Tracy zones
  to `try_finish`
- `src/rust/soroban/p25/soroban-env-host/src/events/internal.rs` -- added
  Tracy zones to `InternalContractEvent::to_xdr`
