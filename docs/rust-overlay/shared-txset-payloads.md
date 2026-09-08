# Share cached tx-set payloads during fanout

Serving a cached set previously cloned its entire XDR on the Rust App event
loop for every requesting peer. Legacy sends then copied it again to prepend
the StellarMessage discriminant. The first copy happened before send
admission, so producers waiting for a permit already owned separate payloads.

The cache now owns an `Arc<Vec<u8>>`. Insertion preserves the original Vec's
buffer; send commands retain references to that immutable buffer. The network writer
sends the length, discriminant and XDR without assembling another payload. A legacy stream's existing mutex
covers the entire frame, including partial writes. Admission still charges
each response's full wire size and retains the existing count and byte limits.

Eviction removes cache ownership; outstanding sends keep their bytes alive
until they finish or are dropped. Cancelling one send does not affect others.
Core IPC borrows the cached XDR when constructing its existing contiguous
message, eliminating an intermediate clone. QUIC and IPC still copy into
their transport buffers. Wire formats, priorities, validation,
and nomination behavior are unchanged.

Tests cover shared storage through actual App requests and queued legacy
commands, eviction and drop lifetimes, and exact XDR framing through a writer
that alternates short writes and pending polls. The App storage regression
fails before the change. Existing real-QUIC tests cover simultaneous frames,
blocked peers, stream reopening, cancellation, and legacy compatibility.

A local optimized microbenchmark using the real cache implementation retained
29 responses to one 4,512,076-byte set. Cache plus responses held 135,362,280
distinct payload bytes before and 4,512,076 afterward. Median time to create
the 29 pending references fell from about 10 ms to below 1 microsecond across
three alternating before/after samples (30 iterations each). This measures
cache lookup and payload ownership only; it excludes transfer, Core work and
consensus. A fleet comparison is required to establish a block-latency gain.
