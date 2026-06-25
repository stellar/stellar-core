// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

use crate::rust_bridge::NtpProbeResult;
use cxx::CxxString;
use rsntp::LeapIndicator;
use rsntp::SntpClient;
use std::time::Duration;

// Query `server` (an NTP host name, e.g. "pool.ntp.org") for the offset between
// the local clock and true time. Blocking; the caller runs this on a background
// thread. Never panics: any failure (bad UTF-8, DNS failure, timeout,
// unreachable server) is reported as `succeeded: false`.
//
// This is detection only -- it does not adjust the local clock.
pub(crate) fn query_ntp_offset(server: &CxxString, timeout_seconds: u64) -> NtpProbeResult {
    let failure = NtpProbeResult {
        succeeded: false,
        offset_millis: 0,
    };

    let host = match server.to_str() {
        Ok(h) => h,
        Err(_) => return failure,
    };

    let mut client = SntpClient::new();
    client.set_timeout(Duration::from_secs(timeout_seconds));

    match client.synchronize(host) {
        Ok(result)
            if (1..=15).contains(&result.stratum())
                && result.leap_indicator() != LeapIndicator::AlarmCondition =>
        {
            // as_secs_f64 keeps the sign: positive means the local clock is
            // behind the server (i.e. add this to the local clock to match).
            let offset_secs = result.clock_offset().as_secs_f64();
            NtpProbeResult {
                succeeded: true,
                offset_millis: (offset_secs * 1000.0) as i64,
            }
        }
        Ok(_) | Err(_) => failure,
    }
}
