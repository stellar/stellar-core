// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

/**
 * FuzzUtils.h - Shared utilities for fuzzing infrastructure
 *
 * This file contains constants, types, and utility functions used by multiple
 * fuzz targets. The fuzzing infrastructure uses a compact XDR representation
 * that maps bytes to complex XDR structures efficiently.
 */

#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-overlay.h"
#include "xdr/Stellar-transaction.h"
#include "xdrpp/marshal.h"
#include "xdrpp/message.h"

#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace stellar
{

class Application;
class Config;

namespace FuzzUtils
{

// ============================================================================
// Constants for compact XDR representation
// ============================================================================

size_t static constexpr NUM_STORED_LEDGER_KEYS = 0x100U;
using StoredLedgerKeys = std::array<LedgerKey, NUM_STORED_LEDGER_KEYS>;
StoredLedgerKeys::difference_type static constexpr NUM_UNVALIDATED_LEDGER_KEYS =
    0x40;
size_t static constexpr NUM_VALIDATED_LEDGER_KEYS =
    NUM_STORED_LEDGER_KEYS - NUM_UNVALIDATED_LEDGER_KEYS;

size_t static constexpr NUM_STORED_POOL_IDS = 0x7U;
using StoredPoolIDs = std::array<PoolID, NUM_STORED_POOL_IDS>;

// Used by getFuzzConfig
auto constexpr FUZZING_FEE = 1;
auto constexpr FUZZING_RESERVE = 4;

// ============================================================================
// Key encoding/decoding utilities (used by compactor/unpacker)
// ============================================================================

void setShortKey(uint256& ed25519, int i);
void setShortKey(PublicKey& pk, int i);
uint8_t getShortKey(uint256 const& ed25519);
uint8_t getShortKey(PublicKey const& pk);
uint8_t getShortKey(Asset const& asset);
uint8_t getShortKey(AssetCode const& code);
uint8_t getShortKey(ClaimableBalanceID const& balanceID);
uint8_t getShortKey(LedgerKey const& key);

void setShortKey(StoredLedgerKeys const& storedKeys, LedgerKey& key,
                 uint8_t byte);
void setShortKey(StoredPoolIDs const& storedPoolIDs, PoolID& key, uint8_t byte);

// ============================================================================
// Asset utilities (used by compactor/unpacker)
// ============================================================================

Asset makeAsset(int issuer, int digit);
Asset makeAsset(int i);
AssetCode makeAssetCode(int i);

// ============================================================================
// Ledger key generation
// ============================================================================

void generateStoredLedgerKeys(StoredLedgerKeys::iterator begin,
                              StoredLedgerKeys::iterator end);

} // namespace FuzzUtils

// ============================================================================
// Global configuration
// ============================================================================

// Creates a deterministic configuration for fuzzing
Config getFuzzConfig(int instanceNumber);

} // namespace stellar

// ============================================================================
// XDR Fuzzer Compactor/Unpacker (in xdr namespace)
// ============================================================================

namespace xdr
{

/**
 * xdr_fuzzer_compactor - Compresses XDR to a compact binary format for fuzzing.
 *
 * The compact format uses:
 * - 1 byte for uint32 values
 * - 2 bytes for uint64 values
 * - 1 byte for AccountID, Asset, etc. (using short key encoding)
 */
struct xdr_fuzzer_compactor
{
    std::uint8_t* const mStart;
    std::uint8_t* mCur;
    std::uint8_t* const mEnd;

    xdr_fuzzer_compactor(void* start, void* end);
    xdr_fuzzer_compactor(msg_ptr& m);

    void put_bytes(void const* buf, size_t len);
    void check(std::size_t n) const;
    uint32_t size() const;

    // Generic handlers using SFINAE
    template <typename T>
    typename std::enable_if<std::is_same<
        std::uint32_t, typename xdr_traits<T>::uint_type>::value>::type
    operator()(T t)
    {
        // convert uint32 -> 1 byte
        check(1);
        auto v = xdr_traits<T>::to_uint(t);
        uint8_t b = static_cast<uint8_t>(v & 0xFF);
        put_bytes(&b, 1);
    }

    template <typename T>
    typename std::enable_if<std::is_same<
        std::uint64_t, typename xdr_traits<T>::uint_type>::value>::type
    operator()(T t)
    {
        // convert uint64 -> 2 bytes
        check(2);
        uint16_t v = static_cast<uint16_t>(xdr_traits<T>::to_uint(t) & 0xFFFF);
        put_bytes(&v, 2);
    }

