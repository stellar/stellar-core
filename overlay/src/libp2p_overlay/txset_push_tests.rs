//! Tests for the leader's tx-set push.

use super::*;

/// One overlay node on loopback, with access to its shared state.
struct Node {
    handle: OverlayHandle,
    events: mpsc::UnboundedReceiver<OverlayEvent>,
    state: Arc<SharedState>,
    metrics: Arc<OverlayMetrics>,
    peer_id: PeerId,
    port: u16,
    _admissions: mpsc::UnboundedReceiver<CoreCommand>,
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
        _admissions: admissions,
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

fn small_txset(seed: u8) -> ([u8; 32], Arc<TxSetData>) {
    let (hash, xdr) = test_txset_xdr(seed);
    (hash, Arc::new(TxSetData::from_local(xdr).unwrap()))
}

/// Wait for a `TxSetReceived` of `hash`; returns the slot it carries (None =
/// nobody on this node asked for it) and the sender.
async fn expect_txset(node: &mut Node, hash: [u8; 32]) -> (Option<u32>, PeerId) {
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

/// Wait for a `TxSetRequested` of `hash` and return the requester.
async fn expect_txset_request(node: &mut Node, hash: [u8; 32]) -> PeerId {
    let deadline = tokio::time::Instant::now() + Duration::from_secs(10);
    loop {
        match tokio::time::timeout_at(deadline, node.events.recv()).await {
            Ok(Some(OverlayEvent::TxSetRequested { hash: got, from })) if got == hash => {
                return from
            }
            Ok(Some(_)) => {}
            _ => panic!("no TxSetRequested for {:02x?}", &hash[..4]),
        }
    }
}

/// Collect the tx-set events (received hashes, requested hashes) that arrive
/// within `window`.
async fn txset_events_within(node: &mut Node, window: Duration) -> (Vec<[u8; 32]>, Vec<[u8; 32]>) {
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

fn load(counter: &AtomicU64) -> u64 {
    counter.load(Ordering::Relaxed)
}

/// The leader's push reaches every connected peer without any request, and
/// each peer is sent a given set once.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn push_reaches_every_connected_peer_once() {
    let leader = start_node(21711).await;
    let mut follower_a = start_node(21712).await;
    let mut follower_b = start_node(21713).await;
    connect(&follower_a, &leader).await;
    connect(&follower_b, &leader).await;

    let (hash, data) = small_txset(0x51);
    leader.handle.push_txset(hash, Arc::clone(&data)).await;

    for follower in [&mut follower_a, &mut follower_b] {
        let (slot, from) = expect_txset(follower, hash).await;
        assert_eq!(slot, None, "a pushed set arrives unrequested");
        assert_eq!(from, leader.peer_id);
        assert_eq!(load(&follower.metrics.fetch_txset_count), 0);
    }
    assert_eq!(load(&leader.metrics.txset_push_sent), 2);

    // Pushing the same set again sends nothing.
    leader.handle.push_txset(hash, data).await;
    for follower in [&mut follower_a, &mut follower_b] {
        let (received, _) = txset_events_within(follower, Duration::from_millis(300)).await;
        assert!(received.is_empty(), "set was sent twice");
    }
    assert_eq!(load(&leader.metrics.txset_push_sent), 2);
}

/// A follower whose request crosses the push is not sent a second copy. A
/// repeated request (the follower lost the pushed copy) is answered.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn request_for_a_pushed_set_is_skipped_once_then_answered() {
    let mut leader = start_node(21721).await;
    let mut follower = start_node(21722).await;
    connect(&follower, &leader).await;

    let (hash, data) = small_txset(0x52);
    leader.handle.push_txset(hash, data).await;
    expect_txset(&mut follower, hash).await;

    // First request: the leader already sent the set on this connection.
    follower.handle.fetch_txset(hash, 7).await;
    let (_, requested) = txset_events_within(&mut leader, Duration::from_millis(400)).await;
    assert!(requested.is_empty(), "request should have been skipped");
    assert_eq!(load(&leader.metrics.txset_push_request_skipped), 1);

    // Core's retry: asked again, the leader answers through the request path.
    follower.handle.refetch_txset(hash, 7).await;
    assert_eq!(
        expect_txset_request(&mut leader, hash).await,
        follower.peer_id
    );
    assert_eq!(load(&leader.metrics.txset_push_request_skipped), 1);
}

/// A request that arrives later than a crossing can explain means the peer
/// does not hold the pushed copy (it dropped or evicted it): the leader
/// answers it, the first time.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn request_long_after_the_push_is_answered() {
    let mut leader = start_node(21771).await;
    let mut follower = start_node(21772).await;
    connect(&follower, &leader).await;

