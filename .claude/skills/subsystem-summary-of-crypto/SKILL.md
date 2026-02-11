---
name: subsystem-summary-of-crypto
description: "read this skill for a token-efficient summary of the crypto subsystem"
---

# Crypto Subsystem Technical Summary

## Overview

The `src/crypto/` subsystem provides all cryptographic primitives for stellar-core: hashing (SHA-256, BLAKE2b, SipHash), Ed25519 key management and signatures, Curve25519 ECDH key agreement, StrKey encoding/decoding, hex utilities, and random number generation. It is built on top of libsodium and uses xdrpp for serialization. There are no background threads or event loops in this subsystem; it is a stateless utility layer with one notable piece of process-wide shared state: the signature verification cache.

---

## Key Classes and Data Structures

### `ByteSlice` (ByteSlice.h)
A lightweight, non-owning, read-only view over contiguous byte data. Acts as a universal adaptor for passing byte containers into crypto functions. Implicitly constructs from `xdr::opaque_array<N>`, `xdr::msg_ptr`, `std::vector<uint8_t>`, `std::string`, `rust::Vec<uint8_t>`, `RustBuf`, `char const*`, and raw `(void*, size_t)`. Provides `data()`, `size()`, `begin()`, `end()`, `operator[]` (bounds-checked), and `empty()`.

### `CryptoError` (CryptoError.h)
Simple exception class inheriting `std::runtime_error`. Thrown by all crypto functions on failure (e.g., libsodium errors, invalid inputs).

### `SecretKey` (SecretKey.h / SecretKey.cpp)
Represents an Ed25519 signing keypair (secret key + derived public key).

**Internal state:**
- `mKeyType` (`PublicKeyType`) — always `PUBLIC_KEY_TYPE_ED25519`
- `mSecretKey` (`uint512` / 64-byte opaque array) — the libsodium combined secret key
- `mPublicKey` (`PublicKey`) — the corresponding public key (XDR type)
- Nested `Seed` struct holds a 32-byte seed; its destructor zeroes memory.

**Key methods:**
- `getPublicKey()` — returns const ref to `mPublicKey`
- `getStrKeySeed()` — returns StrKey-encoded seed as `SecretValue`
- `getStrKeyPublic()` — returns StrKey-encoded public key as `std::string`
- `isZero()` — true if seed is all-zero
- `sign(ByteSlice)` — produces a 64-byte Ed25519 detached signature via `crypto_sign_detached`
- `random()` — generates a cryptographically random keypair via `crypto_sign_keypair`
- `fromSeed(ByteSlice)` — derives keypair from a 32-byte seed via `crypto_sign_seed_keypair`
- `fromStrKeySeed(string)` — decodes a StrKey seed string, then derives keypair

**Destructor** zeroes `mSecretKey` memory to prevent key leakage.

### `PublicKey` (XDR-defined, utilities in SecretKey.h/cpp)
The XDR union type for public keys. The crypto subsystem provides `KeyFunctions<PublicKey>` specialization and the `PubKeyUtils` namespace.

### `PubKeyUtils` namespace (SecretKey.h / SecretKey.cpp)
Signature verification and public key utilities.

**Key functions:**
- `verifySig(PublicKey, Signature, ByteSlice)` → `VerifySigResult` — Verifies an Ed25519 signature. Uses a process-wide `RandomEvictionCache<Hash, bool>` (capacity 250,000 entries) protected by `gVerifySigCacheMutex`. Returns both the validity result and whether it was a cache hit/miss. Supports switching between libsodium (`crypto_sign_verify_detached`) and Rust ed25519-dalek (`rust_bridge::verify_ed25519_signature_dalek`) at the protocol 24 boundary.
- `clearVerifySigCache()` — clears the global cache
- `seedVerifySigCache(unsigned int)` — seeds the cache's random eviction PRNG
- `flushVerifySigCacheCounts(hits, misses)` — atomically reads and resets cache hit/miss counters
- `enableRustDalekVerify()` — one-way flag to switch signature verification to Rust ed25519-dalek
- `random()` — generates a random (non-signing-capable) public key

### `VerifySigResult` (SecretKey.h)
Struct with two fields: `bool valid` (signature validity) and `VerifySigCacheLookupResult cacheResult` (enum: `MISS`, `HIT`, `NO_LOOKUP`).

---

## Hashing Modules

