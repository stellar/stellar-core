//! Mempool manager that handles transaction storage and TX set building.
//!
//! Network communication is handled by the libp2p QUIC overlay.
//! This module provides:
//! - Transaction mempool (fee-ordered, with dedup)
//! - TX set caching for consensus
//! - Core command handling for mempool operations

use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Duration;
use tokio::sync::{mpsc, RwLock};
use tracing::{debug, info, trace};

use crate::flood::{compute_tx_hash, Mempool};

/// Peer ID type
pub type PeerId = u64;

/// Commands from Core to Overlay
#[derive(Debug, Clone)]
pub enum CoreCommand {
    /// Broadcast SCP envelope to all peers (handled by libp2p)
    BroadcastScp { envelope: Vec<u8> },

    /// Submit a transaction for flooding
    SubmitTx {
        data: Vec<u8>,
        fee: u64,
        num_ops: u32,
    },

    /// Request top N transactions by fee
    GetTopTxs {
        count: usize,
        reply: mpsc::Sender<Vec<([u8; 32], Vec<u8>)>>,
    },

    /// Configure bootstrap peers for Kademlia
    SetPeerConfig {
        known_peers: Vec<String>,
        preferred_peers: Vec<String>,
        listen_port: u16,
    },

    /// Remove transactions from mempool (after ledger close)
    RemoveTxsFromMempool {
        tx_hashes: Vec<[u8; 32]>,
        reply: Option<mpsc::Sender<()>>,
    },

    /// Fetch a TX set from peers by hash (libp2p handles network)
    FetchTxSet {
        hash: [u8; 32],
        reply: mpsc::Sender<Option<Vec<u8>>>,
    },

    /// Cache a locally-built TX set
    CacheTxSet { hash: [u8; 32], xdr: Vec<u8> },
}

/// Events from Overlay to Core
#[derive(Debug, Clone)]
pub enum OverlayEvent {
    /// SCP envelope received from a peer
    ScpReceived {
        envelope: Vec<u8>,
        from_peer: PeerId,
    },

    /// Peer connected
    PeerConnected {
        peer_id: PeerId,
        addr: SocketAddr,
        public_key: [u8; 32],
    },

    /// Peer disconnected
    PeerDisconnected { peer_id: PeerId },
}

/// Mempool manager (no longer handles network connections).
pub struct Overlay {
    /// Commands from Core
    core_commands: mpsc::UnboundedReceiver<CoreCommand>,

    /// TX mempool
    mempool: Arc<RwLock<Mempool>>,

    /// Local TX set cache (hash -> XDR)
    local_tx_sets: Arc<RwLock<HashMap<[u8; 32], Vec<u8>>>>,
}

impl Overlay {
    /// Create a new mempool manager.
    pub fn new(core_commands: mpsc::UnboundedReceiver<CoreCommand>) -> Self {
        Self {
            core_commands,
            mempool: Arc::new(RwLock::new(Mempool::new(100000, Duration::from_secs(300)))),
            local_tx_sets: Arc::new(RwLock::new(HashMap::new())),
        }
    }

    /// Run the mempool manager.
    pub async fn run(mut self) -> std::io::Result<()> {
        info!("Mempool manager started (libp2p handles networking)");

        while let Some(cmd) = self.core_commands.recv().await {
            self.handle_core_command(cmd).await;
        }

        info!("Mempool manager shutting down");
        Ok(())
    }