    template <typename T>
    typename std::enable_if<xdr_traits<T>::is_bytes>::type
    operator()(T const& t)
    {
        // convert array -> 0/1 byte
        uint8_t s2 = t.empty() ? 0 : 1;
        if (xdr_traits<T>::variable_nelem)
        {
            check(1 + s2);
            put_bytes(&s2, 1);
        }
        else
        {
            check(s2);
        }
        put_bytes(t.data(), s2);
    }

    template <typename T>
    typename std::enable_if<
        (!std::is_same<stellar::AccountID, T>::value &&
         !std::is_same<stellar::MuxedAccount, T>::value &&
         !std::is_same<stellar::Asset, T>::value &&
         !std::is_same<stellar::AssetCode, T>::value &&
         !std::is_same<stellar::ClaimableBalanceID, T>::value &&
         !std::is_same<stellar::LedgerKey, T>::value) &&
        (xdr_traits<T>::is_class || xdr_traits<T>::is_container)>::type
    operator()(T const& t)
    {
        xdr_traits<T>::save(*this, t);
    }

    // Specialized handlers for Stellar types
    void archive(stellar::AccountID const& pk);
    void archive(stellar::MuxedAccount const& m);
    void archive(stellar::Asset const& asset);
    void archive(stellar::AssetCode const& code);
    void archive(stellar::ClaimableBalanceID const& balanceID);
    void archive(stellar::LedgerKey const& key);

    template <typename T>
    typename std::enable_if<std::is_same<stellar::AccountID, T>::value>::type
    operator()(T const& pk)
    {
        archive(pk);
    }

    template <typename T>
    typename std::enable_if<std::is_same<stellar::MuxedAccount, T>::value>::type
    operator()(T const& m)
    {
        archive(m);
    }

    template <typename T>
    typename std::enable_if<std::is_same<stellar::Asset, T>::value>::type
    operator()(T const& asset)
    {
        archive(asset);
    }

    template <typename T>
    typename std::enable_if<std::is_same<stellar::AssetCode, T>::value>::type
    operator()(T const& code)
    {
        archive(code);
    }

    template <typename T>
    typename std::enable_if<
        std::is_same<stellar::ClaimableBalanceID, T>::value>::type
    operator()(T const& balanceID)
    {
        archive(balanceID);
    }

    template <typename T>
    typename std::enable_if<std::is_same<stellar::LedgerKey, T>::value>::type
    operator()(T const& key)
    {
        archive(key);
    }
};

/**
 * xdr_fuzzer_unpacker - Expands compact binary format to XDR for fuzzing.
 */
struct xdr_fuzzer_unpacker
{
    stellar::FuzzUtils::StoredLedgerKeys mStoredLedgerKeys;
    stellar::FuzzUtils::StoredPoolIDs mStoredPoolIDs;
    std::uint8_t const* mCur;
    std::uint8_t const* const mEnd;

    xdr_fuzzer_unpacker(
        stellar::FuzzUtils::StoredLedgerKeys const& storedLedgerKeys,
        stellar::FuzzUtils::StoredPoolIDs const& storedPoolIDs,
        void const* start, void const* end);
    xdr_fuzzer_unpacker(
        stellar::FuzzUtils::StoredLedgerKeys const& storedLedgerKeys,
        stellar::FuzzUtils::StoredPoolIDs const& storedPoolIDs,
        msg_ptr const& m);

    void get_bytes(void* buf, size_t len);
    uint8_t get_byte();
    void check(std::size_t n) const;
    void done();

    template <typename T>
    T
    get32()
    {
        // 1 byte --> uint32
        check(1);
        uint32_t w = get_byte();
        if (w == UINT8_MAX)
        {
            return std::numeric_limits<T>::max();
        }
        else if (w == UINT8_MAX - 1)
        {
            auto maxT = std::numeric_limits<T>::max();
            return xdr_traits<T>::from_uint(maxT - 1);
        }
        return xdr_traits<T>::from_uint(w);
    }

    template <typename T>
    T
    get64()
    {
        // 2 bytes --> uint64 **with** "sign extension"
        check(2);
        int16_t w;
        get_bytes(&w, 2);
        int64_t ww = w;
        if (ww == INT16_MAX)
        {
            return std::numeric_limits<T>::max();
        }
        else if (ww == INT16_MAX - 1)
        {
            return std::numeric_limits<T>::max() - 1;
        }
        return xdr_traits<T>::from_uint(ww);
    }

    template <typename T>
    typename std::enable_if<std::is_same<
        std::uint32_t, typename xdr_traits<T>::uint_type>::value>::type
    operator()(T& t)
    {
        t = get32<T>();
    }