### SHA-256 (SHA.h / SHA.cpp)

**Free functions:**
- `sha256(ByteSlice)` → `uint256` — one-shot SHA-256 hash via `crypto_hash_sha256`
- `subSha256(ByteSlice seed, uint64_t counter)` → `Hash` — SHA-256 of seed concatenated with XDR-serialized counter; used for sub-seeding per-transaction PRNGs in Soroban
- `hmacSha256(HmacSha256Key, ByteSlice)` → `HmacSha256Mac` — HMAC-SHA-256 via `crypto_auth_hmacsha256`
- `hmacSha256Verify(HmacSha256Mac, HmacSha256Key, ByteSlice)` → `bool` — constant-time HMAC verification via `crypto_auth_hmacsha256_verify`
- `hkdfExtract(ByteSlice)` → `HmacSha256Key` — unsalted HKDF-extract: `HMAC(<zero_key>, bytes)`
- `hkdfExpand(HmacSha256Key, ByteSlice)` → `HmacSha256Key` — single-step HKDF-expand: `HMAC(key, bytes||0x01)`

**`SHA256` class** — incremental (streaming) SHA-256 hasher:
- `reset()` — reinitializes state
- `add(ByteSlice)` — feeds data
- `finish()` → `uint256` — finalizes and returns hash (single use; throws if called again)

**`XDRSHA256` struct** — CRTP subclass of `XDRHasher<XDRSHA256>` wrapping `SHA256`. Used by `xdrSha256<T>(t)` template to hash any XDR object without intermediate serialization buffer.

### BLAKE2b (BLAKE2.h / BLAKE2.cpp)

**Free function:**
- `blake2(ByteSlice)` → `uint256` — one-shot BLAKE2b (256-bit output) via `crypto_generichash`

**`BLAKE2` class** — incremental BLAKE2b hasher (same API pattern as `SHA256`):
- `reset()`, `add(ByteSlice)`, `finish()` → `uint256`

**`XDRBLAKE2` struct** — CRTP subclass of `XDRHasher<XDRBLAKE2>` wrapping `BLAKE2`. Used by `xdrBlake2<T>(t)` template.

BLAKE2 is used internally in the signature verification cache key computation (`verifySigCacheKey` hashes public key + signature + message via BLAKE2).

### ShortHash / SipHash (ShortHash.h / ShortHash.cpp)

Fast, randomized, non-cryptographic hash for in-memory data structures (hash maps, etc.).

**`shortHash` namespace:**
- `initialize()` — generates a random per-process SipHash key via `crypto_shorthash_keygen`; must be called once at startup
- `getShortHashInitKey()` → `array<unsigned char, 16>` — returns current key (used for child process inheritance)
- `computeHash(ByteSlice)` → `uint64_t` — SipHash-2-4 via `crypto_shorthash`, mutex-protected to set `gHaveHashed` flag
- `xdrComputeHash<T>(t)` → `uint64_t` — hashes any XDR object without intermediate buffer, using `XDRShortHasher`

**`XDRShortHasher` struct** — CRTP subclass of `XDRHasher<XDRShortHasher>` wrapping `SipHash24` state. Initialized with the process-wide key.

**Thread safety:** All access to the global key `gKey` is mutex-protected via `gKeyMutex`.

### XDRHasher (XDRHasher.h)

CRTP base class template `XDRHasher<Derived>` providing an xdrpp-compatible archiver that feeds XDR-serialized bytes to a hash function without allocating an intermediate serialization buffer.

**Mechanism:**
- Maintains a 256-byte internal buffer (`mBuf`) for batching small writes
- `queueOrHash(bytes, size)` — buffers small writes; flushes and calls `Derived::hashBytes()` for larger ones
- `flush()` — sends any buffered bytes to the derived hasher
- `operator()` overloads handle XDR scalars (32/64-bit with endian swap), byte arrays (with XDR padding), and composite types (delegating to `xdr::xdr_traits<T>::save`)

Three concrete derivations: `XDRSHA256`, `XDRBLAKE2`, `XDRShortHasher`.

---

## Key Encoding / Decoding Modules

### StrKey (StrKey.h / StrKey.cpp)

Implements Stellar's base32-encoded key format with version byte and CRC-16 checksum.

