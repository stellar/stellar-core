//! Tests for the erasure-coded pieces of the leader's tx set (mode `coded`):
//! nine overlay nodes on loopback, fully connected, each with a validator
//! identity. With eight other nodes a set is cut into eight pieces, four of
//! which rebuild it; every other node relays one piece to the seven others.

use super::txset_tree_tests::{
    config, expect_set, full_mesh, index_of, serve_request, set_events_within, test_set, Node,
    NETWORK_ID, SEED, SLOT,
};
use super::*;
use crate::flood::txset_tree::TREE_FETCH_GRACE;

const NODES: usize = 9;
const PIECES: u64 = 8;
const DATA: usize = 4;

/// A full mesh whose tx-set streams are known to work in both directions.
///
/// The stream library hands inbound streams to the overlay through a channel
/// without a buffer and drops a stream that arrives while another is being
/// handed over; the sender notices at its first write and opens the stream
/// again (three attempts). When the mesh forms, and when every node writes to
/// every other at once for the first time, as here, three attempts are not
/// always enough. So every pair exchanges a ping first, one after the other.
async fn coded_mesh(first_port: u16) -> Vec<Node> {
    let nodes = full_mesh(first_port, NODES).await;
    for (a, from) in nodes.iter().enumerate() {
        for (b, to) in nodes.iter().enumerate() {
            if a != b {
                let ping = Probe::Ping(1).encode();
                send_to_peer_stream(&from.state, to.peer_id, StreamType::TxSet, &ping)
                    .await
                    .unwrap();
            }
        }
        // The answers come back on streams that may have to be opened too.
        tokio::time::sleep(Duration::from_millis(60)).await;
    }
    tokio::time::sleep(Duration::from_millis(300)).await;
    for node in &nodes {
        node.metrics.error_write.store(0, Ordering::Relaxed);
    }
    nodes
}

fn coded_config(nodes: &[Node]) -> TreeConfig {
    config(nodes, TreeMode::Coded, 0)
}

fn layout_of(nodes: &[Node], leader: usize) -> CodedLayout {
    let order = coded_config(nodes)
        .hash_order(SLOT, &nodes[leader].node_id())
        .unwrap();
    let layout = CodedLayout::new(&order).unwrap();
    assert_eq!(
        (
            layout.params.data,
            layout.params.pieces as u64,
            layout.params.relays
        ),
        (DATA, PIECES, 1)
    );
    layout
}

fn identity_of(node: &Node) -> NodeIdentity {
    node.state.identity.lock().unwrap().ours.clone().unwrap()
}

fn sum(nodes: &[Node], pick: impl Fn(&OverlayMetrics) -> &AtomicU64 + Copy) -> u64 {
    nodes.iter().map(|n| n.count(pick)).sum()
}

/// The pieces of `data` as `leader` would send them for `SLOT`, after `spoil`
/// had its way with them (before the root is computed and signed).
fn pieces_of(
    leader: &Node,
    data: &TxSetData,
    params: CodedParams,
    spoil: impl FnOnce(&mut Vec<Vec<u8>>),
) -> SignedPieces {
    let mut pieces = txset_coded::encode_pieces(data.encoded(), params).unwrap();
    spoil(&mut pieces);
    sign_pieces(
        &leader.state.keypair,
        &identity_of(leader),
        SLOT,
        &SEED,
        params,
        data.encoded().len() as u32,
        pieces,
        unix_ms_now(),
    )
    .unwrap()
}

/// Write piece `index` of `signed` to `to` from the node the layout says
/// owes it (with the hop count that goes with that sender).
async fn deliver_piece(
    nodes: &[Node],
    layout: &CodedLayout,
    signed: &SignedPieces,
    index: usize,
    to: usize,
) {
    let sender = layout.sender_of(&nodes[to].node_id(), index).unwrap();
    let from = index_of(nodes, &sender);
    let header = PieceHeader {
        hop: if sender == layout.leader { 1 } else { 2 },
        ..signed.headers[index].clone()
    }
    .encode();
    send_to_peer_stream_parts(
        &nodes[from].state,
        nodes[to].peer_id,
        StreamType::TxSet,
        &[header.as_slice(), signed.pieces[index].as_slice()],
    )
    .await
    .unwrap();
}

