//! Tests for sending transactions to the target validators only, and for
//! the identity binding that ties a peer connection to a validator.

use super::*;
use libp2p::identity::ed25519;

const NETWORK_ID: [u8; 32] = [7; 32];

/// One overlay node on loopback with a validator key of its own.
struct Node {
    handle: OverlayHandle,
    state: Arc<SharedState>,
    metrics: Arc<OverlayMetrics>,
    peer_id: PeerId,
    port: u16,
    validator: ed25519::Keypair,
    admissions: mpsc::UnboundedReceiver<CoreCommand>,
    /// Transactions read from `admissions` so far.
    admitted: HashSet<[u8; 32]>,
    _events: mpsc::UnboundedReceiver<OverlayEvent>,
}

impl Node {
    fn node_id(&self) -> NodeId {
        self.validator.public().to_bytes()
    }

    /// What Core sends in `SetNodeIdentity`: this node's validator key
    /// signing the binding to this overlay's peer id.
    fn identity(&self) -> NodeIdentity {
        sign_identity(&self.validator, &NETWORK_ID, &self.peer_id)
    }

    async fn identify(&self) {
        self.handle.set_node_identity(self.identity()).await;
    }

    fn identified_peers(&self) -> usize {
        self.state.tx_router.lock().unwrap().identified_peer_count()
    }

    /// Core accepted `tx`.
    async fn submit(&self, tx: &Arc<ValidatedTx>) {
        let plan = self.handle.add_local_tx(Arc::clone(tx));
        self.handle.send_local_tx(plan).await;
    }

    async fn set_targets(&self, targets: Option<Vec<NodeId>>) {
        if self.handle.set_tx_targets(targets) {
            self.handle.flush_local_txs().await;
        }
    }

    /// Transactions newly admitted to this node's mempool within `window`.
    async fn admitted_within(&mut self, window: Duration) -> Vec<[u8; 32]> {
        let deadline = tokio::time::Instant::now() + window;
        let mut hashes = Vec::new();
        while let Ok(Some(command)) = tokio::time::timeout_at(deadline, self.admissions.recv()).await
        {
            if let CoreCommand::SubmitTx { tx, .. } = command {
                self.admitted.insert(*tx.hash());
                hashes.push(*tx.hash());
            }
        }
        hashes
    }

    /// Wait until `tx` has been admitted to this node's mempool (in any
    /// order relative to other transactions).
    async fn expect_admitted(&mut self, tx: &Arc<ValidatedTx>) {
        let deadline = tokio::time::Instant::now() + Duration::from_secs(10);
        while !self.admitted.contains(tx.hash()) {
            match tokio::time::timeout_at(deadline, self.admissions.recv()).await {
                Ok(Some(CoreCommand::SubmitTx { tx: got, .. })) => {
                    if got.hash() == tx.hash() {
                        assert_eq!(got.bytes(), tx.bytes());
                    }
                    self.admitted.insert(*got.hash());
                }
                Ok(Some(_)) => {}
                _ => panic!("transaction {:02x?} was not admitted", &tx.hash()[..4]),
            }
        }
    }

    fn counter(&self, pick: impl Fn(&OverlayMetrics) -> &AtomicU64) -> u64 {
        pick(&self.metrics).load(Ordering::Relaxed)
    }
}

fn sign_identity(validator: &ed25519::Keypair, network_id: &[u8; 32], peer: &PeerId) -> NodeIdentity {
    let signature: [u8; 64] = validator
        .sign(&node_id_binding(network_id, peer))
        .try_into()
        .unwrap();
    NodeIdentity {
        network_id: *network_id,
        node_id: validator.public().to_bytes(),
        signature,
    }
}

