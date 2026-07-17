//! Trust boundary for the flooding pipeline.
//!
//! This module is the single place where raw transaction bytes are turned into
//! a [`ValidatedTx`]. There are exactly two ways in, one per provenance:
//!
//! * [`ValidatedTx::from_network`] — bytes that arrived from an untrusted peer.
//!   The caller (a per-peer stream reader) has *already* strict-decoded the
//!   enclosing `StellarMessage`, so we accept the decoded envelope and its
//!   original bytes and do **not** decode again (see the crate perf notes on
//!   parse-once). Fee-bumps are rejected here.
//! * [`ValidatedTx::from_core_trusted`] — bytes submitted by our local core over
//!   IPC. Core is trusted for encoding, so we do not decode; we only reject
//!   fee-bumps via a cheap 4-byte `EnvelopeType` discriminant check and hash the
//!   bytes.
//!
//! Because the fields are private and the only constructors are these two, every
//! `ValidatedTx` in the system upholds the invariant: `bytes` is a canonical,
//! non-fee-bump `TransactionEnvelope` encoding and `hash == sha256(bytes)`.

use std::fmt;
use std::sync::Arc;

use stellar_xdr::curr::{EnvelopeType, TransactionEnvelope};

use crate::xdr::{self, XdrError};

/// A transaction whose bytes are known-valid, canonical, non-fee-bump
/// `TransactionEnvelope` XDR, with its hash and fee metadata computed once.
///
/// Shared through the pipeline as `Arc<ValidatedTx>`; immutable after
/// construction, so sharing across tasks can never expose stale metadata.
pub struct ValidatedTx {
    bytes: Vec<u8>,
    hash: [u8; 32],
    fee: u64,
    num_ops: u32,
}

impl ValidatedTx {
    /// Mint from bytes received off the network.
    ///
    /// `envelope` must be the decode of `envelope_bytes` (the reader produced
    /// both from a single `StellarMessage` decode). We read fee/op metadata off
    /// the already-decoded `envelope` and hash the original `envelope_bytes` —
    /// no re-decode, no re-encode. Fee-bumps are rejected.
    ///
    /// Crate-private: this constructor cannot check that `envelope` really is
    /// the decode of `envelope_bytes`, so it is only exposed to the per-peer
    /// stream readers that produce both from a single decode.
    pub(crate) fn from_network(
        envelope: &TransactionEnvelope,
        envelope_bytes: &[u8],
    ) -> Result<Arc<Self>, XdrError> {
        let (fee, num_ops) = fee_and_ops(envelope)?;
        Ok(Arc::new(Self {
            hash: xdr::sha256_hash(envelope_bytes),
            bytes: envelope_bytes.to_vec(),
            fee,
            num_ops,
        }))
    }

    /// Mint from bytes submitted by the trusted local core.
    ///
    /// Core supplies `fee`/`num_ops` alongside the bytes, so we take them as
    /// given rather than decoding. We still reject fee-bumps (core forwards
    /// every tx, including fee-bumps, and the overlay does not yet support
    /// them) via the envelope's `EnvelopeType` discriminant, which is the first
    /// 4 bytes of the XDR encoding.
    pub fn from_core_trusted(
        bytes: Vec<u8>,
        fee: u64,
        num_ops: u32,
    ) -> Result<Arc<Self>, XdrError> {
        if bytes.len() < 4 {
            return Err(XdrError::Malformed("transaction envelope too short".into()));
        }
        let discriminant = i32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
        if discriminant == EnvelopeType::TxFeeBump as i32 {
            return Err(XdrError::UnsupportedFeeBump);
        }
        Ok(Arc::new(Self {
            hash: xdr::sha256_hash(&bytes),
            bytes,
            fee,
            num_ops,
        }))
    }

    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }

    pub fn hash(&self) -> &[u8; 32] {
        &self.hash
    }

    pub fn fee(&self) -> u64 {
        self.fee
    }

    pub fn num_ops(&self) -> u32 {
        self.num_ops
    }

    /// Fee per operation, used for flood prioritization. Guards against a zero
    /// op count so it never divides by zero.
    pub fn fee_per_op(&self) -> i64 {
        (self.fee / u64::from(self.num_ops.max(1))) as i64
    }

    /// The `StellarMessage::Transaction(..)` wire framing for flooding this tx.
    pub fn to_flood_frame(&self) -> Vec<u8> {
        xdr::frame_transaction(&self.bytes)
    }
}

