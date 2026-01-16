// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "config.h"

#ifdef USE_TCMALLOC
#include <gperftools/malloc_extension.h>

namespace
{
// Configure tcmalloc parameters at startup using a constructor attribute.
// This runs before main() to ensure optimal allocator settings from the start.
//
// Parameters:
// - Release rate 10.0: Aggressively return memory to OS (default is 1.0)
//   Whenever core makes a large allocation (in-memory soroban state, module
//   cache, Bucket indexes), it is almost always on startup or in a background
//   thread. Allocation speed is not critical for these larger allocations, so
//   we free memory aggressively.
// - Max thread cache 512MB: Allow larger per-thread caches for high-throughput
//   The most commonly allocated objects are around ~1KB BucketEntry/LedgerEntry
//   and are routinely allocated from multiple threads. We allocate large
//   per-thread caches to reduce contention.
// - Large alloc threshold 10GB: Functionally disable allocation related logs
//   for performance, since we don't read stderr and have lots of other
//   telemetry.
__attribute__((constructor)) void
initTcmallocConfig()
{
    MallocExtension* ext = MallocExtension::instance();
    if (ext)
    {
        // TCMALLOC_RELEASE_RATE=10.0
        ext->SetMemoryReleaseRate(10.0);

        // TCMALLOC_MAX_TOTAL_THREAD_CACHE_BYTES=536870912 (512MB)
        ext->SetNumericProperty("tcmalloc.max_total_thread_cache_bytes",
                                536870912);

        // TCMALLOC_LARGE_ALLOC_REPORT_THRESHOLD=10737418240 (10GB)
        ext->SetNumericProperty("tcmalloc.large_alloc_report_threshold",
                                10737418240ULL);
    }
}
} // namespace
#endif // USE_TCMALLOC
