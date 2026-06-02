use crate::{FbasAnalyzer, ResourceLimiter, SolveStatus};
use std::collections::BTreeMap;

#[test]
fn test() -> Result<(), Box<dyn std::error::Error>> {
    let expected_results: BTreeMap<&str, bool> = BTreeMap::from([
        ("missing_1", false),
        ("circular_2", false),
        ("validators_broken_1", false),
        ("circular_1", false),
        ("top_tier", false),
        ("conflicted_2", true),
        ("homedomain_test_1", false),
        ("conflicted_3", true),
        ("conflicted", true),
    ]);

    for entry in std::fs::read_dir("./tests/test_data/")? {
        let path = entry?.path();
        if let Some(extension) = path.extension() {
            if extension == "json" {
                let case_name = path.file_stem().unwrap().to_str().unwrap();
                let expected_sat = expected_results.get(case_name).unwrap_or_else(|| {
                    panic!("No expected result found for test case: {case_name}")
                });

                let mut solver = FbasAnalyzer::from_json_path(
                    path.as_os_str().to_str().unwrap(),
                    ResourceLimiter::unlimited(),
                )?;

                let res = solver.solve()?;
                let actual_sat = matches!(res, SolveStatus::SAT(_));

                assert_eq!(
                    actual_sat, *expected_sat,
                    "Case {case_name} failed: expected {expected_sat}, got {actual_sat}"
                );

                // Print the split if one was found
                if actual_sat {
                    let (qa, qb) = solver.get_potential_split()?;
                    println!("\nFound quorum split for {case_name}:");
                    println!("Quorum A:");
                    for validator in &qa {
                        println!("  - {validator}");
                    }
                    println!("\nQuorum B:");
                    for validator in &qb {
                        println!("  - {validator}");
                    }
                    println!();
                }
            }
        }
    }
    Ok(())
}