async fn start_node(port: u16) -> Node {
    let keypair = Keypair::generate_ed25519();
    let peer_id = keypair.public().to_peer_id();
    let metrics = Arc::new(OverlayMetrics::new());
    let (handle, events, admissions, overlay) =
        create_test_overlay(keypair, Arc::clone(&metrics)).unwrap();
    let state = Arc::clone(&overlay.state);
    tokio::spawn(async move { overlay.run("127.0.0.1", port).await });
    tokio::time::sleep(Duration::from_millis(100)).await;
    Node {
        handle,
        state,
        metrics,
        peer_id,
        port,
        validator: ed25519::Keypair::generate(),
        admissions,
        admitted: HashSet::new(),
        _events: events,
    }
}

/// Dial `listener` from `dialer` and wait until both sides have streams.
async fn connect(dialer: &Node, listener: &Node) {
    let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", listener.port)
        .parse()
        .unwrap();
    dialer.handle.dial(addr).await;
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let a = dialer
            .state
            .peer_streams
            .read()
            .await
            .contains_key(&listener.peer_id);
        let b = listener
            .state
            .peer_streams
            .read()
            .await
            .contains_key(&dialer.peer_id);
        if a && b {
            break;
        }
        assert!(Instant::now() < deadline, "peers did not connect");
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    // Let the SCP state exchange of a new connection settle.
    tokio::time::sleep(Duration::from_millis(200)).await;
}

async fn wait_until(what: &str, condition: impl Fn() -> bool) {
    let deadline = Instant::now() + Duration::from_secs(10);
    while !condition() {
        assert!(Instant::now() < deadline, "timed out waiting for {what}");
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
}

/// The key scenario: in a full mesh a transaction reaches the two targets
/// and nobody else, without a single advert; a pending transaction follows
/// the targets; an externalized one is not sent again.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_transaction_goes_to_the_targets_only_and_follows_them() {
    let origin = start_node(19951).await;
    let mut leader1 = start_node(19952).await;
    let mut leader2 = start_node(19953).await;
    let mut other = start_node(19954).await;
    connect(&origin, &leader1).await;
    connect(&origin, &leader2).await;
    connect(&origin, &other).await;
    connect(&leader1, &leader2).await;
    connect(&leader1, &other).await;
    connect(&leader2, &other).await;

    // Identities arrive one node at a time, so some claims reach a node
    // before its own identity (and with it the network id) does.
    for node in [&leader1, &leader2, &other, &origin] {
        node.identify().await;
    }
    for node in [&origin, &leader1, &leader2, &other] {
        wait_until("every peer to be identified", || {
            node.identified_peers() == 3
        })
        .await;
    }
    {
        let router = origin.state.tx_router.lock().unwrap();
        assert_eq!(router.node_of(&leader1.peer_id), Some(&leader1.node_id()));
        assert_eq!(router.node_of(&other.peer_id), Some(&other.node_id()));
    }

    origin
        .set_targets(Some(vec![leader1.node_id(), leader2.node_id()]))
        .await;
    let included = test_tx(1);
    let pending = test_tx(2);
    origin.submit(&included).await;
    origin.submit(&pending).await;

    leader1.expect_admitted(&included).await;
    leader1.expect_admitted(&pending).await;
    leader2.expect_admitted(&included).await;
    leader2.expect_admitted(&pending).await;
    // Nobody else gets them: the origin sent no advert, and the targets do
    // not relay what was sent to them directly.
    assert!(other.admitted_within(Duration::from_millis(700)).await.is_empty());
    assert_eq!(origin.counter(|m| &m.flood_advertised), 0);
    assert_eq!(origin.counter(|m| &m.flood_tx_direct_sent), 4);
    assert_eq!(origin.counter(|m| &m.flood_tx_local_flooded), 0);
    for leader in [&leader1, &leader2] {
        assert_eq!(leader.counter(|m| &m.flood_tx_direct_recv), 2);
        assert_eq!(leader.counter(|m| &m.flood_advertised), 0);
        assert_eq!(leader.counter(|m| &m.flood_demanded), 0);
    }

    // `included` externalizes; the next ledger's leaders are `other` and
    // `leader1`. `pending` is not new any more and may be in the set the
    // old targets are voting on: it does not move yet.
    origin.handle.ledger_externalized(&[*included.hash()]);
    assert_eq!(origin.handle.local_tx_count(), 1);
    origin
        .set_targets(Some(vec![other.node_id(), leader1.node_id()]))
        .await;
    assert!(other.admitted_within(Duration::from_millis(300)).await.is_empty());
    assert_eq!(origin.counter(|m| &m.flood_tx_direct_sent), 4);
    // The next two ledgers do not have it either: now it moves, and only
    // to the new target.
    origin.handle.ledger_externalized(&[]);
    origin.handle.ledger_externalized(&[]);
    origin.handle.flush_local_txs().await;
    other.expect_admitted(&pending).await;
    assert!(other.admitted_within(Duration::from_millis(500)).await.is_empty());
    assert_eq!(origin.counter(|m| &m.flood_tx_direct_sent), 5);
    assert_eq!(leader1.counter(|m| &m.flood_tx_direct_recv), 2);
    assert_eq!(origin.counter(|m| &m.flood_advertised), 0);

    // A copy that reaches a node after its transaction externalized there is
    // admitted all the same (the mempool does not know), and counted.
    let late = test_tx(3);
    other.handle.ledger_externalized(&[*late.hash()]);
    assert_eq!(other.counter(|m| &m.flood_tx_late_copy), 0);
    origin.submit(&late).await;
    other.expect_admitted(&late).await;
    wait_until("the late copy to be counted", || {
        other.counter(|m| &m.flood_tx_late_copy) == 1
    })
    .await;
    assert_eq!(leader1.counter(|m| &m.flood_tx_late_copy), 0);
}

