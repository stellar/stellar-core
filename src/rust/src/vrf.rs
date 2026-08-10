// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// ECVRF-EDWARDS25519-SHA512-TAI as specified in RFC 9381
// (https://www.rfc-editor.org/rfc/rfc9381), Section 5.
//
// This implements the "try-and-increment" ciphersuite (suite_string = 0x03)
// over the edwards25519 group, reusing the curve25519-dalek group operations
// already pulled into the dependency tree through ed25519-dalek. The
// implementation follows RFC 9381 faithfully so it can be checked against the
// official test vectors in RFC 9381 Appendix B.3.

use curve25519_dalek::edwards::{CompressedEdwardsY, EdwardsPoint};
use curve25519_dalek::scalar::Scalar;
use curve25519_dalek::traits::Identity;
use sha2::digest::generic_array::GenericArray;
use sha2::{Digest, Sha512};
use zeroize::Zeroizing;

// The suite_string for ECVRF-EDWARDS25519-SHA512-TAI (RFC 9381 Section 5.5).
const SUITE_STRING: u8 = 0x03;

// Challenge length in octets (RFC 9381: cLen = 16).
const CLEN: usize = 16;

// Field/group element length in octets (RFC 9381: qLen = fLen = 32).
const QLEN: usize = 32;

// Proof layout: Gamma (32) || c (16) || s (32).
const PI_GAMMA_OFFSET: usize = 0;
const PI_C_OFFSET: usize = PI_GAMMA_OFFSET + QLEN;
const PI_S_OFFSET: usize = PI_C_OFFSET + CLEN;
const PI_LEN: usize = PI_S_OFFSET + QLEN; // 80

// RFC 9381 Section 5.5 for ECVRF-EDWARDS25519-SHA512-TAI:
//   * encode_to_curve_salt = PK_string
//   * int_to_string / string_to_int are little-endian (RFC 8032 Section 5.1.2)
//   * point_to_string / string_to_point follow RFC 8032 Section 5.1.2/5.1.3
//   * Hash = SHA-512, hLen = 64
//   * ECVRF_encode_to_curve = try-and-increment (Section 5.4.1.1) with
//     interpret_hash_value_as_a_point(s) = string_to_point(s[0..32])

// RFC 9381 Sections 5.4.2.2 and 5.4.3 reduce the 64-byte hash output to an
// integer mod q by interpreting the octets little-endian. dalek's Scalar is
// arithmetic mod the group order L, and Scalar::from_bytes_mod_order_wide
// performs exactly the reduction of a 64-byte little-endian value.
fn hash_to_scalar(bytes: &[u8]) -> Scalar {
    // The 64-byte copy is wiped on return: this path is shared by the secret
    // nonce reduction (nonce_generation_rfc8032), where a stale copy of the
    // nonce hash would let an attacker recover the long-term scalar from the
    // public proof equation. Zeroizing it is harmless for the public
    // challenge path.
    let mut buf = Zeroizing::new([0u8; 64]);
    buf[..bytes.len()].copy_from_slice(bytes);
    Scalar::from_bytes_mod_order_wide(&buf)
}

// RFC 9381 Section 5.1.3 of RFC 8032: string_to_point for edwards25519.
//
// RFC 8032 Section 5.1.3 requires rejecting non-canonical encodings: an
// encoding whose integer value is >= p (recovered after clearing the sign
// bit), and an encoding whose sign bit is set while the decoded x-coordinate
// is 0 (e.g. the identity or the order-2 point encoded with the sign bit
// set). curve25519-dalek's FieldElement::from_bytes reduces values mod p and
// ignores the high bit, so its decompress accepts several of these non-
// canonical forms. A decompress/recompress round trip is enforced here to
// make the accepted set exactly the canonical encodings.
fn string_to_point(s: &[u8]) -> Option<EdwardsPoint> {
    let compressed = CompressedEdwardsY::from_slice(s).ok()?;
    let point = compressed.decompress()?;
    if point.compress().as_bytes() != compressed.as_bytes() {
        return None;
    }
    Some(point)
}

