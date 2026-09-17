use super::*;

// Hold the actual outbound stream lock to model a write that cannot complete.
// This is deterministic: no large allocation, socket-buffer assumption, or
// timing-dependent attempt to saturate a loopback connection is needed.
async fn dispatcher_with_blocked_write(command: &str) {
    let (handle, _events, _admissions, overlay) =
        create_test_overlay(Keypair::generate_ed25519(), Arc::new(OverlayMetrics::new())).unwrap();
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
    let envelope = test_scp_envelope_xdr(1);
    let scp_send = handle.send_scp_to_peer(peer, &envelope);
    tokio::pin!(scp_send);
    match command {
        "txset" => {
            let (hash, data) = test_txset_xdr(1);
            handle
                .send_txset(hash, Arc::new(TxSetData::from_local(data).unwrap()), peer)
                .await;
        }
        "fetch" => handle.fetch_txset([1; 32], 1).await,
        // Awaiting a direct send must preserve ordering within an SCP-state
        // response, while the dispatcher remains free to process commands.
        "scp" => assert!(futures::poll!(scp_send.as_mut()).is_pending()),
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
    if command == "scp" {
        assert!(
            scp_send.await.is_err(),
            "shutdown reported an unwritten SCP as sent"
        );
    }
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
    _admissions: mpsc::UnboundedReceiver<CoreCommand>,
    state: Arc<SharedState>,
    peer: PeerId,
    address: Multiaddr,
    task: tokio::task::JoinHandle<()>,
}

impl TestNode {
    async fn start() -> Self {
        let (handle, events, admissions, mut overlay) =
            create_test_overlay(Keypair::generate_ed25519(), Arc::new(OverlayMetrics::new()))
                .unwrap();
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
        let task = tokio::spawn(overlay.run_event_loop());
        Self {
            handle,
            events,
            _admissions: admissions,
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

#[tokio::test]
async fn txset_protocol_is_mandatory_and_survives_reopening() {
    let mut sender = TestNode::start().await;
    let mut receiver = TestNode::start().await;
    sender.connect(&receiver).await;
    let streams = sender.streams_to(receiver.peer).await;
    for legacy in ["/stellar/txset/1.0.0", "/stellar/txset/zstd/1.0.0"] {
        let mut control = sender.state.control.clone();
        assert!(matches!(
            control
                .open_stream(receiver.peer, StreamProtocol::new(legacy))
                .await,
            Err(libp2p_stream::OpenStreamError::UnsupportedProtocol(_))
        ));
    }
    let (hash, xdr) = large_txset();
    let shared = Arc::new(TxSetData::from_local(xdr.clone()).unwrap());
    for reopen in [false, true] {
        if reopen {
            streams.txset.lock().await.take();
        }
        send_to_peer_stream(
            &sender.state,
            receiver.peer,
            StreamType::TxSet,
            shared.encoded(),
        )
        .await
        .unwrap();
        assert!(shared.encoded().len() < xdr.len() / 2);
        {
            let guard = streams.txset.lock().await;
            let outbound = guard.as_ref().unwrap();
            assert_eq!(outbound.protocol, TXSET_PROTOCOL);
            assert_eq!(outbound.stream.priority().unwrap(), 1);
        }
        tokio::time::timeout(Duration::from_secs(10), async {
            loop {
                if let OverlayEvent::TxSetReceived {
                    hash: got, data, ..
                } = receiver.events.recv().await.unwrap()
                {
                    assert_eq!(got, hash);
                    assert_eq!(data.as_slice(), xdr);
                    assert_eq!(data.encoded(), shared.encoded());
                    break;
                }
            }
        })
        .await
        .expect("compressed response was not delivered intact");
    }
    sender.stop().await;
    receiver.stop().await;
}

#[tokio::test]
async fn relay_forwards_original_compressed_bytes() {
    let mut leader = TestNode::start().await;
    let mut relay = TestNode::start().await;
    let mut follower = TestNode::start().await;
    leader.connect(&relay).await;
    relay.connect(&follower).await;
    let (hash, xdr) = large_txset();
    // A different zstd level makes accidental re-encoding detectable.
    let encoded = [
        (xdr.len() as u32).to_be_bytes().as_slice(),
        zstd::bulk::compress(&xdr, 7).unwrap().as_slice(),
    ]
    .concat();
    assert_ne!(
        encoded,
        TxSetData::from_local(xdr.clone()).unwrap().encoded()
    );
    send_to_peer_stream(&leader.state, relay.peer, StreamType::TxSet, &encoded)
        .await
        .unwrap();
    let received = tokio::time::timeout(Duration::from_secs(10), async {
        loop {
            if let OverlayEvent::TxSetReceived {
                hash: got, data, ..
            } = relay.events.recv().await.unwrap()
            {
                assert_eq!(got, hash);
                assert_eq!(data.encoded(), encoded);
                break data;
            }
        }
    })
    .await
    .unwrap();
    // Forward the very object a receiving application puts in its cache.
    relay.handle.send_txset(hash, received, follower.peer).await;
    tokio::time::timeout(Duration::from_secs(10), async {
        loop {
            if let OverlayEvent::TxSetReceived {
                hash: got, data, ..
            } = follower.events.recv().await.unwrap()
            {
                assert_eq!(got, hash);
                assert_eq!(data.as_slice(), xdr);
                assert_eq!(data.encoded(), encoded);
                break;
            }
        }
    })
    .await
    .unwrap();
    leader.stop().await;
    relay.stop().await;
    follower.stop().await;
}

#[tokio::test]
async fn malformed_compressed_txsets_are_dropped_without_losing_next_frame() {
    let mut sender = TestNode::start().await;
    let mut receiver = TestNode::start().await;
    sender.connect(&receiver).await;
    // Well-formed compression is insufficient: XDR must also strict-decode.
    let malformed = Arc::new(TxSetData::from_local(vec![0xff; 1000]).unwrap());
    send_to_peer_stream(
        &sender.state,
        receiver.peer,
        StreamType::TxSet,
        malformed.encoded(),
    )
    .await
    .unwrap();
    let (hash, xdr) = test_txset_xdr(19);
    let valid = TxSetData::from_local(xdr.clone()).unwrap();
    // The old raw escape is rejected, as are corrupt compressed payloads.
    let raw = [
        ((xdr.len() as u32) | (1 << 31)).to_be_bytes().as_slice(),
        &xdr,
    ]
    .concat();
    for bad in [raw, vec![0xff; 20]] {
        send_to_peer_stream(&sender.state, receiver.peer, StreamType::TxSet, &bad)
            .await
            .unwrap();
    }
    send_to_peer_stream(
        &sender.state,
        receiver.peer,
        StreamType::TxSet,
        valid.encoded(),
    )
    .await
    .unwrap();
    tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            if let OverlayEvent::TxSetReceived {
                hash: got, data, ..
            } = receiver.events.recv().await.unwrap()
            {
                assert_eq!(got, hash, "malformed XDR was forwarded");
                assert_eq!(data.as_slice(), xdr);
                break;
            }
        }
    })
    .await
    .expect("a bad compressed frame prevented the next response");
    sender.stop().await;
    receiver.stop().await;
}

#[tokio::test]
async fn getdata_batches_preserve_frames_and_leave_control_responsive() {
    use stellar_xdr::curr::{
        HostFunction, InvokeHostFunctionOp, Limits, Operation, OperationBody, ReadXdr,
        TransactionEnvelope, WriteXdr,
    };

    let mut sender = TestNode::start().await;
    let mut receiver = TestNode::start().await;
    sender.connect(&receiver).await;

    let mut transactions = Vec::new();
    for sequence in 0..160 {
        let bytes = crate::xdr::tests::valid_transaction_xdr(1000, sequence, 16);
        transactions.push(ValidatedTx::from_core_trusted(bytes, 1000, 16).unwrap());
    }
    // A wire-valid upload larger than the batching budget exercises the
    // standalone write between two batches. Ledger validation is unrelated
    // to this transport test.
    let mut envelope = TransactionEnvelope::from_xdr(
        crate::xdr::tests::valid_transaction_xdr(1000, 200, 1),
        Limits::none(),
    )
    .unwrap();
    let TransactionEnvelope::Tx(v1) = &mut envelope else {
        unreachable!()
    };
    v1.tx.operations = vec![Operation {
        source_account: None,
        body: OperationBody::InvokeHostFunction(InvokeHostFunctionOp {
            host_function: HostFunction::UploadContractWasm(vec![0; 70_000].try_into().unwrap()),
            auth: Default::default(),
        }),
    }]
    .try_into()
    .unwrap();
    let large =
        ValidatedTx::from_core_trusted(envelope.to_xdr(Limits::none()).unwrap(), 1000, 1).unwrap();
    transactions.insert(80, large);
    let mut demand = GetData::new();
    demand.push([0xaa; 32]);
    for tx in &transactions {
        sender.state.tx_buffer.write().await.insert(Arc::clone(tx));
        demand.push(*tx.hash());
    }
    demand.push([0xbb; 32]);

    let streams = sender.streams_to(receiver.peer).await;
    let blocked = streams.tx.lock().await;
    for (encoded, _) in demand.encode_chunked().unwrap() {
        send_to_peer_stream(&receiver.state, sender.peer, StreamType::Tx, &encoded)
            .await
            .unwrap();
    }
    // A second demand on the same incoming stream is a barrier: its misses
    // prove the reader dispatched both requests while the first TX writer is
    // still blocked. The original per-TX task path has already looked up all
    // transactions by this point; a bounded response job retains only its next
    // batch. Use unknown hashes so the barrier needs no response stream.
    let mut barrier = GetData::new();
    for byte in 0xc0..0xe0 {
        barrier.push([byte; 32]);
    }
    for (encoded, _) in barrier.encode_chunked().unwrap() {
        send_to_peer_stream(&receiver.state, sender.peer, StreamType::Tx, &encoded)
            .await
            .unwrap();
    }
    tokio::time::timeout(Duration::from_secs(5), async {
        while sender
            .state
            .metrics
            .flood_unfulfilled_unknown
            .load(Ordering::Relaxed)
            < 32
            || sender.state.metrics.flood_fulfilled.load(Ordering::Relaxed) == 0
        {
            tokio::task::yield_now().await;
        }
    })
    .await
    .expect("GETDATA reader stopped progressing behind a blocked TX writer");
    assert!(
        sender.state.metrics.flood_fulfilled.load(Ordering::Relaxed) < transactions.len() as u64,
        "blocked response retained every requested transaction"
    );
    let scp = test_scp_envelope_xdr(987);
    sender.handle.broadcast_scp(scp.clone()).await;
    tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            if let Some(OverlayEvent::ScpReceived { envelope, .. }) = receiver.events.recv().await {
                if envelope == scp {
                    break;
                }
            }
        }
    })
    .await
    .expect("a blocked GETDATA response held up the control stream");
    assert!(receiver._admissions.try_recv().is_err());
    drop(blocked);

    tokio::time::timeout(Duration::from_secs(5), async {
        for expected in &transactions {
            let Some(CoreCommand::SubmitTx { tx, .. }) = receiver._admissions.recv().await else {
                panic!("expected transaction admission");
            };
            assert_eq!(tx.bytes(), expected.bytes());
            assert_eq!(tx.hash(), expected.hash());
        }
    })
    .await
    .expect("batched, oversized, and trailing transactions must all arrive");
    assert_eq!(
        sender.state.metrics.flood_fulfilled.load(Ordering::Relaxed),
        transactions.len() as u64
    );
    assert_eq!(
        sender
            .state
            .metrics
            .flood_unfulfilled_unknown
            .load(Ordering::Relaxed),
        34
    );
    sender.stop().await;
    receiver.stop().await;
}

