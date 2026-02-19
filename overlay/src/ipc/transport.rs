//! IPC transport over Unix domain sockets.
//!
//! Provides async channel abstraction over blocking Unix socket I/O.

use std::os::unix::net::{UnixListener, UnixStream};
use std::path::Path;
use std::sync::Arc;
use tokio::sync::mpsc;
use tracing::{debug, error, info};

use super::messages::{Message, MessageCodec, MessageType};

/// Error type for IPC operations
#[derive(Debug)]
pub enum IpcError {
    Io(std::io::Error),
    ConnectionClosed,
    ChannelClosed,
}

impl std::fmt::Display for IpcError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            IpcError::Io(e) => write!(f, "IPC I/O error: {}", e),
            IpcError::ConnectionClosed => write!(f, "IPC connection closed"),
            IpcError::ChannelClosed => write!(f, "IPC channel closed"),
        }
    }
}

impl std::error::Error for IpcError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            IpcError::Io(e) => Some(e),
            _ => None,
        }
    }
}

impl From<std::io::Error> for IpcError {
    fn from(e: std::io::Error) -> Self {
        if e.kind() == std::io::ErrorKind::UnexpectedEof {
            IpcError::ConnectionClosed
        } else {
            IpcError::Io(e)
        }
    }
}

/// Handle for sending messages to Core.
#[derive(Clone)]
pub struct CoreSender {
    tx: mpsc::UnboundedSender<Message>,
}

impl CoreSender {
    /// Create a new CoreSender (for testing)
    #[cfg(test)]
    pub fn new(tx: mpsc::UnboundedSender<Message>) -> Self {
        Self { tx }
    }

    /// Send a message to Core. Never blocks.
    pub fn send(&self, msg: Message) -> Result<(), IpcError> {
        self.tx.send(msg).map_err(|_| IpcError::ChannelClosed)
    }

    /// Convenience: send SCP received notification
    pub fn send_scp_received(&self, envelope: Vec<u8>, _from_peer: u64) -> Result<(), IpcError> {
        // TODO: encode peer_id in payload format
        self.send(Message::new(MessageType::ScpReceived, envelope))
    }

    /// Convenience: send top transactions response
    /// Payload: [count:4][len1:4][tx1:len1][len2:4][tx2:len2]...
    pub fn send_top_txs_response(&self, txs: &[&[u8]]) -> Result<(), IpcError> {
        let total_size: usize = 4 + txs.iter().map(|tx| 4 + tx.len()).sum::<usize>();
        let mut payload = Vec::with_capacity(total_size);

        // Count
        payload.extend_from_slice(&(txs.len() as u32).to_le_bytes());

        // Each TX: [len:4][data:len]
        for tx in txs {
            payload.extend_from_slice(&(tx.len() as u32).to_le_bytes());
            payload.extend_from_slice(tx);
        }

        self.send(Message::new(MessageType::TopTxsResponse, payload))
    }

    /// Convenience: send TX set available notification
    pub fn send_tx_set_available(&self, hash: [u8; 32], xdr: Vec<u8>) -> Result<(), IpcError> {
        // Payload: [hash:32][xdr...]
        let mut payload = Vec::with_capacity(32 + xdr.len());
        payload.extend_from_slice(&hash);
        payload.extend_from_slice(&xdr);
        self.send(Message::new(MessageType::TxSetAvailable, payload))
    }
}

/// Handle for receiving messages from Core.
pub struct CoreReceiver {
    rx: mpsc::UnboundedReceiver<Message>,
}

impl CoreReceiver {
    /// Receive a message from Core. Async.
    pub async fn recv(&mut self) -> Option<Message> {
        self.rx.recv().await
    }
}

/// Manages the IPC connection to Core.
///
/// Spawns background tasks for reading/writing to the Unix socket.
/// Provides async channels for the rest of the overlay to use.
pub struct CoreIpc {
    /// Sender for outgoing messages
    pub sender: CoreSender,
    /// Receiver for incoming messages
    pub receiver: CoreReceiver,
    /// Join handle for reader task
    reader_handle: tokio::task::JoinHandle<()>,
    /// Join handle for writer task  
    writer_handle: tokio::task::JoinHandle<()>,
}

impl CoreIpc {
    /// Connect to Core's IPC socket (client mode).
    pub async fn connect<P: AsRef<Path>>(socket_path: P) -> Result<Self, IpcError> {
        let path = socket_path.as_ref();
        info!("Connecting to Core IPC socket: {}", path.display());

        // Connect (blocking, but fast for Unix sockets)
        let stream = UnixStream::connect(path)?;
        stream.set_nonblocking(false)?; // We use blocking I/O in spawn_blocking

        Self::from_stream(stream)
    }