/// The leader writes one piece to each of the eight other nodes, each passes
/// its piece on to the seven others, and every node rebuilds the set from the
/// first four pieces it holds, without asking.
#[tokio::test]
async fn a_coded_set_reaches_every_node_as_pieces() {
    let mut nodes = coded_mesh(27101).await;
    for node in &nodes {
        node.handle.set_txset_tree(coded_config(&nodes));
    }
    let leader = 2;
    let leader_peer = nodes[leader].peer_id;
    let (hash, data) = test_set(71);
    // The proposal names the hash before the pieces come: nobody asks.
    for i in (0..NODES).filter(|i| *i != leader) {
        nodes[i].handle.record_txset_source(hash, leader_peer).await;
        nodes[i].handle.fetch_txset(hash, SLOT).await;
    }
    nodes[leader]
        .handle
        .push_txset_for_slot(hash, Arc::clone(&data), SLOT)
        .await;
    for i in (0..NODES).filter(|i| *i != leader) {
        let (slot, _) = expect_set(&mut nodes[i], hash).await;
        assert_eq!(slot, None, "node {i} had not asked");
    }
    // Let the pieces nobody needed any more arrive, and the waits end.
    tokio::time::sleep(TREE_FETCH_GRACE + Duration::from_millis(300)).await;

    // The leader: eight pieces, no whole copy, nothing relayed.
    assert_eq!(nodes[leader].count(|m| &m.txset_coded_sets_encoded), 1);
    assert_eq!(nodes[leader].count(|m| &m.txset_coded_pieces_sent), PIECES);
    assert_eq!(nodes[leader].count(|m| &m.txset_push_sent), 0);
    assert_eq!(nodes[leader].count(|m| &m.txset_tree_root_sent), 0);
    assert_eq!(nodes[leader].count(|m| &m.txset_coded_pieces_relayed), 0);
    assert_eq!(nodes[leader].count(|m| &m.txset_coded_pieces_received), 0);
    let piece = txset_coded::piece_len(data.encoded().len(), DATA) as u64;
    let header = (txset_coded::PIECE_HEADER_FIXED + 32 * txset_coded::merkle_depth(8)) as u64;
    assert_eq!(
        nodes[leader].count(|m| &m.txset_push_sent_bytes),
        PIECES * (piece + header)
    );
    for i in (0..NODES).filter(|i| *i != leader) {
        let node = &nodes[i];
        // Its own piece to the seven others; all eight pieces received, four
        // of them after the set was rebuilt.
        assert_eq!(
            node.count(|m| &m.txset_coded_pieces_relayed),
            PIECES - 1,
            "node {i}"
        );
        assert_eq!(
            node.count(|m| &m.txset_tree_relayed_bytes),
            (PIECES - 1) * (piece + header)
        );
        assert_eq!(
            node.count(|m| &m.txset_coded_pieces_received),
            PIECES,
            "node {i}"
        );
        assert_eq!(
            node.count(|m| &m.txset_coded_pieces_late),
            PIECES - DATA as u64
        );
        assert_eq!(node.count(|m| &m.txset_coded_sets_rebuilt), 1);
        assert_eq!(node.count(|m| &m.txset_tree_received), 1);
        assert_eq!(node.count(|m| &m.txset_tree_delay_count), 1);
        // Nothing wrong, nothing asked for.
        assert_eq!(node.count(|m| &m.txset_coded_pieces_refused), 0, "node {i}");
        assert_eq!(node.count(|m| &m.txset_coded_pieces_invalid), 0);
        assert_eq!(node.count(|m| &m.txset_coded_pieces_duplicate), 0);
        assert_eq!(node.count(|m| &m.txset_coded_pieces_skipped), 0);
        assert_eq!(node.count(|m| &m.error_write), 0, "node {i}");
        assert_eq!(node.count(|m| &m.txset_coded_sets_failed), 0);
        assert_eq!(node.count(|m| &m.txset_coded_sets_inconsistent), 0);
        assert_eq!(node.count(|m| &m.txset_tree_fetch_deferred), 1);
        assert_eq!(node.count(|m| &m.txset_tree_fetch_fallback), 0);
        assert_eq!(node.count(|m| &m.fetch_txset_count), 0);
    }
    // Nobody asked the leader, and no pieces are left in memory.
    let (_, requested) = set_events_within(&mut nodes[leader], Duration::from_millis(100)).await;
    assert!(requested.is_empty());
    // A second push of the same set sends nothing.
    nodes[leader]
        .handle
        .push_txset_for_slot(hash, data, SLOT)
        .await;
    tokio::time::sleep(Duration::from_millis(300)).await;
    assert_eq!(nodes[leader].count(|m| &m.txset_coded_pieces_sent), PIECES);
    assert_eq!(nodes[leader].count(|m| &m.txset_push_sent), 0);
}