**`strKey` namespace:**
- `StrKeyVersionByte` enum — version bytes for different key types: `STRKEY_PUBKEY_ED25519` ('G'), `STRKEY_SEED_ED25519` ('S'), `STRKEY_PRE_AUTH_TX` ('T'), `STRKEY_HASH_X` ('X'), `STRKEY_SIGNED_PAYLOAD_ED25519` ('P'), `STRKEY_MUXED_ACCOUNT_ED25519` ('M'), `STRKEY_CONTRACT` ('C')
- `toStrKey(ver, ByteSlice)` → `SecretValue` — encodes: `base32(version_byte || payload || crc16)`
- `fromStrKey(string, &ver, &decoded)` → `bool` — decodes and validates CRC-16 checksum
- `getStrKeySize(dataSize)` → `size_t` — computes encoded string length for a given payload size

### KeyUtils (KeyUtils.h / KeyUtils.cpp)

Template-based key conversion utilities using the `KeyFunctions<T>` trait.

**`KeyFunctions<T>` trait** — specialization point for each key type, providing:
- `getKeyTypeName()`, `getKeyVersionIsSupported()`, `getKeyVersionIsVariableLength()`
- `toKeyType()` / `toKeyVersion()` — convert between `StrKeyVersionByte` and the key type enum
- `getEd25519Value()` / `getKeyValue()` / `setKeyValue()` — access raw key bytes

**Specializations provided:** `KeyFunctions<PublicKey>` (in SecretKey.h/cpp), `KeyFunctions<SignerKey>` (in SignerKey.h/cpp).

**`KeyUtils` namespace template functions:**
- `toStrKey<T>(key)` — converts any key type to StrKey string
- `toShortString<T>(key)` — first 5 characters of StrKey (for logging)
- `fromStrKey<T>(string)` — parses StrKey string into a typed key
- `getKeyVersionSize(StrKeyVersionByte)` — returns expected raw key size for a version byte
- `canConvert<T, F>(fromKey)` — checks if key type conversion is possible
- `convertKey<T, F>(fromKey)` — converts between key types sharing the same Ed25519 value

### Hex (Hex.h / Hex.cpp)

Hex encoding/decoding utilities using libsodium.

- `binToHex(ByteSlice)` → `string` — hex-encodes bytes
- `hexAbbrev(ByteSlice)` → `string` — returns first 6 hex characters (3 bytes) for logging
- `hexToBin(string)` → `vector<uint8_t>` — hex-decodes a string
- `hexToBin256(string)` → `uint256` — hex-decodes exactly 32 bytes, throws otherwise

---

## Signer Key Utilities

### SignerKey (SignerKey.h / SignerKey.cpp)

Provides `KeyFunctions<SignerKey>` specialization supporting four signer key types:
- `SIGNER_KEY_TYPE_ED25519` — standard Ed25519 public key
- `SIGNER_KEY_TYPE_PRE_AUTH_TX` — pre-authorized transaction hash
- `SIGNER_KEY_TYPE_HASH_X` — hash(x) preimage signer
- `SIGNER_KEY_TYPE_ED25519_SIGNED_PAYLOAD` — Ed25519 key with attached payload (variable length, up to 96 bytes)

### SignerKeyUtils (SignerKeyUtils.h / SignerKeyUtils.cpp)

Factory functions for creating `SignerKey` objects:
- `preAuthTxKey(TransactionFrame)` — creates a pre-auth signer key from a transaction's contents hash
- `preAuthTxKey(FeeBumpTransactionFrame)` — same for fee-bump transactions
- `hashXKey(ByteSlice)` — creates a hash-x signer key by SHA-256 hashing the input
- `ed25519PayloadKey(uint256, opaque_vec<64>)` — creates an ed25519-signed-payload signer key

---

## Curve25519 / ECDH (Curve25519.h / Curve25519.cpp)

Provides Curve25519 Diffie-Hellman key agreement for the P2P overlay authentication system (PeerAuth). These keys are distinct from Ed25519 signing keys and are generated per-session.

**Key functions:**
- `curve25519RandomSecret()` → `Curve25519Secret` — generates random 32-byte scalar via `randombytes_buf`
- `curve25519DerivePublic(Curve25519Secret)` → `Curve25519Public` — derives public point via `crypto_scalarmult_base`
- `clearCurve25519Keys(pub, sec)` — zeroes both keys via `sodium_memzero`
- `curve25519DeriveSharedKey(localSecret, localPublic, remotePublic, localFirst)` → `HmacSha256Key` — performs ECDH (`crypto_scalarmult`), concatenates `shared_secret || publicA || publicB` (ordered by `localFirst`), then applies `hkdfExtract`
- `curve25519Encrypt<N>(remotePublic, ByteSlice)` → `opaque_vec<N>` — sealed box encryption via `crypto_box_seal`
- `curve25519Decrypt(localSecret, localPublic, ByteSlice)` → `opaque_vec<>` — sealed box decryption via `crypto_box_seal_open`