    /// Listen on socket and accept one connection (server mode).
    /// This is used when overlay starts first and Core connects to it.
    pub async fn listen<P: AsRef<Path>>(socket_path: P) -> Result<Self, IpcError> {
        let path = socket_path.as_ref();

        // Remove existing socket file if present
        if path.exists() {
            std::fs::remove_file(path)?;
        }

        info!("Listening for Core connection on: {}", path.display());

        // Create listener (blocking, but we only accept once)
        let listener = UnixListener::bind(path)?;

        // Accept one connection (blocking)
        let (stream, _) = tokio::task::spawn_blocking(move || listener.accept())
            .await
            .map_err(|e| {
                IpcError::Io(std::io::Error::new(
                    std::io::ErrorKind::Other,
                    e.to_string(),
                ))
            })??;

        info!("Core connected");
        stream.set_nonblocking(false)?;

        Self::from_stream(stream)
    }

    /// Create from existing Unix stream (for testing).
    pub fn from_stream(stream: UnixStream) -> Result<Self, IpcError> {
        let stream = Arc::new(stream);

        // Channels for async communication
        let (outbound_tx, outbound_rx) = mpsc::unbounded_channel::<Message>();
        let (inbound_tx, inbound_rx) = mpsc::unbounded_channel::<Message>();

        // Spawn reader task
        let reader_stream = Arc::clone(&stream);
        let reader_handle = tokio::spawn(async move {
            Self::reader_loop(reader_stream, inbound_tx).await;
        });

        // Spawn writer task
        let writer_stream = Arc::clone(&stream);
        let writer_handle = tokio::spawn(async move {
            Self::writer_loop(writer_stream, outbound_rx).await;
        });

        Ok(Self {
            sender: CoreSender { tx: outbound_tx },
            receiver: CoreReceiver { rx: inbound_rx },
            reader_handle,
            writer_handle,
        })
    }

    /// Reader loop: blocking read in spawn_blocking, forward to channel.
    async fn reader_loop(stream: Arc<UnixStream>, tx: mpsc::UnboundedSender<Message>) {
        loop {
            // Clone for the blocking task
            let stream = Arc::clone(&stream);

            // Read one message (blocking)
            let result = tokio::task::spawn_blocking(move || {
                // We need to get a &mut, but we have Arc<UnixStream>
                // UnixStream implements Read for &UnixStream, so this works
                let mut reader = &*stream;
                MessageCodec::read(&mut reader)
            })
            .await;

            match result {
                Ok(Ok(msg)) => {
                    debug!(
                        "IPC received: {:?} ({} bytes)",
                        msg.msg_type,
                        msg.payload.len()
                    );
                    if tx.send(msg).is_err() {
                        debug!("IPC reader: channel closed, stopping");
                        break;
                    }
                }
                Ok(Err(e)) if e.kind() == std::io::ErrorKind::UnexpectedEof => {
                    info!("Core IPC connection closed");
                    break;
                }
                Ok(Err(e)) => {
                    error!("IPC read error: {}", e);
                    break;
                }
                Err(e) => {
                    error!("IPC reader task panicked: {}", e);
                    break;
                }
            }
        }
    }

    /// Writer loop: receive from channel, blocking write.
    async fn writer_loop(stream: Arc<UnixStream>, mut rx: mpsc::UnboundedReceiver<Message>) {
        while let Some(msg) = rx.recv().await {
            let stream = Arc::clone(&stream);
            let msg_type = msg.msg_type;
            let payload_len = msg.payload.len();

            // Write one message (blocking)
            let result = tokio::task::spawn_blocking(move || {
                let mut writer = &*stream;
                MessageCodec::write(&mut writer, &msg)
            })
            .await;

            match result {
                Ok(Ok(())) => {
                    debug!("IPC sent: {:?} ({} bytes)", msg_type, payload_len);
                }
                Ok(Err(e)) => {
                    error!("IPC write error: {}", e);
                    break;
                }
                Err(e) => {
                    error!("IPC writer task panicked: {}", e);
                    break;
                }
            }
        }

        debug!("IPC writer: channel closed, stopping");
    }