/// Half of the relays pass nothing on: every node still has four pieces (its
/// own from the leader and those of the relays that work), rebuilds the set
/// with parity where it must, and nobody waits or asks. In a tree every child
/// of such a relay would wait and ask.
#[tokio::test]
async fn relays_that_withhold_cost_no_wait() {
    let mut nodes = coded_mesh(27201).await;
    let leader = 5;
    let layout = layout_of(&nodes, leader);
    // The relays of pieces 0, 2, 5 and 7 withhold: data and parity pieces.
    let withholders: Vec<usize> = [0usize, 2, 5, 7]
        .iter()
        .map(|piece| index_of(&nodes, &layout.relays_of(*piece)[0]))
        .collect();
    for (i, node) in nodes.iter().enumerate() {
        node.handle
            .set_txset_tree(coded_config(&nodes).with_withhold(withholders.contains(&i)));
    }
    let leader_peer = nodes[leader].peer_id;
    let (hash, data) = test_set(72);
    for i in (0..NODES).filter(|i| *i != leader) {
        nodes[i].handle.record_txset_source(hash, leader_peer).await;
        nodes[i].handle.fetch_txset(hash, SLOT).await;
    }
    nodes[leader]
        .handle
        .push_txset_for_slot(hash, data, SLOT)
        .await;
    for i in (0..NODES).filter(|i| *i != leader) {
        let (slot, _) = expect_set(&mut nodes[i], hash).await;
        assert_eq!(slot, None, "node {i} had not asked");
    }
    tokio::time::sleep(TREE_FETCH_GRACE + Duration::from_millis(300)).await;
    for i in (0..NODES).filter(|i| *i != leader) {
        let node = &nodes[i];
        let withholds = withholders.contains(&i);
        assert_eq!(
            node.count(|m| &m.txset_tree_withheld),
            u64::from(withholds),
            "node {i}"
        );
        assert_eq!(
            node.count(|m| &m.txset_coded_pieces_relayed),
            if withholds { 0 } else { PIECES - 1 }
        );
        // Its own piece and those of the four relays that work (three, for
        // a node that is one of them).
        assert_eq!(
            node.count(|m| &m.txset_coded_pieces_received),
            if withholds { 5 } else { 4 },
            "node {i}"
        );
        assert_eq!(node.count(|m| &m.txset_coded_sets_rebuilt), 1);
        assert_eq!(node.count(|m| &m.txset_coded_sets_failed), 0);
        assert_eq!(node.count(|m| &m.txset_tree_fetch_deferred), 1);
        assert_eq!(node.count(|m| &m.txset_tree_fetch_fallback), 0, "node {i}");
        assert_eq!(node.count(|m| &m.fetch_txset_count), 0);
    }
    // Two of the four data pieces never left their relays: all but those
    // two relays needed parity.
    assert!(sum(&nodes, |m| &m.txset_coded_sets_rebuilt_with_parity) >= 6);
    let (_, requested) = set_events_within(&mut nodes[leader], Duration::from_millis(100)).await;
    assert!(requested.is_empty());
}