// RFC 9381 Section 5.4.1.1: ECVRF_encode_to_curve_try_and_increment with
// interpret_hash_value_as_a_point(s) = string_to_point(s[0..32]).
//
// Returns None (RFC 9381's INVALID) if none of the 256 counter values yields
// a valid point, instead of panicking: a panic escaping the cxx bridge would
// terminate stellar-core, whereas the bridge contract is to fail closed on
// invalid input.
fn encode_to_curve(pk_string: &[u8], alpha: &[u8]) -> Option<EdwardsPoint> {
    // front = 0x01, back = 0x00 (domain separators for encode_to_curve).
    for ctr in 0u8..=255 {
        let mut hasher = Sha512::new();
        hasher.update([SUITE_STRING]);
        hasher.update([0x01]); // encode_to_curve_domain_separator_front
        hasher.update(pk_string); // encode_to_curve_salt
        hasher.update(alpha);
        hasher.update([ctr]);
        hasher.update([0x00]); // encode_to_curve_domain_separator_back
        let hash = hasher.finalize();

        if let Some(h) = string_to_point(&hash[..QLEN]) {
            // Multiply by the cofactor (8) to land in the prime-order group.
            let h = h * Scalar::from(8u64);
            if h != EdwardsPoint::identity() {
                return Some(h);
            }
        }
    }
    // Each attempt succeeds with probability ~1/2, so all 256 failing is
    // astronomically unlikely; still, honor the RFC's INVALID outcome.
    None
}

// RFC 9381 Section 5.4.2.2: ECVRF_nonce_generation_RFC8032.
fn nonce_generation_rfc8032(sk: &[u8; 32], h_string: &[u8]) -> Scalar {
    // hashed_sk_string = Hash(SK) = SHA-512(SK)
    // The keyed intermediates are wrapped in Zeroizing so they are wiped when
    // this function returns: a stale k (with the public proof equation) would
    // otherwise leak the long-term seed. finalize_into writes the digest
    // straight into the Zeroizing buffer so no unwiped digest temporary is
    // ever created.
    let mut hashed_sk = Zeroizing::new([0u8; 64]);
    let mut hasher = Sha512::new();
    hasher.update(sk);
    hasher.finalize_into(GenericArray::from_mut_slice(&mut *hashed_sk));
    // k_string = Hash(truncated_hashed_sk_string || h_string)
    let mut k_string = Zeroizing::new([0u8; 64]);
    let mut hasher = Sha512::new();
    hasher.update(&hashed_sk[32..]);
    hasher.update(h_string);
    hasher.finalize_into(GenericArray::from_mut_slice(&mut *k_string));
    // k = string_to_int(k_string) mod q
    hash_to_scalar(&*k_string)
}

// RFC 9381 Section 5.4.3: ECVRF_challenge_generation(P1..P5).
fn challenge_generation(points: [&EdwardsPoint; 5]) -> [u8; CLEN] {
    let mut hasher = Sha512::new();
    hasher.update([SUITE_STRING]);
    hasher.update([0x02]); // challenge_generation_domain_separator_front
    for p in points.iter() {
        hasher.update(p.compress().to_bytes());
    }
    hasher.update([0x00]); // challenge_generation_domain_separator_back
    let c_string = hasher.finalize();
    let mut c = [0u8; CLEN];
    c.copy_from_slice(&c_string[..CLEN]);
    c
}

// Derive the VRF secret scalar x and public key from a 32-byte seed,
// following RFC 8032 Section 5.1.5 (Ed25519 key clamping).
//
// x is held in a Zeroizing wrapper so it is wiped when the VrfKey is dropped
// (proving is the only place the long-term scalar must live in memory).
// The public point y is retained so vrf_prove does not recompute the fixed-base
// multiplication solely to recover it for challenge generation.
struct VrfKey {
    x: Zeroizing<Scalar>,
    y: EdwardsPoint,
    pk_bytes: [u8; QLEN],
}

fn derive_key(sk: &[u8; 32]) -> VrfKey {
    // The expanded key (SHA-512(SK)) is secret seed material; keep the digest
    // and the clamped scalar bytes in Zeroizing wrappers as well.
    // finalize_into writes the digest directly into the Zeroizing buffer so
    // the expanded key never sits in a separately dropped plaintext temporary.
    let mut hashed = Zeroizing::new([0u8; 64]);
    let mut hasher = Sha512::new();
    hasher.update(sk);
    hasher.finalize_into(GenericArray::from_mut_slice(&mut *hashed));
    // RFC 8032 clamping of the scalar from the first half of SHA-512(SK).
    let mut h = Zeroizing::new([0u8; 32]);
    h.copy_from_slice(&hashed[..32]);
    h[0] &= 248;
    h[31] &= 127;
    h[31] |= 64;
    // Note: the scalar here is interpreted as little-endian per RFC 8032.
    let x = Zeroizing::new(Scalar::from_bytes_mod_order(*h));
    let y = EdwardsPoint::mul_base(&x);
    VrfKey {
        x,
        y,
        pk_bytes: y.compress().to_bytes(),
    }
}

// The full ECVRF proof. Layout matches RFC 9381: Gamma || c || s.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VrfProof {
    pub gamma: [u8; QLEN],
    pub c: [u8; CLEN],
    pub s: [u8; QLEN],
}

