use super::rust_bridge::{CxxBuf, QuorumCheckerResource, QuorumCheckerStatus, QuorumSplit};
use stellar_quorum_analyzer::{FbasAnalyzer, FbasError, ResourceLimiter, SolveStatus};

#[allow(dead_code)]
#[derive(Debug)]
pub(crate) enum QuorumCheckerError {
    Fbas(FbasError),
    General(String),
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
            stellar_quorum_analyzer::SolveStatus::UNSAT => QuorumCheckerStatus::UNSAT,
            stellar_quorum_analyzer::SolveStatus::SAT(_) => QuorumCheckerStatus::SAT,
            stellar_quorum_analyzer::SolveStatus::UNKNOWN => QuorumCheckerStatus::UNKNOWN,
        }
    }
}

fn update_resource_usage(
    resource_limiter: &ResourceLimiter,
    resource_usage: &mut QuorumCheckerResource,
) {
    *resource_usage = QuorumCheckerResource {
        time_ms: resource_limiter.get_time_ms(),
        mem_bytes: resource_limiter.get_mem_bytes(),
    };
}

pub(crate) fn network_enjoys_quorum_intersection(
    nodes: &Vec<CxxBuf>,
    quorum_set: &Vec<CxxBuf>,
    potential_split: &mut QuorumSplit,
    resource_limit: &QuorumCheckerResource,
    resource_usage: &mut QuorumCheckerResource,
) -> Result<QuorumCheckerStatus, Box<dyn std::error::Error>> {
    // The panic handler catches any unwind-able panics, and convert it to an
    // error (which will be converted to a C++ exception). It **cannot** catch
    // an abort, which can happen if the `FbasAnalyzer` program exceeds the
    // memory limit.
    //
    // Making the memory limit a hard limit is a design choice, because we want
    // the memory usage of the process running quorum checker to be isolated
    // from the main stellar-core running process.
    let res = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let resource_limiter =
            ResourceLimiter::new(resource_limit.time_ms, resource_limit.mem_bytes);
        let mut solver = match FbasAnalyzer::from_quorum_set_map_buf(
            nodes.iter(),
            quorum_set.iter(),
            resource_limiter.clone(),
        ) {
            Ok(solver) => solver,
            Err(e) => {
                update_resource_usage(&resource_limiter, resource_usage);
                return Err(e.into());
            }
        };
        let status = match solver.solve() {
            Ok(status) => status,
            Err(e) => {
                update_resource_usage(&resource_limiter, resource_usage);
                return Err(e.into());
            }
        };
        let (left, right) = match solver.get_potential_split() {
            Ok(split) => split,
            Err(e) => {
                update_resource_usage(&resource_limiter, resource_usage);
                return Err(e.into());
            }
        };
        update_resource_usage(&resource_limiter, resource_usage);
        potential_split.left = left;
        potential_split.right = right;
        Ok(status.into())
    }));
    match res {
        Err(r) => {
            if let Some(s) = r.downcast_ref::<String>() {
                Err(QuorumCheckerError::General(format!("solver panicked: {s}")).into())
            } else if let Some(s) = r.downcast_ref::<&'static str>() {
                Err(QuorumCheckerError::General(format!("solver panicked: {s}")).into())
            } else {
                Err(QuorumCheckerError::General("solver panicked".into()).into())
            }
        }
        Ok(r) => r,
    }
}