#[tokio::test]
async fn scp_state_sends_preserve_slot_order_without_blocking_other_peers() {
    let mut sender = TestNode::start().await;
    let mut receiver = TestNode::start().await;
    let mut healthy = TestNode::start().await;
    sender.connect(&receiver).await;
    sender.connect(&healthy).await;

    let streams = sender.streams_to(receiver.peer).await;
    let blocked = streams.scp.lock().await;
    // Match Core's SCP-state order: recent slots, then an older checkpoint.
    let slots = [10, 11, 12, 8];
    let handle = sender.handle.clone();
    let peer = receiver.peer;
    let sends = tokio::spawn(async move {
        for slot in slots {
            handle
                .send_scp_to_peer(peer, &test_scp_envelope_xdr(slot))
                .await
                .unwrap();
        }
    });

    let independent = test_scp_envelope_xdr(13);
    tokio::time::timeout(Duration::from_secs(5), async {
        sender
            .handle
            .send_scp_to_peer(healthy.peer, &independent)
            .await
            .unwrap();
        loop {
            if let OverlayEvent::ScpReceived { envelope, .. } = healthy.events.recv().await.unwrap()
            {
                assert_eq!(envelope, independent);
                break;
            }
        }
    })
    .await
    .expect("blocked SCP-state response prevented a send to another peer");
    assert!(!sends.is_finished());

    drop(blocked);
    tokio::time::timeout(Duration::from_secs(5), async {
        sends.await.unwrap();
        for slot in slots {
            loop {
                if let OverlayEvent::ScpReceived { envelope, .. } =
                    receiver.events.recv().await.unwrap()
                {
                    assert_eq!(envelope, test_scp_envelope_xdr(slot));
                    break;
                }
            }
        }
    })
    .await
    .expect("SCP-state response did not resume after release");
    sender.stop().await;
    receiver.stop().await;
    healthy.stop().await;
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
        .send_txset(
            hash,
            Arc::new(TxSetData::from_local(data.clone()).unwrap()),
            slow.peer,
        )
        .await;

    // Also exercise reopening: this write can complete only if the dispatcher
    // keeps polling the swarm to service Control::open_stream().
    let healthy_streams = sender.streams_to(healthy.peer).await;
    healthy_streams.txset.lock().await.take();
    sender
        .handle
        .send_txset(
            hash,
            Arc::new(TxSetData::from_local(data.clone()).unwrap()),
            healthy.peer,
        )
        .await;
    let (second_hash, second_data) = test_txset_xdr(2);
    sender
        .handle
        .send_txset(
            second_hash,
            Arc::new(TxSetData::from_local(second_data.clone()).unwrap()),
            healthy.peer,
        )
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
        assert_eq!(received.get(&hash).unwrap().as_slice(), data);
        assert_eq!(received.get(&second_hash).unwrap().as_slice(), second_data);
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
                assert_eq!(received_data.as_slice(), data);
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
        .send_txset(
            response_hash,
            Arc::new(TxSetData::from_local(response_data.clone()).unwrap()),
            receiver.peer,
        )
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
                assert_eq!(data.as_slice(), response_data);
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
    streams.txset.lock().await.take();
    let (hash, data) = test_txset_xdr(1);
    // Await the actual write, rather than command enqueue, to observe reopening.
    send_to_peer_stream(
        &sender.state,
        receiver.peer,
        StreamType::TxSet,
        TxSetData::from_local(data.clone()).unwrap().encoded(),
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

async fn bulk_send_admission_is_bounded(byte_limit: bool) {
    let (mut handle, _events, _admissions, overlay) =
        create_test_overlay(Keypair::generate_ed25519(), Arc::new(OverlayMetrics::new())).unwrap();
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
    handle
        .send_txset(
            hash,
            Arc::new(TxSetData::from_local(data.clone()).unwrap()),
            peer,
        )
        .await;
    let waiting = handle.send_txset(hash, Arc::new(TxSetData::from_local(data).unwrap()), peer);
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
    let (handle, _events, _admissions, overlay) =
        create_test_overlay(Keypair::generate_ed25519(), Arc::new(OverlayMetrics::new())).unwrap();
    let mut task = tokio::spawn(async move { overlay.run("127.0.0.1", 0).await });
    let (hash, data) = test_txset_xdr(1);
    handle
        .send_txset(
            hash,
            Arc::new(TxSetData::from_local(data).unwrap()),
            PeerId::random(),
        )
        .await;
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
        let (_handle, _events, _admissions, mut overlay) =
            create_test_overlay(Keypair::generate_ed25519(), Arc::new(OverlayMetrics::new()))
                .unwrap();
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
    let (handle, _events, _admissions, mut overlay) =
        create_test_overlay(Keypair::generate_ed25519(), Arc::new(OverlayMetrics::new())).unwrap();
    let (hash, data) = test_txset_xdr(1);
    let data = Arc::new(TxSetData::from_local(data).unwrap());
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
