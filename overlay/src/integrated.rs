//! Mempool manager that handles transaction storage and TX set building.
//!
//! Network communication is handled by the libp2p QUIC overlay.
//! This module provides:
//! - Transaction mempool (fee-ordered, with dedup)
//! - Core command handling for mempool operations

use std::sync::Arc;
use std::time::Duration;
use tokio::sync::{mpsc, RwLock};
use tracing::{debug, info};

use crate::flood::{compute_tx_hash, Mempool};

/// Commands from Core to Overlay
#[derive(Debug, Clone)]
pub enum CoreCommand {
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

    /// Remove transactions from mempool (after ledger close)
    RemoveTxsFromMempool {
        tx_hashes: Vec<[u8; 32]>,
        reply: Option<mpsc::Sender<()>>,
    },
}

/// Mempool manager (no longer handles network connections).
pub struct Overlay {
    /// Commands from Core
    core_commands: mpsc::UnboundedReceiver<CoreCommand>,

    /// TX mempool
    mempool: Arc<RwLock<Mempool>>,
}

impl Overlay {
    /// Create a new mempool manager.
    pub fn new(core_commands: mpsc::UnboundedReceiver<CoreCommand>) -> Self {
        Self {
            core_commands,
            mempool: Arc::new(RwLock::new(Mempool::new(100000, Duration::from_secs(300)))),
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
        }
    }

    /// Get mempool reference (for testing)
    pub fn mempool(&self) -> &Arc<RwLock<Mempool>> {
        &self.mempool
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
