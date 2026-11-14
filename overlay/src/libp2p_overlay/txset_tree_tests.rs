//! Tests for the spanning tree of the leader's tx-set push: several overlay
//! nodes on loopback, fully connected, each with a validator identity.

use super::*;
use crate::flood::txset_tree::{children_of, parent_of, TREE_FETCH_GRACE};
use libp2p::identity::ed25519;

const NETWORK_ID: [u8; 32] = [9; 32];
const SLOT: u32 = 40;
const SEED: [u8; 32] = [3; 32];

struct Node {
    handle: OverlayHandle,
    events: mpsc::UnboundedReceiver<OverlayEvent>,
    state: Arc<SharedState>,
    metrics: Arc<OverlayMetrics>,
    peer_id: PeerId,
    port: u16,
    validator: ed25519::Keypair,
    _admissions: mpsc::UnboundedReceiver<CoreCommand>,
}

impl Node {
    fn node_id(&self) -> NodeId {
        self.validator.public().to_bytes()
    }

    fn count(&self, pick: impl Fn(&OverlayMetrics) -> &AtomicU64) -> u64 {
        pick(&self.metrics).load(Ordering::Relaxed)
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
        events,
        state,
        metrics,
        peer_id,
        port,
        validator: ed25519::Keypair::generate(),
        _admissions: admissions,
    }
}

/// `count` nodes, every pair connected, every node knowing which validator
/// each of its peers is.
async fn full_mesh(first_port: u16, count: usize) -> Vec<Node> {
    let mut nodes = Vec::new();
    for i in 0..count {
        nodes.push(start_node(first_port + i as u16).await);
    }
    for a in 0..count {
        for b in a + 1..count {
            let addr: Multiaddr = format!("/ip4/127.0.0.1/udp/{}/quic-v1", nodes[b].port)
                .parse()
                .unwrap();
            nodes[a].handle.dial(addr).await;
        }
    }
    for node in &nodes {
        let signature: [u8; 64] = node
            .validator
            .sign(&node_id_binding(&NETWORK_ID, &node.peer_id))
            .try_into()
            .unwrap();
        node.handle
            .set_node_identity(NodeIdentity {
                network_id: NETWORK_ID,
                node_id: node.node_id(),
                signature,
            })
            .await;
    }
    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        let mut ready = true;
        for node in &nodes {
            let streams = node.state.peer_streams.read().await.len();
            let identified = node.state.tx_router.lock().unwrap().identified_peer_count();
            ready &= streams == count - 1 && identified == count - 1;
        }
        if ready {
            break;
        }
        assert!(Instant::now() < deadline, "the mesh did not form");
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    // Let the SCP state exchange of the new connections settle.
    tokio::time::sleep(Duration::from_millis(300)).await;
    nodes
}

fn node_ids(nodes: &[Node]) -> Vec<NodeId> {
    nodes.iter().map(Node::node_id).collect()
}

fn config(nodes: &[Node], mode: TreeMode, fanout: u8) -> TreeConfig {
    TreeConfig::new(mode, fanout, vec![(SLOT, SEED)], node_ids(nodes))
}

fn configure(nodes: &[Node], mode: TreeMode, fanout: u8) {
    for node in nodes {
        node.handle.set_txset_tree(config(nodes, mode, fanout));
    }
}

fn index_of(nodes: &[Node], id: &NodeId) -> usize {
    nodes.iter().position(|n| n.node_id() == *id).unwrap()
}

fn test_set(seed: u8) -> ([u8; 32], Arc<TxSetData>) {
    let (hash, xdr) = test_txset_xdr(seed);
    (hash, Arc::new(TxSetData::from_local(xdr).unwrap()))
}

/// Wait for a `TxSetReceived` of `hash`; returns the slot it carries (None:
/// nobody on this node had asked) and the sender.
async fn expect_set(node: &mut Node, hash: [u8; 32]) -> (Option<u32>, PeerId) {
    let deadline = tokio::time::Instant::now() + Duration::from_secs(10);
    loop {
        match tokio::time::timeout_at(deadline, node.events.recv()).await {
            Ok(Some(OverlayEvent::TxSetReceived {
                hash: got,
                slot,
                from,
                data,
            })) if got == hash => {
                assert_eq!(crate::xdr::sha256_hash(data.as_slice()), hash);
                return (slot, from);
            }
            Ok(Some(_)) => {}
            _ => panic!("no TxSetReceived for {:02x?}", &hash[..4]),
        }
    }
}

