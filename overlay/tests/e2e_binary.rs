//! End-to-end tests that spawn the actual stellar-overlay binary.
//!
//! These tests verify that the compiled binary works correctly,
//! catching issues that in-memory tests might miss (like main.rs wiring bugs).

use std::os::unix::net::UnixStream;
use std::path::PathBuf;
use std::process::{Child, Command, ExitStatus, Stdio};
use std::thread;
use std::time::{Duration, Instant};
use stellar_xdr::{Limits, ScpEnvelope, WriteXdr};

/// IPC message types (must match src/ipc/messages.rs)
mod ipc {
    use std::io::{Read, Write};
    use std::os::unix::net::UnixStream;

    pub const BROADCAST_SCP: u32 = 1;
    pub const SET_PEER_CONFIG: u32 = 8;
    pub const SHUTDOWN: u32 = 7;
    pub const REQUEST_TX_SET: u32 = 11;
    pub const CACHE_TX_SET: u32 = 12;
    pub const REQUEST_OVERLAY_METRICS: u32 = 13;
    pub const PUSH_TX_SET: u32 = 14;
    pub const TX_SET_AVAILABLE: u32 = 103;
    pub const OVERLAY_METRICS_RESPONSE: u32 = 105;
    pub const SCP_RECEIVED: u32 = 100;
    pub const PEER_REQUESTS_SCP_STATE: u32 = 102;

    pub fn send_message(
        stream: &mut UnixStream,
        msg_type: u32,
        payload: &[u8],
    ) -> std::io::Result<()> {
        let mut header = [0u8; 8];
        header[0..4].copy_from_slice(&msg_type.to_ne_bytes());
        header[4..8].copy_from_slice(&(payload.len() as u32).to_ne_bytes());
        stream.write_all(&header)?;
        if !payload.is_empty() {
            stream.write_all(payload)?;
        }
        Ok(())
    }

    pub fn recv_message(stream: &mut UnixStream) -> std::io::Result<(u32, Vec<u8>)> {
        let mut header = [0u8; 8];
        stream.read_exact(&mut header)?;
        let msg_type = u32::from_ne_bytes(header[0..4].try_into().unwrap());
        let payload_len = u32::from_ne_bytes(header[4..8].try_into().unwrap()) as usize;
        let mut payload = vec![0u8; payload_len];
        if payload_len > 0 {
            stream.read_exact(&mut payload)?;
        }
        Ok((msg_type, payload))
    }
}

/// Find the stellar-overlay binary
fn find_binary() -> PathBuf {
    if let Ok(path) = std::env::var("STELLAR_OVERLAY_BINARY") {
        if !path.is_empty() {
            return PathBuf::from(path);
        }
    }

    // The test runs from the overlay directory, so look in parent's target
    let debug = PathBuf::from("../target/debug/stellar-overlay");
    if debug.exists() {
        return debug;
    }
    let debug2 = PathBuf::from("target/debug/stellar-overlay");
    if debug2.exists() {
        return debug2;
    }
    let release = PathBuf::from("../target/release/stellar-overlay");
    if release.exists() {
        return release;
    }
    let release2 = PathBuf::from("target/release/stellar-overlay");
    if release2.exists() {
        return release2;
    }

    // Try manifest dir
    if let Ok(manifest) = std::env::var("CARGO_MANIFEST_DIR") {
        let debug = PathBuf::from(&manifest).join("../target/debug/stellar-overlay");
        if debug.exists() {
            return debug;
        }
        let release = PathBuf::from(&manifest).join("../target/release/stellar-overlay");
        if release.exists() {
            return release;
        }
    }

    panic!("stellar-overlay binary not found. Run `cargo build --release` first.");
}

