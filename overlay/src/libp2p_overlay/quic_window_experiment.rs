//! Latency experiment behind the initial QUIC congestion window (see
//! `DEFAULT_QUIC_INITIAL_CONGESTION_WINDOW`). Ignored by default: it needs a
//! link with real delay.

use super::*;
use rand::{RngCore, SeedableRng};

/// A valid `GeneralizedTransactionSet` whose zstd encoding is about
/// `encoded_bytes` long: one classic phase of transactions with random
/// accounts, memos and signatures (incompressible, like real payments).
fn large_test_txset(seed: u64, encoded_bytes: usize) -> ([u8; 32], Arc<TxSetData>) {
    use stellar_xdr::{
        BytesM, DecoratedSignature, GeneralizedTransactionSet, Hash, Limits, Memo, MuxedAccount,
        Operation, SequenceNumber, Signature, SignatureHint, Transaction, TransactionEnvelope,
        TransactionPhase, TransactionV1Envelope, TxSetComponent,
        TxSetComponentTxsMaybeDiscountedFee, Uint256, VecM, WriteXdr,
    };
    let mut rng = rand::rngs::StdRng::seed_from_u64(seed);
    // Each envelope carries about 132 random bytes.
    let count = encoded_bytes / 132 + 1;
    let mut txs = Vec::with_capacity(count);
    for i in 0..count {
        let mut account = [0u8; 32];
        let mut memo = [0u8; 32];
        let mut hint = [0u8; 4];
        let mut signature = [0u8; 64];
        rng.fill_bytes(&mut account);
        rng.fill_bytes(&mut memo);
        rng.fill_bytes(&mut hint);
        rng.fill_bytes(&mut signature);
        let tx = Transaction {
            source_account: MuxedAccount::Ed25519(Uint256(account)),
            fee: 100,
            seq_num: SequenceNumber(i as i64),
            memo: Memo::Hash(Hash(memo)),
            operations: VecM::try_from(vec![Operation::default()]).unwrap(),
            ..Transaction::default()
        };
        txs.push(TransactionEnvelope::Tx(TransactionV1Envelope {
            tx,
            signatures: VecM::try_from(vec![DecoratedSignature {
                hint: SignatureHint(hint),
                signature: Signature(BytesM::try_from(signature.to_vec()).unwrap()),
            }])
            .unwrap(),
        }));
    }
    let mut tx_set = GeneralizedTransactionSet::default();
    let GeneralizedTransactionSet::V1(v1) = &mut tx_set;
    v1.phases = VecM::try_from(vec![TransactionPhase::V0(
        VecM::try_from(vec![TxSetComponent::TxsetCompTxsMaybeDiscountedFee(
            TxSetComponentTxsMaybeDiscountedFee {
                base_fee: Some(100),
                txs: VecM::try_from(txs).unwrap(),
            },
        )])
        .unwrap(),
    )])
    .unwrap();
    let bytes = tx_set.to_xdr(Limits::none()).unwrap();
    let hash = crate::xdr::sha256_hash(&bytes);
    (hash, Arc::new(TxSetData::from_local(bytes).unwrap()))
}

fn env_or<T: std::str::FromStr>(name: &str, default: T) -> T {
    std::env::var(name)
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(default)
}

/// Latency experiment, not a pass/fail test: how long does one tx-set frame
/// take from `send_txset` on one node to `TxSetReceived` on the other, on a
/// link with real delay? Run it in a network namespace whose loopback has a
/// netem delay (75 ms each way = a 150 ms round trip):
///
/// ```text
/// unshare -rn sh -c 'ip link set lo up; \
///   tc qdisc add dev lo root netem delay 75ms limit 100000; \
///   <test binary> --ignored --nocapture txset_transfer_latency_experiment'
/// ```
///
/// Knobs: `EXP_ENCODED_KB` (frame size), `EXP_ROUNDS`, `EXP_GAP_MS` (idle
/// time between transfers), and `STELLAR_OVERLAY_QUIC_INITIAL_CWND`.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
#[ignore]
async fn txset_transfer_latency_experiment() {
    let encoded_kb: usize = env_or("EXP_ENCODED_KB", 600);
    let rounds: u64 = env_or("EXP_ROUNDS", 8);
    let gap_ms: u64 = env_or("EXP_GAP_MS", 2000);

    let keypair1 = Keypair::generate_ed25519();
    let keypair2 = Keypair::generate_ed25519();
    let peer2 = keypair2.public().to_peer_id();
    let (handle1, mut events1, _admissions1, overlay1) =
        create_test_overlay(keypair1, Arc::new(OverlayMetrics::new())).unwrap();
    let (handle2, mut events2, _admissions2, overlay2) =
        create_test_overlay(keypair2, Arc::new(OverlayMetrics::new())).unwrap();
    let listen_port = 21701;
    tokio::spawn(async move { overlay1.run("127.0.0.1", listen_port).await });
    tokio::spawn(async move { overlay2.run("127.0.0.1", listen_port + 1).await });
    tokio::time::sleep(Duration::from_millis(200)).await;
    let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listen_port)
        .parse()
        .unwrap();
    handle2.dial(addr).await;
    // Handshake, identify and stream negotiation take several round trips.
    tokio::time::sleep(Duration::from_millis(3000)).await;
    while events1.try_recv().is_ok() {}
    while events2.try_recv().is_ok() {}

    // One-way latency of a small control message, for reference.
    let scp = test_scp_envelope_xdr(200);
    let start = Instant::now();
    handle1.broadcast_scp(scp.clone()).await;
    loop {
        match tokio::time::timeout(Duration::from_secs(10), events2.recv()).await {
            Ok(Some(OverlayEvent::ScpReceived { envelope, .. })) if envelope == scp => break,
            Ok(Some(_)) => {}
            other => panic!("no SCP message: {:?}", other.is_ok()),
        }
    }
    println!(
        "EXP scp one-way: {:.1} ms (initial cwnd {:?})",
        start.elapsed().as_secs_f64() * 1e3,
        quic_initial_congestion_window()
    );

    for round in 0..rounds {
        let (hash, data) = large_test_txset(round, encoded_kb * 1024);
        let encoded = data.encoded().len();
        let start = Instant::now();
        handle1.send_txset(hash, data, peer2).await;
        loop {
            match tokio::time::timeout(Duration::from_secs(30), events2.recv()).await {
                Ok(Some(OverlayEvent::TxSetReceived { hash: got, .. })) if got == hash => break,
                Ok(Some(_)) => {}
                other => panic!("no tx set: {:?}", other.is_ok()),
            }
        }
        println!(
            "EXP round {} encoded {} bytes: {:.1} ms",
            round,
            encoded,
            start.elapsed().as_secs_f64() * 1e3
        );
        tokio::time::sleep(Duration::from_millis(gap_ms)).await;
    }
    handle1.shutdown().await;
    handle2.shutdown().await;
}
