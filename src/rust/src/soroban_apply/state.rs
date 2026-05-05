// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

//! Canonical in-memory Soroban state, owned by Rust. Replaces the C++
//! `InMemorySorobanState`. The `SorobanState` struct is the cxx-bridged
//! object that lives across ledger closes; the typed CRUD methods are
//! consumed by the apply phase, while the `_xdr` variants (and the
//! initialization helpers) are the entry points the C++ shim calls.

use std::fs::File;
use std::io::{BufReader, Read};

use crate::soroban_proto_all::soroban_curr::soroban_env_host::xdr::{
    BucketEntry, Hash, LedgerEntry, LedgerEntryData, LedgerEntryExt, LedgerKey, Limits, ReadXdr,
    TtlEntry, WriteXdr,
};

use super::common::{
    compute_contract_code_size_for_rent, ledger_entry_key, ttl_key_hash_for, xdr_serialized_size,
    FastMap, FastSet, TtlKeyHash,
};
use crate::{CxxBuf, RustBuf};

// First protocol that has Soroban entry types (CONTRACT_DATA, CONTRACT_CODE,
// TTL). On older protocols, SorobanState init is a no-op.
const SOROBAN_PROTOCOL_VERSION: u32 = 20;

// Iterate XDR-frame-marked BucketEntry records from a single bucket file,
// matching C++ `XDRInputFileStream::readOne()`.
//
// Frame format (RFC 5531 record marking, single fragment per record as
// produced by stellar-core):
//   - 4-byte big-endian header: bit 31 = last-fragment flag, bits 0..30 =
//     fragment byte length.
//   - `length` bytes of XDR-serialized BucketEntry follow.
//
// stellar-core only writes single-fragment records (the high bit is always
// set), so we don't need to concatenate continuation fragments.
//
// Returns an iterator that lazily reads + deserializes records as it goes.
fn read_bucket_entries(path: &str) -> impl Iterator<Item = BucketEntry> {
    let file = match File::open(path) {
        Ok(f) => f,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            // Empty bucket levels can correspond to non-existent files
            // (e.g. snap(i) before level i has filled). Treat as no entries.
            return Box::new(std::iter::empty()) as Box<dyn Iterator<Item = BucketEntry>>;
        }
        Err(e) => panic!("read_bucket_entries: open {}: {}", path, e),
    };
    let mut reader = BufReader::new(file);
    let mut buf = Vec::new();
    Box::new(std::iter::from_fn(move || {
        let mut header = [0u8; 4];
        match reader.read_exact(&mut header) {
            Ok(()) => {}
            Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => return None,
            Err(e) => panic!("read_bucket_entries: header read error: {}", e),
        }
        // Mask the last-fragment flag bit (top bit of the high byte).
        header[0] &= 0x7f;
        let len = u32::from_be_bytes(header) as usize;
        if buf.len() < len {
            buf.resize(len, 0);
        }
        reader
            .read_exact(&mut buf[..len])
            .expect("read_bucket_entries: fragment truncated");
        let entry = BucketEntry::from_xdr(&buf[..len], Limits::none())
            .expect("read_bucket_entries: malformed BucketEntry");
        Some(entry)
    }))
}

// TTL bookkeeping co-located with the entry it applies to. Mirrors the C++
// `TTLData` struct — kept here as a plain pair of u32s rather than a serialized
// TTL LedgerEntry so we can answer TTL queries without re-parsing XDR.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct TtlData {
    pub live_until_ledger_seq: u32,
    pub last_modified_ledger_seq: u32,
}

impl TtlData {
    pub fn new(live_until_ledger_seq: u32, last_modified_ledger_seq: u32) -> Self {
        Self {
            live_until_ledger_seq,
            last_modified_ledger_seq,
        }
    }

    // Mirrors `TTLData::isDefault()` — returns true iff both fields are zero.
    // Asserts the C++ invariant that the two fields are either both zero or
    // both non-zero (a half-populated TtlData is a bug).
    pub fn is_default(&self) -> bool {
        if self.live_until_ledger_seq == 0 {
            assert_eq!(
                self.last_modified_ledger_seq, 0,
                "TtlData with zero live_until_ledger_seq must also have zero last_modified"
            );
            true
        } else {
            assert_ne!(
                self.last_modified_ledger_seq, 0,
                "TtlData with non-zero live_until_ledger_seq must also have non-zero last_modified"
            );
            false
        }
    }
}

// One stored CONTRACT_DATA entry. The LedgerEntry is held as the canonical
// (latest-protocol) typed `stellar_xdr::curr::LedgerEntry`; older pinned
// soroban-env-host versions decode bytes serialized from this type without
// trouble thanks to XDR wire-format backwards compatibility.
//
// `size_bytes` is the cached size used by rent accounting. For CONTRACT_DATA
// this is just `xdr_size(entry)`; for CONTRACT_CODE it's a protocol-version-
// aware size that includes the parsed-module memory footprint.
pub struct ContractDataEntry {
    pub ledger_entry: LedgerEntry,
    pub ttl_data: TtlData,
    pub size_bytes: u32,
}

// One stored CONTRACT_CODE entry. Same shape as ContractDataEntry; the
// `size_bytes` semantics differ as described above. `xdr_size_bytes`
// is a separate cache of the entry's pure XDR size (without the
// in-memory module memory cost) — needed by the host-input cap check
// in `apply_invoke_host_function` which compares the wire size of the
// entry against `max_contract_size_bytes`. Stored separately because
// `size_bytes` includes parsed-WASM memory cost on protocol >= 23.
pub struct ContractCodeEntry {
    pub ledger_entry: LedgerEntry,
    pub ttl_data: TtlData,
    pub size_bytes: u32,
    pub xdr_size_bytes: u32,
}

// Result of `SorobanState::get`. CONTRACT_DATA and CONTRACT_CODE lookups
// borrow into the stored entry (zero allocation). TTL lookups synthesize a
// fresh LedgerEntry from the stored TtlData, so they own their result.
pub enum EntryRef<'a> {
    Borrowed(&'a LedgerEntry),
    Owned(LedgerEntry),
}