/// Spawn an overlay process
fn spawn_overlay(socket_path: &str, peer_port: u16) -> Child {
    let binary = find_binary();
    let show_child_logs = std::env::var_os("STELLAR_OVERLAY_TEST_LOGS").is_some();

    if show_child_logs {
        eprintln!(
            "spawning stellar-overlay: {} --listen {} --peer-port {}",
            binary.display(),
            socket_path,
            peer_port
        );
    }

    let mut command = Command::new(binary);
    command
        .arg("--listen")
        .arg(socket_path)
        .arg("--peer-port")
        .arg(peer_port.to_string());

    if std::env::var_os("RUST_LOG").is_none() {
        command.env("RUST_LOG", "debug");
    }

    if show_child_logs {
        command.stdout(Stdio::inherit()).stderr(Stdio::inherit());
    } else {
        command.stdout(Stdio::null()).stderr(Stdio::null());
    }

    command.spawn().expect("Failed to spawn overlay process")
}

fn wait_for_child_exit(child: &mut Child, name: &str) -> ExitStatus {
    let start = Instant::now();
    let timeout = Duration::from_secs(10);

    loop {
        match child.try_wait() {
            Ok(Some(status)) => return status,
            Ok(None) if start.elapsed() < timeout => {
                thread::sleep(Duration::from_millis(50));
            }
            Ok(None) => {
                let _ = child.kill();
                let _ = child.wait();
                panic!("{} did not exit within {:?}", name, timeout);
            }
            Err(e) => panic!("Failed waiting for {}: {}", name, e),
        }
    }
}

fn valid_scp_envelope_xdr(slot_index: u64) -> Vec<u8> {
    let mut envelope = ScpEnvelope::default();
    envelope.statement.slot_index = slot_index;
    envelope.to_xdr(Limits::none()).unwrap()
}

/// A minimal valid GeneralizedTransactionSet and its content hash.
fn txset_xdr(seed: u8) -> ([u8; 32], Vec<u8>) {
    use sha2::{Digest, Sha256};
    use stellar_xdr::{GeneralizedTransactionSet, Hash};
    let mut tx_set = GeneralizedTransactionSet::default();
    let GeneralizedTransactionSet::V1(v1) = &mut tx_set;
    v1.previous_ledger_hash = Hash([seed; 32]);
    let bytes = tx_set.to_xdr(Limits::none()).unwrap();
    let hash: [u8; 32] = Sha256::digest(&bytes).into();
    (hash, bytes)
}

/// `[hash:32][slot:4 LE]`, the payload of REQUEST_TX_SET and PUSH_TX_SET and
/// the prefix of CACHE_TX_SET.
fn txset_key(hash: &[u8; 32], slot: u32) -> Vec<u8> {
    let mut payload = hash.to_vec();
    payload.extend_from_slice(&slot.to_le_bytes());
    payload
}

/// Read IPC messages until one of `msg_type` arrives (skipping others, such
/// as SCP state requests of a new connection). None on timeout.
fn recv_until(stream: &mut UnixStream, msg_type: u32, timeout: Duration) -> Option<Vec<u8>> {
    let deadline = Instant::now() + timeout;
    loop {
        let remaining = deadline.checked_duration_since(Instant::now())?;
        stream
            .set_read_timeout(Some(remaining.max(Duration::from_millis(1))))
            .unwrap();
        match ipc::recv_message(stream) {
            Ok((got, payload)) if got == msg_type => return Some(payload),
            Ok(_) => {}
            Err(_) => return None,
        }
    }
}

/// One counter of the overlay's metrics snapshot.
fn overlay_metric(stream: &mut UnixStream, name: &str) -> u64 {
    ipc::send_message(stream, ipc::REQUEST_OVERLAY_METRICS, &[]).unwrap();
    let payload = recv_until(
        stream,
        ipc::OVERLAY_METRICS_RESPONSE,
        Duration::from_secs(5),
    )
    .expect("metrics response");
    let json: serde_json::Value = serde_json::from_slice(&payload).unwrap();
    json[name]
        .as_u64()
        .unwrap_or_else(|| panic!("no metric {name}"))
}

/// Two overlay processes, B connected to A, each with its Core-side socket.
struct TwoOverlays {
    child_a: Child,
    child_b: Child,
    core_a: UnixStream,
    core_b: UnixStream,
    socket_a: String,
    socket_b: String,
}

