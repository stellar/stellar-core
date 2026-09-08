use super::*;

// Hold the actual outbound stream lock to model a write that cannot complete.
// This is deterministic: no large allocation, socket-buffer assumption, or
// timing-dependent attempt to saturate a loopback connection is needed.
async fn dispatcher_with_blocked_write(command: &str) {
    let (handle, _events, _tx_events, overlay) =
        create_overlay(Keypair::generate_ed25519(), Arc::new(OverlayMetrics::new())).unwrap();
    let peer = PeerId::random();
    let streams = Arc::new(PeerOutboundStreams::new());
    overlay
        .state
        .peer_streams
        .write()
        .await
        .insert(peer, Arc::clone(&streams));
    let blocked = match command {
        "txset" => streams.txset.lock().await,
        "fetch" => streams.scp.lock().await,
        _ => streams.scp.lock().await,
    };
    let mut task = tokio::spawn(async move { overlay.run("127.0.0.1", 0).await });
    match command {
        "txset" => {
            let (hash, data) = test_txset_xdr(1);
            handle.send_txset(hash, data, peer).await;
        }
        "fetch" => handle.fetch_txset([1; 32], 1).await,
        "scp" => handle
            .send_scp_to_peer(peer, &test_scp_envelope_xdr(1))
            .await
            .unwrap(),
        "scp-state" => handle.request_scp_state_from_all_peers(1).await,
        _ => unreachable!(),
    }
    // FIFO command delivery means the write command precedes this ping. The
    // reply must arrive while the stream is still blocked, not after release.
    let progress = tokio::time::timeout(Duration::from_secs(1), handle.ping()).await;
    let shutdown = tokio::time::timeout(Duration::from_secs(1), async {
        handle.shutdown().await;
        (&mut task).await.unwrap();
    })
    .await;
    task.abort();
    drop(blocked);
    assert!(
        matches!(progress, Ok(Ok(()))),
        "dispatcher stopped responding during {command}: {progress:?}"
    );
    assert!(shutdown.is_ok(), "shutdown waited for the blocked write");
}

#[tokio::test]
async fn blocked_txset_write_does_not_block_dispatcher() {
    dispatcher_with_blocked_write("txset").await;
}

#[tokio::test]
async fn blocked_txset_request_does_not_block_dispatcher() {
    dispatcher_with_blocked_write("fetch").await;
}

#[tokio::test]
async fn blocked_direct_scp_write_does_not_block_dispatcher() {
    dispatcher_with_blocked_write("scp").await;
}

#[tokio::test]
async fn blocked_scp_state_request_does_not_block_dispatcher() {
    dispatcher_with_blocked_write("scp-state").await;
}

struct TestNode {
    handle: OverlayHandle,
    events: mpsc::UnboundedReceiver<OverlayEvent>,
    _tx_events: mpsc::Receiver<OverlayEvent>,
    state: Arc<SharedState>,
    peer: PeerId,
    address: Multiaddr,
    task: tokio::task::JoinHandle<()>,
}

impl TestNode {
    async fn start() -> Self {
        Self::start_with_control(true).await
    }

    async fn start_with_control(accept_control: bool) -> Self {
        let (handle, events, tx_events, mut overlay) =
            create_overlay(Keypair::generate_ed25519(), Arc::new(OverlayMetrics::new())).unwrap();
        let peer = *overlay.swarm.local_peer_id();
        let state = Arc::clone(&overlay.state);
        // Ask the OS for a port and observe the bound address, rather than
        // reserving fixed ports or assuming a listener is ready after a sleep.
        overlay
            .swarm
            .listen_on("/ip4/127.0.0.1/udp/0/quic-v1".parse().unwrap())
            .unwrap();
        let address = tokio::time::timeout(Duration::from_secs(5), async {
            loop {
                if let SwarmEvent::NewListenAddr { address, .. } =
                    overlay.swarm.select_next_some().await
                {
                    break address;
                }
            }
        })
        .await
        .expect("listener did not start");
        let task = tokio::spawn(overlay.run_event_loop_with_control(accept_control));
        Self {
            handle,
            events,
            _tx_events: tx_events,
            state,
            peer,
            address,
            task,
        }
    }