    /// Handle a command from Core.
    async fn handle_core_command(&self, cmd: CoreCommand) {
        match cmd {
            CoreCommand::BroadcastScp { .. } => {
                trace!("BroadcastScp ignored (handled by libp2p)");
            }

            CoreCommand::SubmitTx { data, fee, num_ops } => {
                let hash = compute_tx_hash(&data);
                debug!(
                    "[SubmitTx] TX: hash={:?}, size={}, fee={}, ops={}",
                    &hash[..4],
                    data.len(),
                    fee,
                    num_ops
                );

                // TODO: Parse XDR to extract source_account and sequence instead of zeros
                // This breaks:
                // 1. Account-based TX ordering in mempool
                // 2. Per-account TX queries
                // 3. Sequence number validation
                // Need to parse TransactionEnvelope.tx.sourceAccount and seqNum from XDR
                let mut mempool = self.mempool.write().await;
                let entry = crate::flood::TxEntry {
                    data,
                    hash,
                    source_account: [0u8; 32], // TODO: Parse from XDR
                    sequence: 0,               // TODO: Parse from XDR
                    fee,
                    num_ops,
                    received_at: std::time::Instant::now(),
                    from_peer: 0,
                };
                mempool.insert(entry);
            }

            CoreCommand::GetTopTxs { count, reply } => {
                let mempool = self.mempool.read().await;
                let top_hashes = mempool.top_by_fee(count);
                let txs: Vec<([u8; 32], Vec<u8>)> = top_hashes
                    .iter()
                    .filter_map(|h| mempool.get(h).map(|e| (*h, e.data.clone())))
                    .collect();
                let _ = reply.send(txs).await;
            }

            CoreCommand::SetPeerConfig { .. } => {
                trace!("SetPeerConfig ignored (handled by libp2p)");
            }

            CoreCommand::RemoveTxsFromMempool { tx_hashes, reply } => {
                let mut mempool = self.mempool.write().await;
                let count = tx_hashes.len();
                for hash in tx_hashes {
                    mempool.remove(&hash);
                }
                info!("Removed {} TXs from mempool", count);
                // Signal completion if caller is waiting
                if let Some(tx) = reply {
                    let _ = tx.send(()).await;
                }
            }

            CoreCommand::FetchTxSet { hash, reply } => {
                let cache = self.local_tx_sets.read().await;
                if let Some(xdr) = cache.get(&hash) {
                    let _ = reply.send(Some(xdr.clone())).await;
                } else {
                    let _ = reply.send(None).await;
                }
            }

            CoreCommand::CacheTxSet { hash, xdr } => {
                info!("Caching TX set {:?} ({} bytes)", &hash[..4], xdr.len());
                let mut cache = self.local_tx_sets.write().await;
                cache.insert(hash, xdr);
            }
        }
    }

    /// Get mempool reference (for testing)
    pub fn mempool(&self) -> &Arc<RwLock<Mempool>> {
        &self.mempool
    }

    /// Get TX set cache reference (for testing)
    pub fn tx_set_cache(&self) -> &Arc<RwLock<HashMap<[u8; 32], Vec<u8>>>> {
        &self.local_tx_sets
    }
}

/// Handle for sending commands to the mempool manager.
#[derive(Clone)]
pub struct OverlayHandle {
    cmd_tx: mpsc::UnboundedSender<CoreCommand>,
}

impl OverlayHandle {
    /// Create a new handle.
    pub fn new(cmd_tx: mpsc::UnboundedSender<CoreCommand>) -> Self {
        Self { cmd_tx }
    }

    /// Submit a transaction.
    pub fn submit_tx(&self, data: Vec<u8>, fee: u64, num_ops: u32) {
        let _ = self
            .cmd_tx
            .send(CoreCommand::SubmitTx { data, fee, num_ops });
    }

    /// Get top transactions by fee.
    pub async fn get_top_txs(&self, count: usize) -> Vec<([u8; 32], Vec<u8>)> {
        let (reply_tx, mut reply_rx) = mpsc::channel(1);
        let _ = self.cmd_tx.send(CoreCommand::GetTopTxs {
            count,
            reply: reply_tx,
        });
        reply_rx.recv().await.unwrap_or_default()
    }

    /// Remove transactions from mempool (fire-and-forget).
    pub fn remove_txs(&self, tx_hashes: Vec<[u8; 32]>) {
        let _ = self.cmd_tx.send(CoreCommand::RemoveTxsFromMempool {
            tx_hashes,
            reply: None,
        });
    }

    /// Remove transactions from mempool and wait for completion.
    /// This prevents race conditions where GetTopTxs queries stale data.
    pub async fn remove_txs_sync(&self, tx_hashes: Vec<[u8; 32]>) {
        let (reply_tx, mut reply_rx) = mpsc::channel(1);
        let _ = self.cmd_tx.send(CoreCommand::RemoveTxsFromMempool {
            tx_hashes,
            reply: Some(reply_tx),
        });
        let _ = reply_rx.recv().await;
    }

    /// Cache a TX set.
    pub fn cache_tx_set(&self, hash: [u8; 32], xdr: Vec<u8>) {
        let _ = self.cmd_tx.send(CoreCommand::CacheTxSet { hash, xdr });
    }