/// With five of the eight relays withholding, a node that relays honestly
/// holds three pieces, one too few: it asks after the wait, as in the tree,
/// and gets the set as an answer.
#[tokio::test]
async fn with_too_few_pieces_a_node_asks_after_the_wait() {
    let mut nodes = coded_mesh(27301).await;
    let leader = 0;
    let layout = layout_of(&nodes, leader);
    let withholders: Vec<usize> = [0usize, 1, 3, 4, 6]
        .iter()
        .map(|piece| index_of(&nodes, &layout.relays_of(*piece)[0]))
        .collect();
    let starved: Vec<usize> = (0..NODES)
        .filter(|i| *i != leader && !withholders.contains(i))
        .collect();
    assert_eq!(starved.len(), 3);
    for (i, node) in nodes.iter().enumerate() {
        node.handle
            .set_txset_tree(coded_config(&nodes).with_withhold(withholders.contains(&i)));
    }
    let leader_peer = nodes[leader].peer_id;
    let (hash, data) = test_set(73);
    let asked_at = Instant::now();
    for i in (0..NODES).filter(|i| *i != leader) {
        nodes[i].handle.record_txset_source(hash, leader_peer).await;
        nodes[i].handle.fetch_txset(hash, SLOT).await;
    }
    nodes[leader]
        .handle
        .push_txset_for_slot(hash, Arc::clone(&data), SLOT)
        .await;
    // The withholders hold their own piece and three more.
    for i in &withholders {
        let (slot, _) = expect_set(&mut nodes[*i], hash).await;
        assert_eq!(slot, None);
    }
    // The others ask the leader after the wait: pieces are not a push of the
    // set to them, so the leader answers.
    let mut askers = Vec::new();
    for _ in 0..starved.len() {
        askers.push(serve_request(&mut nodes[leader], hash, &data).await);
    }
    assert!(asked_at.elapsed() >= TREE_FETCH_GRACE);
    askers.sort();
    let mut expected: Vec<PeerId> = starved.iter().map(|i| nodes[*i].peer_id).collect();
    expected.sort();
    assert_eq!(askers, expected);
    for i in &starved {
        let (slot, from) = expect_set(&mut nodes[*i], hash).await;
        assert_eq!(slot, Some(SLOT), "node {i} had to ask");
        assert_eq!(from, leader_peer);
        assert_eq!(nodes[*i].count(|m| &m.txset_coded_pieces_received), 3);
        assert_eq!(nodes[*i].count(|m| &m.txset_coded_sets_rebuilt), 0);
        assert_eq!(nodes[*i].count(|m| &m.txset_tree_fetch_fallback), 1);
    }
    assert_eq!(nodes[leader].count(|m| &m.txset_push_request_skipped), 0);
}