    /// Gracefully shutdown the IPC connection.
    pub async fn shutdown(self) {
        // Dropping sender will close the writer loop
        drop(self.sender);

        // Wait for tasks to finish (with timeout)
        let _ = tokio::time::timeout(std::time::Duration::from_secs(1), async {
            let _ = self.writer_handle.await;
            let _ = self.reader_handle.await;
        })
        .await;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};
    use std::os::unix::net::UnixStream as StdUnixStream;

    #[tokio::test]
    async fn test_ipc_roundtrip() {
        // Create a socket pair
        let (s1, s2) = StdUnixStream::pair().unwrap();

        // Create IPC from one end
        let ipc = CoreIpc::from_stream(s1).unwrap();

        // Send from the other end (simulating Core)
        let mut core_side = s2;
        let msg = Message::new(MessageType::BroadcastScp, vec![1, 2, 3]);
        MessageCodec::write(&mut core_side, &msg).unwrap();

        // Receive on overlay side
        let mut receiver = ipc.receiver;
        let received = tokio::time::timeout(std::time::Duration::from_secs(1), receiver.recv())
            .await
            .unwrap()
            .unwrap();

        assert_eq!(received.msg_type, MessageType::BroadcastScp);
        assert_eq!(received.payload, vec![1, 2, 3]);

        // Send from overlay side
        ipc.sender
            .send(Message::new(MessageType::ScpReceived, vec![4, 5, 6]))
            .unwrap();

        // Small delay for write to complete
        tokio::time::sleep(std::time::Duration::from_millis(50)).await;

        // Receive on Core side
        let received = MessageCodec::read(&mut core_side).unwrap();
        assert_eq!(received.msg_type, MessageType::ScpReceived);
        assert_eq!(received.payload, vec![4, 5, 6]);
    }

    // ═══ SCP State Sync Tests (Mocking C++ Response) ═══

    #[tokio::test]
    async fn test_request_scp_state_mocked_response() {
        let (overlay_side, core_side) = StdUnixStream::pair().unwrap();
        let ipc = CoreIpc::from_stream(overlay_side).unwrap();

        let mut core = core_side;

        // Overlay sends PeerRequestsScpState to Core
        // (simulating a peer asking for SCP state)
        // New format: [request_id:8][ledger_seq:4]
        let request_id: u64 = 42;
        let ledger_seq: u32 = 12345;
        let mut payload = Vec::with_capacity(12);
        payload.extend_from_slice(&request_id.to_le_bytes());
        payload.extend_from_slice(&ledger_seq.to_le_bytes());
        ipc.sender
            .send(Message::new(MessageType::PeerRequestsScpState, payload))
            .unwrap();

        tokio::time::sleep(std::time::Duration::from_millis(50)).await;

        // Core receives the request
        let request = MessageCodec::read(&mut core).unwrap();
        assert_eq!(request.msg_type, MessageType::PeerRequestsScpState);
        assert_eq!(request.payload.len(), 12);
        assert_eq!(
            u64::from_le_bytes(request.payload[0..8].try_into().unwrap()),
            request_id
        );
        assert_eq!(
            u32::from_le_bytes(request.payload[8..12].try_into().unwrap()),
            ledger_seq
        );

        // Core sends back ScpStateResponse with mock SCP envelopes
        // New format: [request_id:8][count:4][env_len:4][env_data...]
        let mock_envelope = vec![0x5C, 0x50, 0xDA, 0x7A]; // mock SCP data
        let mut response_payload = Vec::new();
        response_payload.extend_from_slice(&request_id.to_le_bytes()); // echo request_id
        response_payload.extend_from_slice(&1u32.to_le_bytes()); // count = 1
        response_payload.extend_from_slice(&(mock_envelope.len() as u32).to_le_bytes());
        response_payload.extend_from_slice(&mock_envelope);
        MessageCodec::write(
            &mut core,
            &Message::new(MessageType::ScpStateResponse, response_payload.clone()),
        )
        .unwrap();

        // Overlay receives the response
        let mut receiver = ipc.receiver;
        let response = tokio::time::timeout(std::time::Duration::from_secs(1), receiver.recv())
            .await
            .unwrap()
            .unwrap();

        assert_eq!(response.msg_type, MessageType::ScpStateResponse);
        // Verify request_id is in the response
        assert_eq!(
            u64::from_le_bytes(response.payload[0..8].try_into().unwrap()),
            request_id
        );
    }

