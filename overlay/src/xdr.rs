//! Stellar XDR parsing helpers for overlay wire data.

use sha2::{Digest, Sha256};
use std::fmt;
use stellar_xdr::curr as xdr;
use xdr::{
    GeneralizedTransactionSet, Limits, MuxedAccount, Operation, OperationBody, ReadXdr, ScpBallot,
    ScpEnvelope, ScpStatementPledges, StellarMessage, StellarValue, TransactionEnvelope, WriteXdr,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum TxClass {
    Classic,
    Soroban,
}

#[derive(Debug)]
pub(crate) struct ParsedTx {
    pub(crate) envelope_xdr: Vec<u8>,
    pub(crate) full_hash: [u8; 32],
    pub(crate) source_account: [u8; 32],
    pub(crate) sequence: i64,
    pub(crate) fee: u64,
    pub(crate) num_ops: u32,
    pub(crate) class: TxClass,
}

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

pub(crate) fn parse_supported_transaction(bytes: &[u8]) -> Result<ParsedTx, XdrError> {
    let envelope = TransactionEnvelope::from_xdr(bytes, Limits::none())?;
    parse_supported_transaction_envelope(envelope)
}

pub(crate) fn parse_supported_transaction_envelope(
    envelope: TransactionEnvelope,
) -> Result<ParsedTx, XdrError> {
    let (source_account, sequence, fee, num_ops, class) = match &envelope {
        TransactionEnvelope::TxV0(v0) => {
            let tx = &v0.tx;
            (
                tx.source_account_ed25519.0,
                tx.seq_num.0,
                tx.fee as u64,
                tx.operations.len() as u32,
                classify_operations(tx.operations.as_ref()),
            )
        }
        TransactionEnvelope::Tx(v1) => {
            let tx = &v1.tx;
            (
                source_account_bytes(&tx.source_account),
                tx.seq_num.0,
                tx.fee as u64,
                tx.operations.len() as u32,
                classify_operations(tx.operations.as_ref()),
            )
        }
        TransactionEnvelope::TxFeeBump(_) => {
            // TODO: Support fee-bump transactions by classifying the inner
            // transaction and using the outer fee bid for prioritization.
            return Err(XdrError::UnsupportedFeeBump);
        }
    };

    let envelope_xdr = envelope.to_xdr(Limits::none())?;
    let full_hash = sha256_hash(&envelope_xdr);

    Ok(ParsedTx {
        envelope_xdr,
        full_hash,
        source_account,
        sequence,
        fee,
        num_ops,
        class,
    })
}

pub(crate) fn encode_transaction_message_from_xdr(bytes: &[u8]) -> Result<Vec<u8>, XdrError> {
    let envelope = TransactionEnvelope::from_xdr(bytes, Limits::none())?;
    match envelope {
        TransactionEnvelope::TxFeeBump(_) => {
            // TODO: Support fee-bump transactions by classifying the inner
            // transaction and using the outer fee bid for prioritization.
            Err(XdrError::UnsupportedFeeBump)
        }
        envelope => encode_stellar_message(&StellarMessage::Transaction(envelope)),
    }
}

pub(crate) fn parse_stellar_message(bytes: &[u8]) -> Result<StellarMessage, XdrError> {
    Ok(StellarMessage::from_xdr(bytes, Limits::none())?)
}

pub(crate) fn encode_stellar_message(message: &StellarMessage) -> Result<Vec<u8>, XdrError> {
    Ok(message.to_xdr(Limits::none())?)
}

pub(crate) fn canonical_transaction_xdr(
    envelope: TransactionEnvelope,
) -> Result<Vec<u8>, XdrError> {
    parse_supported_transaction_envelope(envelope).map(|parsed| parsed.envelope_xdr)
}

pub(crate) fn encode_scp_message(envelope_xdr: &[u8]) -> Result<Vec<u8>, XdrError> {
    let envelope = ScpEnvelope::from_xdr(envelope_xdr, Limits::none())?;
    encode_stellar_message(&StellarMessage::ScpMessage(envelope))
}

pub(crate) fn canonical_scp_envelope_xdr(envelope: ScpEnvelope) -> Result<Vec<u8>, XdrError> {
    Ok(envelope.to_xdr(Limits::none())?)
}

pub(crate) fn encode_get_scp_state(ledger_seq: u32) -> Result<Vec<u8>, XdrError> {
    encode_stellar_message(&StellarMessage::GetScpState(ledger_seq))
}

pub(crate) fn encode_generalized_tx_set_message(
    data: &[u8],
    expected_hash: &[u8; 32],
) -> Result<Vec<u8>, XdrError> {
    let tx_set = GeneralizedTransactionSet::from_xdr(data, Limits::none())?;
    let canonical = tx_set.to_xdr(Limits::none())?;
    let actual_hash = sha256_hash(&canonical);
    if &actual_hash != expected_hash {
        return Err(XdrError::Malformed(format!(
            "tx set hash mismatch: expected {:02x?}, got {:02x?}",
            &expected_hash[..4],
            &actual_hash[..4]
        )));
    }
    encode_stellar_message(&StellarMessage::GeneralizedTxSet(tx_set))
}

pub(crate) fn canonical_generalized_tx_set_xdr(
    tx_set: GeneralizedTransactionSet,
) -> Result<([u8; 32], Vec<u8>), XdrError> {
    let canonical = tx_set.to_xdr(Limits::none())?;
    let hash = sha256_hash(&canonical);
    Ok((hash, canonical))
}

pub fn verify_generalized_tx_set_xdr(
    expected_hash: &[u8; 32],
    data: &[u8],
) -> Result<Vec<u8>, XdrError> {
    let tx_set = GeneralizedTransactionSet::from_xdr(data, Limits::none())?;
    let canonical = tx_set.to_xdr(Limits::none())?;
    let actual_hash = sha256_hash(&canonical);
    if &actual_hash == expected_hash {
        Ok(canonical)
    } else {
        Err(XdrError::Malformed(format!(
            "tx set hash mismatch: expected {:02x?}, got {:02x?}",
            &expected_hash[..4],
            &actual_hash[..4]
        )))
    }
}

pub fn extract_txset_hashes_from_scp(envelope_xdr: &[u8]) -> Vec<[u8; 32]> {
    let Ok(envelope) = ScpEnvelope::from_xdr(envelope_xdr, Limits::none()) else {
        return Vec::new();
    };

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

fn source_account_bytes(account: &MuxedAccount) -> [u8; 32] {
    match account {
        MuxedAccount::Ed25519(ed25519) => ed25519.0,
        MuxedAccount::MuxedEd25519(muxed) => muxed.ed25519.0,
    }
}

fn classify_operations(operations: &[Operation]) -> TxClass {
    if operations.iter().any(|op| {
        matches!(
            op.body,
            OperationBody::InvokeHostFunction(_)
                | OperationBody::ExtendFootprintTtl(_)
                | OperationBody::RestoreFootprint(_)
        )
    }) {
        TxClass::Soroban
    } else {
        TxClass::Classic
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use xdr::{
        DecoratedSignature, Hash, Operation, ScpNomination, ScpStatementPledges, SequenceNumber,
        StellarValueExt, TimePoint, Transaction, TransactionV1Envelope, Value, VecM,
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

    #[test]
    fn parses_supported_transaction_metadata() {
        let tx_xdr = valid_transaction_xdr(1000, 12345, 1);
        let parsed = parse_supported_transaction(&tx_xdr).unwrap();

        assert_eq!(parsed.fee, 1000);
        assert_eq!(parsed.num_ops, 1);
        assert_eq!(parsed.sequence, 12345);
        assert_eq!(parsed.class, TxClass::Classic);
        assert_eq!(parsed.full_hash, sha256_hash(&parsed.envelope_xdr));
    }

    #[test]
    fn rejects_malformed_transaction_xdr() {
        assert!(matches!(
            parse_supported_transaction(&[1, 2, 3]),
            Err(XdrError::Malformed(_))
        ));
    }

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

        let envelope_xdr = envelope.to_xdr(Limits::none()).unwrap();
        let hashes = extract_txset_hashes_from_scp(&envelope_xdr);

        assert_eq!(hashes, vec![expected_hash]);
    }
}