impl TwoOverlays {
    fn start(name: &str, port_a: u16, port_b: u16) -> Self {
        let socket_a = format!("/tmp/e2e-{}-a-{}.sock", name, std::process::id());
        let socket_b = format!("/tmp/e2e-{}-b-{}.sock", name, std::process::id());
        let _ = std::fs::remove_file(&socket_a);
        let _ = std::fs::remove_file(&socket_b);
        let child_a = spawn_overlay(&socket_a, port_a);
        let child_b = spawn_overlay(&socket_b, port_b);
        let core_a = wait_for_socket(&socket_a, 5000).expect("Socket A should be ready");
        let mut core_b = wait_for_socket(&socket_b, 5000).expect("Socket B should be ready");
        let peer_config = format!(
            r#"{{"known_peers":["127.0.0.1:{}"],"preferred_peers":[],"listen_port":{}}}"#,
            port_a, port_b
        );
        ipc::send_message(&mut core_b, ipc::SET_PEER_CONFIG, peer_config.as_bytes())
            .expect("Should send peer config");
        // Connection and stream setup.
        thread::sleep(Duration::from_millis(1000));
        Self {
            child_a,
            child_b,
            core_a,
            core_b,
            socket_a,
            socket_b,
        }
    }

    fn stop(self) {
        let TwoOverlays {
            mut child_a,
            mut child_b,
            mut core_a,
            mut core_b,
            socket_a,
            socket_b,
        } = self;
        ipc::send_message(&mut core_a, ipc::SHUTDOWN, &[]).ok();
        ipc::send_message(&mut core_b, ipc::SHUTDOWN, &[]).ok();
        drop(core_a);
        drop(core_b);
        wait_for_child_exit(&mut child_a, "overlay A");
        wait_for_child_exit(&mut child_b, "overlay B");
        let _ = std::fs::remove_file(&socket_a);
        let _ = std::fs::remove_file(&socket_b);
    }
}