/// Wait until a peer asks `node` for `hash`, and answer as the App does
/// (these tests have no App behind the overlay). Returns the requester.
async fn serve_request(node: &mut Node, hash: [u8; 32], data: &Arc<TxSetData>) -> PeerId {
    let deadline = tokio::time::Instant::now() + Duration::from_secs(10);
    loop {
        match tokio::time::timeout_at(deadline, node.events.recv()).await {
            Ok(Some(OverlayEvent::TxSetRequested { hash: got, from })) if got == hash => {
                node.handle.send_txset(hash, Arc::clone(data), from).await;
                return from;
            }
            Ok(Some(_)) => {}
            _ => panic!("nobody asked for {:02x?}", &hash[..4]),
        }
    }
}

/// The hashes of the sets received and of the sets peers asked for within
/// `window`.
async fn set_events_within(node: &mut Node, window: Duration) -> (Vec<[u8; 32]>, Vec<[u8; 32]>) {
    let deadline = tokio::time::Instant::now() + window;
    let (mut received, mut requested) = (Vec::new(), Vec::new());
    loop {
        match tokio::time::timeout_at(deadline, node.events.recv()).await {
            Ok(Some(OverlayEvent::TxSetReceived { hash, .. })) => received.push(hash),
            Ok(Some(OverlayEvent::TxSetRequested { hash, .. })) => requested.push(hash),
            Ok(Some(_)) => {}
            _ => return (received, requested),
        }
    }
}

/// With a hash tree the leader writes its set to its children only, the
/// children pass it on, and every node receives it exactly once from its
/// parent in the tree, without asking.
#[tokio::test]
async fn a_set_reaches_every_node_down_the_hash_tree() {
    let mut nodes = full_mesh(24201, 7).await;
    configure(&nodes, TreeMode::Hash, 2);
    let leader = 3;
    let leader_id = nodes[leader].node_id();
    let order = config(&nodes, TreeMode::Hash, 2)
        .hash_order(SLOT, &leader_id)
        .unwrap();
    let (hash, data) = test_set(51);
    nodes[leader]
        .handle
        .push_txset_for_slot(hash, data, SLOT)
        .await;

    for i in 0..nodes.len() {
        if i == leader {
            continue;
        }
        let id = nodes[i].node_id();
        let parent = parent_of(&order, &id, 2).unwrap();
        let parent_peer = nodes[index_of(&nodes, &parent)].peer_id;
        let (slot, from) = expect_set(&mut nodes[i], hash).await;
        assert_eq!(slot, None, "node {i} had not asked");
        assert_eq!(from, parent_peer, "node {i} gets the set from its parent");
    }
    // Give stray copies time to show up.
    tokio::time::sleep(Duration::from_millis(300)).await;
    // The leader wrote two copies; the two relays two each; nobody else any.
    assert_eq!(nodes[leader].count(|m| &m.txset_tree_root_sent), 2);
    assert_eq!(nodes[leader].count(|m| &m.txset_push_sent), 2);
    assert_eq!(nodes[leader].count(|m| &m.txset_tree_relayed), 0);
    let mut relayed = 0;
    for i in 0..nodes.len() {
        if i == leader {
            continue;
        }
        let id = nodes[i].node_id();
        let children = children_of(&order, &id, 2).len() as u64;
        assert_eq!(
            nodes[i].count(|m| &m.txset_tree_relayed),
            children,
            "node {i}"
        );
        relayed += children;
        // One tagged frame each, none refused, none asked for.
        assert_eq!(nodes[i].count(|m| &m.txset_tree_received), 1, "node {i}");
        assert_eq!(nodes[i].count(|m| &m.txset_tree_refused), 0);
        assert_eq!(nodes[i].count(|m| &m.txset_tree_duplicate), 0);
        assert_eq!(nodes[i].count(|m| &m.fetch_txset_count), 0);
        assert_eq!(nodes[i].count(|m| &m.txset_tree_delay_count), 1);
        assert_eq!(nodes[i].count(|m| &m.txset_tree_root_sent), 0);
    }
    assert_eq!(relayed, 4);
    // Two frames came straight from the leader, four through a relay.
    let hop1: u64 = nodes
        .iter()
        .map(|n| n.count(|m| &m.txset_tree_delay_hop1_count))
        .sum();
    let hop2: u64 = nodes
        .iter()
        .map(|n| n.count(|m| &m.txset_tree_delay_hop2_count))
        .sum();
    assert_eq!((hop1, hop2), (2, 4));
    // A second push of the same set goes nowhere (each peer gets a set once).
    let (_, again) = test_set(51);
    nodes[leader]
        .handle
        .push_txset_for_slot(hash, again, SLOT)
        .await;
    tokio::time::sleep(Duration::from_millis(300)).await;
    assert_eq!(nodes[leader].count(|m| &m.txset_tree_root_sent), 2);
}

