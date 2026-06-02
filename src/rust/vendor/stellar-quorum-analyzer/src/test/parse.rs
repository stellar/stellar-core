use crate::json_parser::quorum_set_map_from_json;
use std::str::FromStr;
use stellar_strkey::ed25519::PublicKey as StrKeyPublicKey;

#[test]
fn test_parse_quorum_set_map_from_json() {
    let quorum_map = quorum_set_map_from_json("./tests/test_data/random/for_stellar_core/almost_symmetric_network_6_orgs_delete_prob_factor_3_for_stellar_core.json").unwrap();
    assert_eq!(quorum_map.len(), 18);

    // Test parsing of a specific node's quorum set
    let test_key =
        StrKeyPublicKey::from_str("GARHWC6Y4WNGLKCAC7SCFFLEV5GKTKB2AHVIA6C7SU5WLJTDW5W3MPHX")
            .unwrap();
    let test_node_id = test_key.to_string();

    let test_qset = quorum_map.get(&test_node_id).unwrap();
    assert_eq!(test_qset.threshold, 3);
    assert_eq!(test_qset.inner_sets.len(), 3);

    // Verify first inner set in detail
    let first_inner = &test_qset.inner_sets[0];
    assert_eq!(first_inner.threshold, 2);
    assert_eq!(first_inner.validators.len(), 3);

    // Check specific validator IDs in first inner set
    let expected_validators = [
        "GARHWC6Y4WNGLKCAC7SCFFLEV5GKTKB2AHVIA6C7SU5WLJTDW5W3MPHX",
        "GCJIDPIMNOJU4PASPDEHKLQWG2KAM45NNAUEQVY33XMYGAMSYICOK4H4",
        "GDRTHCOC6K6GAT3LNO7L2PQZHM7E3JDUHHSNRKSTH53A2AHBM6WZOUOC",
    ];

    for (i, expected_key) in expected_validators.iter().enumerate() {
        assert_eq!(&first_inner.validators[i], expected_key);
    }
}

#[test]
fn test_parse_quorum_set_map_from_stellarbeats_json() {
    let quorum_map = quorum_set_map_from_json("./tests/test_data/top_tier.json").unwrap();

    // Test parsing of a specific node's quorum set
    let test_key =
        StrKeyPublicKey::from_str("GD6SZQV3WEJUH352NTVLKEV2JM2RH266VPEM7EH5QLLI7ZZAALMLNUVN")
            .unwrap();
    let test_node_id = test_key.to_string();

    let test_qset = quorum_map.get(&test_node_id).unwrap();
    assert_eq!(test_qset.threshold, 5);
    assert!(test_qset.validators.is_empty());
    assert_eq!(test_qset.inner_sets.len(), 7);

    // Verify first inner set
    let first_inner = &test_qset.inner_sets[0];
    assert_eq!(first_inner.threshold, 2);
    assert_eq!(first_inner.validators.len(), 3);
    assert!(first_inner.inner_sets.is_empty());

    // Check specific validator in first inner set
    let expected_validator = "GAAV2GCVFLNN522ORUYFV33E76VPC22E72S75AQ6MBR5V45Z5DWVPWEU";
    assert_eq!(&first_inner.validators[0], expected_validator);
}
