//! End-to-end tests that spawn the actual stellar-overlay binary.
//!
//! These tests verify that the compiled binary works correctly,
//! catching issues that in-memory tests might miss (like main.rs wiring bugs).

use std::os::unix::net::UnixStream;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::thread;
use std::time::Duration;

/// IPC message types (must match src/ipc/messages.rs)
mod ipc {
    use std::io::{Read, Write};
    use std::os::unix::net::UnixStream;

    pub const BROADCAST_SCP: u32 = 1;
    pub const SET_PEER_CONFIG: u32 = 8;
    pub const SHUTDOWN: u32 = 7;
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
    // The test runs from the overlay directory, so look in parent's target
    let release = PathBuf::from("../target/release/stellar-overlay");
    if release.exists() {
        return release;
    }
    let release2 = PathBuf::from("target/release/stellar-overlay");
    if release2.exists() {
        return release2;
    }
    let debug = PathBuf::from("../target/debug/stellar-overlay");
    if debug.exists() {
        return debug;
    }
    let debug2 = PathBuf::from("target/debug/stellar-overlay");
    if debug2.exists() {
        return debug2;
    }

    // Try manifest dir
    if let Ok(manifest) = std::env::var("CARGO_MANIFEST_DIR") {
        let release = PathBuf::from(&manifest).join("../target/release/stellar-overlay");
        if release.exists() {
            return release;
        }
        let debug = PathBuf::from(&manifest).join("../target/debug/stellar-overlay");
        if debug.exists() {
            return debug;
        }
    }

    panic!("stellar-overlay binary not found. Run `cargo build --release` first.");
}

/// Spawn an overlay process
fn spawn_overlay(socket_path: &str, peer_port: u16) -> Child {
    let binary = find_binary();

    Command::new(binary)
        .arg("--listen")
        .arg(socket_path)
        .arg("--peer-port")
        .arg(peer_port.to_string())
        .env("RUST_LOG", "debug")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("Failed to spawn overlay process")
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

        // Wait for process to exit
        let _status = child.wait().expect("Should wait");
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
        let scp_envelope = vec![0u8; 100]; // Mock SCP envelope
        ipc::send_message(&mut stream, ipc::BROADCAST_SCP, &scp_envelope).expect("Should send SCP");

        // Give it time to process
        thread::sleep(Duration::from_millis(100));

        // Shutdown
        ipc::send_message(&mut stream, ipc::SHUTDOWN, &[]).expect("Should send shutdown");
        child.wait().expect("Should wait");

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
        let mut scp_envelope = vec![0u8; 100];
        scp_envelope[3] = 10; // SCP_MESSAGE discriminant
        scp_envelope[10..20].copy_from_slice(b"test12345!");

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
        child_a.wait().ok();
        child_b.wait().ok();

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
}
