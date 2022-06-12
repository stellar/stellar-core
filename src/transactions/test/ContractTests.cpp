// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION

#include "crypto/SecretKey.h"
#include "ledger/LedgerTxn.h"
#include "lib/catch.hpp"
#include "rust/RustBridge.h"
#include "test/test.h"
#include "xdr/Stellar-ledger-entries.h"
#include <autocheck/autocheck.hpp>
#include <fmt/format.h>
#include <limits>
#include <type_traits>
#include <variant>

#include "transactions/contracts/ContractUtils.h"
#include "xdr/Stellar-contract.h"

using namespace stellar;

// This is an example WASM from the SDK that unpacks two SCV_I32 arguments, adds
// them with an overflow check, and re-packs them as an SCV_I32 if successful.
//
// To regenerate, check out the SDK, install a nightly toolchain with
// the rust-src component (to enable the 'tiny' build) using the following:
//
//  $ rustup component add rust-src --toolchain nightly
//
// then do:
//
//  $ make tiny
//  $ xxd -i target/wasm32-unknown-unknown/release/example_add_i32.wasm

std::vector<uint8_t> add_i32_wasm{
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01, 0x60,
    0x02, 0x7e, 0x7e, 0x01, 0x7e, 0x03, 0x02, 0x01, 0x00, 0x05, 0x03, 0x01,
    0x00, 0x10, 0x06, 0x11, 0x02, 0x7f, 0x00, 0x41, 0x80, 0x80, 0xc0, 0x00,
    0x0b, 0x7f, 0x00, 0x41, 0x80, 0x80, 0xc0, 0x00, 0x0b, 0x07, 0x2b, 0x04,
    0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x03, 0x61, 0x64,
    0x64, 0x00, 0x00, 0x0a, 0x5f, 0x5f, 0x64, 0x61, 0x74, 0x61, 0x5f, 0x65,
    0x6e, 0x64, 0x03, 0x00, 0x0b, 0x5f, 0x5f, 0x68, 0x65, 0x61, 0x70, 0x5f,
    0x62, 0x61, 0x73, 0x65, 0x03, 0x01, 0x0a, 0x47, 0x01, 0x45, 0x01, 0x02,
    0x7f, 0x02, 0x40, 0x20, 0x00, 0x42, 0x0f, 0x83, 0x42, 0x03, 0x52, 0x20,
    0x01, 0x42, 0x0f, 0x83, 0x42, 0x03, 0x52, 0x72, 0x45, 0x04, 0x40, 0x20,
    0x01, 0x42, 0x04, 0x88, 0xa7, 0x22, 0x02, 0x41, 0x00, 0x48, 0x20, 0x02,
    0x20, 0x00, 0x42, 0x04, 0x88, 0xa7, 0x22, 0x03, 0x6a, 0x22, 0x02, 0x20,
    0x03, 0x48, 0x73, 0x45, 0x0d, 0x01, 0x0b, 0x00, 0x0b, 0x20, 0x02, 0xad,
    0x42, 0x04, 0x86, 0x42, 0x03, 0x84, 0x0b};

TEST_CASE("WASM test", "[wasm]")
{
    SCVal x, y;

    x.type(SCV_I32);
    x.i32() = 5;

    y.type(SCV_I32);
    y.i32() = 7;

    SCVec args{x, y};

    namespace ch = std::chrono;
    using clock = ch::high_resolution_clock;
    using usec = ch::microseconds;

    Hash contract_id = HashUtils::pseudoRandomForTesting();

    LedgerFootprint footprint;
    LedgerEntry wasm_le;
    LedgerKey wasm_lk;
    SCVal wasm_key;
    SCVal wasm_val;

    wasm_key.type(SCValType::SCV_STATIC);
    wasm_key.ic() = SCStatic::SCS_LEDGER_KEY_CONTRACT_CODE_WASM;

    wasm_val.type(SCValType::SCV_OBJECT);
    wasm_val.obj().activate();
    wasm_val.obj()->type(SCO_BINARY);
    wasm_val.obj()->bin().assign(add_i32_wasm.begin(), add_i32_wasm.end());

    wasm_lk.type(CONTRACT_DATA);
    wasm_lk.contractData().contractID = contract_id;
    wasm_lk.contractData().key = wasm_key;

    wasm_le.data.type(CONTRACT_DATA);
    wasm_le.data.contractData().contractID = contract_id;
    wasm_le.data.contractData().key = wasm_key;
    wasm_le.data.contractData().val = wasm_val;

    footprint.readOnly.emplace_back(wasm_lk);

    std::unique_ptr<std::vector<uint8_t>> wasm_xdr_ptr =
        std::make_unique<std::vector<uint8_t>>(xdr::xdr_to_opaque(wasm_le));
    std::vector<std::unique_ptr<std::vector<uint8_t>>> xdr_buffers;
    xdr_buffers.emplace_back(std::move(wasm_xdr_ptr));

    auto begin = clock::now();
    auto res = invokeContract(contract_id, "add", args, footprint,
                              std::move(xdr_buffers));
    auto end = clock::now();

    auto us = ch::duration_cast<usec>(end - begin);

    REQUIRE(res.type() == SCV_I32);
    REQUIRE(res.i32() == x.i32() + y.i32());

    LOG_INFO(DEFAULT_LOG, "calculated {} + {} = {} in {} usecs", x.i32(),
             y.i32(), res.i32(), us.count());
}

#endif