/// What a node does with pieces that are wrong: a changed piece, a piece
/// from a node that does not owe it, a piece signed by another validator in
/// the leader's name, and the pieces of a validator that may not lead the
/// slot. None is kept or passed on, and the leader's real set then arrives
/// everywhere.
#[tokio::test]
async fn wrong_pieces_are_dropped_and_not_passed_on() {
    let mut nodes = coded_mesh(27401).await;
    let leader = 4;
    let leader_id = nodes[leader].node_id();
    let layout = layout_of(&nodes, leader);
    // Only the leader may lead the slot.
    for node in &nodes {
        node.handle
            .set_txset_tree(coded_config(&nodes).with_leaders(vec![Some(vec![leader_id])]));
    }
    let (hash, data) = test_set(74);
    let good = pieces_of(&nodes[leader], &data, layout.params, |_| {});
    // The relay of piece 1, and an attacker: the relay of piece 6.
    let target = index_of(&nodes, &layout.relays_of(1)[0]);
    let attacker = index_of(&nodes, &layout.relays_of(6)[0]);
    let invalid = |nodes: &[Node]| nodes[target].count(|m| &m.txset_coded_pieces_invalid);
    let refused = |nodes: &[Node]| nodes[target].count(|m| &m.txset_coded_pieces_refused);
    let settle = || tokio::time::sleep(Duration::from_millis(200));

    // 1. The target's own piece from the leader, one bit changed: its path
    //    does not reach the signed root.
    let mut changed = good.pieces[1].as_ref().clone();
    changed[0] ^= 1;
    send_to_peer_stream_parts(
        &nodes[leader].state,
        nodes[target].peer_id,
        StreamType::TxSet,
        &[good.headers[1].encode().as_slice(), changed.as_slice()],
    )
    .await
    .unwrap();
    settle().await;
    assert_eq!((invalid(&nodes), refused(&nodes)), (1, 0));

    // 2. The right piece 1, but from the attacker: the leader owes the
    //    target that piece, nobody else.
    send_to_peer_stream_parts(
        &nodes[attacker].state,
        nodes[target].peer_id,
        StreamType::TxSet,
        &[
            good.headers[1].encode().as_slice(),
            good.pieces[1].as_slice(),
        ],
    )
    .await
    .unwrap();
    // And piece 6, which the attacker does owe the target, marked as if it
    // came straight from the leader.
    send_to_peer_stream_parts(
        &nodes[attacker].state,
        nodes[target].peer_id,
        StreamType::TxSet,
        &[
            good.headers[6].encode().as_slice(),
            good.pieces[6].as_slice(),
        ],
    )
    .await
    .unwrap();
    settle().await;
    assert_eq!((invalid(&nodes), refused(&nodes)), (1, 2));

    // 3. The attacker cuts a set of its own and signs it with its own keys,
    //    in the leader's name: its overlay is not the leader's.
    let (_, other) = test_set(75);
    let mut forged = pieces_of(&nodes[attacker], &other, layout.params, |_| {});
    for header in &mut forged.headers {
        header.leader = leader_id;
    }
    deliver_piece(&nodes, &layout, &forged, 6, target).await;
    settle().await;
    assert_eq!((invalid(&nodes), refused(&nodes)), (2, 2));
    // The leader's signature over another root does not help either.
    let mut grafted = pieces_of(&nodes[attacker], &other, layout.params, |_| {});
    for header in &mut grafted.headers {
        header.leader = leader_id;
        header.overlay_key = good.headers[0].overlay_key;
        header.binding_sig = good.headers[0].binding_sig;
        header.root_sig = good.headers[0].root_sig;
    }
    deliver_piece(&nodes, &layout, &grafted, 6, target).await;
    settle().await;
    assert_eq!((invalid(&nodes), refused(&nodes)), (3, 2));

    // 4. The attacker cuts a set of its own and signs it honestly, as the
    //    leader of a slot it may not lead, and sends the target the piece
    //    the target would relay in the attacker's layout: refused.
    let own_layout = layout_of(&nodes, attacker);
    let own = pieces_of(&nodes[attacker], &other, own_layout.params, |_| {});
    let owed = own_layout.relayed_piece(&nodes[target].node_id()).unwrap();
    deliver_piece(&nodes, &own_layout, &own, owed, target).await;
    settle().await;
    assert_eq!((invalid(&nodes), refused(&nodes)), (3, 3));

    // 5. A piece signed for another history of the same network (another
    //    seed for the slot): the signature is of no use here. The pieces
    //    and the root are those of the real set, so only the signature
    //    differs.
    let replayed = txset_coded::encode_pieces(data.encoded(), layout.params)
        .ok()
        .and_then(|pieces| {
            sign_pieces(
                &nodes[leader].state.keypair,
                &identity_of(&nodes[leader]),
                SLOT,
                &[0x77; 32],
                layout.params,
                data.encoded().len() as u32,
                pieces,
                unix_ms_now(),
            )
        })
        .unwrap();
    assert_eq!(replayed.headers[1].root, good.headers[1].root);
    deliver_piece(&nodes, &layout, &replayed, 1, target).await;
    settle().await;
    assert_eq!((invalid(&nodes), refused(&nodes)), (4, 3));

    // 6. A sender is listened to six times per slot and leader, whatever it
    //    sends: the attacker has used four; after two more, even the piece
    //    it owes the target is not looked at.
    for _ in 0..3 {
        send_to_peer_stream_parts(
            &nodes[attacker].state,
            nodes[target].peer_id,
            StreamType::TxSet,
            &[
                good.headers[1].encode().as_slice(),
                good.pieces[1].as_slice(),
            ],
        )
        .await
        .unwrap();
    }
    deliver_piece(&nodes, &layout, &good, 6, target).await;
    settle().await;
    assert_eq!((invalid(&nodes), refused(&nodes)), (4, 7));
    assert_eq!(sum(&nodes, |m| &m.txset_coded_pieces_refused), 7);

    // Nothing of all this was kept, passed on or rebuilt.
    assert_eq!(sum(&nodes, |m| &m.txset_coded_pieces_received), 0);
    assert_eq!(sum(&nodes, |m| &m.txset_coded_pieces_relayed), 0);
    assert_eq!(sum(&nodes, |m| &m.txset_coded_sets_rebuilt), 0);
    for node in &nodes {
        assert_eq!(node.state.txset_tree.lock().unwrap().coded.held_bytes(), 0);
    }

    // The leader's set still arrives everywhere, the target relaying its
    // piece as if nothing had happened.
    nodes[leader]
        .handle
        .push_txset_for_slot(hash, data, SLOT)
        .await;
    for i in (0..NODES).filter(|i| *i != leader) {
        expect_set(&mut nodes[i], hash).await;
    }
    settle().await;
    assert_eq!(
        nodes[target].count(|m| &m.txset_coded_pieces_relayed),
        PIECES - 1
    );
    // (The attacker's own piece for the target is one frame too many now:
    // the target makes do with the other seven.)
    assert_eq!(
        nodes[target].count(|m| &m.txset_coded_pieces_received),
        PIECES - 1
    );
    assert_eq!(
        sum(&nodes, |m| &m.txset_coded_sets_rebuilt),
        NODES as u64 - 1
    );
    assert_eq!(sum(&nodes, |m| &m.txset_coded_sets_failed), 0);
    assert_eq!(sum(&nodes, |m| &m.txset_coded_sets_inconsistent), 0);
}

