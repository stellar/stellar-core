# Ordered transaction admission and removal

Network TX admissions now enter the mempool command FIFO directly from the
network handler. Core submissions, removals and nomination pulls use that same
FIFO. The intermediate App TX event queue and its forwarding select arm are gone.
An admission enqueued before removal is processed first, so that removal cannot
be undone by an older TX event that App has not polled yet.

The existing flooding seen-set lookup and the admission enqueue happen together,
without an intervening await. Only successfully enqueued TXs are marked seen.
Pending-request cleanup, the relay buffer and announcements happen afterward;
a reader delayed there cannot later enqueue its admission again. Exhausted
admission capacity or a closed command stream drops the attempt without poisoning the seen set, allowing a
later response to be admitted.

Network admission remains nonblocking and bounded at 10,000 queued plus active
insertions across all cloned handles. Each accepted command owns a semaphore
permit until insertion completes, including live-duplicate rejection. Cancellation,
receiver shutdown and send failure release permits automatically. Removal and
query commands need no admission permit, so network saturation cannot prevent
them from entering the FIFO. Local Core submissions retain their existing enqueue
policy. This is a bound on pending network admissions, not a bound on all overlay
memory or on Core submissions.

Existing flooding and live-mempool dedup remain; there is no separate
finalized-transaction history or admission lookup.
An admission first enqueued *after* removal can still enter the mempool; this is
an intentional tradeoff. This ordering fix makes no assumption that QUIC orders
transactions against SCP or tx sets on different streams. Stateful validation
remains Core's responsibility. No wire format or C++ builder changes are involved.

Tests exercise actual network ingress without App polling, removal while the
reader is paused at its first post-admission bookkeeping await, saturation and
retry, duplicate responses, shared capacity across handles, control commands at
capacity, and permit release during insertion, cancellation and shutdown. The
network-without-App regression failed before the change and passes afterward.
App tests exercise removal through the real Core-message handler and explicitly
verify that later admissions remain allowed. Transport tests observe the actual
mempool command stream in place of the deleted TX event queue.
