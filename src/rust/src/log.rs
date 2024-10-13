// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

use cxx::let_cxx_string;
use itertools::Itertools;
use log::{Level, LevelFilter, Metadata, Record, SetLoggerError};
use std::sync::atomic::{AtomicBool, Ordering};

use crate::rust_bridge::LogLevel;

// The following functions are implemented in the C++ side of the bridge.
// When running tests, the rust runner doesn't have the C++ logging subsystem
// available, so we provide a simple implementation that just logs to stdout.
#[cfg(not(test))]
use crate::rust_bridge::{shim_isLogLevelAtLeast, shim_logAtPartitionAndLevel};
#[cfg(test)]
fn shim_isLogLevelAtLeast(
    _partition: &std::pin::Pin<&mut cxx::CxxString>,
    _level: LogLevel,
) -> std::result::Result<bool, ()> {
    Ok(true)
}
#[cfg(test)]
fn shim_logAtPartitionAndLevel(
    partition: &std::pin::Pin<&mut cxx::CxxString>,
    level: LogLevel,
    msg: &std::pin::Pin<&mut cxx::CxxString>,
) -> std::result::Result<(), ()> {
    let lvl = match level {
        LogLevel::LVL_ERROR => "ERROR",
        LogLevel::LVL_WARNING => "WARNING",
        LogLevel::LVL_INFO => "INFO",
        LogLevel::LVL_DEBUG => "DEBUG",
        LogLevel::LVL_TRACE => "TRACE",
        _ => "UNKNOWN",
    };
    println!("{} ({}): {}", partition, lvl, msg);
    Ok(())
}

// This is a simple implementation of Rust's standard logging API that routes
// log messages over to the stellar-core logging system, which uses spdlog.
struct StellarLogger;

static LOGGER: StellarLogger = StellarLogger;
static HAVE_INITIALIZED: AtomicBool = AtomicBool::new(false);

// These need to be kept in sync with util/LogPartitions.def on the C++ side.
#[allow(unused)]
pub(crate) mod partition {
    pub(crate) static FS: &'static str = "Fs";
    pub(crate) static SCP: &'static str = "SCP";
    pub(crate) static BUCKET: &'static str = "Bucket";
    pub(crate) static DATABASE: &'static str = "Database";
    pub(crate) static HISTORY: &'static str = "History";
    pub(crate) static PROCESS: &'static str = "Process";
    pub(crate) static LEDGER: &'static str = "Ledger";
    pub(crate) static OVERLAY: &'static str = "Overlay";
    pub(crate) static HERDER: &'static str = "Herder";
    pub(crate) static TX: &'static str = "Tx";
    pub(crate) static LOAD_GEN: &'static str = "LoadGen";
    pub(crate) static WORK: &'static str = "Work";
    pub(crate) static INVARIANT: &'static str = "Invariant";
    pub(crate) static PERF: &'static str = "Perf";
}

pub fn init_logging(maxLevel: LogLevel) -> Result<(), SetLoggerError> {
    let maxFilter: LevelFilter = {
        match maxLevel {
            LogLevel::LVL_ERROR => LevelFilter::Error,
            LogLevel::LVL_WARNING => LevelFilter::Warn,
            LogLevel::LVL_INFO => LevelFilter::Info,
            LogLevel::LVL_DEBUG => LevelFilter::Debug,
            LogLevel::LVL_TRACE => LevelFilter::Trace,
            _ => LevelFilter::Info,
        }
    };
    if HAVE_INITIALIZED
        .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
        .is_ok()
    {
        log::set_logger(&LOGGER)?;
    }
    log::set_max_level(maxFilter);
    Ok(())
}

pub(crate) fn is_tx_tracing_enabled() -> bool {
    let_cxx_string!(partition = partition::TX);
    // ignore errors from this, worst case we just turn off tracing.
    shim_isLogLevelAtLeast(&partition, LogLevel::LVL_TRACE).unwrap_or(false)
}

pub(crate) fn diff_line(last: &String, new: &String) -> String {
    last.split(',')
        .zip(new.split(','))
        .filter(|(a, b)| a != b)
        .map(|(_, b)| b)
        .join(",")
}

fn convertLogLevel(lvl: Level) -> LogLevel {
    match lvl {
        Level::Error => LogLevel::LVL_ERROR,
        Level::Warn => LogLevel::LVL_WARNING,
        Level::Info => LogLevel::LVL_INFO,
        Level::Debug => LogLevel::LVL_DEBUG,
        Level::Trace => LogLevel::LVL_TRACE,
    }
}

impl log::Log for StellarLogger {
    fn enabled(&self, metadata: &Metadata) -> bool {
        let_cxx_string!(partition = metadata.target());
        let level = convertLogLevel(metadata.level());
        // ignore errors from this, worst case we just don't log.
        shim_isLogLevelAtLeast(&partition, level).unwrap_or(false)
    }

    fn log(&self, record: &Record) {
        let_cxx_string!(partition = record.target());
        let level = convertLogLevel(record.level());
        let_cxx_string!(msg = record.args().to_string());
        // ignore errors from this, worst case we just don't log.
        shim_logAtPartitionAndLevel(&partition, level, &msg).unwrap_or(())
    }

    fn flush(&self) {}
}
