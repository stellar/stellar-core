//! Stellar XDR codec helpers for overlay wire data.
//!
//! This module is a pure codec over bytes and XDR values: it strict-decodes
//! messages, frames outbound messages, and derives content hashes. It holds no
//! domain types and no `Arc`/channel concerns — the trust-boundary transaction
//! type lives in [`crate::wire`].

use sha2::{Digest, Sha256};
use std::fmt;
use stellar_xdr::curr as xdr;
use xdr::{
    Limits, MessageType, ReadXdr, ScpBallot, ScpEnvelope, ScpStatementPledges, StellarMessage,
    StellarValue,
};

#[derive(Debug)]
pub enum XdrError {
    Malformed(String),
    UnsupportedFeeBump,
}

impl fmt::Display for XdrError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            XdrError::Malformed(e) => write!(f, "malformed XDR: {e}"),
            XdrError::UnsupportedFeeBump => write!(f, "fee-bump transactions are unsupported"),
        }
    }
}

impl std::error::Error for XdrError {}

impl From<xdr::Error> for XdrError {
    fn from(value: xdr::Error) -> Self {
        XdrError::Malformed(value.to_string())
    }
}

pub(crate) fn sha256_hash(data: &[u8]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(data);
    let result = hasher.finalize();
    let mut hash = [0u8; 32];
    hash.copy_from_slice(&result);
    hash
}

// --- Wire framing --------------------------------------------------------
//
// A `StellarMessage` is an XDR union `switch (MessageType type)`, encoded as a
// 4-byte big-endian discriminant followed by the selected arm's body with no
// additional framing. For the arms the overlay sends, the arm body is either an
// already-canonical payload (a `TransactionEnvelope`/`ScpEnvelope`/
// `GeneralizedTransactionSet` we hold as validated bytes) or a fixed-layout
// value (`Hash`, `uint32`). So we can produce the exact wire bytes by prefixing
// the discriminant, avoiding a decode + typed re-encode round trip on the hot
// path.
//
// This equivalence is load-bearing and is pinned by `frames_match_typed_*`
// tests below (one per arm); if stellar-xdr ever changes union layout or an
// arm's canonical encoding, those tests fail loudly.

/// Prefix `payload` with the 4-byte big-endian discriminant for `kind`.
fn frame(kind: MessageType, payload: &[u8]) -> Vec<u8> {
    let discriminant = (kind as i32).to_be_bytes();
    let mut out = Vec::with_capacity(discriminant.len() + payload.len());
    out.extend_from_slice(&discriminant);
    out.extend_from_slice(payload);
    out
}

/// `StellarMessage::Transaction(..)` framing over canonical envelope bytes.
pub(crate) fn frame_transaction(envelope_bytes: &[u8]) -> Vec<u8> {
    frame(MessageType::Transaction, envelope_bytes)
}

/// `StellarMessage::ScpMessage(..)` framing over canonical envelope bytes.
pub(crate) fn frame_scp(envelope_bytes: &[u8]) -> Vec<u8> {
    frame(MessageType::ScpMessage, envelope_bytes)
}

/// `StellarMessage::GeneralizedTxSet(..)` framing over canonical tx set bytes.
pub(crate) fn frame_tx_set(tx_set_bytes: &[u8]) -> Vec<u8> {
    frame(MessageType::GeneralizedTxSet, tx_set_bytes)
}

/// `StellarMessage::GetTxSet(hash)` framing.
pub(crate) fn frame_get_tx_set(hash: [u8; 32]) -> Vec<u8> {
    frame(MessageType::GetTxSet, &hash)
}

/// `StellarMessage::GetScpState(ledger_seq)` framing.
pub(crate) fn frame_get_scp_state(ledger_seq: u32) -> Vec<u8> {
    frame(MessageType::GetScpState, &ledger_seq.to_be_bytes())
}

/// Cheap content-hash check with no decode — used for tx sets built by our
/// trusted local core, where we skip decoding but still guard against a
/// hash/bytes mismatch that would make the set unfetchable network-wide.
pub fn tx_set_hash_matches(expected_hash: &[u8; 32], data: &[u8]) -> bool {
    &sha256_hash(data) == expected_hash
}

/// Strict-decode a `StellarMessage` off the wire (full consumption + zero
/// padding enforced by stellar-xdr).
pub(crate) fn parse_stellar_message(bytes: &[u8]) -> Result<StellarMessage, XdrError> {
    Ok(StellarMessage::from_xdr(bytes, Limits::none())?)
}