    /// Fetch a TX set from cache.
    pub async fn fetch_tx_set(&self, hash: [u8; 32]) -> Option<Vec<u8>> {
        let (reply_tx, mut reply_rx) = mpsc::channel(1);
        let _ = self.cmd_tx.send(CoreCommand::FetchTxSet {
            hash,
            reply: reply_tx,
        });
        reply_rx.recv().await.flatten()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn test_submit_tx_adds_to_mempool() {
        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
        let overlay = Overlay::new(cmd_rx);
        let handle = OverlayHandle::new(cmd_tx);

        // Start overlay in background
        let mempool = overlay.mempool.clone();
        tokio::spawn(async move {
            let _ = overlay.run().await;
        });

        // Submit a TX
        handle.submit_tx(vec![1, 2, 3], 100, 1);
        tokio::time::sleep(Duration::from_millis(50)).await;

        // Verify it's in mempool
        let mp = mempool.read().await;
        assert_eq!(mp.len(), 1);
    }

    #[tokio::test]
    async fn test_get_top_txs() {
        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
        let overlay = Overlay::new(cmd_rx);
        let handle = OverlayHandle::new(cmd_tx);

        tokio::spawn(async move {
            let _ = overlay.run().await;
        });

        // Submit TXs with different fees
        handle.submit_tx(vec![1], 100, 1);
        handle.submit_tx(vec![2], 500, 1);
        handle.submit_tx(vec![3], 200, 1);
        tokio::time::sleep(Duration::from_millis(50)).await;

        // Get top 2
        let top = handle.get_top_txs(2).await;
        assert_eq!(top.len(), 2);
        // First should be highest fee
        assert_eq!(top[0].1, vec![2]);
    }

    #[tokio::test]
    async fn test_remove_txs() {
        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
        let overlay = Overlay::new(cmd_rx);
        let handle = OverlayHandle::new(cmd_tx);
        let mempool = overlay.mempool.clone();

        tokio::spawn(async move {
            let _ = overlay.run().await;
        });

        // Submit TXs
        handle.submit_tx(vec![1], 100, 1);
        handle.submit_tx(vec![2], 200, 1);
        tokio::time::sleep(Duration::from_millis(50)).await;

        // Remove first TX
        let hash1 = compute_tx_hash(&[1]);
        handle.remove_txs(vec![hash1]);
        tokio::time::sleep(Duration::from_millis(50)).await;

        // Only one should remain
        let mp = mempool.read().await;
        assert_eq!(mp.len(), 1);
    }

    #[tokio::test]
    async fn test_cache_and_fetch_tx_set() {
        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
        let overlay = Overlay::new(cmd_rx);
        let handle = OverlayHandle::new(cmd_tx);

        tokio::spawn(async move {
            let _ = overlay.run().await;
        });

        let hash = [42u8; 32];
        let xdr = vec![1, 2, 3, 4, 5];

        // Cache it
        handle.cache_tx_set(hash, xdr.clone());
        tokio::time::sleep(Duration::from_millis(50)).await;

        // Fetch it
        let result = handle.fetch_tx_set(hash).await;
        assert_eq!(result, Some(xdr));

        // Fetch non-existent
        let result = handle.fetch_tx_set([0u8; 32]).await;
        assert_eq!(result, None);
    }

    // ═══ Additional Tests ═══

    #[tokio::test]
    async fn test_remove_multiple_txs_at_once() {
        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
        let overlay = Overlay::new(cmd_rx);
        let handle = OverlayHandle::new(cmd_tx);
        let mempool = overlay.mempool.clone();

        tokio::spawn(async move {
            let _ = overlay.run().await;
        });

        // Submit 5 TXs
        for i in 0..5u8 {
            handle.submit_tx(vec![i], (i as u64 + 1) * 100, 1);
        }
        tokio::time::sleep(Duration::from_millis(50)).await;

        assert_eq!(mempool.read().await.len(), 5);

        // Remove 3 of them at once
        let hashes_to_remove = vec![
            compute_tx_hash(&[0]),
            compute_tx_hash(&[2]),
            compute_tx_hash(&[4]),
        ];
        handle.remove_txs(hashes_to_remove);
        tokio::time::sleep(Duration::from_millis(50)).await;

        // Should have 2 remaining
        let mp = mempool.read().await;
        assert_eq!(mp.len(), 2);
        assert!(mp.contains(&compute_tx_hash(&[1])));
        assert!(mp.contains(&compute_tx_hash(&[3])));
    }

    #[tokio::test]
    async fn test_remove_nonexistent_tx() {
        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
        let overlay = Overlay::new(cmd_rx);
        let handle = OverlayHandle::new(cmd_tx);
        let mempool = overlay.mempool.clone();

        tokio::spawn(async move {
            let _ = overlay.run().await;
        });

        // Submit 1 TX
        handle.submit_tx(vec![1], 100, 1);
        tokio::time::sleep(Duration::from_millis(50)).await;

        // Try to remove a TX that doesn't exist
        handle.remove_txs(vec![[0u8; 32]]);
        tokio::time::sleep(Duration::from_millis(50)).await;

        // Original TX should still be there
        assert_eq!(mempool.read().await.len(), 1);
    }

    #[tokio::test]
    async fn test_get_top_txs_more_than_available() {
        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
        let overlay = Overlay::new(cmd_rx);
        let handle = OverlayHandle::new(cmd_tx);

        tokio::spawn(async move {
            let _ = overlay.run().await;
        });

        // Submit only 2 TXs
        handle.submit_tx(vec![1], 100, 1);
        handle.submit_tx(vec![2], 200, 1);
        tokio::time::sleep(Duration::from_millis(50)).await;

        // Ask for 10
        let top = handle.get_top_txs(10).await;

        // Should return only 2
        assert_eq!(top.len(), 2);
    }

    #[tokio::test]
    async fn test_get_top_txs_empty_mempool() {
        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
        let overlay = Overlay::new(cmd_rx);
        let handle = OverlayHandle::new(cmd_tx);

        tokio::spawn(async move {
            let _ = overlay.run().await;
        });

        tokio::time::sleep(Duration::from_millis(50)).await;

        let top = handle.get_top_txs(10).await;
        assert!(top.is_empty());
    }

    #[tokio::test]
    async fn test_cache_multiple_tx_sets() {
        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
        let overlay = Overlay::new(cmd_rx);
        let handle = OverlayHandle::new(cmd_tx);

        tokio::spawn(async move {
            let _ = overlay.run().await;
        });

        // Cache multiple TX sets
        let hash1 = [1u8; 32];
        let hash2 = [2u8; 32];
        let hash3 = [3u8; 32];

        handle.cache_tx_set(hash1, vec![1, 1, 1]);
        handle.cache_tx_set(hash2, vec![2, 2, 2]);
        handle.cache_tx_set(hash3, vec![3, 3, 3]);
        tokio::time::sleep(Duration::from_millis(50)).await;

        // All should be retrievable
        assert_eq!(handle.fetch_tx_set(hash1).await, Some(vec![1, 1, 1]));
        assert_eq!(handle.fetch_tx_set(hash2).await, Some(vec![2, 2, 2]));
        assert_eq!(handle.fetch_tx_set(hash3).await, Some(vec![3, 3, 3]));
    }

    #[tokio::test]
    async fn test_cache_overwrite_tx_set() {
        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
        let overlay = Overlay::new(cmd_rx);
        let handle = OverlayHandle::new(cmd_tx);

        tokio::spawn(async move {
            let _ = overlay.run().await;
        });

        let hash = [42u8; 32];

        // Cache original
        handle.cache_tx_set(hash, vec![1, 2, 3]);
        tokio::time::sleep(Duration::from_millis(50)).await;

        // Overwrite with new data
        handle.cache_tx_set(hash, vec![4, 5, 6, 7]);
        tokio::time::sleep(Duration::from_millis(50)).await;

        // Should return new data
        assert_eq!(handle.fetch_tx_set(hash).await, Some(vec![4, 5, 6, 7]));
    }

    #[tokio::test]
    async fn test_tx_ordering_by_fee_per_op() {
        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
        let overlay = Overlay::new(cmd_rx);
        let handle = OverlayHandle::new(cmd_tx);

        tokio::spawn(async move {
            let _ = overlay.run().await;
        });

        // TX1: 200 fee / 2 ops = 100 per op
        // TX2: 150 fee / 1 op = 150 per op (HIGHER priority)
        // TX3: 300 fee / 4 ops = 75 per op (LOWER priority)
        handle.submit_tx(vec![1], 200, 2);
        handle.submit_tx(vec![2], 150, 1);
        handle.submit_tx(vec![3], 300, 4);
        tokio::time::sleep(Duration::from_millis(50)).await;

        let top = handle.get_top_txs(3).await;
        assert_eq!(top.len(), 3);

        // Order should be: TX2 (150/op), TX1 (100/op), TX3 (75/op)
        assert_eq!(top[0].1, vec![2]);
        assert_eq!(top[1].1, vec![1]);
        assert_eq!(top[2].1, vec![3]);
    }
}
