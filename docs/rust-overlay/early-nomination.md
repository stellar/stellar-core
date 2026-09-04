# Preparing nomination proposals before the trigger

After ledger close, Herder schedules private tx-set construction for validators
selected by the first two nomination calls. The preview runs the existing SCP
leader-selection algorithm on temporary state. It preserves skipped rounds,
weights, and ties; it does not change live SCP state. Ties can select more than
two validators, and different quorum configurations need not agree on leaders.

Preparation uses the normal Rust mempool pull and C++ builder. It runs on the
main thread after the ledger-close callback returns. This moves work into the
trigger's waiting period; it does not parallelize validation or shorten the
construction itself. When the trigger is already due, there is no waiting period
to hide construction in.

The result records the previous ledger hash, target sequence, and exact close
time used for validation. The close time is chosen for the scheduled trigger and
retained if processing runs late. Before the trigger, the result is not added to
PendingEnvelopes, advertised, nominated, or used to remove invalid transactions
from the mempool. Externalization, ledger advancement, loss of sync, rescheduling,
and shutdown discard it. Reuse checks the ledger identity and close-time bound.

If all valid pulled candidates fit, the trigger rebuilds from a fresh mempool
pull. This prevents an early empty or small snapshot from freezing out arrivals
during the wait. Otherwise it reuses the selection-limited snapshot. This is not
incremental packing: later arrivals, including higher-fee transactions or arrivals
in another phase, may wait for the next ledger. Neither this policy nor the
existing bounded pull guarantees a globally maximal set for an arbitrary mempool.

SCP starts at the existing trigger. Its existing nomination timeout controls when
the second leader can vote for its own value. Other validators retain the normal
build-at-trigger path. Manual-close/apply-load runs do not schedule preparation.

Run the scheduling and leader-preview tests with
`stellar-core test '[early-nomination]'`.
