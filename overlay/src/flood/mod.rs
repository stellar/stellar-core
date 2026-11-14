//! TX flooding module.
//!
//! Provides mempool management, TX set building, and INV/GETDATA flooding.

mod inv_batcher;
mod inv_messages;
mod inv_tracker;
mod mempool;
mod pending_requests;
mod tx_buffer;
pub(crate) mod tx_router;
mod txset;
pub(crate) mod txset_encoding;
pub(crate) mod txset_push;
mod txset_store;
pub mod txset_coded;
pub mod txset_tree;

pub use inv_batcher::InvBatcher;
pub use inv_messages::{GetData, InvBatch, InvEntry, TxStreamMessage, INV_BATCH_MAX_SIZE};
pub use inv_tracker::InvTracker;
pub use mempool::Mempool;
pub use pending_requests::PendingRequests;
pub use tx_buffer::TxBuffer;
pub use tx_router::{NodeId, SendPlan, TxRouter};
pub use txset::{CachedTxSet, Hash256, TxSetCache};
pub use txset_encoding::TxSetData;
pub use txset_push::{PushRateLimiter, PushRecord, UnclaimedSetBudget};

/// Limits of the tx-set push, for tests outside this crate's library.
pub mod txset_push_limits {
    pub use super::txset_push::{
        MAX_UNCLAIMED_BYTES, MAX_UNCLAIMED_REQUESTED_SETS_PER_PEER, MAX_UNCLAIMED_SETS_PER_PEER,
        PUSH_BURST, PUSH_REFILL_PER_SEC, PUSH_SKIP_WINDOW,
    };
}
pub use txset_store::TxSetStore;