/// A node that expects the set through the tree does not ask for it when the
/// proposal names it; it asks after the wait if the set has not come (here:
/// its parent does not relay).
#[tokio::test]
async fn a_node_waits_for_the_tree_and_asks_if_it_fails() {
    let mut nodes = full_mesh(24301, 7).await;
    configure(&nodes, TreeMode::Hash, 2);
    let leader = 0;
    let leader_id = nodes[leader].node_id();
    let leader_peer = nodes[leader].peer_id;
    let order = config(&nodes, TreeMode::Hash, 2)
        .hash_order(SLOT, &leader_id)
        .unwrap();
    // The first relay is switched off: it files the set and passes nothing on.
    let dead = index_of(&nodes, &order[1]);
    nodes[dead]
        .handle
        .set_txset_tree(config(&nodes, TreeMode::Off, 2));
    let starved: Vec<usize> = children_of(&order, &order[1], 2)
        .iter()
        .map(|id| index_of(&nodes, id))
        .collect();
    assert_eq!(starved.len(), 2);
    let fed: Vec<usize> = children_of(&order, &order[2], 2)
        .iter()
        .map(|id| index_of(&nodes, id))
        .collect();

    let (hash, data) = test_set(52);
    // The proposal reaches every follower first (it names the hash and comes
    // from the leader), then the leader pushes.
    let asked_at = Instant::now();
    for i in starved.iter().chain(fed.iter()) {
        nodes[*i]
            .handle
            .record_txset_source(hash, leader_peer)
            .await;
        nodes[*i].handle.fetch_txset(hash, SLOT).await;
        // Core asks too; that changes nothing while the wait runs.
        nodes[*i].handle.fetch_txset(hash, SLOT).await;
    }
    nodes[leader]
        .handle
        .push_txset_for_slot(hash, Arc::clone(&data), SLOT)
        .await;

    // The children of the working relay get the set through the tree and
    // never ask.
    for i in &fed {
        let (slot, _) = expect_set(&mut nodes[*i], hash).await;
        assert_eq!(slot, None);
    }
    // The children of the dead relay ask the leader after the wait, and get
    // the set as an answer.
    let mut askers = vec![
        serve_request(&mut nodes[leader], hash, &data).await,
        serve_request(&mut nodes[leader], hash, &data).await,
    ];
    assert!(asked_at.elapsed() >= TREE_FETCH_GRACE);
    askers.sort();
    let mut expected: Vec<PeerId> = starved.iter().map(|i| nodes[*i].peer_id).collect();
    expected.sort();
    assert_eq!(askers, expected);
    for i in &starved {
        let (slot, from) = expect_set(&mut nodes[*i], hash).await;
        assert_eq!(slot, Some(SLOT), "node {i} had to ask");
        assert_eq!(from, leader_peer);
    }
    assert!(asked_at.elapsed() >= TREE_FETCH_GRACE);
    tokio::time::sleep(Duration::from_millis(200)).await;
    for i in &fed {
        assert_eq!(nodes[*i].count(|m| &m.txset_tree_fetch_deferred), 1);
        assert_eq!(nodes[*i].count(|m| &m.txset_tree_fetch_fallback), 0);
        assert_eq!(nodes[*i].count(|m| &m.fetch_txset_count), 0);
    }
    for i in &starved {
        assert_eq!(nodes[*i].count(|m| &m.txset_tree_fetch_deferred), 1);
        assert_eq!(nodes[*i].count(|m| &m.txset_tree_fetch_fallback), 1);
        assert_eq!(nodes[*i].count(|m| &m.fetch_txset_count), 1);
    }
    // Nobody else asked the leader.
    let (_, requested) = set_events_within(&mut nodes[leader], Duration::from_millis(200)).await;
    assert!(requested.is_empty());
    // The switched-off relay got its copy and relayed nothing.
    assert_eq!(nodes[dead].count(|m| &m.txset_tree_received), 1);
    assert_eq!(nodes[dead].count(|m| &m.txset_tree_relayed), 0);
}