    template <typename T>
    typename std::enable_if<std::is_same<
        std::uint64_t, typename xdr_traits<T>::uint_type>::value>::type
    operator()(T& t)
    {
        t = get64<T>();
    }

    template <typename T>
    typename std::enable_if<xdr_traits<T>::is_bytes>::type
    operator()(T& t)
    {
        std::uint32_t s2 = 0;
        if (xdr_traits<T>::variable_nelem)
        {
            check(1);
            s2 = get_byte();
            check(s2);
            if (s2 > 1)
            {
                throw xdr_overflow("large vector in xdr_fuzzer_unpacker");
            }
            t.resize(s2);
        }
        else
        {
            if (!t.empty())
            {
                s2 = 1;
            }
            check(s2);
        }
        get_bytes(t.data(), s2);
    }

    template <typename T>
    typename std::enable_if<
        (!std::is_same<stellar::AccountID, T>::value &&
         !std::is_same<stellar::MuxedAccount, T>::value &&
         !std::is_same<stellar::Asset, T>::value &&
         !std::is_same<stellar::AssetCode, T>::value &&
         !std::is_same<stellar::ClaimableBalanceID, T>::value &&
         !std::is_same<stellar::LiquidityPoolDepositOp, T>::value &&
         !std::is_same<stellar::LiquidityPoolWithdrawOp, T>::value &&
         !std::is_same<stellar::LedgerKey, T>::value) &&
        (xdr_traits<T>::is_class || xdr_traits<T>::is_container)>::type
    operator()(T& t)
    {
        xdr_traits<T>::load(*this, t);
    }

    // Specialized handlers for Stellar types
    void unarchive(stellar::AccountID& pk);
    void unarchive(stellar::MuxedAccount& m);
    void unarchive(stellar::Asset& asset);
    void unarchive(stellar::AssetCode& code);
    void unarchive(stellar::ClaimableBalanceID& balanceID);
    void unarchive(stellar::LiquidityPoolDepositOp& depositOp);
    void unarchive(stellar::LiquidityPoolWithdrawOp& withdrawOp);
    void unarchive(stellar::LedgerKey& key);

    template <typename T>
    typename std::enable_if<std::is_same<stellar::AccountID, T>::value>::type
    operator()(T& pk)
    {
        unarchive(pk);
    }

    template <typename T>
    typename std::enable_if<std::is_same<stellar::MuxedAccount, T>::value>::type
    operator()(T& m)
    {
        unarchive(m);
    }

    template <typename T>
    typename std::enable_if<std::is_same<stellar::Asset, T>::value>::type
    operator()(T& asset)
    {
        unarchive(asset);
    }

    template <typename T>
    typename std::enable_if<std::is_same<stellar::AssetCode, T>::value>::type
    operator()(T& code)
    {
        unarchive(code);
    }

    template <typename T>
    typename std::enable_if<
        std::is_same<stellar::ClaimableBalanceID, T>::value>::type
    operator()(T& balanceID)
    {
        unarchive(balanceID);
    }

    template <typename T>
    typename std::enable_if<
        std::is_same<stellar::LiquidityPoolDepositOp, T>::value>::type
    operator()(T& depositOp)
    {
        unarchive(depositOp);
    }

    template <typename T>
    typename std::enable_if<
        std::is_same<stellar::LiquidityPoolWithdrawOp, T>::value>::type
    operator()(T& withdrawOp)
    {
        unarchive(withdrawOp);
    }

    template <typename T>
    typename std::enable_if<std::is_same<stellar::LedgerKey, T>::value>::type
    operator()(T& key)
    {
        unarchive(key);
    }
};

// Template helper functions for XDR conversion
template <typename... Args>
opaque_vec<>
xdr_to_fuzzer_opaque(Args const&... args)
{
    opaque_vec<> m(opaque_vec<>::size_type{xdr_argpack_size(args...)});
    xdr_fuzzer_compactor p(m.data(), m.data() + m.size());
    xdr_argpack_archive(p, args...);
    m.resize(p.size());
    return m;
}

template <typename Bytes, typename... Args>
auto
xdr_from_fuzzer_opaque(
    stellar::FuzzUtils::StoredLedgerKeys const& storedLedgerKeys,
    stellar::FuzzUtils::StoredPoolIDs const& storedPoolIDs, Bytes const& m,
    Args&... args) -> decltype(detail::bytes_to_void(m))
{
    xdr_fuzzer_unpacker g(storedLedgerKeys, storedPoolIDs, m.data(),
                          m.data() + m.size());
    xdr_argpack_archive(g, args...);
    g.done();
}

} // namespace xdr