impl EntryRef<'_> {
    pub fn as_ref(&self) -> &LedgerEntry {
        match self {
            EntryRef::Borrowed(e) => e,
            EntryRef::Owned(e) => e,
        }
    }
}

// Canonical in-memory Soroban state. Replaces the C++ `InMemorySorobanState`.
//
// Concurrency: outside the apply phase, all read methods are safe to call from
// any thread provided no mutator runs concurrently. Inside the apply phase,
// the state is wrapped `Arc<HashMap<...>>`-style and threads read borrowed
// references to entries; writes are staged per-thread and merged at stage
// boundaries. See the design doc.
//
// This struct is currently a skeleton — the parsing-heavy CRUD and
// initialization methods land in subsequent commits.
pub struct SorobanState {
    contract_data: FastMap<TtlKeyHash, ContractDataEntry>,
    contract_code: FastMap<TtlKeyHash, ContractCodeEntry>,
    // Holding pen for TTLs that arrive before their CONTRACT_DATA/CONTRACT_CODE
    // entry during initialization. Stored as bare TtlData (live_until +
    // last_modified) rather than the full TTL LedgerEntry — that's all we need
    // to merge into the data/code entry once it arrives.
    // Mirrors the C++ `mPendingTTLs` map. Asserted empty at the end of any
    // update batch via `check_update_invariants`.
    //
    // `pub(super)` so the in-tree unit tests (under
    // `soroban_apply::tests::state`) can poke this from outside `state.rs`
    // without a public-API hole; nothing else in the crate accesses it.
    pub(super) pending_ttls: FastMap<TtlKeyHash, TtlData>,

    last_closed_ledger_seq: u32,
    // Signed even though they're stored as u64 in the protocol — matches the
    // C++ choice for safer arithmetic on deltas. The runtime invariant is that
    // both stay non-negative.
    contract_code_state_size: i64,
    contract_data_state_size: i64,
}

impl SorobanState {
    pub fn new() -> Self {
        Self {
            contract_data: FastMap::default(),
            contract_code: FastMap::default(),
            pending_ttls: FastMap::default(),
            last_closed_ledger_seq: 0,
            contract_code_state_size: 0,
            contract_data_state_size: 0,
        }
    }

    // Mirrors `InMemorySorobanState::isEmpty()`.
    pub fn is_empty(&self) -> bool {
        self.contract_data.is_empty()
            && self.contract_code.is_empty()
            && self.pending_ttls.is_empty()
    }

    // Mirrors `InMemorySorobanState::getLedgerSeq()`.
    pub fn ledger_seq(&self) -> u32 {
        self.last_closed_ledger_seq
    }

    // Mirrors `InMemorySorobanState::assertLastClosedLedger()`.
    pub fn assert_last_closed_ledger(&self, expected_ledger_seq: u32) {
        assert_eq!(
            self.last_closed_ledger_seq, expected_ledger_seq,
            "SorobanState ledger_seq mismatch"
        );
    }

    // Mirrors `InMemorySorobanState::getContractDataEntryCount()`.
    pub fn contract_data_entry_count(&self) -> usize {
        self.contract_data.len()
    }

    // Mirrors `InMemorySorobanState::getContractCodeEntryCount()`.
    pub fn contract_code_entry_count(&self) -> usize {
        self.contract_code.len()
    }

    // Mirrors `InMemorySorobanState::getSize()`.
    pub fn size(&self) -> u64 {
        assert!(self.contract_code_state_size >= 0);
        assert!(self.contract_data_state_size >= 0);
        (self.contract_code_state_size + self.contract_data_state_size) as u64
    }

    // Mirrors `InMemorySorobanState::manuallyAdvanceLedgerHeader()`.
    pub fn manually_advance_ledger_header(&mut self, ledger_seq: u32) {
        self.last_closed_ledger_seq = ledger_seq;
    }

    // Mirrors `InMemorySorobanState::checkUpdateInvariants()` — must be called
    // at the end of any batch of updates to verify no orphaned TTLs remain.
    pub fn check_update_invariants(&self) {
        assert!(
            self.pending_ttls.is_empty(),
            "SorobanState has {} orphaned pending TTL(s) after update batch",
            self.pending_ttls.len()
        );
    }

    // ===== Reads =====