impl fmt::Debug for ValidatedTx {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ValidatedTx")
            .field("hash", &format_args!("{:02x?}", &self.hash[..4]))
            .field("fee", &self.fee)
            .field("num_ops", &self.num_ops)
            .field("len", &self.bytes.len())
            .finish()
    }
}

/// Read fee and operation count off a decoded envelope, rejecting fee-bumps.
fn fee_and_ops(envelope: &TransactionEnvelope) -> Result<(u64, u32), XdrError> {
    match envelope {
        TransactionEnvelope::TxV0(v0) => Ok((u64::from(v0.tx.fee), v0.tx.operations.len() as u32)),
        TransactionEnvelope::Tx(v1) => Ok((u64::from(v1.tx.fee), v1.tx.operations.len() as u32)),
        TransactionEnvelope::TxFeeBump(_) => Err(XdrError::UnsupportedFeeBump),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::xdr::tests::{valid_fee_bump_xdr, valid_transaction_xdr};
    use stellar_xdr::curr::{Limits, TransactionEnvelope, WriteXdr};

    fn decode(bytes: &[u8]) -> TransactionEnvelope {
        use stellar_xdr::curr::ReadXdr;
        TransactionEnvelope::from_xdr(bytes, Limits::none()).unwrap()
    }

    #[test]
    fn from_network_extracts_metadata_and_hashes_original_bytes() {
        let bytes = valid_transaction_xdr(1000, 42, 3);
        let tx = ValidatedTx::from_network(&decode(&bytes), &bytes).unwrap();

        assert_eq!(tx.fee(), 1000);
        assert_eq!(tx.num_ops(), 3);
        assert_eq!(tx.bytes(), &bytes[..]);
        assert_eq!(tx.hash(), &xdr::sha256_hash(&bytes));
    }

    #[test]
    fn from_core_trusted_takes_core_metadata_without_decoding() {
        let bytes = valid_transaction_xdr(555, 7, 1);
        // Core's fee/ops are trusted verbatim, even if they differed from the
        // envelope contents (they should not, but we do not verify).
        let tx = ValidatedTx::from_core_trusted(bytes.clone(), 555, 2).unwrap();

        assert_eq!(tx.fee(), 555);
        assert_eq!(tx.num_ops(), 2);
        assert_eq!(tx.hash(), &xdr::sha256_hash(&bytes));
    }

    #[test]
    fn fee_per_op_guards_zero_ops() {
        let bytes = valid_transaction_xdr(300, 1, 1);
        let tx = ValidatedTx::from_core_trusted(bytes, 300, 0).unwrap();
        assert_eq!(tx.fee_per_op(), 300); // max(1) guard, no divide-by-zero
    }

    #[test]
    fn both_paths_reject_fee_bumps() {
        let bytes = valid_fee_bump_xdr();
        assert!(matches!(
            ValidatedTx::from_network(&decode(&bytes), &bytes),
            Err(XdrError::UnsupportedFeeBump)
        ));
        assert!(matches!(
            ValidatedTx::from_core_trusted(bytes, 0, 0),
            Err(XdrError::UnsupportedFeeBump)
        ));
    }

    #[test]
    fn from_core_trusted_rejects_short_input() {
        assert!(matches!(
            ValidatedTx::from_core_trusted(vec![0, 0], 0, 0),
            Err(XdrError::Malformed(_))
        ));
    }

    #[test]
    fn to_flood_frame_matches_typed_stellar_message() {
        use stellar_xdr::curr::StellarMessage;
        let bytes = valid_transaction_xdr(1000, 1, 1);
        let tx = ValidatedTx::from_network(&decode(&bytes), &bytes).unwrap();

        let expected = StellarMessage::Transaction(decode(&bytes))
            .to_xdr(Limits::none())
            .unwrap();
        assert_eq!(tx.to_flood_frame(), expected);
    }
}
