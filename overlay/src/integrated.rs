//! Mempool manager that handles transaction storage and TX set building.
//!
//! Network communication is handled by the libp2p QUIC overlay.
//! This module provides:
//! - Transaction mempool (fee-ordered per kind, with dedup)
//! - Core command handling for mempool operations

use std::sync::Arc;
use std::time::Duration;
use tokio::sync::{mpsc, RwLock};
use tracing::{debug, info};

use crate::flood::{Mempool, TxBudget};
use crate::wire::{TxKind, ValidatedTx};

/// What Core wants from one nomination pull: a budget per tx set phase. Core
/// sizes each budget to what fits in the corresponding phase of the next
/// ledger, so the reply is never larger than the tx set it feeds.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct TopTxsRequest {
    pub classic: TxBudget,
    pub soroban: TxBudget,
}

impl TopTxsRequest {
    pub fn budget(&self, kind: TxKind) -> TxBudget {
        match kind {
            TxKind::Classic => self.classic,
            TxKind::Soroban => self.soroban,
        }
    }
}

/// Commands from Core to Overlay
#[derive(Debug)]
pub enum CoreCommand {
    /// Submit a transaction to the mempool
    SubmitTx(Arc<ValidatedTx>),

    /// Request the top transactions by fee, per kind, within budgets
    GetTopTxs {
        request: TopTxsRequest,
        reply: mpsc::Sender<Vec<Arc<ValidatedTx>>>,
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
            CoreCommand::SubmitTx(tx) => {
                debug!(
                    "[SubmitTx] TX: hash={:02x?}, size={}, fee={}, ops={}, kind={:?}",
                    &tx.hash()[..4],
                    tx.bytes().len(),
                    tx.fee(),
                    tx.num_ops(),
                    tx.kind()
                );
                let mut mempool = self.mempool.write().await;
                mempool.insert(tx);
            }

            CoreCommand::GetTopTxs { request, reply } => {
                // Collect Arc clones under the read lock, then drop it before
                // the (bounded) reply send so a slow receiver can't hold up
                // mempool writers. Classic first, then Soroban, matching the
                // phase order Core builds.
                let txs: Vec<Arc<ValidatedTx>> = {
                    let mempool = self.mempool.read().await;
                    TxKind::ALL
                        .iter()
                        .flat_map(|kind| mempool.top_by_fee(*kind, request.budget(*kind)))
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

    /// Submit a validated transaction to the mempool.
    ///
    /// The send is ordered with respect to every other command issued
    /// through this handle, so a `get_top_txs` issued afterwards observes
    /// the insert.
    pub fn submit_tx(&self, tx: Arc<ValidatedTx>) {
        let _ = self.cmd_tx.send(CoreCommand::SubmitTx(tx));
    }

    /// Get top transactions by fee, per kind, within the given budgets.
    ///
    /// Returns `None` if the mempool manager is gone (shutdown); callers must
    /// not answer Core with an empty list in that case.
    pub async fn get_top_txs(&self, request: TopTxsRequest) -> Option<Vec<Arc<ValidatedTx>>> {
        let (reply_tx, mut reply_rx) = mpsc::channel(1);
        self.cmd_tx
            .send(CoreCommand::GetTopTxs {
                request,
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

    fn classic(bytes: Vec<u8>, fee: i64, num_ops: u32) -> Arc<ValidatedTx> {
        ValidatedTx::from_core_trusted(bytes, fee, num_ops, TxKind::Classic).unwrap()
    }

    fn soroban(bytes: Vec<u8>, fee: i64) -> Arc<ValidatedTx> {
        ValidatedTx::from_core_trusted(bytes, fee, 1, TxKind::Soroban).unwrap()
    }

    /// Budgets that never bind.
    fn all(max_count: usize) -> TopTxsRequest {
        let budget = TxBudget {
            max_count,
            max_bytes: usize::MAX,
        };
        TopTxsRequest {
            classic: budget,
            soroban: budget,
        }
    }

    fn spawn_manager() -> (OverlayHandle, Arc<RwLock<Mempool>>) {
        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
        let overlay = Overlay::new(cmd_rx);
        let mempool = overlay.mempool.clone();
        tokio::spawn(async move {
            let _ = overlay.run().await;
        });
        (OverlayHandle::new(cmd_tx), mempool)
    }

    #[tokio::test]
    async fn test_submit_tx_adds_to_mempool() {
        let (handle, mempool) = spawn_manager();

        // Submit a TX
        let tx = classic(valid_transaction_xdr(100, 1, 1), 100, 1);
        handle.submit_tx(tx);
        tokio::time::sleep(Duration::from_millis(50)).await;

        // Verify it's in mempool
        let mp = mempool.read().await;
        assert_eq!(mp.len(), 1);
    }

    #[tokio::test]
    async fn test_get_top_txs() {
        let (handle, _mempool) = spawn_manager();

        // Submit TXs with different fees
        let tx1 = valid_transaction_xdr(100, 1, 1);
        let tx2 = valid_transaction_xdr(500, 2, 1);
        let tx3 = valid_transaction_xdr(200, 3, 1);
        handle.submit_tx(classic(tx1, 100, 1));
        handle.submit_tx(classic(tx2.clone(), 500, 1));
        handle.submit_tx(classic(tx3, 200, 1));

        // Get top 2. Ordering with the submits is guaranteed by the handle.
        let top = handle.get_top_txs(all(2)).await.unwrap();
        assert_eq!(top.len(), 2);
        // First should be highest fee
        assert_eq!(top[0].bytes(), &tx2[..]);
    }

    #[tokio::test]
    async fn test_get_top_txs_more_than_available() {
        let (handle, _mempool) = spawn_manager();

        // Submit only 2 TXs
        handle.submit_tx(classic(valid_transaction_xdr(100, 1, 1), 100, 1));
        handle.submit_tx(classic(valid_transaction_xdr(200, 2, 1), 200, 1));

        // Ask for 10
        let top = handle.get_top_txs(all(10)).await.unwrap();

        // Should return only 2
        assert_eq!(top.len(), 2);
    }

    #[tokio::test]
    async fn test_get_top_txs_empty_mempool() {
        let (handle, _mempool) = spawn_manager();

        let top = handle.get_top_txs(all(10)).await.unwrap();
        assert!(top.is_empty());
    }

    #[tokio::test]
    async fn test_tx_ordering_by_fee_per_op() {
        let (handle, _mempool) = spawn_manager();

        // TX1: 200 fee / 2 ops = 100 per op
        // TX2: 150 fee / 1 op = 150 per op (HIGHER priority)
        // TX3: 300 fee / 4 ops = 75 per op (LOWER priority)
        let tx1 = valid_transaction_xdr(200, 1, 2);
        let tx2 = valid_transaction_xdr(150, 2, 1);
        let tx3 = valid_transaction_xdr(300, 3, 4);
        handle.submit_tx(classic(tx1.clone(), 200, 2));
        handle.submit_tx(classic(tx2.clone(), 150, 1));
        handle.submit_tx(classic(tx3.clone(), 300, 4));

        let top = handle.get_top_txs(all(3)).await.unwrap();
        assert_eq!(top.len(), 3);

        // Order should be: TX2 (150/op), TX1 (100/op), TX3 (75/op)
        assert_eq!(top[0].bytes(), &tx2[..]);
        assert_eq!(top[1].bytes(), &tx1[..]);
        assert_eq!(top[2].bytes(), &tx3[..]);
    }

    #[tokio::test]
    async fn test_get_top_txs_applies_per_kind_budgets() {
        let (handle, _mempool) = spawn_manager();

        // Two Soroban txs (high fees) and three classic ones.
        let s1 = valid_transaction_xdr(9000, 1, 1);
        let s2 = valid_transaction_xdr(8000, 2, 1);
        let c1 = valid_transaction_xdr(300, 3, 1);
        let c2 = valid_transaction_xdr(200, 4, 1);
        let c3 = valid_transaction_xdr(100, 5, 1);
        handle.submit_tx(soroban(s1.clone(), 9000));
        handle.submit_tx(soroban(s2.clone(), 8000));
        handle.submit_tx(classic(c1.clone(), 300, 1));
        handle.submit_tx(classic(c2.clone(), 200, 1));
        handle.submit_tx(classic(c3.clone(), 100, 1));

        // Two classic, one Soroban: the reply lists classic first, and the
        // Soroban txs' higher fees do not steal classic slots.
        let request = TopTxsRequest {
            classic: TxBudget {
                max_count: 2,
                max_bytes: usize::MAX,
            },
            soroban: TxBudget {
                max_count: 1,
                max_bytes: usize::MAX,
            },
        };
        let top = handle.get_top_txs(request).await.unwrap();
        let bytes: Vec<&[u8]> = top.iter().map(|tx| tx.bytes()).collect();
        assert_eq!(bytes, vec![&c1[..], &c2[..], &s1[..]]);

        // A zero Soroban budget returns classic only.
        let request = TopTxsRequest {
            classic: TxBudget {
                max_count: 10,
                max_bytes: usize::MAX,
            },
            soroban: TxBudget::default(),
        };
        let top = handle.get_top_txs(request).await.unwrap();
        assert_eq!(top.len(), 3);
        assert!(top.iter().all(|tx| tx.kind() == TxKind::Classic));
    }
}
