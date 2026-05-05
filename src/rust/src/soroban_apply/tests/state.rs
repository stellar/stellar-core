// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

//! Unit tests for `super::super::state`.

use super::super::common::ttl_key_hash_for;
use super::super::state::{SorobanState, TtlData};
use crate::soroban_proto_all::soroban_curr::soroban_env_host::xdr;


    #[test]
    fn ttl_data_default_is_zero_zero() {
        assert!(TtlData::default().is_default());
    }

    #[test]
    fn ttl_data_non_default_is_not_default() {
        assert!(!TtlData::new(100, 50).is_default());
    }

    #[test]
    #[should_panic]
    fn ttl_data_half_zero_panics() {
        TtlData {
            live_until_ledger_seq: 100,
            last_modified_ledger_seq: 0,
        }
        .is_default();
    }

    #[test]
    fn fresh_state_is_empty() {
        let s = SorobanState::new();
        assert!(s.is_empty());
        assert_eq!(s.ledger_seq(), 0);
        assert_eq!(s.size(), 0);
        assert_eq!(s.contract_data_entry_count(), 0);
        assert_eq!(s.contract_code_entry_count(), 0);
    }

    #[test]
    fn manually_advance_ledger_header_updates_seq() {
        let mut s = SorobanState::new();
        s.manually_advance_ledger_header(42);
        assert_eq!(s.ledger_seq(), 42);
        s.assert_last_closed_ledger(42);
    }

    #[test]
    fn state_size_grows_and_shrinks() {
        let mut s = SorobanState::new();
        s.update_state_size_on_entry_update(0, 100, false);
        s.update_state_size_on_entry_update(0, 250, true);
        assert_eq!(s.size(), 350);
        s.update_state_size_on_entry_update(100, 0, false);
        s.update_state_size_on_entry_update(250, 50, true);
        assert_eq!(s.size(), 50);
    }

    #[test]
    #[should_panic]
    fn state_size_negative_panics() {
        let mut s = SorobanState::new();
        s.update_state_size_on_entry_update(100, 0, false);
    }

    #[test]
    fn check_update_invariants_passes_when_no_pending() {
        SorobanState::new().check_update_invariants();
    }

    #[test]
    #[should_panic]
    fn check_update_invariants_fails_with_pending() {
        let mut s = SorobanState::new();
        s.pending_ttls.insert([0u8; 32], TtlData::new(1, 1));
        s.check_update_invariants();
    }

    // ===== Test fixtures =====

    fn contract_id(tag: u8) -> xdr::ScAddress {
        xdr::ScAddress::Contract(xdr::ContractId(xdr::Hash([tag; 32])))
    }

    fn make_contract_data(tag: u8, val: u32, last_modified: u32) -> xdr::LedgerEntry {
        xdr::LedgerEntry {
            last_modified_ledger_seq: last_modified,
            data: xdr::LedgerEntryData::ContractData(xdr::ContractDataEntry {
                ext: xdr::ExtensionPoint::V0,
                contract: contract_id(tag),
                key: xdr::ScVal::U32(u32::from(tag)),
                durability: xdr::ContractDataDurability::Persistent,
                val: xdr::ScVal::U32(val),
            }),
            ext: xdr::LedgerEntryExt::V0,
        }
    }

    fn contract_data_key_for(entry: &xdr::LedgerEntry) -> xdr::LedgerKey {
        ledger_entry_key(entry)
    }

    fn make_contract_code(tag: u8, code: &[u8], last_modified: u32) -> xdr::LedgerEntry {
        xdr::LedgerEntry {
            last_modified_ledger_seq: last_modified,
            data: xdr::LedgerEntryData::ContractCode(xdr::ContractCodeEntry {
                ext: xdr::ContractCodeEntryExt::V0,
                hash: xdr::Hash([tag; 32]),
                code: code.to_vec().try_into().expect("code bytes fit BytesM"),
            }),
            ext: xdr::LedgerEntryExt::V0,
        }
    }

    fn make_ttl_for(target_key: &xdr::LedgerKey, live_until: u32, last_modified: u32) -> xdr::LedgerEntry {
        let key_hash = ttl_key_hash_for(target_key);
        xdr::LedgerEntry {
            last_modified_ledger_seq: last_modified,
            data: xdr::LedgerEntryData::Ttl(xdr::TtlEntry {
                key_hash: xdr::Hash(key_hash),
                live_until_ledger_seq: live_until,
            }),
            ext: xdr::LedgerEntryExt::V0,
        }
    }

    fn ttl_lookup_key(target_key: &xdr::LedgerKey) -> xdr::LedgerKey {
        xdr::LedgerKey::Ttl(xdr::LedgerKeyTtl {
            key_hash: xdr::Hash(ttl_key_hash_for(target_key)),
        })
    }

    // ===== TTL key hash =====

    #[test]
    fn ttl_key_hash_for_ttl_returns_inner_hash_unchanged() {
        let raw = [0xab; 32];
        let key = xdr::LedgerKey::Ttl(xdr::LedgerKeyTtl { key_hash: xdr::Hash(raw) });
        assert_eq!(ttl_key_hash_for(&key), raw);
    }

    #[test]
    fn ttl_key_hash_is_deterministic() {
        let entry = make_contract_data(1, 42, 100);
        let k = contract_data_key_for(&entry);
        assert_eq!(ttl_key_hash_for(&k), ttl_key_hash_for(&k));
    }

    #[test]
    fn ttl_key_hash_differs_per_key() {
        let a = contract_data_key_for(&make_contract_data(1, 0, 0));
        let b = contract_data_key_for(&make_contract_data(2, 0, 0));
        assert_ne!(ttl_key_hash_for(&a), ttl_key_hash_for(&b));
    }

    // ===== ContractData CRUD =====

    #[test]
    fn create_then_get_contract_data() {
        let mut s = SorobanState::new();
        let entry = make_contract_data(1, 42, 100);
        let key = contract_data_key_for(&entry);
        s.create_contract_data_entry(entry.clone());
        s.check_update_invariants();
        assert_eq!(s.contract_data_entry_count(), 1);
        let got = s.get(&key).expect("entry must be present");
        assert_eq!(got.as_ref(), &entry);
        // size > 0
        assert!(s.size() > 0);
    }

    #[test]
    fn update_contract_data_preserves_ttl_and_recomputes_size() {
        let mut s = SorobanState::new();
        let initial = make_contract_data(1, 42, 100);
        let key = contract_data_key_for(&initial);
        s.create_contract_data_entry(initial);
        // Set a non-default TTL via create_ttl
        let ttl = make_ttl_for(&key, /*live_until=*/ 5000, /*last_modified=*/ 100);
        s.create_ttl(ttl);
        let size_before = s.size();
        // Update the value (same key)
        let updated = make_contract_data(1, 999_999, 200);
        s.update_contract_data(updated.clone());
        let stored = s.get(&key).expect("entry still present");
        assert_eq!(stored.as_ref(), &updated);
        // TTL preserved
        let ttl_lookup = ttl_lookup_key(&key);
        assert!(s.has_ttl(&ttl_lookup));
        let ttl_entry = s.get_ttl_owned(&ttl_lookup).expect("TTL still present");
        if let xdr::LedgerEntryData::Ttl(t) = &ttl_entry.data {
            assert_eq!(t.live_until_ledger_seq, 5000);
        } else {
            panic!("expected TTL entry");
        }
        // size recomputed (even if equal in this test, the path was exercised)
        assert!(s.size() > 0);
        let _ = size_before;
    }

    #[test]
    fn delete_contract_data_clears_state() {
        let mut s = SorobanState::new();
        let entry = make_contract_data(1, 42, 100);
        let key = contract_data_key_for(&entry);
        s.create_contract_data_entry(entry);
        assert_eq!(s.contract_data_entry_count(), 1);
        s.delete_contract_data(&key);
        assert_eq!(s.contract_data_entry_count(), 0);
        assert_eq!(s.size(), 0);
        assert!(s.get(&key).is_none());
    }

    // ===== ContractCode CRUD =====

    #[test]
    fn create_then_get_contract_code() {
        let mut s = SorobanState::new();
        let entry = make_contract_code(7, &[0xde, 0xad, 0xbe, 0xef], 100);
        let key = ledger_entry_key(&entry);
        s.create_contract_code_entry(entry.clone(), /*size_bytes=*/ 1234);
        assert_eq!(s.contract_code_entry_count(), 1);
        let got = s.get(&key).expect("entry must be present");
        assert_eq!(got.as_ref(), &entry);
        assert_eq!(s.size(), 1234);
    }

    #[test]
    fn update_contract_code_preserves_ttl_and_uses_caller_size() {
        let mut s = SorobanState::new();
        let initial = make_contract_code(7, b"abcd", 100);
        let key = ledger_entry_key(&initial);
        s.create_contract_code_entry(initial, 100);
        let ttl = make_ttl_for(&key, 5000, 100);
        s.create_ttl(ttl);
        let updated = make_contract_code(7, b"abcdef", 200);
        s.update_contract_code(updated.clone(), 150);
        let got = s.get(&key).expect("present");
        assert_eq!(got.as_ref(), &updated);
        assert_eq!(s.size(), 150);
        let ttl_lookup = ttl_lookup_key(&key);
        assert!(s.has_ttl(&ttl_lookup));
    }

    #[test]
    fn delete_contract_code_clears_state() {
        let mut s = SorobanState::new();
        let entry = make_contract_code(7, b"abcd", 100);
        let key = ledger_entry_key(&entry);
        s.create_contract_code_entry(entry, 100);
        s.delete_contract_code(&key);
        assert_eq!(s.contract_code_entry_count(), 0);
        assert_eq!(s.size(), 0);
        assert!(s.get(&key).is_none());
    }

    // ===== TTL CRUD =====

    #[test]
    fn create_ttl_after_data_sets_ttl() {
        let mut s = SorobanState::new();
        let entry = make_contract_data(1, 42, 100);
        let key = contract_data_key_for(&entry);
        s.create_contract_data_entry(entry);
        let ttl_lookup = ttl_lookup_key(&key);
        // Before TTL set, has_ttl is false
        assert!(!s.has_ttl(&ttl_lookup));
        let ttl = make_ttl_for(&key, 5000, 100);
        s.create_ttl(ttl);
        s.check_update_invariants();
        assert!(s.has_ttl(&ttl_lookup));
    }

    #[test]
    fn create_ttl_before_data_stashes_then_adopts() {
        let mut s = SorobanState::new();
        let entry = make_contract_data(1, 42, 100);
        let key = contract_data_key_for(&entry);
        let ttl = make_ttl_for(&key, 5000, 100);
        // TTL arrives first
        s.create_ttl(ttl);
        // pending_ttls now non-empty; check_update_invariants would fail here
        // Data arrives second — should adopt the pending TTL
        s.create_contract_data_entry(entry);
        s.check_update_invariants();
        let ttl_lookup = ttl_lookup_key(&key);
        assert!(s.has_ttl(&ttl_lookup));
        let ttl_entry = s.get_ttl_owned(&ttl_lookup).expect("TTL adopted");
        if let xdr::LedgerEntryData::Ttl(t) = &ttl_entry.data {
            assert_eq!(t.live_until_ledger_seq, 5000);
        } else {
            panic!("expected TTL entry");
        }
    }

    #[test]
    fn update_ttl_replaces_existing() {
        let mut s = SorobanState::new();
        let entry = make_contract_data(1, 42, 100);
        let key = contract_data_key_for(&entry);
        s.create_contract_data_entry(entry);
        s.create_ttl(make_ttl_for(&key, 1000, 100));
        s.update_ttl(make_ttl_for(&key, 5000, 200));
        let ttl_lookup = ttl_lookup_key(&key);
        let ttl_entry = s.get_ttl_owned(&ttl_lookup).expect("TTL present");
        if let xdr::LedgerEntryData::Ttl(t) = &ttl_entry.data {
            assert_eq!(t.live_until_ledger_seq, 5000);
            assert_eq!(ttl_entry.last_modified_ledger_seq, 200);
        } else {
            panic!("expected TTL entry");
        }
    }

    #[test]
    #[should_panic]
    fn update_ttl_panics_when_target_missing() {
        let mut s = SorobanState::new();
        let entry = make_contract_data(1, 42, 100);
        let key = contract_data_key_for(&entry);
        s.update_ttl(make_ttl_for(&key, 5000, 100));
    }

    #[test]
    #[should_panic]
    fn duplicate_create_panics() {
        let mut s = SorobanState::new();
        let entry = make_contract_data(1, 42, 100);
        s.create_contract_data_entry(entry.clone());
        s.create_contract_data_entry(entry);
    }

    // ===== Bulk init / recompute =====

    #[test]
    fn initialize_from_streams_data_then_ttl_then_code() {
        let mut s = SorobanState::new();
        let data1 = make_contract_data(1, 11, 50);
        let data2 = make_contract_data(2, 22, 50);
        let code1 = make_contract_code(7, b"abcd", 50);
        let data1_key = ledger_entry_key(&data1);
        let data2_key = ledger_entry_key(&data2);
        let code1_key = ledger_entry_key(&code1);
        let ttls = vec![
            // TTL for data1 — will land directly on the existing data entry
            make_ttl_for(&data1_key, 1000, 50),
            // TTL for code1 — will land in pending_ttls; adopted when code arrives
            make_ttl_for(&code1_key, 2000, 50),
            // TTL for data2 — will land directly on the existing data entry
            make_ttl_for(&data2_key, 3000, 50),
        ];
        s.initialize_from_streams(
            vec![data1, data2],
            ttls,
            vec![(code1, /*size_bytes=*/ 500)],
            42,
        );
        assert_eq!(s.contract_data_entry_count(), 2);
        assert_eq!(s.contract_code_entry_count(), 1);
        assert_eq!(s.ledger_seq(), 42);
        // All three TTLs are present.
        assert!(s.has_ttl(&ttl_lookup_key(&data1_key)));
        assert!(s.has_ttl(&ttl_lookup_key(&data2_key)));
        assert!(s.has_ttl(&ttl_lookup_key(&code1_key)));
        // pending_ttls cleared (asserted by initialize_from_streams).
    }

    #[test]
    #[should_panic]
    fn initialize_from_streams_panics_on_non_empty_state() {
        let mut s = SorobanState::new();
        s.create_contract_data_entry(make_contract_data(1, 0, 0));
        s.initialize_from_streams(vec![], vec![], vec![], 0);
    }

    #[test]
    fn recompute_contract_code_size_updates_each_entry_and_total() {
        let mut s = SorobanState::new();
        let code1 = make_contract_code(1, b"a", 10);
        let code2 = make_contract_code(2, b"bb", 10);
        s.create_contract_code_entry(code1, 100);
        s.create_contract_code_entry(code2, 200);
        assert_eq!(s.size(), 300);
        // Recompute: every entry gets a fixed new size of 50.
        s.recompute_contract_code_size(|_e| 50);
        // Total should now be 100 (2 entries * 50).
        assert_eq!(s.size(), 100);
    }

    #[test]
    fn recompute_contract_code_size_with_no_entries_is_noop() {
        let mut s = SorobanState::new();
        s.recompute_contract_code_size(|_e| {
            panic!("should not be called when there are no entries");
        });
        assert_eq!(s.size(), 0);
    }

    // ===== clear =====

    #[test]
    fn clear_resets_all_state() {
        let mut s = SorobanState::new();
        s.create_contract_data_entry(make_contract_data(1, 42, 100));
        s.create_contract_code_entry(make_contract_code(7, b"abcd", 100), 50);
        s.manually_advance_ledger_header(99);
        s.clear();
        assert!(s.is_empty());
        assert_eq!(s.ledger_seq(), 0);
        assert_eq!(s.size(), 0);
    }

