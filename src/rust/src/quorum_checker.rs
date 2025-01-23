use super::{
    rust_bridge::{CxxBuf, QuorumSplit},
    QuorumCheckerStatus,
};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};
use stellar_quorum_analyzer::{Callbacks, FbasAnalyzer, FbasError, SolveStatus};

#[allow(dead_code)]
#[derive(Debug)]
pub(crate) enum QuorumCheckerError {
    Fbas(FbasError),
    General(&'static str),
}

impl std::fmt::Display for QuorumCheckerError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}", self)
    }
}

impl From<FbasError> for QuorumCheckerError {
    fn from(h: FbasError) -> Self {
        QuorumCheckerError::Fbas(h)
    }
}

impl std::error::Error for QuorumCheckerError {}

impl From<SolveStatus> for QuorumCheckerStatus {
    fn from(ss: SolveStatus) -> Self {
        match ss {
            stellar_quorum_analyzer::SolveStatus::SAT(_) => QuorumCheckerStatus::SAT,
            stellar_quorum_analyzer::SolveStatus::UNSAT => QuorumCheckerStatus::UNSAT,
            stellar_quorum_analyzer::SolveStatus::UNKNOWN => QuorumCheckerStatus::UNKNOWN,
        }
    }
}

/// [`Callbacks`] that allow the solver to be asynchronously interrupted
#[derive(Clone)]
pub struct Interrupt(Arc<AtomicBool>);

impl Callbacks for Interrupt {
    fn stop(&self) -> bool {
        self.0.load(Ordering::SeqCst)
    }
}

impl Default for Interrupt {
    fn default() -> Self {
        Interrupt(Arc::new(AtomicBool::new(false)))
    }
}

impl Interrupt {
    pub fn fire(&self) {
        self.0.store(true, Ordering::SeqCst)
    }

    pub fn reset(&self) {
        self.0.store(false, Ordering::SeqCst)
    }
}

pub(crate) fn new_interrupt() -> Box<Interrupt> {
    Box::new(Interrupt::default())
}

pub(crate) fn network_enjoys_quorum_intersection(
    nodes: &Vec<CxxBuf>,
    quorum_set: &Vec<CxxBuf>,
    interrupt: &Interrupt,
    potential_split: &mut QuorumSplit,
) -> Result<QuorumCheckerStatus, Box<dyn std::error::Error>> {
    let res = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let mut solver = FbasAnalyzer::from_quorum_set_map_buf(
            nodes.iter(),
            quorum_set.iter(),
            interrupt.clone(),
        )?;
        let status = solver.solve();
        let (left, right) = solver.get_potential_split()?;
        potential_split.left = left;
        potential_split.right = right;
        Ok(status.into())
    }));
    match res {
        Err(_) => Err(QuorumCheckerError::General("solver panicked").into()),
        Ok(r) => r,
    }
}