/// A node passes on only what its parent in the tree sent it: the same frame
/// from another validator is filed and not relayed.
#[tokio::test]
async fn a_frame_from_a_node_that_is_not_the_parent_is_not_relayed() {
    let mut nodes = full_mesh(24401, 7).await;
    configure(&nodes, TreeMode::Hash, 2);
    let leader = 0;
    let leader_id = nodes[leader].node_id();
    let order = config(&nodes, TreeMode::Hash, 2)
        .hash_order(SLOT, &leader_id)
        .unwrap();
    let relay = index_of(&nodes, &order[1]);
    let child = index_of(&nodes, &children_of(&order, &order[1], 2)[0]);
    // A leaf of the other relay sends the relay a frame that claims to be
    // the leader's tree frame for the slot.
    let impostor = index_of(&nodes, &children_of(&order, &order[2], 2)[0]);
    let (hash, data) = test_set(53);
    let header = TreeHeader {
        kind: FrameKind::Hash,
        fanout: 2,
        hop: 1,
        slot: SLOT,
        leader: leader_id,
        sent_unix_ms: unix_ms_now(),
        order: Vec::new(),
        nodes_digest: [0; txset_tree::TREE_NODES_DIGEST],
    }
    .encode();
    send_to_peer_stream_parts(
        &nodes[impostor].state,
        nodes[relay].peer_id,
        StreamType::TxSet,
        &[header.as_slice(), data.encoded()],
    )
    .await
    .unwrap();
    let (slot, from) = expect_set(&mut nodes[relay], hash).await;
    assert_eq!((slot, from), (None, nodes[impostor].peer_id));
    let (received, _) = set_events_within(&mut nodes[child], Duration::from_millis(400)).await;
    assert!(
        received.is_empty(),
        "the relay passed the impostor's frame on"
    );
    assert_eq!(nodes[relay].count(|m| &m.txset_tree_refused), 1);
    assert_eq!(nodes[relay].count(|m| &m.txset_tree_relayed), 0);

    // The same holds for a slot the relay's Core has not described, also
    // from the true parent.
    let (hash2, data2) = test_set(54);
    let header = TreeHeader {
        kind: FrameKind::Hash,
        fanout: 2,
        hop: 1,
        slot: SLOT + 5,
        leader: leader_id,
        sent_unix_ms: unix_ms_now(),
        order: Vec::new(),
        nodes_digest: [0; txset_tree::TREE_NODES_DIGEST],
    }
    .encode();
    send_to_peer_stream_parts(
        &nodes[leader].state,
        nodes[relay].peer_id,
        StreamType::TxSet,
        &[header.as_slice(), data2.encoded()],
    )
    .await
    .unwrap();
    expect_set(&mut nodes[relay], hash2).await;
    let (received, _) = set_events_within(&mut nodes[child], Duration::from_millis(400)).await;
    assert!(received.is_empty());
    assert_eq!(nodes[relay].count(|m| &m.txset_tree_refused), 2);

    // A frame with a broken header is dropped whole.
    let mut broken = header.clone();
    broken[4] = 77;
    send_to_peer_stream_parts(
        &nodes[leader].state,
        nodes[relay].peer_id,
        StreamType::TxSet,
        &[broken.as_slice(), data2.encoded()],
    )
    .await
    .unwrap();
    let (received, _) = set_events_within(&mut nodes[relay], Duration::from_millis(400)).await;
    assert!(received.is_empty());
}

/// Star mode: the leader writes to every peer, with a header that says when;
/// nobody relays and nobody waits before asking. A slot whose tree the leader
/// does not know is pushed the same way in hash mode.
#[tokio::test]
async fn star_mode_and_an_unknown_slot_push_to_every_peer() {
    let mut nodes = full_mesh(24501, 5).await;
    configure(&nodes, TreeMode::Star, 0);
    let (hash, data) = test_set(55);
    nodes[1].handle.push_txset_for_slot(hash, data, SLOT).await;
    let leader_peer = nodes[1].peer_id;
    for i in [0usize, 2, 3, 4] {
        let (slot, from) = expect_set(&mut nodes[i], hash).await;
        assert_eq!((slot, from), (None, leader_peer));
        assert_eq!(nodes[i].count(|m| &m.txset_tree_received), 1);
        assert_eq!(nodes[i].count(|m| &m.txset_tree_delay_hop1_count), 1);
        assert_eq!(nodes[i].count(|m| &m.txset_tree_relayed), 0);
        assert_eq!(nodes[i].count(|m| &m.txset_tree_refused), 0);
    }
    assert_eq!(nodes[1].count(|m| &m.txset_tree_root_sent), 4);
    assert!(nodes[1].count(|m| &m.txset_push_sent_bytes) > 0);
    // No wait in star mode: a request goes out at once.
    let (other, _) = test_set(56);
    nodes[0].handle.fetch_txset(other, SLOT).await;
    assert_eq!(nodes[0].count(|m| &m.txset_tree_fetch_deferred), 0);

    // Hash mode, a slot without a tree: a star as well.
    configure(&nodes, TreeMode::Hash, 2);
    let (hash, data) = test_set(57);
    nodes[2]
        .handle
        .push_txset_for_slot(hash, data, SLOT + 9)
        .await;
    for i in [0usize, 1, 3, 4] {
        let (_, from) = expect_set(&mut nodes[i], hash).await;
        assert_eq!(from, nodes[2].peer_id);
    }
    assert_eq!(nodes[2].count(|m| &m.txset_tree_root_sent), 4);
    // Without a slot (callers that do not know it) the frame is the old one.
    let (hash, data) = test_set(58);
    nodes[3].handle.push_txset(hash, data).await;
    for i in [0usize, 1, 2, 4] {
        expect_set(&mut nodes[i], hash).await;
    }
    assert_eq!(nodes[3].count(|m| &m.txset_tree_root_sent), 0);
    assert_eq!(nodes[3].count(|m| &m.txset_push_sent), 4);
}

