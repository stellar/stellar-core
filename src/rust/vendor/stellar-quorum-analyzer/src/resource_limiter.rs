use crate::FbasError;
use batsat::{
    callbacks::{Callbacks, ProgressStatus},
    lbool,
};
use log::{error, trace};
use std::{
    cell::RefCell,
    rc::Rc,
    time::{Duration, Instant},
};

#[derive(Clone, Debug, Copy)]
pub struct ResourceQuantity {
    pub time: Duration,
    pub mem_bytes: usize,
}

impl ResourceQuantity {
    pub fn zero() -> Self {
        ResourceQuantity {
            time: Duration::ZERO,
            mem_bytes: 0,
        }
    }

    pub fn new(time_ms: u64, mem_bytes: usize) -> Self {
        ResourceQuantity {
            time: Duration::from_millis(time_ms),
            mem_bytes,
        }
    }

    pub fn exceeds(&self, limit: &Self) -> bool {
        self.time > limit.time || self.mem_bytes > limit.mem_bytes
    }
}

/// An implementation of the `Callbacks` trait that limits solver runtime.
///
/// The memory field is retained for API compatibility but currently remains at
/// zero and is not enforced.

#[derive(Debug)]
pub(crate) struct ResourceLimiterImpl {
    start_time: Instant,
    limits: ResourceQuantity,
    current_usage: ResourceQuantity,
}

#[derive(Debug, Clone)]
pub struct ResourceLimiter(Rc<RefCell<ResourceLimiterImpl>>);

impl ResourceLimiterImpl {
    pub fn new(time_limit_ms: u64, global_mem_limit_bytes: usize) -> Self {
        Self {
            start_time: Instant::now(),
            limits: ResourceQuantity::new(time_limit_ms, global_mem_limit_bytes),
            current_usage: ResourceQuantity::zero(),
        }
    }

    fn measure(&mut self, verbose: bool) {
        let time = self.start_time.elapsed();
        let mem_bytes = 0;
        if verbose {
            trace!( target: "SCP",
                "Time elapsed: {} ms, Time limit: {} ms; Memory usage: {} bytes, Memory limit: {} bytes",
                self.current_usage.time.as_millis(), self.limits.time.as_millis(), self.current_usage.mem_bytes, self.limits.mem_bytes
            );
        }
        self.current_usage = ResourceQuantity { time, mem_bytes };
    }

    fn measure_and_enforce_limits(&mut self) -> Result<(), FbasError> {
        self.measure(false);
        if self.current_usage.exceeds(&self.limits) {
            error!( target: "SCP",
                "Resource limits exceeded -- Time elapsed: {} ms, Time limit: {} ms; Memory usage: {} bytes, Memory limit: {} bytes",
                self.current_usage.time.as_millis(), self.limits.time.as_millis(), self.current_usage.mem_bytes, self.limits.mem_bytes
            );
            return Err(FbasError::ResourcelimitExceeded(self.current_usage));
        }
        Ok(())
    }
}

impl ResourceLimiter {
    pub fn new(time_limit_ms: u64, global_mem_limit_bytes: usize) -> Self {
        Self(Rc::new(RefCell::new(ResourceLimiterImpl::new(
            time_limit_ms,
            global_mem_limit_bytes,
        ))))
    }

    #[cfg(test)]
    pub(crate) fn unlimited() -> Self {
        Self(Rc::new(RefCell::new(ResourceLimiterImpl::new(
            u64::MAX,
            usize::MAX,
        ))))
    }

    pub(crate) fn measure(&self, verbose: bool) {
        self.0.borrow_mut().measure(verbose);
    }

    pub fn measure_and_enforce_limits(&self) -> Result<(), FbasError> {
        self.0.borrow_mut().measure_and_enforce_limits()
    }

    pub fn get_time_ms(&self) -> u64 {
        self.0.borrow().current_usage.time.as_millis() as u64
    }

    pub fn get_mem_bytes(&self) -> usize {
        self.0.borrow().current_usage.mem_bytes
    }
}

impl Callbacks for ResourceLimiter {
    fn on_start(&mut self) {
        self.measure(true);
        trace!( target: "SCP",
            "c ====================================[ Search Statistics ]===================================="
        );
        trace!( target: "SCP",
            "c | Conflicts |          ORIGINAL         |          LEARNT          |        Resource        |"
        );
        trace!( target: "SCP",
            "c |           |    Vars  Clauses Literals |    Limit  Clauses Lit/Cl | Time(ms) Memory(bytes) |"
        );
        trace!( target: "SCP",
            "c ============================================================================================="
        );
    }

    fn on_result(&mut self, _: lbool) {
        trace!( target: "SCP",
            "c ============================================================================================="
        );
        self.measure(true);
    }

    fn on_progress<F>(&mut self, p: F)
    where
        F: FnOnce() -> ProgressStatus,
    {
        self.measure(false);
        let current = self.0.borrow().current_usage;
        let p = p();
        trace!( target: "SCP",
            "c | {:9} | {:7} {:8} {:8} | {:8} {:8} {:6.0} | {:8} {:13} |",
            p.conflicts,
            p.dec_vars,
            p.n_clauses,
            p.n_clause_lits,
            p.max_learnt,
            p.n_learnt,
            p.n_learnt_lits,
            current.time.as_millis(),
            current.mem_bytes
        );
    }

    fn on_gc(&mut self, old: usize, new: usize) {
        trace!( target: "SCP",
            "|  Garbage collection:   {:12} bytes => {:12} bytes             |",
            old, new
        );
    }

    fn stop(&self) -> bool {
        self.measure_and_enforce_limits().is_err()
    }
}