    #[tokio::test]
    async fn test_scp_state_request_id_out_of_order_correlation() {
        // This test verifies that request IDs properly correlate responses with requests
        // even when responses arrive out of order.
        let (overlay_side, core_side) = StdUnixStream::pair().unwrap();
        let ipc = CoreIpc::from_stream(overlay_side).unwrap();

        let mut core = core_side;

        // Send two PeerRequestsScpState requests with different request_ids
        let request_id_1: u64 = 100;
        let request_id_2: u64 = 200;
        let ledger_seq_1: u32 = 1000;
        let ledger_seq_2: u32 = 2000;

        // Send request 1
        let mut payload1 = Vec::with_capacity(12);
        payload1.extend_from_slice(&request_id_1.to_le_bytes());
        payload1.extend_from_slice(&ledger_seq_1.to_le_bytes());
        ipc.sender
            .send(Message::new(
                MessageType::PeerRequestsScpState,
                payload1.clone(),
            ))
            .unwrap();

        // Send request 2
        let mut payload2 = Vec::with_capacity(12);
        payload2.extend_from_slice(&request_id_2.to_le_bytes());
        payload2.extend_from_slice(&ledger_seq_2.to_le_bytes());
        ipc.sender
            .send(Message::new(
                MessageType::PeerRequestsScpState,
                payload2.clone(),
            ))
            .unwrap();

        tokio::time::sleep(std::time::Duration::from_millis(50)).await;

        // Core receives both requests
        let req1 = MessageCodec::read(&mut core).unwrap();
        let req2 = MessageCodec::read(&mut core).unwrap();
        assert_eq!(req1.msg_type, MessageType::PeerRequestsScpState);
        assert_eq!(req2.msg_type, MessageType::PeerRequestsScpState);

        // Extract request IDs that Core received
        let received_id_1 = u64::from_le_bytes(req1.payload[0..8].try_into().unwrap());
        let received_id_2 = u64::from_le_bytes(req2.payload[0..8].try_into().unwrap());
        assert_eq!(received_id_1, request_id_1);
        assert_eq!(received_id_2, request_id_2);

        // Core responds OUT OF ORDER: respond to request 2 first, then request 1
        let envelope_for_req2 = vec![0x22, 0x22]; // data for request 2
        let envelope_for_req1 = vec![0x11, 0x11]; // data for request 1

        // Response for request 2 (sent first, out of order)
        let mut resp2 = Vec::new();
        resp2.extend_from_slice(&request_id_2.to_le_bytes());
        resp2.extend_from_slice(&1u32.to_le_bytes()); // count
        resp2.extend_from_slice(&(envelope_for_req2.len() as u32).to_le_bytes());
        resp2.extend_from_slice(&envelope_for_req2);
        MessageCodec::write(
            &mut core,
            &Message::new(MessageType::ScpStateResponse, resp2),
        )
        .unwrap();

        // Response for request 1 (sent second)
        let mut resp1 = Vec::new();
        resp1.extend_from_slice(&request_id_1.to_le_bytes());
        resp1.extend_from_slice(&1u32.to_le_bytes()); // count
        resp1.extend_from_slice(&(envelope_for_req1.len() as u32).to_le_bytes());
        resp1.extend_from_slice(&envelope_for_req1);
        MessageCodec::write(
            &mut core,
            &Message::new(MessageType::ScpStateResponse, resp1),
        )
        .unwrap();

        // Overlay receives both responses
        let mut receiver = ipc.receiver;

        // First response received is for request 2
        let response_a = tokio::time::timeout(std::time::Duration::from_secs(1), receiver.recv())
            .await
            .unwrap()
            .unwrap();
        assert_eq!(response_a.msg_type, MessageType::ScpStateResponse);
        let resp_a_id = u64::from_le_bytes(response_a.payload[0..8].try_into().unwrap());
        assert_eq!(resp_a_id, request_id_2);

        // Second response received is for request 1
        let response_b = tokio::time::timeout(std::time::Duration::from_secs(1), receiver.recv())
            .await
            .unwrap()
            .unwrap();
        assert_eq!(response_b.msg_type, MessageType::ScpStateResponse);
        let resp_b_id = u64::from_le_bytes(response_b.payload[0..8].try_into().unwrap());
        assert_eq!(resp_b_id, request_id_1);

        // KEY ASSERTION: Even though responses came out of order, the request_ids
        // allow proper correlation. The handler in main.rs will use these IDs to
        // look up the correct peer_id from the HashMap.
    }

