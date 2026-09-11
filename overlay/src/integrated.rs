//! Mempool manager that handles transaction storage and TX set building.
//!
//! Network communication is handled by the libp2p QUIC overlay.
//! This module provides:
//! - Transaction mempool (fee-ordered, with dedup)
//! - Core command handling for mempool operations

use std::sync::Arc;
use std::time::Duration;
use tokio::sync::{mpsc, OwnedSemaphorePermit, RwLock, Semaphore};
use tracing::{debug, info};

use crate::flood::Mempool;
use crate::wire::ValidatedTx;

/// Bound network admissions waiting for or undergoing mempool insertion.
const MAX_NETWORK_ADMISSIONS: usize = 10_000;

/// One ordered stream for Core commands and network TX admissions.
#[derive(Debug)]
pub enum CoreCommand {
    /// Insert a transaction. Network admission owns capacity until processed;
    /// local Core submissions retain their existing unbounded enqueue policy.
    SubmitTx {
        tx: Arc<ValidatedTx>,
        admission: Option<OwnedSemaphorePermit>,
    },

    /// Request top N transactions by fee
    GetTopTxs {
        count: usize,
        reply: mpsc::Sender<Vec<Arc<ValidatedTx>>>,
    },

    /// Remove discarded candidates or finalized transactions.
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
            CoreCommand::SubmitTx { tx, admission } => {
                debug!(
                    "[SubmitTx] TX: hash={:02x?}, size={}, fee={}, ops={}",
                    &tx.hash()[..4],
                    tx.bytes().len(),
                    tx.fee(),
                    tx.num_ops()
                );
                let mut mempool = self.mempool.write().await;
                mempool.insert(tx);
                drop(admission);
            }

            CoreCommand::GetTopTxs { count, reply } => {
                // Collect Arc clones under the read lock, then drop it before
                // the (bounded) reply send so a slow receiver can't hold up
                // mempool writers.
                let txs: Vec<Arc<ValidatedTx>> = {
                    let mempool = self.mempool.read().await;
                    mempool
                        .top_by_fee(count)
                        .iter()
                        .filter_map(|h| mempool.get(h).map(Arc::clone))
                        .collect()
                };
                let _ = reply.send(txs).await;
            }

            CoreCommand::RemoveTxsFromMempool { tx_hashes, reply } => {
                let mut mempool = self.mempool.write().await;
                let count = tx_hashes.len();
                for hash in tx_hashes {
                    mempool.remove(&hash);
                }
                let expired = mempool.evict_expired();
                info!(
                    "Removed {} (requested) + {} (expired) TXs from mempool",
                    count, expired
                );
                drop(mempool);
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
    network_admissions: Arc<Semaphore>,
}

impl OverlayHandle {
    /// Create one handle per manager, then clone it to share admission capacity.
    pub fn new(cmd_tx: mpsc::UnboundedSender<CoreCommand>) -> Self {
        Self::with_admission_capacity(cmd_tx, MAX_NETWORK_ADMISSIONS)
    }

    pub(crate) fn with_admission_capacity(
        cmd_tx: mpsc::UnboundedSender<CoreCommand>,
        capacity: usize,
    ) -> Self {
        Self {
            cmd_tx,
            network_admissions: Arc::new(Semaphore::new(capacity)),
        }
    }

    /// Submit a validated transaction.
    pub fn submit_tx(&self, tx: Arc<ValidatedTx>) {
        let _ = self.cmd_tx.send(CoreCommand::SubmitTx {
            tx,
            admission: None,
        });
    }

    /// Admit directly to the same FIFO as removal, without an intermediate
    /// queue, task, or await. Clones share the network admission bound. Control
    /// commands can still enqueue when that bound is full.
    ///
    /// Returns false if full or closed. Failure and shutdown release capacity
    /// by dropping the command's permit; the caller may retry the TX later.
    pub fn try_submit_network_tx(&self, tx: Arc<ValidatedTx>) -> bool {
        let Ok(admission) = self.network_admissions.clone().try_acquire_owned() else {
            return false;
        };
        self.cmd_tx
            .send(CoreCommand::SubmitTx {
                tx,
                admission: Some(admission),
            })
            .is_ok()
    }