/// Rtt mode: the leader lays the tree out by its round-trip times and sends
/// the order along; the other nodes follow it.
#[tokio::test]
async fn an_order_chosen_by_the_leader_is_followed() {
    let mut nodes = full_mesh(24601, 7).await;
    configure(&nodes, TreeMode::Rtt, 2);
    let leader = 6;
    // Two places: nodes 0..2 near (1 ms), nodes 3..5 far (80 ms).
    {
        let mut tree = nodes[leader].state.txset_tree.lock().unwrap();
        // The times are set by hand; real probes on loopback would be
        // shorter and take their place.
        tree.pause_probing();
        for i in 0..6 {
            tree.forget_rtt(&nodes[i].node_id());
        }
        for i in 0..6 {
            let rtt = if i < 3 { 1 } else { 80 };
            tree.note_rtt(
                nodes[i].node_id(),
                Duration::from_millis(rtt + i as u64 % 2),
            );
        }
    }
    let (hash, data) = test_set(59);
    nodes[leader]
        .handle
        .push_txset_for_slot(hash, data, SLOT)
        .await;
    let mut from_leader = Vec::new();
    let mut parent_of_node = HashMap::new();
    for i in 0..6 {
        let (slot, from) = expect_set(&mut nodes[i], hash).await;
        assert_eq!(slot, None);
        if from == nodes[leader].peer_id {
            from_leader.push(i);
        }
        parent_of_node.insert(i, from);
    }
    tokio::time::sleep(Duration::from_millis(300)).await;
    // One relay per place, and each relay's children are in its own place.
    assert_eq!(from_leader.len(), 2);
    assert!(from_leader.iter().any(|i| *i < 3));
    assert!(from_leader.iter().any(|i| *i >= 3));
    for i in 0..6 {
        if from_leader.contains(&i) {
            continue;
        }
        let parent = parent_of_node[&i];
        let parent_index = nodes.iter().position(|n| n.peer_id == parent).unwrap();
        assert_eq!(
            parent_index < 3,
            i < 3,
            "node {i} sits under node {parent_index}"
        );
    }
    assert_eq!(nodes[leader].count(|m| &m.txset_tree_root_sent), 2);
    let relayed: u64 = nodes
        .iter()
        .map(|n| n.count(|m| &m.txset_tree_relayed))
        .sum();
    assert_eq!(relayed, 4);
    for node in &nodes {
        assert_eq!(node.count(|m| &m.txset_tree_refused), 0);
    }
}

/// In rtt mode a node measures the round trip to its identified peers.
#[tokio::test]
async fn round_trips_are_measured_in_rtt_mode_only() {
    let nodes = full_mesh(24801, 3).await;
    tokio::time::sleep(Duration::from_millis(700)).await;
    for node in &nodes {
        let tree = node.state.txset_tree.lock().unwrap();
        for other in &nodes {
            assert_eq!(tree.rtt_of(&other.node_id()), None);
        }
    }
    configure(&nodes, TreeMode::Rtt, 0);
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let mut measured = 0;
        for node in &nodes {
            let tree = node.state.txset_tree.lock().unwrap();
            for other in &nodes {
                if let Some(rtt) = tree.rtt_of(&other.node_id()) {
                    assert!(rtt < Duration::from_secs(1));
                    measured += 1;
                }
            }
        }
        if measured == 6 {
            break;
        }
        assert!(
            Instant::now() < deadline,
            "only {measured} of 6 round trips measured"
        );
        tokio::time::sleep(Duration::from_millis(100)).await;
    }
    // Probes are not sets: nothing was received, dropped or refused.
    for node in &nodes {
        assert_eq!(node.count(|m| &m.txset_tree_received), 0);
        assert_eq!(node.count(|m| &m.txset_push_dropped), 0);
        assert_eq!(node.count(|m| &m.txset_push_received), 0);
    }
}