/// Extract tx set hashes from an already-decoded SCP envelope. Lets the SCP
/// stream reader reuse the single decode it already performed.
pub(crate) fn extract_txset_hashes_from_envelope(envelope: &ScpEnvelope) -> Vec<[u8; 32]> {
    let mut hashes = Vec::new();
    match &envelope.statement.pledges {
        ScpStatementPledges::Prepare(prepare) => {
            collect_ballot_hash(&mut hashes, &prepare.ballot);
            if let Some(ballot) = &prepare.prepared {
                collect_ballot_hash(&mut hashes, ballot);
            }
            if let Some(ballot) = &prepare.prepared_prime {
                collect_ballot_hash(&mut hashes, ballot);
            }
        }
        ScpStatementPledges::Confirm(confirm) => collect_ballot_hash(&mut hashes, &confirm.ballot),
        ScpStatementPledges::Externalize(externalize) => {
            collect_ballot_hash(&mut hashes, &externalize.commit);
        }
        ScpStatementPledges::Nominate(nominate) => {
            for value in nominate.votes.iter().chain(nominate.accepted.iter()) {
                collect_stellar_value_hash(&mut hashes, value.as_ref());
            }
        }
    }
    hashes
}

fn collect_ballot_hash(hashes: &mut Vec<[u8; 32]>, ballot: &ScpBallot) {
    collect_stellar_value_hash(hashes, ballot.value.as_ref());
}