    /// Get top transactions by fee.
    ///
    /// Returns `None` if the mempool manager is gone (shutdown); callers must
    /// not answer Core with an empty list in that case.
    pub async fn get_top_txs(&self, count: usize) -> Option<Vec<Arc<ValidatedTx>>> {
        let (reply_tx, mut reply_rx) = mpsc::channel(1);
        self.cmd_tx
            .send(CoreCommand::GetTopTxs {
                count,
                reply: reply_tx,
            })
            .ok()?;
        reply_rx.recv().await
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
    use crate::xdr::tests::valid_transaction_xdr;
    use futures::poll;

    fn transaction(sequence: i64) -> Arc<ValidatedTx> {
        ValidatedTx::from_core_trusted(valid_transaction_xdr(100, sequence, 1), 100, 1).unwrap()
    }

    #[tokio::test]
    async fn bounded_admissions_share_fifo_with_removal_and_queries() {
        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
        let handle = OverlayHandle::with_admission_capacity(cmd_tx, 2);
        let peer = handle.clone();
        let first = transaction(1);
        let second = transaction(2);
        let third = transaction(3);
        assert!(handle.try_submit_network_tx(first.clone()));
        assert!(peer.try_submit_network_tx(second.clone()));
        assert!(!peer.try_submit_network_tx(third.clone()));

        // Enqueue controls while network capacity is exhausted and the actor
        // has not even started. Both use the same FIFO without a TX permit.
        let removal = handle.remove_txs_sync(vec![*first.hash()]);
        tokio::pin!(removal);
        assert!(poll!(removal.as_mut()).is_pending());
        let query = handle.get_top_txs(10);
        tokio::pin!(query);
        assert!(poll!(query.as_mut()).is_pending());
        let task = tokio::spawn(Overlay::new(cmd_rx).run());
        removal.await;
        let top = query.await.unwrap();
        assert_eq!(top.len(), 1);
        assert_eq!(top[0].hash(), second.hash());
        assert!(handle.try_submit_network_tx(third));
        assert_eq!(handle.get_top_txs(10).await.unwrap().len(), 2);
        task.abort();
    }

    #[tokio::test]
    async fn admission_capacity_is_held_through_insertion_and_duplicate_handling() {
        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
        let handle = OverlayHandle::with_admission_capacity(cmd_tx, 1);
        let mut overlay = Overlay::new(cmd_rx);
        let tx = transaction(1);

        // Exercise both an insertion and a live duplicate rejection.
        for _ in 0..2 {
            assert!(handle.try_submit_network_tx(tx.clone()));
            let command = overlay.core_commands.recv().await.unwrap();
            let guard = overlay.mempool.read().await;
            let insertion = overlay.handle_core_command(command);
            tokio::pin!(insertion);
            assert!(poll!(insertion.as_mut()).is_pending());
            assert!(
                !handle.try_submit_network_tx(tx.clone()),
                "dequeue must not release capacity"
            );
            drop(guard);
            insertion.await;
            assert_eq!(handle.network_admissions.available_permits(), 1);
            assert_eq!(overlay.mempool.read().await.len(), 1);
        }
    }

    #[tokio::test]
    async fn cancellation_and_shutdown_release_admission_capacity() {
        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
        let handle = OverlayHandle::with_admission_capacity(cmd_tx, 1);
        let mut overlay = Overlay::new(cmd_rx);
        let tx = transaction(1);
        assert!(handle.try_submit_network_tx(tx.clone()));
        let command = overlay.core_commands.recv().await.unwrap();
        let guard = overlay.mempool.read().await;
        {
            let insertion = overlay.handle_core_command(command);
            tokio::pin!(insertion);
            assert!(poll!(insertion.as_mut()).is_pending());
            assert_eq!(handle.network_admissions.available_permits(), 0);
            // Dropping an in-progress insertion must release its permit.
        }
        drop(guard);
        assert!(handle.try_submit_network_tx(tx.clone()));
        drop(overlay); // Drops queued admissions as well.
        assert_eq!(handle.network_admissions.available_permits(), 1);
        assert!(!handle.try_submit_network_tx(tx)); // Closed FIFO, permit returned.
        assert_eq!(handle.network_admissions.available_permits(), 1);
    }

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
        let tx = ValidatedTx::from_core_trusted(valid_transaction_xdr(100, 1, 1), 100, 1).unwrap();
        handle.submit_tx(tx);
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
        let tx1 = valid_transaction_xdr(100, 1, 1);
        let tx2 = valid_transaction_xdr(500, 2, 1);
        let tx3 = valid_transaction_xdr(200, 3, 1);
        handle.submit_tx(ValidatedTx::from_core_trusted(tx1, 100, 1).unwrap());
        handle.submit_tx(ValidatedTx::from_core_trusted(tx2.clone(), 500, 1).unwrap());
        handle.submit_tx(ValidatedTx::from_core_trusted(tx3, 200, 1).unwrap());
        tokio::time::sleep(Duration::from_millis(50)).await;

        // Get top 2
        let top = handle.get_top_txs(2).await.unwrap();
        assert_eq!(top.len(), 2);
        // First should be highest fee
        assert_eq!(top[0].bytes(), &tx2[..]);
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
        handle.submit_tx(
            ValidatedTx::from_core_trusted(valid_transaction_xdr(100, 1, 1), 100, 1).unwrap(),
        );
        handle.submit_tx(
            ValidatedTx::from_core_trusted(valid_transaction_xdr(200, 2, 1), 200, 1).unwrap(),
        );
        tokio::time::sleep(Duration::from_millis(50)).await;

        // Ask for 10
        let top = handle.get_top_txs(10).await.unwrap();

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

        let top = handle.get_top_txs(10).await.unwrap();
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
        let tx1 = valid_transaction_xdr(200, 1, 2);
        let tx2 = valid_transaction_xdr(150, 2, 1);
        let tx3 = valid_transaction_xdr(300, 3, 4);
        handle.submit_tx(ValidatedTx::from_core_trusted(tx1.clone(), 200, 2).unwrap());
        handle.submit_tx(ValidatedTx::from_core_trusted(tx2.clone(), 150, 1).unwrap());
        handle.submit_tx(ValidatedTx::from_core_trusted(tx3.clone(), 300, 4).unwrap());
        tokio::time::sleep(Duration::from_millis(50)).await;

        let top = handle.get_top_txs(3).await.unwrap();
        assert_eq!(top.len(), 3);

        // Order should be: TX2 (150/op), TX1 (100/op), TX3 (75/op)
        assert_eq!(top[0].bytes(), &tx2[..]);
        assert_eq!(top[1].bytes(), &tx1[..]);
        assert_eq!(top[2].bytes(), &tx3[..]);
    }
}