    async fn streams_to(&self, peer: PeerId) -> Arc<PeerOutboundStreams> {
        tokio::time::timeout(Duration::from_secs(5), async {
            loop {
                let streams = self.state.peer_streams.read().await.get(&peer).cloned();
                if let Some(streams) = streams {
                    if streams.scp.lock().await.is_some()
                        && streams.tx.lock().await.is_some()
                        && streams.txset.lock().await.is_some()
                    {
                        return streams;
                    }
                }
                tokio::time::sleep(Duration::from_millis(5)).await;
            }
        })
        .await
        .expect("peer streams did not open")
    }

    async fn connect(&self, other: &Self) {
        self.handle.dial(other.address.clone()).await;
        self.streams_to(other.peer).await;
        other.streams_to(self.peer).await;
    }

    async fn stop(&mut self) {
        tokio::time::timeout(Duration::from_secs(2), async {
            self.handle.shutdown().await;
            (&mut self.task).await.unwrap();
        })
        .await
        .expect("overlay did not stop");
    }
}

impl Drop for TestNode {
    fn drop(&mut self) {
        self.task.abort();
    }
}

fn large_txset() -> ([u8; 32], Vec<u8>) {
    use stellar_xdr::curr::{
        GeneralizedTransactionSet, Limits, ReadXdr, TransactionEnvelope, TransactionPhase,
        TxSetComponent, TxSetComponentTxsMaybeDiscountedFee, WriteXdr,
    };
    let txs: Vec<_> = (0..6000)
        .map(|seq| {
            TransactionEnvelope::from_xdr(
                // Extra operations make a multi-MiB transport fixture; this
                // test checks framing/delivery, not ledger validity or SAC TPS.
                crate::xdr::tests::valid_transaction_xdr(1000, seq, 16),
                Limits::none(),
            )
            .unwrap()
        })
        .collect();
    let mut set = GeneralizedTransactionSet::default();
    let GeneralizedTransactionSet::V1(v1) = &mut set;
    v1.phases = vec![TransactionPhase::V0(
        vec![TxSetComponent::TxsetCompTxsMaybeDiscountedFee(
            TxSetComponentTxsMaybeDiscountedFee {
                base_fee: None,
                txs: txs.try_into().unwrap(),
            },
        )]
        .try_into()
        .unwrap(),
    )]
    .try_into()
    .unwrap();
    let data = set.to_xdr(Limits::none()).unwrap();
    assert!(data.len() > 4 * 1024 * 1024);
    (crate::xdr::sha256_hash(&data), data)
}

#[tokio::test]
async fn blocked_peer_does_not_delay_other_txsets_or_scp_delivery() {
    let mut sender = TestNode::start().await;
    let mut slow = TestNode::start().await;
    let mut healthy = TestNode::start().await;
    sender.connect(&slow).await;
    sender.connect(&healthy).await;

    let slow_streams = sender.streams_to(slow.peer).await;
    let blocked = slow_streams.txset.lock().await;
    let (hash, data) = large_txset();
    sender
        .handle
        .send_txset(hash, data.clone(), slow.peer)
        .await;

    // Also exercise reopening: this write can complete only if the dispatcher
    // keeps polling the swarm to service Control::open_stream().
    let healthy_streams = sender.streams_to(healthy.peer).await;
    healthy_streams.txset.lock().await.take();
    sender
        .handle
        .send_txset(hash, data.clone(), healthy.peer)
        .await;
    let (second_hash, second_data) = test_txset_xdr(2);
    sender
        .handle
        .send_txset(second_hash, second_data.clone(), healthy.peer)
        .await;
    let scp = test_scp_envelope_xdr(1234);
    sender.handle.broadcast_scp(scp.clone()).await;

    // Assert receipt, not the latency of enqueueing a send command. Both SCP
    // streams and the healthy peer's tx-set stream must progress before the
    // slow peer's write is released. Two sets on one stream must stay framed.
    tokio::time::timeout(Duration::from_secs(10), async {
        let mut received = HashMap::new();
        let mut got_scp = false;
        while received.len() != 2 || !got_scp {
            match healthy.events.recv().await.unwrap() {
                OverlayEvent::TxSetReceived { hash, data, .. } => {
                    assert!(received.insert(hash, data).is_none());
                }
                OverlayEvent::ScpReceived { envelope, .. } if envelope == scp => got_scp = true,
                _ => {}
            }
        }
        assert_eq!(received.get(&hash), Some(&data));
        assert_eq!(received.get(&second_hash), Some(&second_data));
        loop {
            match slow.events.recv().await.unwrap() {
                OverlayEvent::ScpReceived { envelope, .. } if envelope == scp => break,
                OverlayEvent::TxSetReceived { .. } => {
                    panic!("blocked write unexpectedly completed")
                }
                _ => {}
            }
        }
    })
    .await
    .expect("a blocked tx-set send prevented delivery to another peer or stream");

    drop(blocked);
    tokio::time::timeout(Duration::from_secs(10), async {
        loop {
            if let OverlayEvent::TxSetReceived {
                hash: received_hash,
                data: received_data,
                ..
            } = slow.events.recv().await.unwrap()
            {
                assert_eq!(received_hash, hash);
                assert_eq!(received_data, data);
                break;
            }
        }
    })
    .await
    .expect("blocked send did not resume after release");
    sender.stop().await;
    slow.stop().await;
    healthy.stop().await;
}