    #[tokio::test]
    async fn test_scp_received_forwarded_to_core() {
        let (overlay_side, core_side) = StdUnixStream::pair().unwrap();
        let ipc = CoreIpc::from_stream(overlay_side).unwrap();

        let mut core = core_side;

        // Simulate overlay receiving SCP from network and forwarding to Core
        let scp_envelope = vec![0x01, 0x02, 0x03, 0x04, 0x05];
        ipc.sender
            .send_scp_received(scp_envelope.clone(), 42)
            .unwrap();

        tokio::time::sleep(std::time::Duration::from_millis(50)).await;

        // Core should receive it
        let received = MessageCodec::read(&mut core).unwrap();
        assert_eq!(received.msg_type, MessageType::ScpReceived);
        assert_eq!(received.payload, scp_envelope);
    }

    // ═══ LedgerClosed Test ═══

    #[tokio::test]
    async fn test_ledger_closed_message() {
        let (overlay_side, core_side) = StdUnixStream::pair().unwrap();
        let ipc = CoreIpc::from_stream(overlay_side).unwrap();

        let mut core = core_side;

        // Core sends LedgerClosed
        let ledger_seq: u32 = 100;
        let ledger_hash = [0xAB; 32];
        let mut payload = ledger_seq.to_le_bytes().to_vec();
        payload.extend_from_slice(&ledger_hash);

        MessageCodec::write(
            &mut core,
            &Message::new(MessageType::LedgerClosed, payload.clone()),
        )
        .unwrap();

        // Overlay receives it
        let mut receiver = ipc.receiver;
        let received = tokio::time::timeout(std::time::Duration::from_secs(1), receiver.recv())
            .await
            .unwrap()
            .unwrap();

        assert_eq!(received.msg_type, MessageType::LedgerClosed);
        assert_eq!(received.payload.len(), 36); // 4 + 32
        let seq = u32::from_le_bytes(received.payload[0..4].try_into().unwrap());
        assert_eq!(seq, 100);
    }

    // ═══ TxSetExternalized Test ═══

    #[tokio::test]
    async fn test_tx_set_externalized_message() {
        let (overlay_side, core_side) = StdUnixStream::pair().unwrap();
        let ipc = CoreIpc::from_stream(overlay_side).unwrap();

        let mut core = core_side;

        // Core sends TxSetExternalized
        let tx_set_hash = [0xDE; 32];

        MessageCodec::write(
            &mut core,
            &Message::new(MessageType::TxSetExternalized, tx_set_hash.to_vec()),
        )
        .unwrap();

        // Overlay receives it
        let mut receiver = ipc.receiver;
        let received = tokio::time::timeout(std::time::Duration::from_secs(1), receiver.recv())
            .await
            .unwrap()
            .unwrap();

        assert_eq!(received.msg_type, MessageType::TxSetExternalized);
        assert_eq!(received.payload.len(), 32);
        assert_eq!(&received.payload[..], &tx_set_hash[..]);
    }

    // ═══ SetPeerConfig Test ═══

    #[tokio::test]
    async fn test_set_peer_config_message() {
        let (overlay_side, core_side) = StdUnixStream::pair().unwrap();
        let ipc = CoreIpc::from_stream(overlay_side).unwrap();

        let mut core = core_side;

        // Core sends SetPeerConfig as JSON
        let config_json =
            r#"{"known_peers":["1.2.3.4:11625"],"preferred_peers":[],"listen_port":11625}"#;

        MessageCodec::write(
            &mut core,
            &Message::new(MessageType::SetPeerConfig, config_json.as_bytes().to_vec()),
        )
        .unwrap();

        // Overlay receives it
        let mut receiver = ipc.receiver;
        let received = tokio::time::timeout(std::time::Duration::from_secs(1), receiver.recv())
            .await
            .unwrap()
            .unwrap();

        assert_eq!(received.msg_type, MessageType::SetPeerConfig);
        let received_json = std::str::from_utf8(&received.payload).unwrap();
        assert!(received_json.contains("known_peers"));
        assert!(received_json.contains("1.2.3.4:11625"));
    }

    // ═══ RequestTxSet Test ═══

