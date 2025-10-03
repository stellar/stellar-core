// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

use ed25519_dalek::{Signature, Verifier, VerifyingKey};

// Verifies an ed25519 signature using the dalek library.
// This function takes raw pointers to avoid copying data across the Rust/C++ boundary.
//
// # Safety
// The caller must ensure that:
// - `public_key_ptr` points to at least 32 bytes of readable memory
// - `signature_ptr` points to at least 64 bytes of readable memory
// - `message_ptr` points to at least `message_len` bytes of readable memory
// - All pointers remain valid for the duration of the call
#[no_mangle]
pub unsafe extern "C" fn verify_ed25519_signature_dalek(
    public_key_ptr: *const u8,
    signature_ptr: *const u8,
    message_ptr: *const u8,
    message_len: usize,
) -> bool {
    let _span = tracy_span!("verify_ed25519_signature_dalek");

    // C++ caller must provide valid pointers
    let pk_bytes = &*(public_key_ptr as *const [u8; 32]);
    let sig_bytes = &*(signature_ptr as *const [u8; 64]);
    let message = std::slice::from_raw_parts(message_ptr, message_len);

    // Parse public key
    let verifying_key = match VerifyingKey::from_bytes(pk_bytes) {
        Ok(key) => key,
        Err(_) => return false, // Invalid public key format
    };

    // Create signature (from_bytes returns the signature directly)
    let signature = Signature::from_bytes(sig_bytes);
    verifying_key.verify(message, &signature).is_ok()
}