/// The cached frame of a set with a tree header in front, sent by `from` to
/// `to` as if `from` had pushed it.
async fn send_tagged(from: &Node, to: &Node, header: &TreeHeader, data: &Arc<TxSetData>) {
    let header = header.encode();
    send_to_peer_stream_parts(
        &from.state,
        to.peer_id,
        StreamType::TxSet,
        &[header.as_slice(), data.encoded()],
    )
    .await
    .unwrap();
}

/// Only a node that this node's Core expects to lead the slot can root a
/// tree: a frame that names another validator as leader is filed and not
/// passed on, also when it comes from that validator and the receiver is
/// its child in the tree it claims.
#[tokio::test]
async fn a_validator_that_does_not_lead_the_slot_cannot_root_a_tree() {
    let mut nodes = full_mesh(24901, 7).await;
    let true_leader = nodes[0].node_id();
    for node in &nodes {
        node.handle.set_txset_tree(
            config(&nodes, TreeMode::Hash, 2).with_leaders(vec![Some(vec![true_leader])]),
        );
    }
    // Node 5 roots a tree at itself and pushes down it.
    let pretender = 5;
    let pretender_id = nodes[pretender].node_id();
    let order = config(&nodes, TreeMode::Hash, 2)
        .hash_order(SLOT, &pretender_id)
        .unwrap();
    let relay = index_of(&nodes, &order[1]);
    let child = index_of(&nodes, &children_of(&order, &order[1], 2)[0]);
    let (hash, data) = test_set(61);
    let header = TreeHeader {
        kind: FrameKind::Hash,
        fanout: 2,
        hop: 1,
        slot: SLOT,
        leader: pretender_id,
        sent_unix_ms: unix_ms_now(),
        order: Vec::new(),
        nodes_digest: [0; txset_tree::TREE_NODES_DIGEST],
    };
    send_tagged(&nodes[pretender], &nodes[relay], &header, &data).await;
    expect_set(&mut nodes[relay], hash).await;
    let (received, _) = set_events_within(&mut nodes[child], Duration::from_millis(400)).await;
    assert!(
        received.is_empty(),
        "a tree rooted at a non-leader was relayed"
    );
    assert_eq!(nodes[relay].count(|m| &m.txset_tree_refused), 1);
    assert_eq!(nodes[relay].count(|m| &m.txset_tree_relayed), 0);
    // The real leader's push still travels down its tree.
    let (hash, data) = test_set(62);
    nodes[0].handle.push_txset_for_slot(hash, data, SLOT).await;
    for i in 1..7 {
        expect_set(&mut nodes[i], hash).await;
    }
    let relayed: u64 = nodes
        .iter()
        .map(|n| n.count(|m| &m.txset_tree_relayed))
        .sum();
    assert_eq!(relayed, 4);
}

/// A node in hash mode does not follow an order a sender chose, and a node
/// in rtt mode does not follow one that makes a chain.
#[tokio::test]
async fn an_order_from_the_sender_is_followed_in_rtt_mode_and_two_levels_only() {
    let mut nodes = full_mesh(25001, 7).await;
    configure(&nodes, TreeMode::Hash, 2);
    let cfg = config(&nodes, TreeMode::Hash, 2);
    let sender = 0;
    // The sender first, then the others as they come: node 1 would relay to
    // nodes 3 and 4.
    let order: Vec<NodeId> = node_ids(&nodes);
    let mut header = TreeHeader {
        kind: FrameKind::Explicit,
        fanout: 2,
        hop: 1,
        slot: SLOT,
        leader: nodes[sender].node_id(),
        sent_unix_ms: unix_ms_now(),
        order: cfg.indices_of(&order).unwrap(),
        nodes_digest: cfg.nodes_digest(),
    };
    let (hash, data) = test_set(63);
    send_tagged(&nodes[sender], &nodes[1], &header, &data).await;
    expect_set(&mut nodes[1], hash).await;
    let (received, _) = set_events_within(&mut nodes[3], Duration::from_millis(400)).await;
    assert!(
        received.is_empty(),
        "a node in hash mode followed a sender's order"
    );
    assert_eq!(nodes[1].count(|m| &m.txset_tree_refused), 1);

    // In rtt mode the same order is followed ...
    configure(&nodes, TreeMode::Rtt, 2);
    let (hash, data) = test_set(64);
    send_tagged(&nodes[sender], &nodes[1], &header, &data).await;
    expect_set(&mut nodes[1], hash).await;
    expect_set(&mut nodes[3], hash).await;
    expect_set(&mut nodes[4], hash).await;
    assert_eq!(nodes[1].count(|m| &m.txset_tree_relayed), 2);
    // ... but not as a chain (fanout 1: six levels).
    header.fanout = 1;
    let (hash, data) = test_set(65);
    send_tagged(&nodes[sender], &nodes[1], &header, &data).await;
    expect_set(&mut nodes[1], hash).await;
    let (received, _) = set_events_within(&mut nodes[2], Duration::from_millis(400)).await;
    assert!(received.is_empty(), "a chain was followed");
    assert_eq!(nodes[1].count(|m| &m.txset_tree_refused), 2);
    assert_eq!(nodes[1].count(|m| &m.txset_tree_relayed), 2);
}