impl VrfProof {
    pub fn to_bytes(&self) -> [u8; PI_LEN] {
        let mut out = [0u8; PI_LEN];
        out[PI_GAMMA_OFFSET..PI_C_OFFSET].copy_from_slice(&self.gamma);
        out[PI_C_OFFSET..PI_S_OFFSET].copy_from_slice(&self.c);
        out[PI_S_OFFSET..].copy_from_slice(&self.s);
        out
    }

    pub fn from_bytes(bytes: &[u8]) -> Option<Self> {
        if bytes.len() != PI_LEN {
            return None;
        }
        let mut gamma = [0u8; QLEN];
        let mut c = [0u8; CLEN];
        let mut s = [0u8; QLEN];
        gamma.copy_from_slice(&bytes[PI_GAMMA_OFFSET..PI_C_OFFSET]);
        c.copy_from_slice(&bytes[PI_C_OFFSET..PI_S_OFFSET]);
        s.copy_from_slice(&bytes[PI_S_OFFSET..]);
        Some(VrfProof { gamma, c, s })
    }
}

// RFC 9381 Section 5.1: ECVRF_prove.
//
// Returns None when ECVRF_encode_to_curve reports INVALID (RFC 9381
// Section 5.4.1.1), so callers can fail closed instead of panicking.
pub fn vrf_prove_bytes(sk: &[u8; 32], alpha: &[u8]) -> Option<VrfProof> {
    let key = derive_key(sk);

    // Step 2: H = ECVRF_encode_to_curve(encode_to_curve_salt, alpha)
    //   encode_to_curve_salt = PK_string for this ciphersuite.
    let h = encode_to_curve(&key.pk_bytes, alpha)?;

    // Step 3: h_string = point_to_string(H)
    let h_string = h.compress().to_bytes();

    // Step 4: Gamma = x*H
    let gamma = *key.x * h;

    // Step 5: k = ECVRF_nonce_generation(SK, h_string)
    //   The nonce is secret (a stale k plus the public proof equation would
    //   leak x), so it is wiped when proving returns.
    let k = Zeroizing::new(nonce_generation_rfc8032(sk, &h_string));

    // Step 6: c = ECVRF_challenge_generation(Y, H, Gamma, k*B, k*H)
    let kb = EdwardsPoint::mul_base(&k);
    let kh = *k * h;
    let c = challenge_generation([&key.y, &h, &gamma, &kb, &kh]);

    // Step 7: s = (k + c*x) mod q
    let c_scalar = hash_to_scalar(&c);
    let s = *k + c_scalar * *key.x;

    Some(VrfProof {
        gamma: gamma.compress().to_bytes(),
        c,
        s: s.to_bytes(),
    })
}

// RFC 9381 Section 5.2: ECVRF_proof_to_hash.
//
// Returns the 64-byte VRF output beta. This should only be run on a proof
// that has been produced by vrf_prove_bytes or validated by vrf_verify_bytes.
pub fn vrf_proof_to_hash_bytes(pi: &VrfProof) -> [u8; 64] {
    // Step 6: beta = Hash(suite || 0x03 || point_to_string(cofactor*Gamma)
    //                   || 0x00)
    let gamma = string_to_point(&pi.gamma).expect("invalid gamma in proof");
    // cofactor * Gamma (cofactor = 8)
    let cofactor_gamma = gamma * Scalar::from(8u64);

    let mut hasher = Sha512::new();
    hasher.update([SUITE_STRING]);
    hasher.update([0x03]); // proof_to_hash_domain_separator_front
    hasher.update(cofactor_gamma.compress().to_bytes());
    hasher.update([0x00]); // proof_to_hash_domain_separator_back
    let beta = hasher.finalize();
    let mut out = [0u8; 64];
    out.copy_from_slice(&beta);
    out
}