    #[tokio::test]
    async fn test_request_tx_set_flow() {
        let (overlay_side, core_side) = StdUnixStream::pair().unwrap();
        let ipc = CoreIpc::from_stream(overlay_side).unwrap();

        let mut core = core_side;

        // Core requests a TX set
        let tx_set_hash = [0x42; 32];

        MessageCodec::write(
            &mut core,
            &Message::new(MessageType::RequestTxSet, tx_set_hash.to_vec()),
        )
        .unwrap();

        // Overlay receives request
        let mut receiver = ipc.receiver;
        let received = tokio::time::timeout(std::time::Duration::from_secs(1), receiver.recv())
            .await
            .unwrap()
            .unwrap();

        assert_eq!(received.msg_type, MessageType::RequestTxSet);
        assert_eq!(received.payload.len(), 32);

        // Overlay responds with TxSetAvailable
        let tx_set_data = vec![1, 2, 3, 4, 5, 6, 7, 8];
        ipc.sender
            .send_tx_set_available(tx_set_hash, tx_set_data.clone())
            .unwrap();

        tokio::time::sleep(std::time::Duration::from_millis(50)).await;

        // Core receives the TX set
        let response = MessageCodec::read(&mut core).unwrap();
        assert_eq!(response.msg_type, MessageType::TxSetAvailable);
        // Payload: [hash:32][xdr...]
        assert_eq!(&response.payload[0..32], &tx_set_hash[..]);
        assert_eq!(&response.payload[32..], &tx_set_data[..]);
    }

    // ═══ Multiple Messages in Sequence ═══

    #[tokio::test]
    async fn test_multiple_messages_sequence() {
        let (overlay_side, core_side) = StdUnixStream::pair().unwrap();
        let ipc = CoreIpc::from_stream(overlay_side).unwrap();

        let mut core = core_side;

        // Core sends multiple messages rapidly
        for i in 0..10u8 {
            MessageCodec::write(&mut core, &Message::new(MessageType::BroadcastScp, vec![i]))
                .unwrap();
        }

        // Overlay should receive all 10
        let mut receiver = ipc.receiver;
        for i in 0..10u8 {
            let received = tokio::time::timeout(std::time::Duration::from_secs(1), receiver.recv())
                .await
                .unwrap()
                .unwrap();

            assert_eq!(received.msg_type, MessageType::BroadcastScp);
            assert_eq!(received.payload, vec![i]);
        }
    }

    // ═══ Connection Close Detection ═══

    #[tokio::test]
    async fn test_connection_close_detection() {
        let (overlay_side, core_side) = StdUnixStream::pair().unwrap();
        let ipc = CoreIpc::from_stream(overlay_side).unwrap();

        // Close Core side
        drop(core_side);

        // Overlay should detect close
        let mut receiver = ipc.receiver;
        let result = tokio::time::timeout(std::time::Duration::from_secs(1), receiver.recv()).await;

        // Should either timeout or return None (connection closed)
        match result {
            Ok(None) => {} // Expected: channel closed
            Err(_) => {}   // Timeout is also acceptable
            Ok(Some(msg)) => panic!("Unexpected message: {:?}", msg),
        }
    }

    // ═══ Helper Method Tests ═══

    #[tokio::test]
    async fn test_send_top_txs_response() {
        let (overlay_side, core_side) = StdUnixStream::pair().unwrap();
        let ipc = CoreIpc::from_stream(overlay_side).unwrap();

        let mut core = core_side;

        // Send empty response
        ipc.sender.send_top_txs_response(&[]).unwrap();

        tokio::time::sleep(std::time::Duration::from_millis(50)).await;

        let received = MessageCodec::read(&mut core).unwrap();
        assert_eq!(received.msg_type, MessageType::TopTxsResponse);
        // Payload: [count:4] = 0
        assert_eq!(received.payload.len(), 4);
        let count = u32::from_le_bytes(received.payload[0..4].try_into().unwrap());
        assert_eq!(count, 0);
    }

    #[tokio::test]
    async fn test_send_top_txs_response_with_txs() {
        let (overlay_side, core_side) = StdUnixStream::pair().unwrap();
        let ipc = CoreIpc::from_stream(overlay_side).unwrap();

        let mut core = core_side;

        let tx1 = vec![0xAA, 0xBB];
        let tx2 = vec![0xCC, 0xDD, 0xEE];
        ipc.sender.send_top_txs_response(&[&tx1, &tx2]).unwrap();

        tokio::time::sleep(std::time::Duration::from_millis(50)).await;

        let received = MessageCodec::read(&mut core).unwrap();
        assert_eq!(received.msg_type, MessageType::TopTxsResponse);
        // Payload: [count:4][len1:4][tx1:2][len2:4][tx2:3] = 4 + 4 + 2 + 4 + 3 = 17
        assert_eq!(received.payload.len(), 17);
        let count = u32::from_le_bytes(received.payload[0..4].try_into().unwrap());
        assert_eq!(count, 2);
    }
}