#[test]
fn test_random_data() -> Result<(), Box<dyn std::error::Error>> {
    let expected_results: BTreeMap<&str, bool> = BTreeMap::from([
        ("almost_symmetric_network_10_orgs_", false),
        ("almost_symmetric_network_12_orgs_", false),
        (
            "almost_symmetric_network_12_orgs_delete_prob_factor_1",
            false,
        ),
        (
            "almost_symmetric_network_12_orgs_delete_prob_factor_10",
            false,
        ),
        (
            "almost_symmetric_network_12_orgs_delete_prob_factor_11",
            true,
        ),
        (
            "almost_symmetric_network_12_orgs_delete_prob_factor_2",
            false,
        ),
        (
            "almost_symmetric_network_12_orgs_delete_prob_factor_3",
            false,
        ),
        (
            "almost_symmetric_network_12_orgs_delete_prob_factor_4",
            false,
        ),
        (
            "almost_symmetric_network_12_orgs_delete_prob_factor_5",
            false,
        ),
        (
            "almost_symmetric_network_12_orgs_delete_prob_factor_6",
            false,
        ),
        (
            "almost_symmetric_network_12_orgs_delete_prob_factor_7",
            false,
        ),
        (
            "almost_symmetric_network_12_orgs_delete_prob_factor_8",
            false,
        ),
        (
            "almost_symmetric_network_12_orgs_delete_prob_factor_9",
            false,
        ),
        (
            "almost_symmetric_network_13_orgs_delete_prob_factor_1",
            false,
        ),
        (
            "almost_symmetric_network_13_orgs_delete_prob_factor_10",
            false,
        ),
        (
            "almost_symmetric_network_13_orgs_delete_prob_factor_11",
            true,
        ),
        (
            "almost_symmetric_network_13_orgs_delete_prob_factor_12",
            true,
        ),
        (
            "almost_symmetric_network_13_orgs_delete_prob_factor_2",
            false,
        ),
        (
            "almost_symmetric_network_13_orgs_delete_prob_factor_3",
            false,
        ),
        (
            "almost_symmetric_network_13_orgs_delete_prob_factor_4",
            false,
        ),
        (
            "almost_symmetric_network_13_orgs_delete_prob_factor_5",
            false,
        ),
        (
            "almost_symmetric_network_13_orgs_delete_prob_factor_6",
            false,
        ),
        (
            "almost_symmetric_network_13_orgs_delete_prob_factor_7",
            false,
        ),
        (
            "almost_symmetric_network_13_orgs_delete_prob_factor_8",
            false,
        ),
        (
            "almost_symmetric_network_13_orgs_delete_prob_factor_9",
            false,
        ),
        (
            "almost_symmetric_network_14_orgs_delete_prob_factor_1",
            false,
        ),
        (
            "almost_symmetric_network_14_orgs_delete_prob_factor_10",
            false,
        ),
        (
            "almost_symmetric_network_14_orgs_delete_prob_factor_11",
            false,
        ),
        (
            "almost_symmetric_network_14_orgs_delete_prob_factor_12",
            false,
        ),
        (
            "almost_symmetric_network_14_orgs_delete_prob_factor_13",
            false,
        ),
        (
            "almost_symmetric_network_14_orgs_delete_prob_factor_2",
            false,
        ),
        (
            "almost_symmetric_network_14_orgs_delete_prob_factor_3",
            false,
        ),
        (
            "almost_symmetric_network_14_orgs_delete_prob_factor_4",
            false,
        ),
        (
            "almost_symmetric_network_14_orgs_delete_prob_factor_5",
            false,
        ),
        (
            "almost_symmetric_network_14_orgs_delete_prob_factor_6",
            false,
        ),
        (
            "almost_symmetric_network_14_orgs_delete_prob_factor_7",
            false,
        ),
        (
            "almost_symmetric_network_14_orgs_delete_prob_factor_8",
            false,
        ),
        (
            "almost_symmetric_network_14_orgs_delete_prob_factor_9",
            false,
        ),
        ("almost_symmetric_network_16_orgs_", false),
        (
            "almost_symmetric_network_16_orgs_delete_prob_factor_1",
            false,
        ),
        (
            "almost_symmetric_network_16_orgs_delete_prob_factor_10",
            false,
        ),
        (
            "almost_symmetric_network_16_orgs_delete_prob_factor_11",
            false,
        ),
        (
            "almost_symmetric_network_16_orgs_delete_prob_factor_12",
            false,
        ),
        (
            "almost_symmetric_network_16_orgs_delete_prob_factor_13",
            false,
        ),
        (
            "almost_symmetric_network_16_orgs_delete_prob_factor_14",
            false,
        ),
        (
            "almost_symmetric_network_16_orgs_delete_prob_factor_15",
            true,
        ),
        (
            "almost_symmetric_network_16_orgs_delete_prob_factor_2",
            false,
        ),
        (
            "almost_symmetric_network_16_orgs_delete_prob_factor_3",
            false,
        ),
        (
            "almost_symmetric_network_16_orgs_delete_prob_factor_4",
            false,
        ),
        (
            "almost_symmetric_network_16_orgs_delete_prob_factor_5",
            false,
        ),
        (
            "almost_symmetric_network_16_orgs_delete_prob_factor_6",
            false,
        ),
        (
            "almost_symmetric_network_16_orgs_delete_prob_factor_7",
            false,
        ),
        (
            "almost_symmetric_network_16_orgs_delete_prob_factor_8",
            false,
        ),
        (
            "almost_symmetric_network_16_orgs_delete_prob_factor_9",
            false,
        ),
        ("almost_symmetric_network_2_orgs_", false),
        (
            "almost_symmetric_network_5_orgs_delete_prob_factor_1",
            false,
        ),
        (
            "almost_symmetric_network_5_orgs_delete_prob_factor_2",
            false,
        ),
        (
            "almost_symmetric_network_5_orgs_delete_prob_factor_3",
            false,
        ),
        ("almost_symmetric_network_5_orgs_delete_prob_factor_4", true),
        (
            "almost_symmetric_network_6_orgs_delete_prob_factor_1",
            false,
        ),
        (
            "almost_symmetric_network_6_orgs_delete_prob_factor_2",
            false,
        ),
        (
            "almost_symmetric_network_6_orgs_delete_prob_factor_3",
            false,
        ),
        (
            "almost_symmetric_network_6_orgs_delete_prob_factor_4",
            false,
        ),
        (
            "almost_symmetric_network_6_orgs_delete_prob_factor_5",
            false,
        ),
        ("almost_symmetric_network_8_orgs_", false),
        (
            "almost_symmetric_network_8_orgs_delete_prob_factor_1",
            false,
        ),
        (
            "almost_symmetric_network_8_orgs_delete_prob_factor_2",
            false,
        ),
    ]);

    for entry in std::fs::read_dir("./tests/test_data/random/")? {
        let path = entry?.path();
        if let Some(extension) = path.extension() {
            if extension == "json" {
                let case_name = path.file_stem().unwrap().to_str().unwrap();
                let expected_sat = expected_results.get(case_name).unwrap_or_else(|| {
                    panic!("No expected result found for test case: {case_name}")
                });

                let mut solver = FbasAnalyzer::from_json_path(
                    path.as_os_str().to_str().unwrap(),
                    ResourceLimiter::unlimited(),
                )?;

                let res = solver.solve()?;
                let actual_sat = matches!(res, SolveStatus::SAT(_));

                assert_eq!(
                    actual_sat, *expected_sat,
                    "Case {case_name} failed: expected {expected_sat}, got {actual_sat}"
                );

                // Print the split if one was found
                if actual_sat {
                    let (qa, qb) = solver.get_potential_split()?;
                    println!("\nFound quorum split for {case_name}:");
                    println!("Quorum A:");
                    for validator in &qa {
                        println!("  - {validator}");
                    }
                    println!("\nQuorum B:");
                    for validator in &qb {
                        println!("  - {validator}");
                    }
                    println!();
                }
            }
        }
    }
    Ok(())
}