// RFC 9381 Section 5.3: ECVRF_verify.
//
// Returns (true, beta) when the proof is valid, (false, zeros) otherwise.
// validate_key is always performed (full collision resistance and
// unpredictability under malicious key generation, RFC 9381 Section 7.1).
pub fn vrf_verify_bytes(pk: &[u8; 32], alpha: &[u8], pi: &VrfProof) -> (bool, [u8; 64]) {
    let zero_beta = [0u8; 64];

    // Steps 1-3: parse the public key and validate it.
    let y = match string_to_point(pk) {
        Some(y) => y,
        None => return (false, zero_beta),
    };
    // ECVRF_validate_key: cofactor*Y must not be the identity.
    if y * Scalar::from(8u64) == EdwardsPoint::identity() {
        return (false, zero_beta);
    }

    // Steps 4-6: decode the proof; reject s >= q.
    let gamma = match string_to_point(&pi.gamma) {
        Some(g) => g,
        None => return (false, zero_beta),
    };
    let c = hash_to_scalar(&pi.c);
    let s = match Scalar::from_canonical_bytes(pi.s).into_option() {
        Some(s) => s,
        None => return (false, zero_beta), // s >= q: invalid proof
    };

    // Step 7: H = ECVRF_encode_to_curve(encode_to_curve_salt, alpha)
    //   INVALID here means the proof cannot be valid; fail closed.
    let h = match encode_to_curve(pk, alpha) {
        Some(h) => h,
        None => return (false, zero_beta),
    };

    // Steps 8-9: U = s*B - c*Y, V = s*H - c*Gamma
    let sb = EdwardsPoint::mul_base(&s);
    let cy = c * y;
    let u = sb - cy;
    let sh = s * h;
    let cgamma = c * gamma;
    let v = sh - cgamma;

    // Step 10: c' = ECVRF_challenge_generation(Y, H, Gamma, U, V)
    let c_prime = challenge_generation([&y, &h, &gamma, &u, &v]);

    // Step 11: compare c and c'.
    if c_prime != pi.c {
        return (false, zero_beta);
    }

    (true, vrf_proof_to_hash_bytes(pi))
}

// ---- C++ bridge interface -------------------------------------------------
//
// The pointer-based functions below are what the cxx bridge (bridge.rs)
// exposes to C++. They follow the same conventions as compute_sha256 and
// verify_ed25519_signature_dalek: null pointers and any invalid cryptographic
// input are rejected with `false` rather than panicking.

// ECVRF_prove over the cxx bridge. Writes the 80-byte proof to `pi_out` and
// returns true on success.
//
// # Safety
// `sk_ptr` must point to 32 readable bytes, `alpha_ptr` must point to
// `alpha_len` readable bytes (ignored when `alpha_len` is 0), and `pi_out`
// must point to PI_LEN (80) writable bytes.
pub(crate) unsafe fn vrf_prove(
    sk_ptr: *const u8,
    alpha_ptr: *const u8,
    alpha_len: usize,
    pi_out: *mut u8,
) -> bool {
    if sk_ptr.is_null() || pi_out.is_null() {
        return false;
    }
    let sk = &*(sk_ptr as *const [u8; 32]);
    let alpha: &[u8] = if alpha_len == 0 {
        &[]
    } else {
        if alpha_ptr.is_null() {
            return false;
        }
        std::slice::from_raw_parts(alpha_ptr, alpha_len)
    };
    let pi = match vrf_prove_bytes(sk, alpha) {
        Some(pi) => pi,
        None => return false,
    };
    std::ptr::copy_nonoverlapping(pi.to_bytes().as_ptr(), pi_out, PI_LEN);
    true
}

// ECVRF_generate over the cxx bridge: ECVRF_prove followed by
// ECVRF_proof_to_hash. Writes the 80-byte proof to `pi_out` and the 64-byte
// beta to `beta_out`, returning true on success. This is the one-call entry
// point for callers that only need the pseudorandom output and not the proof
// (the C++ contract advertises vrf_generate(sk, alpha, pi, beta)).
//
// # Safety
// `sk_ptr` must point to 32 readable bytes, `alpha_ptr` must point to
// `alpha_len` readable bytes (ignored when `alpha_len` is 0), `pi_out` must
// point to PI_LEN (80) writable bytes, and `beta_out` must point to 64
// writable bytes.
pub(crate) unsafe fn vrf_generate(
    sk_ptr: *const u8,
    alpha_ptr: *const u8,
    alpha_len: usize,
    pi_out: *mut u8,
    beta_out: *mut u8,
) -> bool {
    if sk_ptr.is_null() || pi_out.is_null() || beta_out.is_null() {
        return false;
    }
    let sk = &*(sk_ptr as *const [u8; 32]);
    let alpha: &[u8] = if alpha_len == 0 {
        &[]
    } else {
        if alpha_ptr.is_null() {
            return false;
        }
        std::slice::from_raw_parts(alpha_ptr, alpha_len)
    };
    let pi = match vrf_prove_bytes(sk, alpha) {
        Some(pi) => pi,
        None => return false,
    };
    std::ptr::copy_nonoverlapping(pi.to_bytes().as_ptr(), pi_out, PI_LEN);
    let beta = vrf_proof_to_hash_bytes(&pi);
    std::ptr::copy_nonoverlapping(beta.as_ptr(), beta_out, 64);
    true
}