#[tokio::test]
async fn txset_fetch_reaches_peer_while_response_to_same_peer_is_blocked() {
    let mut sender = TestNode::start().await;
    let mut receiver = TestNode::start().await;
    sender.connect(&receiver).await;

    // Keep the response stream blocked for the entire request/receive check.
    // The dispatcher already stays responsive; the request must also get
    // past the per-peer stream lock and reach the remote application.
    let streams = sender.streams_to(receiver.peer).await;
    let blocked = streams.txset.lock().await;
    let (response_hash, response_data) = test_txset_xdr(1);
    sender
        .handle
        .send_txset(response_hash, response_data.clone(), receiver.peer)
        .await;
    // Use the pre-opened stream, reuse it for another frame, then exercise
    // reopening. All three requests must arrive while the response is blocked.
    for id in [42, 43, 44] {
        if id == 44 {
            streams.scp.lock().await.take();
        }
        let requested_hash = [id; 32];
        sender
            .handle
            .record_txset_source(requested_hash, receiver.peer)
            .await;
        sender.handle.fetch_txset(requested_hash, 42).await;

        tokio::time::timeout(Duration::from_secs(2), async {
            loop {
                match receiver.events.recv().await.unwrap() {
                    OverlayEvent::TxSetRequested { hash, from } => {
                        assert_eq!(hash, requested_hash);
                        assert_eq!(from, sender.peer);
                        break;
                    }
                    OverlayEvent::TxSetReceived { .. } => {
                        panic!("response passed the held stream lock");
                    }
                    _ => {}
                }
            }
        })
        .await
        .expect("fetch request waited for a blocked response to the same peer");
        let control = streams.scp.lock().await;
        let control = control.as_ref().unwrap();
        assert_eq!(control.protocol, CONTROL_PROTOCOL);
        assert_eq!(control.stream.priority().unwrap(), 2);
    }

    // Releasing the response still delivers the original frame intact.
    drop(blocked);
    tokio::time::timeout(Duration::from_secs(2), async {
        loop {
            if let OverlayEvent::TxSetReceived { hash, data, .. } =
                receiver.events.recv().await.unwrap()
            {
                assert_eq!(hash, response_hash);
                assert_eq!(data, response_data);
                break;
            }
        }
    })
    .await
    .expect("response did not resume after releasing its stream");
    sender.stop().await;
    receiver.stop().await;
}

#[tokio::test]
async fn quic_transport_priorities_are_applied_to_each_route_and_reopening() {
    let mut sender = TestNode::start().await;
    let mut receiver = TestNode::start().await;
    sender.connect(&receiver).await;
    let streams = sender.streams_to(receiver.peer).await;
    // This getter reaches Quinn through the actual swarm, negotiation, erased
    // muxer and QUIC wrappers. It does not read our route constants or a cache.
    for (mutex, expected) in [(&streams.scp, 2), (&streams.txset, 1), (&streams.tx, 0)] {
        let guard = mutex.lock().await;
        assert_eq!(guard.as_ref().unwrap().stream.priority().unwrap(), expected);
    }
    assert!(streams.legacy_txset_request.lock().await.is_none());
    streams.txset.lock().await.take();
    let (hash, data) = test_txset_xdr(1);
    // Await the actual write, rather than command enqueue, to observe reopening.
    send_to_peer_stream(
        &sender.state,
        receiver.peer,
        StreamType::TxSet,
        &crate::xdr::frame_tx_set(&data),
    )
    .await
    .unwrap();
    assert_eq!(
        streams
            .txset
            .lock()
            .await
            .as_ref()
            .unwrap()
            .stream
            .priority()
            .unwrap(),
        1
    );
    tokio::time::timeout(Duration::from_secs(2), async {
        loop {
            if let OverlayEvent::TxSetReceived { hash: received, .. } =
                receiver.events.recv().await.unwrap()
            {
                assert_eq!(received, hash);
                break;
            }
        }
    })
    .await
    .unwrap();
    sender.stop().await;
    receiver.stop().await;
}