fn collect_stellar_value_hash(hashes: &mut Vec<[u8; 32]>, value: &[u8]) {
    let Ok(stellar_value) = StellarValue::from_xdr(value, Limits::none()) else {
        return;
    };
    let hash: [u8; 32] = stellar_value.tx_set_hash.into();
    if !hashes.contains(&hash) {
        hashes.push(hash);
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use xdr::{
        DecoratedSignature, GeneralizedTransactionSet, Hash, Limits, MuxedAccount, Operation,
        ScpNomination, ScpStatementPledges, SequenceNumber, Signature, SignatureHint,
        StellarValueExt, TimePoint, Transaction, TransactionEnvelope, TransactionV1Envelope,
        Uint256, Value, WriteXdr,
    };
    use xdr::{
        FeeBumpTransaction, FeeBumpTransactionEnvelope, FeeBumpTransactionExt,
        FeeBumpTransactionInnerTx, VecM,
    };

    pub(crate) fn valid_transaction_xdr(fee: u32, sequence: i64, num_ops: usize) -> Vec<u8> {
        let mut tx = Transaction {
            fee,
            seq_num: SequenceNumber(sequence),
            ..Transaction::default()
        };
        tx.operations = VecM::try_from(vec![Operation::default(); num_ops]).unwrap();
        let envelope = TransactionEnvelope::Tx(TransactionV1Envelope {
            tx,
            signatures: VecM::<DecoratedSignature, 20>::default(),
        });
        envelope.to_xdr(Limits::none()).unwrap()
    }

    pub(crate) fn valid_fee_bump_xdr() -> Vec<u8> {
        let inner = TransactionV1Envelope {
            tx: Transaction {
                fee: 100,
                ..Transaction::default()
            },
            signatures: VecM::<DecoratedSignature, 20>::default(),
        };
        let envelope = TransactionEnvelope::TxFeeBump(FeeBumpTransactionEnvelope {
            tx: FeeBumpTransaction {
                fee_source: MuxedAccount::Ed25519(Uint256([0; 32])),
                fee: 2000,
                inner_tx: FeeBumpTransactionInnerTx::Tx(inner),
                ext: FeeBumpTransactionExt::V0,
            },
            signatures: VecM::<DecoratedSignature, 20>::default(),
        });
        envelope.to_xdr(Limits::none()).unwrap()
    }

    fn scp_envelope_xdr() -> Vec<u8> {
        let mut envelope = ScpEnvelope::default();
        envelope.statement.slot_index = 7;
        envelope.to_xdr(Limits::none()).unwrap()
    }

    fn generalized_tx_set_xdr() -> Vec<u8> {
        GeneralizedTransactionSet::default()
            .to_xdr(Limits::none())
            .unwrap()
    }

    // --- Canonicality guard --------------------------------------------------
    //
    // The whole "frame by concatenation, hash the original bytes, never
    // re-encode" strategy rests on strict decode implying canonical bytes:
    // `to_xdr(from_xdr(b)) == b` for any `b` that decodes. These tests pin that
    // guarantee so a stellar-xdr change that breaks it fails here rather than
    // silently diverging tx-set/transaction hashes across the network.

    fn assert_round_trips<T>(bytes: &[u8])
    where
        T: ReadXdr + WriteXdr,
    {
        let decoded = T::from_xdr(bytes, Limits::none()).expect("decodes");
        assert_eq!(
            decoded.to_xdr(Limits::none()).unwrap(),
            bytes,
            "re-encode is not byte-identical to input"
        );
    }

    #[test]
    fn strict_decode_is_canonical() {
        assert_round_trips::<TransactionEnvelope>(&valid_transaction_xdr(1000, 1, 2));
        assert_round_trips::<TransactionEnvelope>(&valid_fee_bump_xdr());
        assert_round_trips::<ScpEnvelope>(&scp_envelope_xdr());
        assert_round_trips::<GeneralizedTransactionSet>(&generalized_tx_set_xdr());
    }

    #[test]
    fn strict_decode_rejects_nonzero_padding() {
        // A 5-byte signature encodes with 3 zero padding bytes at the tail of
        // the envelope. A decoder that tolerated non-zero padding would let two
        // distinct byte strings decode to the same value — exactly what would
        // silently break hashing and forwarding the original bytes.
        let envelope = TransactionEnvelope::Tx(TransactionV1Envelope {
            tx: Transaction::default(),
            signatures: vec![DecoratedSignature {
                hint: SignatureHint([0; 4]),
                signature: Signature(vec![7u8; 5].try_into().unwrap()),
            }]
            .try_into()
            .unwrap(),
        });
        let bytes = envelope.to_xdr(Limits::none()).unwrap();
        assert!(TransactionEnvelope::from_xdr(&bytes, Limits::none()).is_ok());

        let mut mutated = bytes;
        let last = mutated.len() - 1;
        assert_eq!(mutated[last], 0, "expected trailing padding byte");
        mutated[last] = 0xff;
        assert!(TransactionEnvelope::from_xdr(&mutated, Limits::none()).is_err());
    }

    #[test]
    fn strict_decode_rejects_trailing_bytes() {
        let mut bytes = valid_transaction_xdr(1000, 1, 1);
        bytes.push(0x00); // one extra byte past a complete encoding
        assert!(TransactionEnvelope::from_xdr(&bytes, Limits::none()).is_err());
    }

    #[test]
    fn strict_decode_rejects_truncated_input() {
        let bytes = valid_transaction_xdr(1000, 1, 1);
        assert!(TransactionEnvelope::from_xdr(&bytes[..bytes.len() - 1], Limits::none()).is_err());
    }

    // --- frame() equivalence: one per arm we emit ---------------------------

    #[test]
    fn frames_match_typed_transaction() {
        let bytes = valid_transaction_xdr(1000, 1, 1);
        let expected = StellarMessage::Transaction(
            TransactionEnvelope::from_xdr(&bytes, Limits::none()).unwrap(),
        )
        .to_xdr(Limits::none())
        .unwrap();
        assert_eq!(frame_transaction(&bytes), expected);
    }

    #[test]
    fn frames_match_typed_scp() {
        let bytes = scp_envelope_xdr();
        let expected =
            StellarMessage::ScpMessage(ScpEnvelope::from_xdr(&bytes, Limits::none()).unwrap())
                .to_xdr(Limits::none())
                .unwrap();
        assert_eq!(frame_scp(&bytes), expected);
    }

    #[test]
    fn frames_match_typed_tx_set() {
        let bytes = generalized_tx_set_xdr();
        let expected = StellarMessage::GeneralizedTxSet(
            GeneralizedTransactionSet::from_xdr(&bytes, Limits::none()).unwrap(),
        )
        .to_xdr(Limits::none())
        .unwrap();
        assert_eq!(frame_tx_set(&bytes), expected);
    }

    #[test]
    fn frames_match_typed_get_tx_set() {
        let hash = [0x5a; 32];
        let expected = StellarMessage::GetTxSet(Uint256(hash))
            .to_xdr(Limits::none())
            .unwrap();
        assert_eq!(frame_get_tx_set(hash), expected);
    }

    #[test]
    fn frames_match_typed_get_scp_state() {
        let expected = StellarMessage::GetScpState(12345)
            .to_xdr(Limits::none())
            .unwrap();
        assert_eq!(frame_get_scp_state(12345), expected);
    }

    // --- content-hash guard -------------------------------------------------

    #[test]
    fn tx_set_hash_matches_only_on_matching_hash() {
        let bytes = generalized_tx_set_xdr();
        let hash = sha256_hash(&bytes);
        assert!(tx_set_hash_matches(&hash, &bytes));
        assert!(!tx_set_hash_matches(&[0xff; 32], &bytes));
    }

    // --- SCP tx set hash extraction -----------------------------------------

    #[test]
    fn extracts_txset_hashes_from_scp_values() {
        let expected_hash = [0x42; 32];
        let stellar_value = StellarValue {
            tx_set_hash: Hash(expected_hash),
            close_time: TimePoint(1_704_067_200),
            upgrades: VecM::default(),
            ext: StellarValueExt::Basic,
        };
        let value = Value::try_from(stellar_value.to_xdr(Limits::none()).unwrap()).unwrap();

        let mut envelope = ScpEnvelope::default();
        envelope.statement.pledges = ScpStatementPledges::Nominate(ScpNomination {
            quorum_set_hash: Hash([0; 32]),
            votes: VecM::try_from(vec![value.clone()]).unwrap(),
            accepted: VecM::try_from(vec![value]).unwrap(),
        });

        let hashes = extract_txset_hashes_from_envelope(&envelope);
        assert_eq!(hashes, vec![expected_hash]);
    }
}