/// Wait for socket to be ready and return the connected stream
fn wait_for_socket(path: &str, timeout_ms: u64) -> Option<UnixStream> {
    let start = std::time::Instant::now();
    while start.elapsed().as_millis() < timeout_ms as u128 {
        if std::path::Path::new(path).exists() {
            // Try connecting
            if let Ok(stream) = UnixStream::connect(path) {
                return Some(stream);
            }
        }
        thread::sleep(Duration::from_millis(50));
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Test that the binary starts and accepts IPC connection
    #[test]
    fn test_binary_starts_and_accepts_connection() {
        let socket_path = format!("/tmp/e2e-test-{}.sock", std::process::id());

        // Clean up old socket
        let _ = std::fs::remove_file(&socket_path);

        // Spawn overlay
        let mut child = spawn_overlay(&socket_path, 11700);

        // Wait for socket and connect (binary only accepts one connection)
        let mut stream = wait_for_socket(&socket_path, 5000).expect("Socket should be ready");
        stream
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();

        // Send shutdown
        ipc::send_message(&mut stream, ipc::SHUTDOWN, &[]).expect("Should send shutdown");
        drop(stream);

        // Wait for process to exit
        let _status = wait_for_child_exit(&mut child, "overlay");
        // Process exits with 0 on shutdown

        // Cleanup
        let _ = std::fs::remove_file(&socket_path);

        println!("✓ Binary starts and accepts IPC connection");
    }

    /// Test that SCP broadcast works through the actual binary
    #[test]
    fn test_binary_scp_broadcast() {
        let socket_path = format!("/tmp/e2e-scp-{}.sock", std::process::id());
        let _ = std::fs::remove_file(&socket_path);

        // Spawn overlay
        let mut child = spawn_overlay(&socket_path, 11701);
        let mut stream = wait_for_socket(&socket_path, 5000).expect("Socket should be ready");
        stream
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();

        // Send SCP broadcast (overlay should accept it even with no peers)
        let scp_envelope = valid_scp_envelope_xdr(1);
        ipc::send_message(&mut stream, ipc::BROADCAST_SCP, &scp_envelope).expect("Should send SCP");

        // Give it time to process
        thread::sleep(Duration::from_millis(100));

        // Shutdown
        ipc::send_message(&mut stream, ipc::SHUTDOWN, &[]).expect("Should send shutdown");
        drop(stream);
        wait_for_child_exit(&mut child, "overlay");

        let _ = std::fs::remove_file(&socket_path);

        println!("✓ Binary accepts SCP broadcast");
    }

    /// Test two overlay binaries can connect and relay SCP messages
    #[test]
    fn test_two_binaries_relay_scp() {
        let socket_a = format!("/tmp/e2e-relay-a-{}.sock", std::process::id());
        let socket_b = format!("/tmp/e2e-relay-b-{}.sock", std::process::id());
        let _ = std::fs::remove_file(&socket_a);
        let _ = std::fs::remove_file(&socket_b);

        // Spawn two overlays on different ports
        let mut child_a = spawn_overlay(&socket_a, 11710);
        let mut child_b = spawn_overlay(&socket_b, 11711);

        let mut stream_a = wait_for_socket(&socket_a, 5000).expect("Socket A should be ready");
        let mut stream_b = wait_for_socket(&socket_b, 5000).expect("Socket B should be ready");
        stream_a
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        stream_b
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        stream_b.set_nonblocking(true).unwrap(); // Non-blocking for recv check

        // Tell B to connect to A's peer port
        let peer_config =
            r#"{"known_peers":["127.0.0.1:11710"],"preferred_peers":[],"listen_port":11711}"#;
        ipc::send_message(&mut stream_b, ipc::SET_PEER_CONFIG, peer_config.as_bytes())
            .expect("Should send peer config");

        // Wait for connection to establish
        thread::sleep(Duration::from_millis(500));

        // A broadcasts SCP
        let scp_envelope = valid_scp_envelope_xdr(2);

        ipc::send_message(&mut stream_a, ipc::BROADCAST_SCP, &scp_envelope)
            .expect("Should send SCP from A");

        // Wait for relay
        thread::sleep(Duration::from_millis(500));

        // B should receive SCP_RECEIVED from its overlay
        // When B connects to A, B sends PeerRequestsScpState to ask Core for SCP state
        // Then B should receive the relayed SCP from A
        stream_b.set_nonblocking(false).unwrap();
        stream_b
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();

        let mut scp_state_requests = 0;
        let mut result = Err("No SCP_RECEIVED message".to_string());
        for _ in 0..5 {
            match ipc::recv_message(&mut stream_b) {
                Ok((msg_type, payload)) => {
                    if msg_type == ipc::SCP_RECEIVED {
                        result = Ok((msg_type, payload));
                        break;
                    } else if msg_type == ipc::PEER_REQUESTS_SCP_STATE {
                        scp_state_requests += 1;
                    }
                }
                Err(e) => {
                    result = Err(e.to_string());
                    break;
                }
            }
        }

        assert_eq!(
            scp_state_requests, 1,
            "B should request SCP state once when connecting to A"
        );

        // Shutdown both
        ipc::send_message(&mut stream_a, ipc::SHUTDOWN, &[]).ok();
        ipc::send_message(&mut stream_b, ipc::SHUTDOWN, &[]).ok();
        drop(stream_a);
        drop(stream_b);
        wait_for_child_exit(&mut child_a, "overlay A");
        wait_for_child_exit(&mut child_b, "overlay B");

        let _ = std::fs::remove_file(&socket_a);
        let _ = std::fs::remove_file(&socket_b);

        // Verify B received the SCP message
        match result {
            Ok((msg_type, payload)) => {
                assert_eq!(
                    msg_type,
                    ipc::SCP_RECEIVED,
                    "Should receive SCP_RECEIVED message"
                );
                assert_eq!(payload, scp_envelope, "Payload should match");
                println!("✓ Two binaries can relay SCP messages!");
            }
            Err(e) => {
                panic!("B did not receive SCP message from A: {}. This indicates peer connection or relay failed.", e);
            }
        }
    }

    /// The leader's Core hands its set to its overlay and asks for the push
    /// (CACHE_TX_SET, PUSH_TX_SET). The follower's overlay then holds the
    /// set before its Core knows the hash: Core's later request is answered
    /// from the pushed copy, without any network request.
    #[test]
    fn test_leader_push_delivers_txset_before_the_request() {
        let mut net = TwoOverlays::start("push", 11720, 11721);
        let (hash, xdr) = txset_xdr(0x61);

        let mut cache = txset_key(&hash, 5);
        cache.extend_from_slice(&xdr);
        ipc::send_message(&mut net.core_a, ipc::CACHE_TX_SET, &cache).unwrap();
        ipc::send_message(&mut net.core_a, ipc::PUSH_TX_SET, &txset_key(&hash, 5)).unwrap();

        // The pushed set reaches B's overlay but not B's Core.
        let deadline = Instant::now() + Duration::from_secs(10);
        while overlay_metric(&mut net.core_b, "txset_push_received") == 0 {
            assert!(Instant::now() < deadline, "push did not arrive");
            thread::sleep(Duration::from_millis(50));
        }
        assert!(
            recv_until(
                &mut net.core_b,
                ipc::TX_SET_AVAILABLE,
                Duration::from_millis(300)
            )
            .is_none(),
            "Core must not receive a set it did not ask for"
        );

        // B's Core sees the proposal and asks for the hash.
        ipc::send_message(&mut net.core_b, ipc::REQUEST_TX_SET, &txset_key(&hash, 5)).unwrap();
        let payload = recv_until(
            &mut net.core_b,
            ipc::TX_SET_AVAILABLE,
            Duration::from_secs(5),
        )
        .expect("requested set");
        assert_eq!(&payload[..32], &hash);
        assert_eq!(&payload[32..], &xdr[..]);

        assert_eq!(overlay_metric(&mut net.core_a, "txset_push_sent"), 1);
        assert_eq!(overlay_metric(&mut net.core_b, "txset_push_received"), 1);
        assert_eq!(overlay_metric(&mut net.core_b, "txset_push_hit"), 1);
        // No request went over the network.
        assert_eq!(overlay_metric(&mut net.core_b, "fetch_txset_count"), 0);
        net.stop();
    }

    /// Fallback: the leader's overlay holds the set but the push never
    /// happened (lost). The follower's request fetches it from the leader.
    #[test]
    fn test_txset_request_path_works_without_a_push() {
        let mut net = TwoOverlays::start("nopush", 11730, 11731);
        let (hash, xdr) = txset_xdr(0x62);

        let mut cache = txset_key(&hash, 5);
        cache.extend_from_slice(&xdr);
        ipc::send_message(&mut net.core_a, ipc::CACHE_TX_SET, &cache).unwrap();
        thread::sleep(Duration::from_millis(300));
        assert_eq!(overlay_metric(&mut net.core_b, "txset_push_received"), 0);

        ipc::send_message(&mut net.core_b, ipc::REQUEST_TX_SET, &txset_key(&hash, 5)).unwrap();
        let payload = recv_until(
            &mut net.core_b,
            ipc::TX_SET_AVAILABLE,
            Duration::from_secs(5),
        )
        .expect("requested set");
        assert_eq!(&payload[..32], &hash);
        assert_eq!(&payload[32..], &xdr[..]);

        assert_eq!(overlay_metric(&mut net.core_a, "txset_push_sent"), 0);
        assert_eq!(overlay_metric(&mut net.core_b, "txset_push_hit"), 0);
        assert_eq!(overlay_metric(&mut net.core_b, "fetch_txset_count"), 1);
        net.stop();
    }

    /// The leader pushes set A but its proposal names B. The follower's
    /// request for B is not answered with A: it is fetched from the leader,
    /// which still answers requests for sets it did not push.
    #[test]
    fn test_pushed_txset_does_not_answer_a_request_for_another_hash() {
        let mut net = TwoOverlays::start("mismatch", 11740, 11741);
        let (pushed_hash, pushed_xdr) = txset_xdr(0x63);
        let (proposed_hash, proposed_xdr) = txset_xdr(0x64);

        for (hash, xdr) in [(&pushed_hash, &pushed_xdr), (&proposed_hash, &proposed_xdr)] {
            let mut cache = txset_key(hash, 5);
            cache.extend_from_slice(xdr);
            ipc::send_message(&mut net.core_a, ipc::CACHE_TX_SET, &cache).unwrap();
        }
        ipc::send_message(
            &mut net.core_a,
            ipc::PUSH_TX_SET,
            &txset_key(&pushed_hash, 5),
        )
        .unwrap();
        let deadline = Instant::now() + Duration::from_secs(10);
        while overlay_metric(&mut net.core_b, "txset_push_received") == 0 {
            assert!(Instant::now() < deadline, "push did not arrive");
            thread::sleep(Duration::from_millis(50));
        }

        ipc::send_message(
            &mut net.core_b,
            ipc::REQUEST_TX_SET,
            &txset_key(&proposed_hash, 5),
        )
        .unwrap();
        let payload = recv_until(
            &mut net.core_b,
            ipc::TX_SET_AVAILABLE,
            Duration::from_secs(5),
        )
        .expect("requested set");
        assert_eq!(&payload[..32], &proposed_hash);
        assert_eq!(&payload[32..], &proposed_xdr[..]);
        // Only the requested set was delivered.
        assert!(recv_until(
            &mut net.core_b,
            ipc::TX_SET_AVAILABLE,
            Duration::from_millis(300)
        )
        .is_none());

        assert_eq!(overlay_metric(&mut net.core_b, "txset_push_hit"), 0);
        assert_eq!(overlay_metric(&mut net.core_b, "fetch_txset_count"), 1);
        assert_eq!(
            overlay_metric(&mut net.core_a, "txset_push_request_skipped"),
            0
        );
        net.stop();
    }

    /// A push that the receiver drops (here: by its per-peer rate limit and
    /// by its limit on sets Core has not asked for) is recovered through the
    /// request path: when the follower's Core later asks for such a set, the
    /// leader answers although it had pushed it. Every set arrives.
    #[test]
    fn test_dropped_push_is_recovered_by_a_request() {
        let mut net = TwoOverlays::start("dropped", 11750, 11751);
        // More unrequested sets in a burst than the follower accepts.
        let sets: Vec<([u8; 32], Vec<u8>)> = (0..14).map(|i| txset_xdr(0x70 + i)).collect();
        for (hash, xdr) in &sets {
            let mut cache = txset_key(hash, 5);
            cache.extend_from_slice(xdr);
            ipc::send_message(&mut net.core_a, ipc::CACHE_TX_SET, &cache).unwrap();
            ipc::send_message(&mut net.core_a, ipc::PUSH_TX_SET, &txset_key(hash, 5)).unwrap();
            thread::sleep(Duration::from_millis(15));
        }
        // Later than any request could have crossed a push.
        thread::sleep(Duration::from_millis(800));
        let pushed = overlay_metric(&mut net.core_a, "txset_push_sent");
        let received = overlay_metric(&mut net.core_b, "txset_push_received");
        let dropped = overlay_metric(&mut net.core_b, "txset_push_dropped");
        assert!(dropped >= 1, "the burst should exceed the follower's limit");
        assert_eq!(received + dropped, pushed);

        for (hash, xdr) in &sets {
            ipc::send_message(&mut net.core_b, ipc::REQUEST_TX_SET, &txset_key(hash, 5)).unwrap();
            let payload = recv_until(
                &mut net.core_b,
                ipc::TX_SET_AVAILABLE,
                Duration::from_secs(5),
            )
            .expect("requested set");
            assert_eq!(&payload[..32], hash);
            assert_eq!(&payload[32..], &xdr[..]);
        }
        // Of the sets that arrived, the follower kept only as many as one
        // peer may have unclaimed; the older ones were evicted.
        let evicted = overlay_metric(&mut net.core_b, "txset_push_evicted");
        assert_eq!(
            received - evicted,
            stellar_overlay::flood::txset_push_limits::MAX_UNCLAIMED_SETS_PER_PEER as u64
        );
        // Every set the follower did not hold (dropped by the rate limit,
        // evicted, or not pushed because an earlier push was still in
        // flight) came through a request, which the leader answered.
        assert_eq!(
            overlay_metric(&mut net.core_b, "fetch_txset_count"),
            sets.len() as u64 - (received - evicted)
        );
        assert_eq!(
            overlay_metric(&mut net.core_b, "txset_push_hit"),
            received - evicted
        );
        assert_eq!(
            overlay_metric(&mut net.core_a, "txset_push_request_skipped"),
            0
        );
        net.stop();
    }
}