#[tokio::test]
async fn legacy_peer_receives_scp_and_fetches_on_its_supported_routes() {
    let mut sender = TestNode::start().await;
    // This peer advertises only the old protocols, and its SCP reader rejects
    // GetTxSet. A silent send on the wrong route therefore fails this test.
    let mut legacy = TestNode::start_with_control(false).await;
    sender.connect(&legacy).await;
    let streams = sender.streams_to(legacy.peer).await;
    let blocked_response = streams.txset.lock().await;
    // Legacy compatibility must preserve request isolation, including when
    // negotiation and request streams need reopening during a blocked response.
    for id in [17, 18] {
        if id == 18 {
            streams.scp.lock().await.take();
            streams.legacy_txset_request.lock().await.take();
        }
        let hash = [id; 32];
        sender.handle.record_txset_source(hash, legacy.peer).await;
        sender.handle.fetch_txset(hash, id as u32).await;
        let scp = test_scp_envelope_xdr(id as u64);
        sender.handle.broadcast_scp(scp.clone()).await;
        tokio::time::timeout(Duration::from_secs(2), async {
            let mut got_request = false;
            let mut got_scp = false;
            while !got_request || !got_scp {
                match legacy.events.recv().await.unwrap() {
                    OverlayEvent::TxSetRequested { hash: h, from } => {
                        assert_eq!(h, hash);
                        assert_eq!(from, sender.peer);
                        got_request = true;
                    }
                    OverlayEvent::ScpReceived { envelope, .. } if envelope == scp => got_scp = true,
                    _ => {}
                }
            }
        })
        .await
        .expect("legacy SCP or fetch waited for a blocked response or used an unsupported route");
        for (mutex, protocol) in [
            (&streams.scp, SCP_PROTOCOL),
            (&streams.legacy_txset_request, TXSET_PROTOCOL),
        ] {
            let guard = mutex.lock().await;
            let stream = guard.as_ref().unwrap();
            assert_eq!(stream.protocol, protocol);
            assert_eq!(stream.stream.priority().unwrap(), 2);
        }
    }
    drop(blocked_response);

    // Updated receivers still understand requests arriving on the old route.
    let hash = [17; 32];
    send_to_peer_stream(
        &legacy.state,
        sender.peer,
        StreamType::TxSet,
        &crate::xdr::frame_get_tx_set(hash),
    )
    .await
    .unwrap();
    tokio::time::timeout(Duration::from_secs(2), async {
        loop {
            if let OverlayEvent::TxSetRequested { hash: h, from } =
                sender.events.recv().await.unwrap()
            {
                assert_eq!(h, hash);
                assert_eq!(from, legacy.peer);
                break;
            }
        }
    })
    .await
    .unwrap();
    sender.stop().await;
    legacy.stop().await;
}

async fn bulk_send_admission_is_bounded(byte_limit: bool) {
    let (mut handle, _events, _tx_events, overlay) =
        create_overlay(Keypair::generate_ed25519(), Arc::new(OverlayMetrics::new())).unwrap();
    let peer = PeerId::random();
    let streams = Arc::new(PeerOutboundStreams::new());
    overlay
        .state
        .peer_streams
        .write()
        .await
        .insert(peer, Arc::clone(&streams));
    let blocked = streams.txset.lock().await;
    let (hash, data) = test_txset_xdr(1);
    // Exercise both limits independently without allocating hundreds of MiB.
    let slots = if byte_limit { 2 } else { 1 };
    let bytes = if byte_limit { data.len() + 4 } else { 1024 };
    handle.txset_send_slots = Arc::new(Semaphore::new(slots));
    handle.txset_send_bytes = Arc::new(Semaphore::new(bytes));
    let mut task = tokio::spawn(async move { overlay.run("127.0.0.1", 0).await });
    handle.send_txset(hash, data.clone(), peer).await;
    let waiting = handle.send_txset(hash, data, peer);
    tokio::pin!(waiting);
    assert!(futures::poll!(&mut waiting).is_pending());
    tokio::time::timeout(Duration::from_secs(1), handle.ping())
        .await
        .unwrap()
        .unwrap();
    assert!(futures::poll!(&mut waiting).is_pending());

    // Shutdown must cancel the active send and wake the producer waiting for
    // admission even though the transport lock is never released.
    tokio::time::timeout(Duration::from_secs(1), async {
        handle.shutdown().await;
        (&mut task).await.unwrap();
        waiting.await;
    })
    .await
    .expect("shutdown blocked on bulk sends");
    assert_eq!(handle.txset_send_slots.available_permits(), slots);
    assert_eq!(handle.txset_send_bytes.available_permits(), bytes);
    drop(blocked);
}