/// A relay passes on two different frames of a slot and leader, not a third:
/// a parent cannot use it to multiply traffic by varying the bytes.
#[tokio::test]
async fn a_relay_passes_on_two_frames_per_slot_and_leader() {
    let mut nodes = full_mesh(25101, 7).await;
    configure(&nodes, TreeMode::Hash, 2);
    let leader = 0;
    let leader_id = nodes[leader].node_id();
    let order = config(&nodes, TreeMode::Hash, 2)
        .hash_order(SLOT, &leader_id)
        .unwrap();
    let relay = index_of(&nodes, &order[1]);
    let child = index_of(&nodes, &children_of(&order, &order[1], 2)[0]);
    let header = TreeHeader {
        kind: FrameKind::Hash,
        fanout: 2,
        hop: 1,
        slot: SLOT,
        leader: leader_id,
        sent_unix_ms: unix_ms_now(),
        order: Vec::new(),
        nodes_digest: [0; txset_tree::TREE_NODES_DIGEST],
    };
    let both: Vec<usize> = children_of(&order, &order[1], 2)
        .iter()
        .map(|id| index_of(&nodes, id))
        .collect();
    // Two different frames, one after the other (a relay has one frame in
    // flight per child): both are passed on to both children.
    for seed in [66u8, 67] {
        let (hash, data) = test_set(seed);
        send_tagged(&nodes[leader], &nodes[relay], &header, &data).await;
        expect_set(&mut nodes[relay], hash).await;
        for i in &both {
            let (_, from) = expect_set(&mut nodes[*i], hash).await;
            assert_eq!(from, nodes[relay].peer_id);
        }
    }
    assert_eq!(nodes[relay].count(|m| &m.txset_tree_relayed), 4);
    // A third one is filed by the relay and goes no further.
    let (hash, data) = test_set(68);
    send_tagged(&nodes[leader], &nodes[relay], &header, &data).await;
    expect_set(&mut nodes[relay], hash).await;
    let (received, _) = set_events_within(&mut nodes[child], Duration::from_millis(500)).await;
    assert!(
        received.is_empty(),
        "a third frame of the slot and leader was relayed"
    );
    assert_eq!(nodes[relay].count(|m| &m.txset_tree_relayed), 4);
    assert_eq!(nodes[relay].count(|m| &m.txset_tree_relay_limited), 1);
    assert_eq!(nodes[relay].count(|m| &m.txset_tree_relay_busy), 0);
    // A second copy of a frame it relayed is a duplicate, not a third frame.
    let (_, again) = test_set(66);
    send_tagged(&nodes[leader], &nodes[relay], &header, &again).await;
    tokio::time::sleep(Duration::from_millis(300)).await;
    assert_eq!(nodes[relay].count(|m| &m.txset_tree_duplicate), 1);
    assert_eq!(nodes[relay].count(|m| &m.txset_tree_relayed), 4);
}