/// A leader that signs pieces which do not fit together (here: one parity
/// piece is not the code's) harms only the delivery of its own set: a node
/// that holds the data pieces files the set and counts the mismatch; a node
/// that needs the bad piece rebuilds bytes that are no tx set, counts that,
/// and files nothing.
#[tokio::test]
async fn pieces_that_do_not_fit_together_are_counted() {
    let mut nodes = coded_mesh(27501).await;
    for node in &nodes {
        node.handle.set_txset_tree(coded_config(&nodes));
    }
    let leader = 1;
    let layout = layout_of(&nodes, leader);
    let (hash, data) = test_set(77);
    let bad = pieces_of(&nodes[leader], &data, layout.params, |pieces| {
        for byte in pieces[DATA].iter_mut() {
            *byte ^= 0x5A;
        }
    });
    // Two nodes that relay pieces which are not sent here (6 and 7): what
    // they get comes through relays, and they pass nothing on.
    let with_data = index_of(&nodes, &layout.relays_of(7)[0]);
    let with_parity = index_of(&nodes, &layout.relays_of(6)[0]);

    // The four data pieces: the set, and a root that is not the code's.
    for index in 0..DATA {
        deliver_piece(&nodes, &layout, &bad, index, with_data).await;
    }
    let (slot, _) = expect_set(&mut nodes[with_data], hash).await;
    assert_eq!(slot, None);
    tokio::time::sleep(Duration::from_millis(200)).await;
    assert_eq!(nodes[with_data].count(|m| &m.txset_coded_sets_rebuilt), 1);
    assert_eq!(
        nodes[with_data].count(|m| &m.txset_coded_sets_rebuilt_with_parity),
        0
    );
    assert_eq!(
        nodes[with_data].count(|m| &m.txset_coded_sets_inconsistent),
        1
    );

    // Three data pieces and the bad parity piece: no tx set.
    for index in 1..=DATA {
        deliver_piece(&nodes, &layout, &bad, index, with_parity).await;
    }
    let (received, _) =
        set_events_within(&mut nodes[with_parity], Duration::from_millis(400)).await;
    assert!(received.is_empty());
    assert_eq!(
        nodes[with_parity].count(|m| &m.txset_coded_pieces_received),
        DATA as u64
    );
    assert_eq!(
        nodes[with_parity].count(|m| &m.txset_coded_pieces_invalid),
        0
    );
    assert_eq!(nodes[with_parity].count(|m| &m.txset_coded_sets_failed), 1);
    assert_eq!(nodes[with_parity].count(|m| &m.txset_coded_sets_rebuilt), 0);
}