/// Without a target list, and when a target is not an identified peer, a
/// transaction is flooded as before and reaches every node.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn without_reachable_targets_a_transaction_is_flooded() {
    let origin = start_node(19956).await;
    let mut relay = start_node(19957).await;
    let mut far = start_node(19958).await;
    // A chain: the origin is not connected to `far`.
    connect(&origin, &relay).await;
    connect(&relay, &far).await;
    for node in [&origin, &relay, &far] {
        node.identify().await;
    }
    wait_until("the origin to identify its peer", || {
        origin.identified_peers() == 1
    })
    .await;

    // No targets yet: flood.
    let first = test_tx(1);
    origin.submit(&first).await;
    relay.expect_admitted(&first).await;
    far.expect_admitted(&first).await;
    assert_eq!(origin.counter(|m| &m.flood_tx_local_flooded), 1);
    assert_eq!(origin.counter(|m| &m.flood_tx_direct_sent), 0);

    // A target the origin has no connection to: flood, through the relay.
    origin
        .set_targets(Some(vec![relay.node_id(), far.node_id()]))
        .await;
    let second = test_tx(2);
    origin.submit(&second).await;
    relay.expect_admitted(&second).await;
    far.expect_admitted(&second).await;
    assert_eq!(origin.counter(|m| &m.flood_tx_local_flooded), 2);
    assert_eq!(origin.counter(|m| &m.flood_tx_direct_sent), 0);
}

/// A node that got a transaction directly does not relay it, but joins in
/// when the origin later has to flood it (a new target it cannot reach).
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_directly_received_transaction_joins_a_later_flood() {
    let origin = start_node(19961).await;
    let mut relay = start_node(19962).await;
    let mut far = start_node(19963).await;
    connect(&origin, &relay).await;
    connect(&relay, &far).await;
    for node in [&origin, &relay, &far] {
        node.identify().await;
    }
    wait_until("the origin to identify its peer", || {
        origin.identified_peers() == 1
    })
    .await;

    origin.set_targets(Some(vec![relay.node_id()])).await;
    let tx = test_tx(1);
    origin.submit(&tx).await;
    relay.expect_admitted(&tx).await;
    assert!(far.admitted_within(Duration::from_millis(700)).await.is_empty());
    assert_eq!(relay.counter(|m| &m.flood_tx_direct_recv), 1);

    // Time passes: the transaction is long gone from the short-lived buffers
    // that serve a flood's demands.
    *origin.state.tx_buffer.write().await = TxBuffer::new();
    *relay.state.tx_buffer.write().await = TxBuffer::new();

    // The next leader is `far`, which the origin cannot reach: it floods
    // the pending transaction, and the relay, which already has it, passes
    // the advert on and serves the body.
    origin.set_targets(Some(vec![far.node_id()])).await;
    far.expect_admitted(&tx).await;
    assert_eq!(origin.counter(|m| &m.flood_tx_local_flooded), 1);
}