    let (hash, data) = small_txset(0x57);
    leader.handle.push_txset(hash, data).await;
    expect_txset(&mut follower, hash).await;

    tokio::time::sleep(crate::flood::txset_push::PUSH_SKIP_WINDOW + Duration::from_millis(100))
        .await;
    follower.handle.fetch_txset(hash, 7).await;
    assert_eq!(
        expect_txset_request(&mut leader, hash).await,
        follower.peer_id
    );
    assert_eq!(load(&leader.metrics.txset_push_request_skipped), 0);
}

/// Fallback: a peer that was not connected when the set was pushed gets it
/// through the request path, and a later push only goes to that peer.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn peer_that_missed_the_push_is_served_on_request() {
    let mut leader = start_node(21731).await;
    let mut early = start_node(21732).await;
    connect(&early, &leader).await;

    let (hash, data) = small_txset(0x53);
    leader.handle.push_txset(hash, Arc::clone(&data)).await;
    expect_txset(&mut early, hash).await;

    let mut late = start_node(21733).await;
    connect(&late, &leader).await;

    // The late peer has nothing and asks: the request is not skipped.
    late.handle.fetch_txset(hash, 9).await;
    let from = expect_txset_request(&mut leader, hash).await;
    assert_eq!(from, late.peer_id);
    assert_eq!(load(&leader.metrics.txset_push_request_skipped), 0);
    leader
        .handle
        .send_txset(hash, Arc::clone(&data), from)
        .await;
    let (slot, _) = expect_txset(&mut late, hash).await;
    assert_eq!(slot, Some(9), "delivered as the answer to the request");
    assert_eq!(load(&late.metrics.fetch_txset_count), 1);

    // Another set, pushed after both connected, reaches both; re-pushing the
    // first set reaches only the late peer.
    leader.handle.push_txset(hash, data).await;
    let (slot, _) = expect_txset(&mut late, hash).await;
    assert_eq!(slot, None);
    let (received, _) = txset_events_within(&mut early, Duration::from_millis(300)).await;
    assert!(received.is_empty());
    assert_eq!(load(&leader.metrics.txset_push_sent), 2);
}

/// A connection closing drops the record of what was pushed to that peer,
/// so its requests are answered again.
/// The close event is injected: two nodes in one process keep each other's
/// streams alive, so a real close only shows up after the idle timeout.
#[tokio::test]
async fn disconnect_forgets_what_was_pushed_to_the_peer() {
    use libp2p::core::{transport::PortUse, ConnectedPoint, Endpoint};
    use libp2p::swarm::ConnectionId;

    let (_handle, _events, _admissions, mut overlay) =
        create_test_overlay(Keypair::generate_ed25519(), Arc::new(OverlayMetrics::new())).unwrap();
    let gone = PeerId::random();
    let stays = PeerId::random();
    let hash = [0x54; 32];
    overlay
        .state
        .txset_pushes
        .lock()
        .unwrap()
        .start_push(hash, [gone, stays]);
    // Use up the unrequested-frame allowance of the peer that will leave.
    let now = Instant::now();
    while overlay
        .state
        .txset_push_limiter
        .lock()
        .unwrap()
        .allow(gone, now)
    {}

    let closed = |peer_id, num_established| SwarmEvent::ConnectionClosed {
        peer_id,
        connection_id: ConnectionId::new_unchecked(1),
        endpoint: ConnectedPoint::Dialer {
            address: "/ip4/127.0.0.1/udp/1/quic-v1".parse().unwrap(),
            role_override: Endpoint::Dialer,
            port_use: PortUse::Reuse,
        },
        num_established,
        cause: None,
    };

    // Any connection to the peer closing clears the record of what was
    // pushed to it (the set may have been on that connection's streams); the
    // other peer is not affected. The rate-limit state stays while another
    // connection to the peer is still up.
    overlay.handle_swarm_event(closed(gone, 1)).await;
    {
        let mut pushes = overlay.state.txset_pushes.lock().unwrap();
        assert!(!pushes.was_pushed(&hash, &gone));
        assert!(pushes.was_pushed(&hash, &stays));
        assert!(!pushes.should_skip_request(&hash, &gone, now));
    }
    assert!(!overlay
        .state
        .txset_push_limiter
        .lock()
        .unwrap()
        .allow(gone, now));

    // The last connection closing also resets the peer's allowance.
    overlay.handle_swarm_event(closed(gone, 0)).await;
    assert!(overlay
        .state
        .txset_push_limiter
        .lock()
        .unwrap()
        .allow(gone, now));
}