// ECVRF_proof_to_hash over the cxx bridge. Writes the 64-byte beta to
// `beta_out` and returns true on success. Returns false if the proof does not
// decode: ECVRF_decode_proof (RFC 9381 Section 5.1) rejects a Gamma that is
// not a valid curve point and a non-canonical s (s >= q), so both are checked
// here before beta is derived, matching vrf_verify.
//
// # Safety
// `pi_ptr` must point to PI_LEN (80) readable bytes and `beta_out` must point
// to 64 writable bytes.
pub(crate) unsafe fn vrf_proof_to_hash(pi_ptr: *const u8, beta_out: *mut u8) -> bool {
    if pi_ptr.is_null() || beta_out.is_null() {
        return false;
    }
    let pi = match VrfProof::from_bytes(std::slice::from_raw_parts(pi_ptr, PI_LEN)) {
        Some(pi) => pi,
        None => return false,
    };
    if string_to_point(&pi.gamma).is_none() {
        return false;
    }
    if Scalar::from_canonical_bytes(pi.s).into_option().is_none() {
        return false; // s >= q: not a canonical scalar
    }
    let beta = vrf_proof_to_hash_bytes(&pi);
    std::ptr::copy_nonoverlapping(beta.as_ptr(), beta_out, 64);
    true
}