---

## Random Number Generation (Random.h / Random.cpp)

- `randomBytes(size_t length)` → `vector<uint8_t>` — generates cryptographically secure random bytes via libsodium's `randombytes_buf`. In fuzzing builds, uses a deterministic PRNG instead.

---

## Logging Utilities

### `StrKeyUtils::logKey(ostream, string)` (SecretKey.cpp)
Attempts to interpret a key string in all known formats (public key StrKey, seed StrKey, raw hex) and logs all possible interpretations. Used for diagnostic/debugging output.

### `HashUtils` namespace (SecretKey.h / SecretKey.cpp)
- `random()` → `Hash` — generates a random 32-byte hash via `randombytes_buf`

---

## Thread Safety and Shared State

The crypto subsystem is mostly stateless/pure-functional. The two pieces of process-wide shared mutable state are:

1. **Signature verification cache** (`gVerifySigCache`, `gVerifyCacheHit`, `gVerifyCacheMiss`, `gUseRustDalekVerify`) — protected by `gVerifySigCacheMutex`. The cache is a `RandomEvictionCache<Hash, bool>` of 250K entries. Cache keys are BLAKE2 hashes of `(public_key || signature || message)`. Access is serialized by mutex, but individual verify operations are performed outside the lock.

2. **ShortHash global key** (`gKey`, `gHaveHashed`) — protected by `gKeyMutex`. Initialized once at startup via `shortHash::initialize()`.

There are no background threads, event loops, or async tasks in the crypto subsystem.

---

## Key Data Flows

### Signature Creation
`SecretKey::sign(message)` → `crypto_sign_detached(message, secret_key)` → 64-byte `Signature`

### Signature Verification
`PubKeyUtils::verifySig(pubkey, sig, message)` → compute BLAKE2 cache key → lock mutex → check `gVerifySigCache` → if miss, unlock, verify via libsodium or Rust dalek (depending on `gUseRustDalekVerify` flag) → lock mutex → insert result into cache → return `VerifySigResult`

### StrKey Encoding
Raw key bytes → `strKey::toStrKey(version_byte, bytes)` → prepend version byte, append CRC-16 → base32-encode → StrKey string

### StrKey Decoding
StrKey string → `strKey::fromStrKey()` → base32-decode → verify CRC-16 → extract version byte and payload → `KeyUtils::fromStrKey<T>()` → construct typed key

### ECDH Shared Key Derivation (P2P overlay)
`curve25519RandomSecret()` → `curve25519DerivePublic()` → exchange public keys → `curve25519DeriveSharedKey()` → `crypto_scalarmult` (ECDH) → concatenate with ordered public keys → `hkdfExtract()` → `HmacSha256Key`

### XDR Hashing (zero-copy)
Any XDR object → `xdrSha256<T>(t)` / `xdrBlake2<T>(t)` / `shortHash::xdrComputeHash<T>(t)` → `xdr::archive(hasher, t)` → `XDRHasher::operator()` dispatches by XDR type → batched `hashBytes()` calls → finalize

---

## Ownership Relationships

- `SecretKey` **owns** its `mSecretKey` (64-byte secret), `mPublicKey` (XDR PublicKey), and `mKeyType`
- `ByteSlice` **borrows** (non-owning view) from any byte container
- `SHA256`, `BLAKE2` classes own their respective libsodium hash states
- `XDRHasher<D>` owns a 256-byte internal buffer; derived types own their hash state
- `XDRShortHasher` owns a `SipHash24` state initialized from the global key
- The global signature cache (`gVerifySigCache`) is a process-wide singleton `RandomEvictionCache<Hash, bool>`
- The global short-hash key (`gKey`) is a process-wide singleton byte array
- `Curve25519Secret` / `Curve25519Public` are value types (XDR `opaque_array<32>` wrappers)
- `SignerKey` is an XDR union owned by its creator; `SignerKeyUtils` functions are pure factories