/// A peer that sends more unrequested sets than the limit allows has the
/// excess dropped undecoded. A set this node asked that peer for still
/// arrives when the peer is over its limit, and does not use the allowance.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn unrequested_frames_over_the_rate_limit_are_dropped() {
    let mut pusher = start_node(21751).await;
    let mut receiver = start_node(21752).await;
    connect(&receiver, &pusher).await;
    // An allowance of exactly 3 frames that never refills, so the counts
    // below do not depend on timing.
    *receiver.state.txset_push_limiter.lock().unwrap() = PushRateLimiter::new(3.0, 0.0);
    let credits = |node: &Node, peer: &PeerId| {
        node.state
            .txset_push_limiter
            .lock()
            .unwrap()
            .request_credits(peer)
    };

    // Frames written straight to the tx-set stream, as a peer that ignores
    // the sender-side rules would.
    for seed in 0..5u8 {
        let (_, data) = small_txset(seed);
        send_to_peer_stream(
            &pusher.state,
            receiver.peer_id,
            StreamType::TxSet,
            data.encoded(),
        )
        .await
        .unwrap();
    }
    let (received, _) = txset_events_within(&mut receiver, Duration::from_millis(1000)).await;
    assert_eq!(received.len(), 3, "the allowance is honoured");
    assert_eq!(load(&receiver.metrics.txset_push_dropped), 2);

    // The peer is over its limit. This node asks it for a set: the request
    // buys the answer's way in ...
    let (wanted, wanted_data) = small_txset(0xf0);
    receiver.handle.fetch_txset(wanted, 11).await;
    let from = expect_txset_request(&mut pusher, wanted).await;
    assert_eq!(credits(&receiver, &pusher.peer_id), 1);
    pusher.handle.send_txset(wanted, wanted_data, from).await;
    let (slot, _) = expect_txset(&mut receiver, wanted).await;
    assert_eq!(slot, Some(11));
    assert_eq!(credits(&receiver, &pusher.peer_id), 0);
    assert_eq!(load(&receiver.metrics.txset_push_dropped), 2);

    // ... and, having answered a request, gave its token back: exactly one
    // more unrequested frame passes, the next one is dropped.
    for seed in 200..202u8 {
        let (_, data) = small_txset(seed);
        send_to_peer_stream(
            &pusher.state,
            receiver.peer_id,
            StreamType::TxSet,
            data.encoded(),
        )
        .await
        .unwrap();
    }
    let (received, _) = txset_events_within(&mut receiver, Duration::from_millis(1000)).await;
    assert_eq!(received.len(), 1);
    assert_eq!(load(&receiver.metrics.txset_push_dropped), 3);
}

/// A frame that is not a valid compressed tx set is dropped, and the stream
/// keeps working: the next (valid) push is delivered. Nothing is filed under
/// a hash the content does not have.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn malformed_push_is_dropped_and_the_stream_survives() {
    let pusher = start_node(21761).await;
    let mut receiver = start_node(21762).await;
    connect(&receiver, &pusher).await;

    let (hash, data) = small_txset(0x56);
    // Garbage, a truncated frame, and a well-formed zstd frame whose content
    // is not a GeneralizedTransactionSet.
    let truncated = &data.encoded()[..data.encoded().len() - 1];
    let not_a_txset = TxSetData::from_local(vec![0xab; 64]).unwrap();
    for frame in [&b"not a tx set"[..], truncated, not_a_txset.encoded()] {
        send_to_peer_stream(&pusher.state, receiver.peer_id, StreamType::TxSet, frame)
            .await
            .unwrap();
    }
    pusher.handle.push_txset(hash, data).await;

    let (received, _) = txset_events_within(&mut receiver, Duration::from_millis(1000)).await;
    assert_eq!(received, vec![hash], "only the valid set is delivered");
}