    // Mirrors `InMemorySorobanState::get()`. CONTRACT_DATA / CONTRACT_CODE keys
    // return a borrowed reference to the stored LedgerEntry. TTL keys return a
    // freshly-constructed TTL LedgerEntry built from the stored TtlData.
    //
    // The TTL case allocates because TTL entries aren't stored as full
    // LedgerEntries — only as TtlData co-located with their corresponding
    // CONTRACT_DATA/CODE entry. See `get_ttl_owned` below for the TTL case
    // factored out (used internally and by FFI callers that don't want to
    // mux on key type).
    pub fn get(&self, key: &LedgerKey) -> Option<EntryRef<'_>> {
        match key {
            LedgerKey::ContractData(_) => self
                .contract_data
                .get(&ttl_key_hash_for(key))
                .map(|e| EntryRef::Borrowed(&e.ledger_entry)),
            LedgerKey::ContractCode(_) => self
                .contract_code
                .get(&ttl_key_hash_for(key))
                .map(|e| EntryRef::Borrowed(&e.ledger_entry)),
            LedgerKey::Ttl(_) => self.get_ttl_owned(key).map(EntryRef::Owned),
            _ => panic!("SorobanState::get called with non-Soroban key type"),
        }
    }

    // Mirrors `InMemorySorobanState::getTTL()`. LedgerKey must be of type TTL.
    // Returns a freshly-constructed TTL LedgerEntry if the corresponding
    // CONTRACT_DATA or CONTRACT_CODE entry is present and has a non-default
    // TTL; otherwise None.
    //
    // Asserts that no pending TTLs are outstanding — this read API is only
    // meaningful after an update batch has finished.
    pub fn get_ttl_owned(&self, key: &LedgerKey) -> Option<LedgerEntry> {
        let LedgerKey::Ttl(ttl_key) = key else {
            panic!("SorobanState::get_ttl_owned called with non-TTL key");
        };
        assert!(
            self.pending_ttls.is_empty(),
            "SorobanState::get_ttl_owned called with pending TTLs outstanding"
        );
        let key_hash = ttl_key.key_hash.0;
        let ttl_data = self
            .contract_data
            .get(&key_hash)
            .map(|e| e.ttl_data)
            .or_else(|| self.contract_code.get(&key_hash).map(|e| e.ttl_data))?;
        if ttl_data.is_default() {
            return None;
        }
        Some(LedgerEntry {
            last_modified_ledger_seq: ttl_data.last_modified_ledger_seq,
            data: LedgerEntryData::Ttl(TtlEntry {
                key_hash: ttl_key.key_hash.clone(),
                live_until_ledger_seq: ttl_data.live_until_ledger_seq,
            }),
            ext: LedgerEntryExt::V0,
        })
    }

    // Returns the cached xdr_serialized_size for a CONTRACT_DATA /
    // CONTRACT_CODE key if the entry is present in this SorobanState.
    // Returns None for any other key type or for missing entries. Used
    // by the apply-time per-entry cap check to skip a redundant
    // `entry.to_xdr().len()` round-trip on the read path.
    pub fn cached_xdr_size_for(&self, key: &LedgerKey) -> Option<u32> {
        match key {
            LedgerKey::ContractData(_) => {
                let h = ttl_key_hash_for(key);
                self.contract_data.get(&h).map(|e| e.size_bytes)
            }
            LedgerKey::ContractCode(_) => {
                let h = ttl_key_hash_for(key);
                self.contract_code.get(&h).map(|e| e.xdr_size_bytes)
            }
            _ => None,
        }
    }

    // Lookup the synthesized TtlEntry for a CONTRACT_DATA / CONTRACT_CODE key
    // when the caller has already computed the TTL key hash (e.g. as part of
    // building a synthetic `LedgerKey::Ttl` for a layered probe).
    pub fn get_ttl_entry_by_hash(
        &self,
        key_hash: TtlKeyHash,
        data_or_code_key: &LedgerKey,
    ) -> Option<TtlEntry> {
        let ttl_data = match data_or_code_key {
            LedgerKey::ContractData(_) => self.contract_data.get(&key_hash)?.ttl_data,
            LedgerKey::ContractCode(_) => self.contract_code.get(&key_hash)?.ttl_data,
            _ => return None,
        };
        if ttl_data.is_default() {
            return None;
        }
        Some(TtlEntry {
            key_hash: Hash(key_hash),
            live_until_ledger_seq: ttl_data.live_until_ledger_seq,
        })
    }

    // Mirrors `InMemorySorobanState::hasTTL()`. LedgerKey must be of type TTL.
    // Returns true iff the TTL is present (either as a pending TTL or
    // co-located with an already-stored CONTRACT_DATA / CONTRACT_CODE entry
    // whose TtlData is non-default).
    pub fn has_ttl(&self, key: &LedgerKey) -> bool {
        let LedgerKey::Ttl(ttl_key) = key else {
            panic!("SorobanState::has_ttl called with non-TTL key");
        };
        let key_hash = ttl_key.key_hash.0;
        if self.pending_ttls.contains_key(&key_hash) {
            return true;
        }
        if let Some(e) = self.contract_data.get(&key_hash) {
            return !e.ttl_data.is_default();
        }
        if let Some(e) = self.contract_code.get(&key_hash) {
            return !e.ttl_data.is_default();
        }
        false
    }

    // ===== ContractData CRUD =====

    // Mirrors `InMemorySorobanState::createContractDataEntry()`. The entry
    // must not already exist; if a TTL has arrived ahead of the data (only
    // possible during initialization), it is adopted from `pending_ttls`.
    pub fn create_contract_data_entry(&mut self, ledger_entry: LedgerEntry) {
        let size_bytes = xdr_serialized_size(&ledger_entry);
        self.create_contract_data_entry_with_size(ledger_entry, size_bytes);
    }

    // Variant of `create_contract_data_entry` that accepts a pre-computed
    // `size_bytes` rather than re-serializing the entry to count its
    // bytes. The orchestrator hands in the host-supplied encoded
    // length here on its hot path. `size_bytes` MUST equal
    // `xdr_serialized_size(&ledger_entry)` — caller responsibility.
    pub fn create_contract_data_entry_with_size(
        &mut self,
        ledger_entry: LedgerEntry,
        size_bytes: u32,
    ) {
        assert!(
            matches!(&ledger_entry.data, LedgerEntryData::ContractData(_)),
            "create_contract_data_entry: entry is not CONTRACT_DATA"
        );
        let lk = ledger_entry_key(&ledger_entry);
        let key_hash = ttl_key_hash_for(&lk);
        assert!(
            !self.contract_data.contains_key(&key_hash),
            "create_contract_data_entry: entry already exists"
        );
        let ttl_data = self.pending_ttls.remove(&key_hash).unwrap_or_default();
        self.update_state_size_on_entry_update(0, size_bytes, /*is_contract_code=*/ false);
        self.contract_data.insert(
            key_hash,
            ContractDataEntry { ledger_entry, ttl_data, size_bytes },
        );
    }

    // Mirrors `InMemorySorobanState::updateContractData()`. The entry must
    // already exist. Preserves the existing TTL data and recomputes
    // `size_bytes` from the new entry.
    pub fn update_contract_data(&mut self, ledger_entry: LedgerEntry) {
        let new_size = xdr_serialized_size(&ledger_entry);
        self.update_contract_data_with_size(ledger_entry, new_size);
    }

    // Single-hash upsert for ContractData on the apply hot path.
    // Accepts the precomputed `TtlKeyHash` produced by the caller
    // (typically the orchestrator's phase-end commit pass which
    // already encoded the key once for the LedgerEntryUpdate and
    // SHA-256'd those bytes). Decides create vs update based on
    // existing state, mutates accordingly, returns `true` when the
    // entry was newly created. The phase-end split pass uses the
    // boolean to route the LedgerEntryUpdate into init vs live
    // without a second hash + HashMap probe.
    pub fn upsert_contract_data_with_key_hash(
        &mut self,
        ledger_entry: LedgerEntry,
        new_size: u32,
        key_hash: TtlKeyHash,
    ) -> bool {
        debug_assert!(
            matches!(&ledger_entry.data, LedgerEntryData::ContractData(_)),
            "upsert_contract_data_with_key_hash: entry is not CONTRACT_DATA"
        );
        if let Some(existing) = self.contract_data.get(&key_hash) {
            let old_size = existing.size_bytes;
            let preserved_ttl = existing.ttl_data;
            self.update_state_size_on_entry_update(old_size, new_size, false);
            self.contract_data.insert(
                key_hash,
                ContractDataEntry {
                    ledger_entry,
                    ttl_data: preserved_ttl,
                    size_bytes: new_size,
                },
            );
            false
        } else {
            let ttl_data = self.pending_ttls.remove(&key_hash).unwrap_or_default();
            self.update_state_size_on_entry_update(0, new_size, false);
            self.contract_data.insert(
                key_hash,
                ContractDataEntry {
                    ledger_entry,
                    ttl_data,
                    size_bytes: new_size,
                },
            );
            true
        }
    }

    // Variant of `update_contract_data` that accepts a pre-computed
    // `size_bytes`. See `create_contract_data_entry_with_size` for the
    // contract.
    pub fn update_contract_data_with_size(
        &mut self,
        ledger_entry: LedgerEntry,
        new_size: u32,
    ) {
        assert!(
            matches!(&ledger_entry.data, LedgerEntryData::ContractData(_)),
            "update_contract_data: entry is not CONTRACT_DATA"
        );
        let lk = ledger_entry_key(&ledger_entry);
        let key_hash = ttl_key_hash_for(&lk);
        let existing = self
            .contract_data
            .get(&key_hash)
            .expect("update_contract_data: entry does not exist");
        let old_size = existing.size_bytes;
        let preserved_ttl = existing.ttl_data;
        self.update_state_size_on_entry_update(old_size, new_size, false);
        self.contract_data.insert(
            key_hash,
            ContractDataEntry {
                ledger_entry,
                ttl_data: preserved_ttl,
                size_bytes: new_size,
            },
        );
    }

    // Mirrors `InMemorySorobanState::deleteContractData()`.
    pub fn delete_contract_data(&mut self, key: &LedgerKey) {
        assert!(
            matches!(key, LedgerKey::ContractData(_)),
            "delete_contract_data: key is not CONTRACT_DATA"
        );
        let key_hash = ttl_key_hash_for(key);
        self.delete_contract_data_by_hash(key_hash);
    }

    // Caller-precomputed-hash variant of `delete_contract_data`.
    pub fn delete_contract_data_by_hash(&mut self, key_hash: TtlKeyHash) {
        let removed = self
            .contract_data
            .remove(&key_hash)
            .expect("delete_contract_data: entry does not exist");
        self.update_state_size_on_entry_update(removed.size_bytes, 0, false);
    }

    // `state.get(key).is_some()` for a CONTRACT_DATA key with a
    // precomputed hash — skips the `ttl_key_hash_for` recompute the
    // current `get()` does. Returns true iff a ContractData entry
    // exists at this hash.
    pub fn contains_contract_data_by_hash(&self, key_hash: TtlKeyHash) -> bool {
        self.contract_data.contains_key(&key_hash)
    }

    // ===== ContractCode CRUD =====

    // Mirrors `InMemorySorobanState::createContractCodeEntry()`. The size is
    // computed by the caller (which has access to the protocol-version-aware
    // contractCodeSizeForRent helper, including the in-memory module footprint
    // for protocol >= 23). On older protocols it equals `xdr_size(entry)`.
    pub fn create_contract_code_entry(&mut self, ledger_entry: LedgerEntry, size_bytes: u32) {
        assert!(
            matches!(&ledger_entry.data, LedgerEntryData::ContractCode(_)),
            "create_contract_code_entry: entry is not CONTRACT_CODE"
        );
        let lk = ledger_entry_key(&ledger_entry);
        let key_hash = ttl_key_hash_for(&lk);
        assert!(
            !self.contract_code.contains_key(&key_hash),
            "create_contract_code_entry: entry already exists"
        );
        let ttl_data = self.pending_ttls.remove(&key_hash).unwrap_or_default();
        let xdr_size_bytes = xdr_serialized_size(&ledger_entry);
        self.update_state_size_on_entry_update(0, size_bytes, /*is_contract_code=*/ true);
        self.contract_code.insert(
            key_hash,
            ContractCodeEntry {
                ledger_entry,
                ttl_data,
                size_bytes,
                xdr_size_bytes,
            },
        );
    }

    // Single-hash upsert for ContractCode on the apply hot path. Same
    // shape as `upsert_contract_data_with_key_hash` — see that method
    // for the precomputed-key-hash rationale. `size_bytes` is the
    // protocol-aware rent size; `xdr_size_bytes` is computed from the
    // entry XDR.
    pub fn upsert_contract_code_with_key_hash(
        &mut self,
        ledger_entry: LedgerEntry,
        size_bytes: u32,
        key_hash: TtlKeyHash,
    ) -> bool {
        debug_assert!(
            matches!(&ledger_entry.data, LedgerEntryData::ContractCode(_)),
            "upsert_contract_code_with_key_hash: entry is not CONTRACT_CODE"
        );
        let xdr_size_bytes = xdr_serialized_size(&ledger_entry);
        if let Some(existing) = self.contract_code.get(&key_hash) {
            let preserved_ttl = existing.ttl_data;
            assert!(
                !preserved_ttl.is_default(),
                "upsert_contract_code: existing TTL is unexpectedly default"
            );
            let old_size = existing.size_bytes;
            self.update_state_size_on_entry_update(old_size, size_bytes, true);
            self.contract_code.insert(
                key_hash,
                ContractCodeEntry {
                    ledger_entry,
                    ttl_data: preserved_ttl,
                    size_bytes,
                    xdr_size_bytes,
                },
            );
            false
        } else {
            let ttl_data = self.pending_ttls.remove(&key_hash).unwrap_or_default();
            self.update_state_size_on_entry_update(0, size_bytes, true);
            self.contract_code.insert(
                key_hash,
                ContractCodeEntry {
                    ledger_entry,
                    ttl_data,
                    size_bytes,
                    xdr_size_bytes,
                },
            );
            true
        }
    }

    pub fn update_contract_code(&mut self, ledger_entry: LedgerEntry, size_bytes: u32) {
        assert!(
            matches!(&ledger_entry.data, LedgerEntryData::ContractCode(_)),
            "update_contract_code: entry is not CONTRACT_CODE"
        );
        let lk = ledger_entry_key(&ledger_entry);
        let key_hash = ttl_key_hash_for(&lk);
        let existing = self
            .contract_code
            .get(&key_hash)
            .expect("update_contract_code: entry does not exist");
        let preserved_ttl = existing.ttl_data;
        assert!(
            !preserved_ttl.is_default(),
            "update_contract_code: existing TTL is unexpectedly default"
        );
        let old_size = existing.size_bytes;
        let xdr_size_bytes = xdr_serialized_size(&ledger_entry);
        self.update_state_size_on_entry_update(old_size, size_bytes, true);
        self.contract_code.insert(
            key_hash,
            ContractCodeEntry {
                ledger_entry,
                ttl_data: preserved_ttl,
                size_bytes,
                xdr_size_bytes,
            },
        );
    }

    // `state.get(key).is_some()` for a CONTRACT_CODE key with a
    // precomputed hash.
    pub fn contains_contract_code_by_hash(&self, key_hash: TtlKeyHash) -> bool {
        self.contract_code.contains_key(&key_hash)
    }

    // Mirrors `InMemorySorobanState::deleteContractCode()`.
    pub fn delete_contract_code(&mut self, key: &LedgerKey) {
        assert!(
            matches!(key, LedgerKey::ContractCode(_)),
            "delete_contract_code: key is not CONTRACT_CODE"
        );
        let key_hash = ttl_key_hash_for(key);
        self.delete_contract_code_by_hash(key_hash);
    }

    // Caller-precomputed-hash variant of `delete_contract_code`.
    pub fn delete_contract_code_by_hash(&mut self, key_hash: TtlKeyHash) {
        let removed = self
            .contract_code
            .remove(&key_hash)
            .expect("delete_contract_code: entry does not exist");
        self.update_state_size_on_entry_update(removed.size_bytes, 0, true);
    }

    // ===== TTL CRUD =====

    // Mirrors `InMemorySorobanState::createTTL()`. Three cases:
    //  - The corresponding CONTRACT_DATA entry exists with a default TTL: set
    //    it.
    //  - The corresponding CONTRACT_CODE entry exists with a default TTL: set
    //    it.
    //  - Neither exists yet: stash in `pending_ttls` to be adopted when the
    //    data/code entry arrives. Only happens during initialization.
    pub fn create_ttl(&mut self, ttl_entry: LedgerEntry) {
        let LedgerEntryData::Ttl(ttl) = &ttl_entry.data else {
            panic!("create_ttl: entry is not TTL");
        };
        let key_hash = ttl.key_hash.0;
        let new_ttl = TtlData::new(ttl.live_until_ledger_seq, ttl_entry.last_modified_ledger_seq);

        if let Some(existing) = self.contract_data.get_mut(&key_hash) {
            assert!(
                existing.ttl_data.is_default(),
                "create_ttl: ContractData entry already has a non-default TTL"
            );
            existing.ttl_data = new_ttl;
            return;
        }
        if let Some(existing) = self.contract_code.get_mut(&key_hash) {
            assert!(
                existing.ttl_data.is_default(),
                "create_ttl: ContractCode entry already has a non-default TTL"
            );
            existing.ttl_data = new_ttl;
            return;
        }
        let prev = self.pending_ttls.insert(key_hash, new_ttl);
        assert!(
            prev.is_none(),
            "create_ttl: pending TTL already exists for this key"
        );
    }

    // Mirrors `InMemorySorobanState::updateTTL()`. The entry must already
    // exist in either contract_data or contract_code. Replaces the existing
    // TTL with the new one.
    pub fn update_ttl(&mut self, ttl_entry: LedgerEntry) {
        let LedgerEntryData::Ttl(ttl) = &ttl_entry.data else {
            panic!("update_ttl: entry is not TTL");
        };
        let key_hash = ttl.key_hash.0;
        let new_ttl = TtlData::new(ttl.live_until_ledger_seq, ttl_entry.last_modified_ledger_seq);

        if let Some(existing) = self.contract_data.get_mut(&key_hash) {
            existing.ttl_data = new_ttl;
            return;
        }
        if let Some(existing) = self.contract_code.get_mut(&key_hash) {
            existing.ttl_data = new_ttl;
            return;
        }
        panic!("update_ttl: target entry does not exist in either data or code map");
    }

    // ===== Bulk init / recompute =====

    // Mirrors `InMemorySorobanState::recomputeContractCodeSize()`. Walks every
    // CONTRACT_CODE entry, asks the caller-supplied `compute_size` for its
    // new protocol-version-aware size, updates `size_bytes` on the entry,
    // and adjusts the contract_code state-size counter by the running delta.
    //
    // The closure form is convenient in tests and in pure-Rust callers; the
    // FFI shim wires it up via a typed callback (or batches the
    // recomputation, which is upgrade-time only) — see the design doc.
    pub fn recompute_contract_code_size(
        &mut self,
        mut compute_size: impl FnMut(&LedgerEntry) -> u32,
    ) {
        let mut delta: i64 = 0;
        for entry in self.contract_code.values_mut() {
            let new_size = compute_size(&entry.ledger_entry);
            delta += i64::from(new_size) - i64::from(entry.size_bytes);
            entry.size_bytes = new_size;
        }
        let updated = self
            .contract_code_state_size
            .checked_add(delta)
            .expect("contract_code_state_size overflow during recompute");
        assert!(
            updated >= 0,
            "contract_code_state_size went negative during recompute"
        );
        self.contract_code_state_size = updated;
    }

    // Mirrors `InMemorySorobanState::updateStateSizeOnEntryUpdate()`.
    // Updates the appropriate state-size counter when an entry is inserted,
    // updated, or removed. `old_size` is 0 for inserts; `new_size` is 0 for
    // removals. Asserts that the running total stays non-negative and doesn't
    // overflow i64.
    pub(super) fn update_state_size_on_entry_update(
        &mut self,
        old_size: u32,
        new_size: u32,
        is_contract_code: bool,
    ) {
        let delta = i64::from(new_size) - i64::from(old_size);
        let counter = if is_contract_code {
            &mut self.contract_code_state_size
        } else {
            &mut self.contract_data_state_size
        };
        let updated = counter
            .checked_add(delta)
            .expect("SorobanState state-size counter overflow");
        assert!(
            updated >= 0,
            "SorobanState state-size counter went negative ({} + {})",
            counter,
            delta
        );
        *counter = updated;
    }

    // ===== Bucket-file initialization =====

    // Initialize SorobanState from a list of live-bucket file paths in
    // priority order (highest priority first — level 0 curr, level 0 snap,
    // level 1 curr, level 1 snap, ...). Replaces the C++ side's
    // initializeStateFromSnapshot path; per the design, all bucket-list
    // iteration and dedup logic lives on the Rust side.
    //
    // The state must be empty when called. Returns with last_closed_ledger_seq
    // set and `pending_ttls` empty (asserted via check_update_invariants).
    //
    // Algorithm (mirrors the C++ scanLiveEntriesOfType + lambda dedup pattern
    // in the old InMemorySorobanState::initializeStateFromSnapshot):
    //
    // - Single linear scan of every bucket file, in priority order.
    //   (The C++ side uses three separate scans, one per entry type, with an
    //    on-disk type-range index. We don't have that index in Rust yet, so
    //    a single pass that dispatches per entry type is more efficient.)
    // - DEADENTRY records add the deleted LedgerKey to a `deleted_keys` set,
    //   shadowing any subsequent (older) LIVE/INIT records for that same key.
    // - LIVE/INIT records insert into the appropriate map only if the key
    //   isn't already deleted AND isn't already inserted.
    //
    // Per-protocol orphaned-TTL handling: a TTL record for a CONTRACT_CODE
    // entry that hasn't yet been ingested lands in `pending_ttls` and is
    // adopted when the code entry is created. Order across types within the
    // single scan doesn't matter because of this.
    pub fn initialize_from_bucket_files_xdr(
        &mut self,
        bucket_paths: &Vec<String>,
        last_closed_ledger_seq: u32,
        ledger_version: u32,
        config_max_protocol: u32,
        cpu_cost_params: &CxxBuf,
        mem_cost_params: &CxxBuf,
    ) {
        assert!(
            self.is_empty(),
            "initialize_from_bucket_files_xdr: state must be empty"
        );

        // Pre-Soroban protocols have no Soroban entries. Just record the
        // ledger seq and return.
        if ledger_version < SOROBAN_PROTOCOL_VERSION {
            self.last_closed_ledger_seq = last_closed_ledger_seq;
            return;
        }

        let cpu_bytes = cpu_cost_params.as_ref();
        let mem_bytes = mem_cost_params.as_ref();

        let mut deleted_keys: FastSet<LedgerKey> = FastSet::default();

        for path in bucket_paths {
            for entry in read_bucket_entries(path) {
                self.process_bucket_entry(
                    entry,
                    &mut deleted_keys,
                    config_max_protocol,
                    ledger_version,
                    cpu_bytes,
                    mem_bytes,
                );
            }
        }

        self.last_closed_ledger_seq = last_closed_ledger_seq;
        self.check_update_invariants();
    }

    // Process a single BucketEntry during initialization. Filters down to
    // CONTRACT_DATA / CONTRACT_CODE / TTL — other entry types are silently
    // ignored.
    fn process_bucket_entry(
        &mut self,
        entry: BucketEntry,
        deleted_keys: &mut FastSet<LedgerKey>,
        config_max_protocol: u32,
        ledger_version: u32,
        cpu_bytes: &[u8],
        mem_bytes: &[u8],
    ) {
        match entry {
            BucketEntry::Liveentry(le) | BucketEntry::Initentry(le) => {
                let lk = match &le.data {
                    LedgerEntryData::ContractData(_)
                    | LedgerEntryData::ContractCode(_)
                    | LedgerEntryData::Ttl(_) => ledger_entry_key(&le),
                    _ => return, // non-Soroban entry; ignore
                };
                if deleted_keys.contains(&lk) {
                    return;
                }
                match &le.data {
                    LedgerEntryData::ContractData(_) => {
                        if self.get(&lk).is_none() {
                            self.create_contract_data_entry(le);
                        }
                    }
                    LedgerEntryData::ContractCode(_) => {
                        if self.get(&lk).is_none() {
                            let size_bytes = compute_contract_code_size_for_rent(
                                &le,
                                config_max_protocol,
                                ledger_version,
                                cpu_bytes,
                                mem_bytes,
                            );
                            self.create_contract_code_entry(le, size_bytes);
                        }
                    }
                    LedgerEntryData::Ttl(_) => {
                        if !self.has_ttl(&lk) {
                            self.create_ttl(le);
                        }
                    }
                    _ => unreachable!(),
                }
            }
            BucketEntry::Deadentry(lk) => match &lk {
                LedgerKey::ContractData(_)
                | LedgerKey::ContractCode(_)
                | LedgerKey::Ttl(_) => {
                    deleted_keys.insert(lk);
                }
                _ => {} // non-Soroban dead entry; ignore
            },
            BucketEntry::Metaentry(_) => {} // first-record bucket metadata
        }
    }

    // ===== Bridge wrappers (FFI) =====
    //
    // The methods below are the cxx-bridge surface, declared in
    // src/rust/src/bridge.rs. They take serialized XDR bytes (as `&CxxBuf`)
    // because cxx can't move foreign Rust types (like `LedgerEntry`)
    // across the FFI. Each wrapper deserializes the input, dispatches to the
    // typed method above, and (for read methods) reserializes the result.
    //
    // Cost: one XDR deserialize on each create/update/delete/lookup, plus one
    // XDR serialize per successful lookup. This is the same per-call XDR
    // cost as today's C++ code paid before this refactor; the per-TX
    // serialization storm during apply is eliminated separately by moving
    // the apply phase itself into Rust (see C9).

    pub fn lookup_entry_xdr(&self, key_xdr: &CxxBuf) -> RustBuf {
        let key = LedgerKey::from_xdr(key_xdr.as_ref(), Limits::none())
            .expect("lookup_entry_xdr: malformed LedgerKey XDR");
        match self.get(&key) {
            None => RustBuf::from(Vec::<u8>::new()),
            Some(entry_ref) => {
                let bytes = entry_ref
                    .as_ref()
                    .to_xdr(Limits::none())
                    .expect("lookup_entry_xdr: serialize LedgerEntry");
                RustBuf::from(bytes)
            }
        }
    }

    pub fn has_ttl_xdr(&self, key_xdr: &CxxBuf) -> bool {
        let key = LedgerKey::from_xdr(key_xdr.as_ref(), Limits::none())
            .expect("has_ttl_xdr: malformed LedgerKey XDR");
        self.has_ttl(&key)
    }

    pub fn create_contract_data_entry_xdr(&mut self, entry_xdr: &CxxBuf) {
        let entry = LedgerEntry::from_xdr(entry_xdr.as_ref(), Limits::none())
            .expect("create_contract_data_entry_xdr: malformed LedgerEntry XDR");
        self.create_contract_data_entry(entry);
    }

    pub fn update_contract_data_xdr(&mut self, entry_xdr: &CxxBuf) {
        let entry = LedgerEntry::from_xdr(entry_xdr.as_ref(), Limits::none())
            .expect("update_contract_data_xdr: malformed LedgerEntry XDR");
        self.update_contract_data(entry);
    }

    pub fn delete_contract_data_xdr(&mut self, key_xdr: &CxxBuf) {
        let key = LedgerKey::from_xdr(key_xdr.as_ref(), Limits::none())
            .expect("delete_contract_data_xdr: malformed LedgerKey XDR");
        self.delete_contract_data(&key);
    }

    pub fn create_contract_code_entry_xdr(&mut self, entry_xdr: &CxxBuf, size_bytes: u32) {
        let entry = LedgerEntry::from_xdr(entry_xdr.as_ref(), Limits::none())
            .expect("create_contract_code_entry_xdr: malformed LedgerEntry XDR");
        self.create_contract_code_entry(entry, size_bytes);
    }

    pub fn update_contract_code_xdr(&mut self, entry_xdr: &CxxBuf, size_bytes: u32) {
        let entry = LedgerEntry::from_xdr(entry_xdr.as_ref(), Limits::none())
            .expect("update_contract_code_xdr: malformed LedgerEntry XDR");
        self.update_contract_code(entry, size_bytes);
    }

    pub fn delete_contract_code_xdr(&mut self, key_xdr: &CxxBuf) {
        let key = LedgerKey::from_xdr(key_xdr.as_ref(), Limits::none())
            .expect("delete_contract_code_xdr: malformed LedgerKey XDR");
        self.delete_contract_code(&key);
    }

    // Notify SorobanState of post-apply eviction events. The C++
    // background eviction scan returns evicted entries (archived to
    // hot archive) and deletedKeys (entries removed entirely from
    // live state — TTLs of evicted entries plus expired temporary
    // CONTRACT_DATA entries). Walk both vectors and remove the
    // CONTRACT_DATA / CONTRACT_CODE entries from the in-memory map;
    // their associated TTL data is stored within the entry so
    // removing the entry implicitly removes the TTL. Plain TTL keys
    // and any non-Soroban keys are no-ops here. Lenient on
    // missing entries (eviction can race with apply-phase deletes,
    // so the entry may already be gone).
    pub fn evict_entries_xdr(
        &mut self,
        archived_entry_keys: &Vec<CxxBuf>,
        deleted_keys: &Vec<CxxBuf>,
    ) {
        for buf in archived_entry_keys.iter().chain(deleted_keys.iter()) {
            let key = LedgerKey::from_xdr(buf.as_ref(), Limits::none())
                .expect("evict_entries_xdr: malformed LedgerKey XDR");
            match &key {
                LedgerKey::ContractData(_) => {
                    let key_hash = ttl_key_hash_for(&key);
                    if let Some(removed) = self.contract_data.remove(&key_hash) {
                        self.update_state_size_on_entry_update(
                            removed.size_bytes, 0, false,
                        );
                    }
                }
                LedgerKey::ContractCode(_) => {
                    let key_hash = ttl_key_hash_for(&key);
                    if let Some(removed) = self.contract_code.remove(&key_hash) {
                        self.update_state_size_on_entry_update(
                            removed.size_bytes, 0, true,
                        );
                    }
                }
                // TTL keys are a no-op: the TTL data is co-located with
                // the entry and removed when the entry above is
                // removed. Non-Soroban keys aren't in our state map.
                _ => {}
            }
        }
    }

    pub fn create_ttl_xdr(&mut self, entry_xdr: &CxxBuf) {
        let entry = LedgerEntry::from_xdr(entry_xdr.as_ref(), Limits::none())
            .expect("create_ttl_xdr: malformed LedgerEntry XDR");
        self.create_ttl(entry);
    }

    pub fn update_ttl_xdr(&mut self, entry_xdr: &CxxBuf) {
        let entry = LedgerEntry::from_xdr(entry_xdr.as_ref(), Limits::none())
            .expect("update_ttl_xdr: malformed LedgerEntry XDR");
        self.update_ttl(entry);
    }

    // Mirror of the legacy `InMemorySorobanState::updateState` batch
    // method: walks init / live / dead vectors and dispatches each
    // entry to the appropriate per-entry CRUD path. Used by the
    // BucketTestUtils replay path that bypasses the normal apply phase
    // (e.g. setNextLedgerEntryBatchForBucketTesting flow) — those
    // entries flow into the live BucketList directly via
    // addLiveBatch, so we need to mirror them into SorobanState too
    // or post-apply paths (eviction lookup, etc.) won't see them.
    //
    // Soroban-only: classic / config / non-Soroban entries are
    // ignored. TTL keys in `dead_keys` are no-ops since TTL data
    // lives co-located with the parent entry.
    pub fn batch_update_xdr(
        &mut self,
        init_entries: &Vec<CxxBuf>,
        live_entries: &Vec<CxxBuf>,
        dead_keys: &Vec<CxxBuf>,
        new_ledger_seq: u32,
        ledger_version: u32,
        config_max_protocol: u32,
        cpu_cost_params: &CxxBuf,
        mem_cost_params: &CxxBuf,
    ) {
        // Caller is responsible for ordering: this fn walks init,
        // then live, then dead in that fixed order.
        let cpu_bytes = cpu_cost_params.as_ref();
        let mem_bytes = mem_cost_params.as_ref();
        for buf in init_entries {
            let entry = LedgerEntry::from_xdr(buf.as_ref(), Limits::none())
                .expect("batch_update_xdr: malformed init LedgerEntry");
            match &entry.data {
                LedgerEntryData::ContractData(_) => {
                    self.create_contract_data_entry(entry);
                }
                LedgerEntryData::ContractCode(_) => {
                    let size_bytes = compute_contract_code_size_for_rent(
                        &entry,
                        config_max_protocol,
                        ledger_version,
                        cpu_bytes,
                        mem_bytes,
                    );
                    self.create_contract_code_entry(entry, size_bytes);
                }
                LedgerEntryData::Ttl(_) => {
                    self.create_ttl(entry);
                }
                _ => {}
            }
        }
        for buf in live_entries {
            let entry = LedgerEntry::from_xdr(buf.as_ref(), Limits::none())
                .expect("batch_update_xdr: malformed live LedgerEntry");
            match &entry.data {
                LedgerEntryData::ContractData(_) => {
                    self.update_contract_data(entry);
                }
                LedgerEntryData::ContractCode(_) => {
                    let size_bytes = compute_contract_code_size_for_rent(
                        &entry,
                        config_max_protocol,
                        ledger_version,
                        cpu_bytes,
                        mem_bytes,
                    );
                    self.update_contract_code(entry, size_bytes);
                }
                LedgerEntryData::Ttl(_) => {
                    self.update_ttl(entry);
                }
                _ => {}
            }
        }
        for buf in dead_keys {
            let key = LedgerKey::from_xdr(buf.as_ref(), Limits::none())
                .expect("batch_update_xdr: malformed dead LedgerKey");
            match &key {
                LedgerKey::ContractData(_) => {
                    self.delete_contract_data(&key);
                }
                LedgerKey::ContractCode(_) => {
                    self.delete_contract_code(&key);
                }
                // TTL deletion is implicit when the parent entry is
                // deleted; classic / config keys aren't in our map.
                _ => {}
            }
        }
        self.last_closed_ledger_seq = new_ledger_seq;
    }

    // Bridge wrapper accepting paths as a Vec<String>. cxx supports
    // Rust String <-> rust::String, and Vec<String> works via cxx as a
    // sequence of strings.
    pub fn initialize_from_bucket_files(
        &mut self,
        bucket_paths: &Vec<String>,
        last_closed_ledger_seq: u32,
        ledger_version: u32,
        config_max_protocol: u32,
        cpu_cost_params: &CxxBuf,
        mem_cost_params: &CxxBuf,
    ) {
        self.initialize_from_bucket_files_xdr(
            bucket_paths,
            last_closed_ledger_seq,
            ledger_version,
            config_max_protocol,
            cpu_cost_params,
            mem_cost_params,
        );
    }

    // Reset state to empty. Used by tests; mirrors the BUILD_TESTS-only
    // C++ `clearForTesting` path.
    pub fn clear(&mut self) {
        self.contract_data.clear();
        self.contract_code.clear();
        self.pending_ttls.clear();
        self.last_closed_ledger_seq = 0;
        self.contract_code_state_size = 0;
        self.contract_data_state_size = 0;
    }

    // Bridge-side recompute. Mirrors C++ contractCodeSizeForRent semantics:
    // in-memory size accounting is only used starting from protocol 23, but
    // the cache itself may be populated in an earlier protocol. To get the
    // correct size on the upgrade-to-23 boundary we always compute as if at
    // protocol >= 23.
    //
    // Cost-params bufs are consumed bytewise via .as_ref(). They're constant
    // across the iteration; the per-entry call into
    // soroban_module_cache::contract_code_memory_size_for_rent_bytes
    // re-deserializes them once per entry, which is fine for an upgrade-time
    // pass.
    pub fn recompute_contract_code_size_xdr(
        &mut self,
        config_max_protocol: u32,
        protocol_version: u32,
        cpu_cost_params: &CxxBuf,
        mem_cost_params: &CxxBuf,
    ) {
        let version_for_size = protocol_version.max(23);
        let cpu_bytes = cpu_cost_params.as_ref();
        let mem_bytes = mem_cost_params.as_ref();

        self.recompute_contract_code_size(|entry| {
            let xdr_size = xdr_serialized_size(entry);
            let cc = match &entry.data {
                LedgerEntryData::ContractCode(c) => c,
                _ => panic!("recompute_contract_code_size_xdr: non-CONTRACT_CODE in code map"),
            };
            let cc_xdr = cc
                .to_xdr(Limits::none())
                .expect("recompute: serialize ContractCodeEntry");
            let memory_size = crate::soroban_module_cache::contract_code_memory_size_for_rent_bytes(
                config_max_protocol,
                version_for_size,
                &cc_xdr,
                cpu_bytes,
                mem_bytes,
            )
            .expect("recompute: contract_code_memory_size_for_rent_bytes");
            let total = u64::from(xdr_size).saturating_add(u64::from(memory_size));
            u32::try_from(total.min(u64::from(u32::MAX))).unwrap_or(u32::MAX)
        });
    }
}

// Bridge constructor — declared in bridge.rs's extern "Rust" block.

// Factory function: allocate a SorobanState in a Box for cxx::Box transfer.
pub fn new_soroban_state() -> Box<SorobanState> {
    Box::new(SorobanState::new())
}

impl Default for SorobanState {
    fn default() -> Self {
        Self::new()
    }
}
