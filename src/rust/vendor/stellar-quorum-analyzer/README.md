# FBAS Quorum Intersection Analyzer

A library for analyzing quorum intersection properties in Federated Byzantine Agreement Systems (FBAS), specifically designed for the Stellar Consensus Protocol (SCP).

## Overview

This analyzer uses a SAT solver to verify the quorum intersection property of FBAS networks. In SCP, the quorum intersection property (any two quorums must share at least one node) is crucial for network safety. The library can detect potential network splits by finding disjoint quorums if they exist.

## Use Cases

- **Primary**: Integrated with stellar-core for runtime quorum analysis during network operations (validator joins, departures, or configuration changes)
- **Secondary**: Experimental analysis of network configurations via JSON input

## Features

- SAT solver-based analysis of quorum intersection properties
- Support for XDR-serialized quorum set maps via buffer interface
- JSON-based quorum set map input (optional, requires `json` feature)

## Building and Testing

- `cargo build --release`

- `cargo build --features json`

- `cargo test`

Test cases can be found in the `tests/test_data` directory. `tests_date/random` directory contains randomly generated configurations up to 16 organizations.

## Performance

Performance benchmarks comparing different SAT solvers are available in the `benches` directory. After evaluating multiple pure-Rust SAT solvers against the test cases, Batsat was chosen as the primary solver for its performance and features (e.g. async interrupt).

To run benchmarks:

- `cargo bench`


## Documentation

- `docs/methods.md`: Contains detailed derivation of the methodology and SAT formulas used in the analyzer
- API documentation: `cargo doc --open`

## Usage

### As a Library

```rust
use fbas_analyzer::{FbasAnalyzer, Basic};
// From XDR-serialized buffer
let analyzer = FbasAnalyzer::from_quorum_set_map_buf(nodes, quorum_sets, Basic::default())?;
let result = analyzer.solve();
// From JSON (requires 'json' feature)
let analyzer = FbasAnalyzer::from_json_path("quorum_map.json", Basic::default())?;
let result = analyzer.solve();
// Get potential split information
if let Ok((quorum_a, quorum_b)) = analyzer.get_potential_split() {
    // Process split information
}
```

## Input Formats

- **Buffer Interface**: Primary method for stellar-core integration, accepts XDR-serialized quorum maps
- **JSON**: Alternative input method for configuration testing (requires `json` feature)

## Future Work

- Command-line interface for JSON-based quorum analysis