/// The signatures of a piece: the validator vouches for its overlay's key,
/// that key for the root, the slot, the code and the length.
#[test]
fn a_piece_is_checked_against_the_leaders_node_id() {
    use libp2p::identity::ed25519;
    let validator = ed25519::Keypair::generate();
    let overlay = Keypair::generate_ed25519();
    let peer = overlay.public().to_peer_id();
    let identity = NodeIdentity {
        network_id: NETWORK_ID,
        node_id: validator.public().to_bytes(),
        signature: validator
            .sign(&node_id_binding(&NETWORK_ID, &peer))
            .try_into()
            .unwrap(),
    };
    let params = CodedParams {
        data: 4,
        pieces: 8,
        relays: 1,
    };
    let frame = vec![7u8; 1001];
    let pieces = txset_coded::encode_pieces(&frame, params).unwrap();
    let signed = sign_pieces(&overlay, &identity, SLOT, &SEED, params, 1001, pieces, 5).unwrap();
    let header = signed.headers[3].clone();
    assert!(verify_piece_signatures(&NETWORK_ID, &SEED, &header));
    // The path of every piece reaches the signed root.
    for (i, piece) in signed.pieces.iter().enumerate() {
        let h = &signed.headers[i];
        assert_eq!(
            txset_coded::merkle_root_from(txset_coded::leaf_hash(i as u8, piece), i, &h.path),
            h.root
        );
    }
    let wrong = |change: &dyn Fn(&mut PieceHeader)| {
        let mut h = header.clone();
        change(&mut h);
        !verify_piece_signatures(&NETWORK_ID, &SEED, &h)
    };
    assert!(wrong(&|h| h.slot += 1));
    assert!(wrong(&|h| h.total_len += 1));
    assert!(wrong(&|h| h.data = 3));
    assert!(wrong(&|h| h.pieces = 9));
    assert!(wrong(&|h| h.root[0] ^= 1));
    assert!(wrong(&|h| h.root_sig[0] ^= 1));
    assert!(wrong(&|h| h.binding_sig[0] ^= 1));
    assert!(wrong(
        &|h| h.leader = ed25519::Keypair::generate().public().to_bytes()
    ));
    assert!(wrong(
        &|h| h.overlay_key = ed25519::Keypair::generate().public().to_bytes()
    ));
    assert!(!verify_piece_signatures(&[1u8; 32], &SEED, &header));
    // Another seed for the slot: another history of the network.
    assert!(!verify_piece_signatures(&NETWORK_ID, &[4u8; 32], &header));
    // What is not signed: where the piece is in the set and how it came.
    let mut other = header.clone();
    other.index = 5;
    other.hop = 2;
    other.sent_unix_ms = 99;
    assert!(verify_piece_signatures(&NETWORK_ID, &SEED, &other));
}

/// A node the others do not expect to lead the slot (a leader of a later
/// round, or one with another view) does not cut its set into pieces, which
/// every node would refuse: it pushes the set to everyone, as a star.
#[tokio::test]
async fn a_leader_nobody_expects_pushes_to_everyone() {
    let mut nodes = coded_mesh(27601).await;
    let expected = nodes[3].node_id();
    for node in &nodes {
        node.handle
            .set_txset_tree(coded_config(&nodes).with_leaders(vec![Some(vec![expected])]));
    }
    let leader = 6;
    let (hash, data) = test_set(78);
    nodes[leader]
        .handle
        .push_txset_for_slot(hash, data, SLOT)
        .await;
    for i in (0..NODES).filter(|i| *i != leader) {
        let (slot, from) = expect_set(&mut nodes[i], hash).await;
        assert_eq!(slot, None);
        assert_eq!(from, nodes[leader].peer_id);
    }
    assert_eq!(nodes[leader].count(|m| &m.txset_coded_sets_encoded), 0);
    assert_eq!(nodes[leader].count(|m| &m.txset_coded_pieces_sent), 0);
    assert_eq!(
        nodes[leader].count(|m| &m.txset_push_sent),
        NODES as u64 - 1
    );
    assert_eq!(sum(&nodes, |m| &m.txset_coded_pieces_refused), 0);
    // The expected leader does code.
    let (hash, data) = test_set(79);
    nodes[3].handle.push_txset_for_slot(hash, data, SLOT).await;
    for i in (0..NODES).filter(|i| *i != 3) {
        expect_set(&mut nodes[i], hash).await;
    }
    assert_eq!(nodes[3].count(|m| &m.txset_coded_pieces_sent), PIECES);
    assert_eq!(nodes[3].count(|m| &m.txset_push_sent), 0);
}