#[tokio::test]
async fn bulk_send_count_is_bounded_without_blocking_dispatcher() {
    bulk_send_admission_is_bounded(false).await;
}

#[tokio::test]
async fn bulk_send_bytes_are_bounded_without_blocking_dispatcher() {
    bulk_send_admission_is_bounded(true).await;
}

#[tokio::test]
async fn failed_txset_send_releases_admission() {
    let (handle, _events, _tx_events, overlay) =
        create_overlay(Keypair::generate_ed25519(), Arc::new(OverlayMetrics::new())).unwrap();
    let mut task = tokio::spawn(async move { overlay.run("127.0.0.1", 0).await });
    let (hash, data) = test_txset_xdr(1);
    handle.send_txset(hash, data, PeerId::random()).await;
    tokio::time::timeout(Duration::from_secs(1), async {
        let _all = Arc::clone(&handle.txset_send_slots)
            .acquire_many_owned(MAX_OUTSTANDING_TXSET_SENDS as u32)
            .await
            .unwrap();
        assert_eq!(
            handle.txset_send_bytes.available_permits(),
            MAX_OUTSTANDING_TXSET_BYTES
        );
        handle.shutdown().await;
        (&mut task).await.unwrap();
    })
    .await
    .expect("failed send retained its permits");
}

#[tokio::test]
async fn fetch_reservation_is_deduplicated_and_failure_preserves_reassignment() {
    for reassigned in [false, true] {
        let (_handle, _events, _tx_events, mut overlay) =
            create_overlay(Keypair::generate_ed25519(), Arc::new(OverlayMetrics::new())).unwrap();
        let peer = PeerId::random();
        overlay
            .state
            .peer_streams
            .write()
            .await
            .insert(peer, Arc::new(PeerOutboundStreams::new()));
        let hash = [3; 32];
        overlay.fetch_txset(hash, 12).await;
        overlay.fetch_txset(hash, 12).await;
        assert_eq!(
            overlay.sends.len(),
            1,
            "duplicate fetch scheduled another write"
        );
        let replacement = (PeerId::random(), Instant::now(), 13);
        if reassigned {
            overlay
                .state
                .pending_txset_requests
                .write()
                .await
                .insert(hash, replacement);
        }
        // These uncontended bookkeeping operations do not yield. Disconnect
        // before the spawned write is polled, making it fail deterministically.
        overlay.state.peer_streams.write().await.remove(&peer);
        tokio::time::timeout(Duration::from_secs(1), overlay.sends.join_next())
            .await
            .unwrap()
            .unwrap()
            .unwrap();
        assert_eq!(
            overlay
                .state
                .pending_txset_requests
                .read()
                .await
                .get(&hash)
                .copied(),
            reassigned.then_some(replacement)
        );
    }
}

#[tokio::test]
async fn queued_peer_sends_share_payload_and_release_it_on_drop() {
    let (handle, _events, _tx_events, mut overlay) =
        create_overlay(Keypair::generate_ed25519(), Arc::new(OverlayMetrics::new())).unwrap();
    let (hash, data) = test_txset_xdr(1);
    let data = Arc::new(data);
    let retained = Arc::downgrade(&data);
    for _ in 0..29 {
        handle
            .send_txset(hash, data.clone(), PeerId::random())
            .await;
    }
    for _ in 0..29 {
        let OverlayCommand::SendTxSet { data: pending, .. } = overlay.cmd_rx.recv().await.unwrap()
        else {
            panic!("expected a queued response");
        };
        assert!(Arc::ptr_eq(&data, &pending));
    }
    assert_eq!(
        handle.txset_send_slots.available_permits(),
        MAX_OUTSTANDING_TXSET_SENDS
    );
    assert_eq!(
        handle.txset_send_bytes.available_permits(),
        MAX_OUTSTANDING_TXSET_BYTES
    );
    drop(data);
    assert!(
        retained.upgrade().is_none(),
        "completed sends retained the payload"
    );
}