/// An identity claim only counts if the validator's key signed it for this
/// very connection.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_forged_identity_claim_is_rejected() {
    let honest = start_node(19966).await;
    let mut forger = start_node(19967).await;
    let mut replayer = start_node(19968).await;
    let victim = ed25519::Keypair::generate();
    connect(&forger, &honest).await;
    connect(&replayer, &honest).await;
    honest.identify().await;

    // Claims the victim's node id, signed with the forger's own key.
    let mut forged = sign_identity(&forger.validator, &NETWORK_ID, &forger.peer_id);
    forged.node_id = victim.public().to_bytes();
    forger.handle.set_node_identity(forged).await;
    // Replays a binding the victim signed for another peer id.
    let replayed = sign_identity(&victim, &NETWORK_ID, &forger.peer_id);
    replayer.handle.set_node_identity(replayed).await;

    wait_until("both claims to be rejected", || {
        honest.counter(|m| &m.node_id_rejected) == 2
    })
    .await;
    assert_eq!(honest.identified_peers(), 0);

    // A binding for another network does not count either.
    let other_network = sign_identity(&victim, &[8; 32], &replayer.peer_id);
    replayer
        .handle
        .set_node_identity(NodeIdentity {
            network_id: NETWORK_ID,
            ..other_network
        })
        .await;
    wait_until("the third claim to be rejected", || {
        honest.counter(|m| &m.node_id_rejected) == 3
    })
    .await;
    assert_eq!(honest.identified_peers(), 0);

    // A peer cannot keep this node busy with claims: beyond a few per
    // connection they are not even checked.
    for _ in 0..(MAX_NODE_ID_CLAIMS_PER_PEER + 4) {
        let forged = sign_identity(&forger.validator, &NETWORK_ID, &replayer.peer_id);
        forger.handle.set_node_identity(forged).await;
    }
    tokio::time::sleep(Duration::from_millis(700)).await;
    assert_eq!(
        honest.counter(|m| &m.node_id_rejected),
        2 + u64::from(MAX_NODE_ID_CLAIMS_PER_PEER),
        "the three before (one of them the forger's), and the forger's up to its limit"
    );
    assert_eq!(honest.identified_peers(), 0);

    // With the victim as the target, nothing is sent directly to either
    // impostor: the transaction is flooded instead.
    honest
        .set_targets(Some(vec![victim.public().to_bytes()]))
        .await;
    let tx = test_tx(1);
    honest.submit(&tx).await;
    forger.expect_admitted(&tx).await;
    replayer.expect_admitted(&tx).await;
    assert_eq!(honest.counter(|m| &m.flood_tx_direct_sent), 0);
    assert_eq!(honest.counter(|m| &m.flood_tx_local_flooded), 1);
}

#[test]
fn the_binding_depends_on_network_and_peer() {
    let peer_a = Keypair::generate_ed25519().public().to_peer_id();
    let peer_b = Keypair::generate_ed25519().public().to_peer_id();
    let validator = ed25519::Keypair::generate();
    let identity = sign_identity(&validator, &NETWORK_ID, &peer_a);
    assert!(verify_node_id_binding(
        &NETWORK_ID,
        &peer_a,
        &identity.node_id,
        &identity.signature
    ));
    assert!(!verify_node_id_binding(
        &NETWORK_ID,
        &peer_b,
        &identity.node_id,
        &identity.signature
    ));
    assert!(!verify_node_id_binding(
        &[8; 32],
        &peer_a,
        &identity.node_id,
        &identity.signature
    ));
    assert_ne!(
        node_id_binding(&NETWORK_ID, &peer_a),
        node_id_binding(&NETWORK_ID, &peer_b)
    );
}
