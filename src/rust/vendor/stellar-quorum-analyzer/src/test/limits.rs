use crate::{FbasAnalyzer, FbasError, ResourceLimiter, SolveStatus};
use std::{
    path::{Path, PathBuf},
};

fn assert_solver_limit_exceeded(res: Result<SolveStatus, FbasError>) -> bool {
    match res {
        Ok(_) => false,
        Err(e) => matches!(e, FbasError::ResourcelimitExceeded(_)),
    }
}

fn solve_unlimited(json_file: &Path) -> Result<SolveStatus, FbasError> {
    let mut solver = FbasAnalyzer::from_json_path(
        json_file.as_os_str().to_str().unwrap(),
        ResourceLimiter::unlimited(),
    )?;
    solver.solve()
}

fn solve_with_time_limit(json_file: &Path, time_limit_ms: u64) -> Result<SolveStatus, FbasError> {
    let mut solver = FbasAnalyzer::from_json_path(
        json_file.as_os_str().to_str().unwrap(),
        ResourceLimiter::new(time_limit_ms, usize::MAX),
    )?;
    solver.solve()
}

#[test]
fn test_time_limit() -> Result<(), Box<dyn std::error::Error>> {
    let json_file = PathBuf::from(
        "./tests/test_data/random/almost_symmetric_network_16_orgs_delete_prob_factor_3.json",
    );
    // first solve it without interruption, it should return `UNSAT`
    assert_eq!(solve_unlimited(&json_file)?, SolveStatus::UNSAT);

    // reaching time limit, it should fail gracefully with an FbasError
    assert_solver_limit_exceeded(solve_with_time_limit(&json_file, 1));
    Ok(())
}