// ECVRF_verify over the cxx bridge. When the proof is valid, writes the
// 64-byte beta to `beta_out` and returns true; returns false otherwise.
//
// # Safety
// `pk_ptr` must point to 32 readable bytes, `alpha_ptr` must point to
// `alpha_len` readable bytes (ignored when `alpha_len` is 0), `pi_ptr` must
// point to PI_LEN (80) readable bytes, and `beta_out` must point to 64
// writable bytes.
pub(crate) unsafe fn vrf_verify(
    pk_ptr: *const u8,
    alpha_ptr: *const u8,
    alpha_len: usize,
    pi_ptr: *const u8,
    beta_out: *mut u8,
) -> bool {
    if pk_ptr.is_null() || pi_ptr.is_null() || beta_out.is_null() {
        return false;
    }
    let pk = &*(pk_ptr as *const [u8; 32]);
    let alpha: &[u8] = if alpha_len == 0 {
        &[]
    } else {
        if alpha_ptr.is_null() {
            return false;
        }
        std::slice::from_raw_parts(alpha_ptr, alpha_len)
    };
    let pi = match VrfProof::from_bytes(std::slice::from_raw_parts(pi_ptr, PI_LEN)) {
        Some(pi) => pi,
        None => return false,
    };
    let (valid, beta) = vrf_verify_bytes(pk, alpha, &pi);
    if !valid {
        return false;
    }
    std::ptr::copy_nonoverlapping(beta.as_ptr(), beta_out, 64);
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    // RFC 9381 Appendix B.3: ECVRF-EDWARDS25519-SHA512-TAI test vectors.

    fn unhex(s: &str) -> Vec<u8> {
        assert_eq!(s.len() % 2, 0, "odd-length hex string");
        s.as_bytes()
            .chunks(2)
            .map(|c| {
                let hi = (c[0] as char).to_digit(16).expect("bad hex digit");
                let lo = (c[1] as char).to_digit(16).expect("bad hex digit");
                ((hi << 4) | lo) as u8
            })
            .collect()
    }

    fn sk(s: &str) -> [u8; 32] {
        unhex(s).try_into().unwrap()
    }

    struct TestCase {
        sk: [u8; 32],
        pk: [u8; 32],
        alpha: Vec<u8>,
        pi: [u8; PI_LEN],
        beta: [u8; 64],
    }

    fn test_cases() -> Vec<TestCase> {
        vec![
            TestCase {
                sk: sk("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"),
                pk: sk("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"),
                alpha: Vec::new(),
                pi: unhex(
                    "8657106690b5526245a92b003bb079ccd1a92130477671f6fc01ad16f26f723f26f8a57cc\
                     aed74ee1b190bed1f479d9727d2d0f9b005a6e456a35d4fb0daab1268a1b0db10836d98\
                     26a528ca76567805",
                )
                .try_into()
                .unwrap(),
                beta: unhex(
                    "90cf1df3b703cce59e2a35b925d411164068269d7b2d29f3301c03dd757876ff66b71dd\
                     a49d2de59d03450451af026798e8f81cd2e333de5cdf4f3e140fdd8ae",
                )
                .try_into()
                .unwrap(),
            },
            TestCase {
                sk: sk("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb"),
                pk: sk("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c"),
                alpha: unhex("72"),
                pi: unhex(
                    "f3141cd382dc42909d19ec5110469e4feae18300e94f304590abdced48aed5933bf086\
                     4a62558b3ed7f2fea45c92a465301b3bbf5e3e54ddf2d935be3b67926da3ef39226bbc35\
                     5bdc9850112c8f4b02",
                )
                .try_into()
                .unwrap(),
                beta: unhex(
                    "eb4440665d3891d668e7e0fcaf587f1b4bd7fbfe99d0eb2211ccec90496310eb5e33821\
                     bc613efb94db5e5b54c70a848a0bef4553a41befc57663b56373a5031",
                )
                .try_into()
                .unwrap(),
            },
            TestCase {
                sk: sk("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7"),
                pk: sk("fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025"),
                alpha: unhex("af82"),
                pi: unhex(
                    "9bc0f79119cc5604bf02d23b4caede71393cedfbb191434dd016d30177ccbf8096bb474\
                     e53895c362d8628ee9f9ea3c0e52c7a5c691b6c18c9979866568add7a2d41b00b05081e\
                     d0f58ee5e31b3a970e",
                )
                .try_into()
                .unwrap(),
                beta: unhex(
                    "645427e5d00c62a23fb703732fa5d892940935942101e456ecca7bb217c61c452118fec\
                     1219202a0edcf038bb6373241578be7217ba85a2687f7a0310b2df19f",
                )
                .try_into()
                .unwrap(),
            },
        ]
    }

    #[test]
    fn prove_matches_test_vectors() {
        for (i, tc) in test_cases().iter().enumerate() {
            let pi = vrf_prove_bytes(&tc.sk, &tc.alpha).expect("RFC vector proves");
            assert_eq!(pi.to_bytes(), tc.pi, "Example {} pi", i + 16);
            assert_eq!(
                vrf_proof_to_hash_bytes(&pi),
                tc.beta,
                "Example {} beta",
                i + 16
            );
        }
    }

    #[test]
    fn verify_accepts_test_vectors() {
        for (i, tc) in test_cases().iter().enumerate() {
            let pi = VrfProof::from_bytes(&tc.pi).unwrap();
            let (valid, beta) = vrf_verify_bytes(&tc.pk, &tc.alpha, &pi);
            assert!(valid, "Example {} should verify", i + 16);
            assert_eq!(beta, tc.beta, "Example {} beta", i + 16);
        }
    }

    #[test]
    fn proof_roundtrips_through_bytes() {
        for tc in test_cases() {
            let pi = VrfProof::from_bytes(&tc.pi).unwrap();
            assert_eq!(pi.to_bytes(), tc.pi);
        }
        assert!(VrfProof::from_bytes(&tc_pi_short()).is_none());
    }

    fn tc_pi_short() -> Vec<u8> {
        // A truncated proof must be rejected.
        test_cases().remove(0).pi[..PI_LEN - 1].to_vec()
    }

    #[test]
    fn verify_rejects_tampered_proof() {
        let tc = test_cases().remove(0);
        let mut pi = tc.pi;
        pi[0] ^= 0xff; // flip a bit in Gamma
        let pi = VrfProof::from_bytes(&pi).unwrap();
        let (valid, _) = vrf_verify_bytes(&tc.pk, &tc.alpha, &pi);
        assert!(!valid);

        // Flipping a bit in s must also be rejected (s != 0 case).
        let tc = test_cases().remove(1);
        let mut pi = tc.pi;
        pi[PI_LEN - 1] ^= 0x01;
        let pi = VrfProof::from_bytes(&pi).unwrap();
        let (valid, _) = vrf_verify_bytes(&tc.pk, &tc.alpha, &pi);
        assert!(!valid);

        // A proof for a different message must not verify.
        let tc = test_cases().remove(0);
        let pi = VrfProof::from_bytes(&tc.pi).unwrap();
        let (valid, _) = vrf_verify_bytes(&tc.pk, b"another message", &pi);
        assert!(!valid);
    }

    #[test]
    fn verify_rejects_bad_public_key() {
        // All-zero public key is not a valid point on the curve.
        let tc = test_cases().remove(0);
        let pi = VrfProof::from_bytes(&tc.pi).unwrap();
        let (valid, _) = vrf_verify_bytes(&[0u8; 32], &tc.alpha, &pi);
        assert!(!valid);
    }

    #[test]
    fn verify_rejects_s_gte_q() {
        // RFC 9381 Section 5.1: ECVRF_decode_proof rejects a non-canonical s,
        // i.e. s >= q where q is the edwards25519 group order
        // (2^252 + 27742317777372353535851937790883648493). Probe the exact
        // boundary instead of an arbitrary large value: s = q is rejected by
        // the decoder before any point arithmetic; s = q - 1 is the largest
        // canonical s, so it decodes but still fails the verification
        // equation; and s = 2^256 - 1 (all ones) is far above q and must also
        // be rejected.
        let q = group_order();
        assert_eq!(
            Scalar::from_canonical_bytes(q).into_option(),
            None,
            "s = q must not decode as a canonical scalar"
        );

        let tc = test_cases().remove(0);

        // s = q (the group order itself): non-canonical, rejected.
        let mut pi = tc.pi;
        pi[PI_S_OFFSET..].copy_from_slice(&q);
        let pi = VrfProof::from_bytes(&pi).unwrap();
        let (valid, beta) = vrf_verify_bytes(&tc.pk, &tc.alpha, &pi);
        assert!(!valid);
        assert_eq!(beta, [0u8; 64]);

        // s = q - 1: the largest canonical encoding, so the proof decodes;
        // verification then fails because the forged s cannot satisfy the
        // ECVRF challenge equation (c' != c).
        let mut q_minus_1 = q;
        q_minus_1[0] = q[0] - 1; // q[0] = 0xed > 0, so no borrow
        assert_eq!(
            Scalar::from_canonical_bytes(q_minus_1)
                .into_option()
                .map(|_| ()),
            Some(()),
            "s = q - 1 must decode as a canonical scalar"
        );
        let mut pi = tc.pi;
        pi[PI_S_OFFSET..].copy_from_slice(&q_minus_1);
        let pi = VrfProof::from_bytes(&pi).unwrap();
        let (valid, _) = vrf_verify_bytes(&tc.pk, &tc.alpha, &pi);
        assert!(!valid);

        // s = 2^256 - 1 (all ones): far above q, must be rejected.
        let mut pi = tc.pi;
        for b in pi[PI_S_OFFSET..].iter_mut() {
            *b = 0xff;
        }
        let pi = VrfProof::from_bytes(&pi).unwrap();
        let (valid, _) = vrf_verify_bytes(&tc.pk, &tc.alpha, &pi);
        assert!(!valid);
    }

    // The edwards25519 group order q = 2^252 + 27742317777372353535851937790883648493,
    // encoded in little-endian bytes as required by Scalar::from_canonical_bytes.
    fn group_order() -> [u8; 32] {
        let mut q = [0u8; 32];
        q[0] = 0xed;
        q[1] = 0xd3;
        q[2] = 0xf5;
        q[3] = 0x5c;
        q[4] = 0x1a;
        q[5] = 0x63;
        q[6] = 0x12;
        q[7] = 0x58;
        q[8] = 0xd6;
        q[9] = 0x9c;
        q[10] = 0xf7;
        q[11] = 0xa2;
        q[12] = 0xde;
        q[13] = 0xf9;
        q[14] = 0xde;
        q[15] = 0x14;
        q[31] = 0x10;
        q
    }

    #[test]
    fn proof_to_hash_rejects_non_canonical_s() {
        // vrf_proof_to_hash must decode the proof like vrf_verify does: a
        // non-canonical s (s >= q, the edwards25519 group order) is rejected
        // instead of hashed, and beta must be left untouched.
        let q = group_order();
        let tc = test_cases().remove(0);

        // s = q exactly: non-canonical, rejected and beta untouched.
        let mut pi = tc.pi;
        pi[PI_S_OFFSET..].copy_from_slice(&q);
        let mut beta_out = [0u8; 64];
        let ok = unsafe { vrf_proof_to_hash(pi.as_ptr(), beta_out.as_mut_ptr()) };
        assert!(!ok);
        assert_eq!(beta_out, [0u8; 64]);

        // s = q - 1 (largest canonical): accepted by the decoder, and since
        // beta is derived only from Gamma, the output is unchanged.
        let mut q_minus_1 = q;
        q_minus_1[0] = q[0] - 1;
        let mut pi = tc.pi;
        pi[PI_S_OFFSET..].copy_from_slice(&q_minus_1);
        let ok = unsafe { vrf_proof_to_hash(pi.as_ptr(), beta_out.as_mut_ptr()) };
        assert!(ok);
        assert_eq!(beta_out, tc.beta);

        // The unmodified proof still hashes to the RFC vector beta.
        let ok = unsafe { vrf_proof_to_hash(tc.pi.as_ptr(), beta_out.as_mut_ptr()) };
        assert!(ok);
        assert_eq!(beta_out, tc.beta);
    }

    #[test]
    fn string_to_point_rejects_non_canonical_encodings() {
        // RFC 8032 Section 5.1.3 step 3: when the decoded x-coordinate is 0,
        // the sign bit must be 0. The identity (y = 1, x = 0) encoded with the
        // sign bit set is accepted by curve25519-dalek's decompress but must
        // be rejected by string_to_point.
        let mut identity_with_sign = [0u8; QLEN];
        identity_with_sign[0] = 0x01; // y = 1
        identity_with_sign[31] = 0x80; // sign bit set (x_0 = 1, x = 0)
        assert!(string_to_point(&identity_with_sign).is_none());

        // The order-2 point (x = 0, y = -1) with the sign bit set must also be
        // rejected: y = p - 1 = 2^255 - 20, which is canonical when the sign
        // bit is clear but not when it is set.
        let mut order2_with_sign = [0u8; QLEN];
        order2_with_sign[0] = 0xec; // low byte of p - 1
        for b in order2_with_sign[1..31].iter_mut() {
            *b = 0xff;
        }
        order2_with_sign[31] = 0xff; // 0x7f (y = p-1) with the sign bit set
        assert!(string_to_point(&order2_with_sign).is_none());

        // A canonical point still parses (the RFC 9381 test-vector public key).
        let tc = test_cases().remove(0);
        assert!(string_to_point(&tc.pk).is_some());

        // A proof carrying a non-canonical Gamma must fail verification rather
        // than being accepted.
        let mut pi = tc.pi;
        pi[PI_GAMMA_OFFSET..PI_C_OFFSET].copy_from_slice(&identity_with_sign);
        let pi = VrfProof::from_bytes(&pi).unwrap();
        let (valid, beta) = vrf_verify_bytes(&tc.pk, &tc.alpha, &pi);
        assert!(!valid);
        assert_eq!(beta, [0u8; 64]);
    }

    #[test]
    fn bridge_api_roundtrip() {
        // Exercise the pointer-based functions exposed to C++ through the cxx
        // bridge, checking them against the RFC vectors and the failure modes.
        let tc = test_cases().remove(0);
        let alpha_ptr = if tc.alpha.is_empty() {
            std::ptr::null()
        } else {
            tc.alpha.as_ptr()
        };

        let mut pi_out = [0u8; PI_LEN];
        let ok = unsafe {
            vrf_prove(
                tc.sk.as_ptr(),
                alpha_ptr,
                tc.alpha.len(),
                pi_out.as_mut_ptr(),
            )
        };
        assert!(ok);
        assert_eq!(pi_out, tc.pi);

        // vrf_generate is prove + proof_to_hash in one call: same proof and
        // same beta as the two-step path.
        let mut pi_gen = [0u8; PI_LEN];
        let mut beta_gen = [0u8; 64];
        let ok = unsafe {
            vrf_generate(
                tc.sk.as_ptr(),
                alpha_ptr,
                tc.alpha.len(),
                pi_gen.as_mut_ptr(),
                beta_gen.as_mut_ptr(),
            )
        };
        assert!(ok);
        assert_eq!(pi_gen, tc.pi);
        assert_eq!(beta_gen, tc.beta);

        let mut beta_out = [0u8; 64];
        let ok = unsafe { vrf_proof_to_hash(pi_out.as_ptr(), beta_out.as_mut_ptr()) };
        assert!(ok);
        assert_eq!(beta_out, tc.beta);

        let mut beta_out = [0u8; 64];
        let ok = unsafe {
            vrf_verify(
                tc.pk.as_ptr(),
                alpha_ptr,
                tc.alpha.len(),
                pi_out.as_ptr(),
                beta_out.as_mut_ptr(),
            )
        };
        assert!(ok);
        assert_eq!(beta_out, tc.beta);

        // A tampered proof must fail verification and leave beta untouched.
        let mut bad_pi = pi_out;
        bad_pi[0] ^= 0x01;
        let mut beta_out = [0u8; 64];
        let ok = unsafe {
            vrf_verify(
                tc.pk.as_ptr(),
                alpha_ptr,
                tc.alpha.len(),
                bad_pi.as_ptr(),
                beta_out.as_mut_ptr(),
            )
        };
        assert!(!ok);
        assert_eq!(beta_out, [0u8; 64]);

        // Null pointers must be rejected, not crash.
        assert!(!unsafe {
            vrf_prove(
                std::ptr::null(),
                alpha_ptr,
                tc.alpha.len(),
                pi_out.as_mut_ptr(),
            )
        });
        assert!(!unsafe {
            vrf_generate(
                std::ptr::null(),
                alpha_ptr,
                tc.alpha.len(),
                pi_out.as_mut_ptr(),
                beta_out.as_mut_ptr(),
            )
        });
        assert!(!unsafe { vrf_proof_to_hash(pi_out.as_ptr(), std::ptr::null_mut()) });
        assert!(!unsafe {
            vrf_verify(
                tc.pk.as_ptr(),
                alpha_ptr,
                tc.alpha.len(),
                pi_out.as_ptr(),
                std::ptr::null_mut(),
            )
        });
    }
}