/// If the leader cannot send a child the set now (an earlier push to it is
/// still in flight), the nodes below that child get it from the leader
/// directly.
#[tokio::test]
async fn the_nodes_below_a_busy_child_get_the_set_from_the_leader() {
    let mut nodes = full_mesh(25201, 7).await;
    configure(&nodes, TreeMode::Hash, 2);
    let leader = 2;
    let leader_id = nodes[leader].node_id();
    let leader_peer = nodes[leader].peer_id;
    let order = config(&nodes, TreeMode::Hash, 2)
        .hash_order(SLOT, &leader_id)
        .unwrap();
    let busy_child = index_of(&nodes, &order[1]);
    let below: Vec<usize> = children_of(&order, &order[1], 2)
        .iter()
        .map(|id| index_of(&nodes, id))
        .collect();
    // An earlier push to that child is still in flight.
    let (earlier, _) = test_set(69);
    let started = nodes[leader]
        .state
        .txset_pushes
        .lock()
        .unwrap()
        .start_push(earlier, vec![nodes[busy_child].peer_id]);
    assert_eq!(started.len(), 1);

    let (hash, data) = test_set(70);
    nodes[leader]
        .handle
        .push_txset_for_slot(hash, data, SLOT)
        .await;
    for i in &below {
        let (slot, from) = expect_set(&mut nodes[*i], hash).await;
        assert_eq!((slot, from), (None, leader_peer), "node {i}");
    }
    // The other child and its subtree are served through the tree.
    let other = index_of(&nodes, &order[2]);
    expect_set(&mut nodes[other], hash).await;
    for id in children_of(&order, &order[2], 2) {
        let i = index_of(&nodes, &id);
        let (_, from) = expect_set(&mut nodes[i], hash).await;
        assert_eq!(from, nodes[other].peer_id);
    }
    // The busy child itself got nothing (it asks when it sees the proposal).
    let (received, _) = set_events_within(&mut nodes[busy_child], Duration::from_millis(300)).await;
    assert!(received.is_empty());
    assert_eq!(nodes[leader].count(|m| &m.txset_tree_child_busy), 1);
    // One child and the two nodes below the busy one.
    assert_eq!(nodes[leader].count(|m| &m.txset_tree_root_sent), 3);
}

/// An answer to a probe this node did not send is not a measurement.
#[tokio::test]
async fn an_unsolicited_probe_answer_is_ignored() {
    let nodes = full_mesh(25301, 2).await;
    configure(&nodes, TreeMode::Rtt, 0);
    nodes[0].state.txset_tree.lock().unwrap().pause_probing();
    nodes[1].state.txset_tree.lock().unwrap().pause_probing();
    tokio::time::sleep(Duration::from_millis(300)).await;
    {
        let mut tree = nodes[0].state.txset_tree.lock().unwrap();
        tree.forget_rtt(&nodes[1].node_id());
    }
    // Node 1 claims a round trip of nothing: an answer "sent" in the future.
    for value in [u64::MAX, 0, monotonic_micros()] {
        let pong = Probe::Pong(value).encode();
        send_to_peer_stream(&nodes[1].state, nodes[0].peer_id, StreamType::TxSet, &pong)
            .await
            .unwrap();
    }
    tokio::time::sleep(Duration::from_millis(300)).await;
    assert_eq!(
        nodes[0]
            .state
            .txset_tree
            .lock()
            .unwrap()
            .rtt_of(&nodes[1].node_id()),
        None
    );
}

/// A node whose mode is off still reads tagged frames (and files the set);
/// it relays nothing and asks at once.
#[tokio::test]
async fn a_node_without_the_tree_reads_tagged_frames() {
    let mut nodes = full_mesh(24701, 3).await;
    nodes[0]
        .handle
        .set_txset_tree(config(&nodes, TreeMode::Hash, 1));
    // Nodes 1 and 2 were never told about trees.
    let order = config(&nodes, TreeMode::Hash, 1)
        .hash_order(SLOT, &nodes[0].node_id())
        .unwrap();
    let first = index_of(&nodes, &order[1]);
    let second = index_of(&nodes, &order[2]);
    let (hash, data) = test_set(60);
    nodes[0]
        .handle
        .push_txset_for_slot(hash, Arc::clone(&data), SLOT)
        .await;
    let (slot, _) = expect_set(&mut nodes[first], hash).await;
    assert_eq!(slot, None);
    assert_eq!(nodes[first].count(|m| &m.txset_tree_received), 1);
    assert_eq!(nodes[first].count(|m| &m.txset_tree_relayed), 0);
    // The second node is not reached by the tree and asks without waiting.
    let (received, _) = set_events_within(&mut nodes[second], Duration::from_millis(300)).await;
    assert!(received.is_empty());
    let leader_peer = nodes[0].peer_id;
    nodes[second]
        .handle
        .record_txset_source(hash, leader_peer)
        .await;
    let asked_at = Instant::now();
    nodes[second].handle.fetch_txset(hash, SLOT).await;
    assert_eq!(
        serve_request(&mut nodes[0], hash, &data).await,
        nodes[second].peer_id
    );
    let (slot, from) = expect_set(&mut nodes[second], hash).await;
    assert_eq!((slot, from), (Some(SLOT), leader_peer));
    assert!(asked_at.elapsed() < TREE_FETCH_GRACE);
    assert_eq!(nodes[second].count(|m| &m.txset_tree_fetch_deferred), 0);
}
