use crate::fbas::{FbasError, InternalScpQuorumSet, QuorumSetMap};
use json::{object::Object, JsonValue};
use std::{fs::File, io::Read, rc::Rc};

pub(crate) fn quorum_set_map_from_json(path: &str) -> Result<QuorumSetMap, FbasError> {
    let mut file = File::open(path).map_err(|_| FbasError::ParseError("fail to open file"))?;
    let mut data = String::new();
    file.read_to_string(&mut data)
        .map_err(|_| FbasError::ParseError("fail to read file"))?;
    let json_data =
        json::parse(&data).map_err(|_| FbasError::ParseError("fail to parse to json"))?;

    match json_data {
        JsonValue::Object(root) => try_parse_quorum_set_map_from_json_regular(root),
        JsonValue::Array(nodes) => try_parse_quorum_set_map_from_stellarbeats_json(nodes),
        _ => Err(FbasError::ParseError(
            "root is neither an object nor an array",
        )),
    }
}

fn try_parse_quorum_set_map_from_json_regular(root: Object) -> Result<QuorumSetMap, FbasError> {
    let nodes = match root.get("nodes") {
        Some(JsonValue::Array(nodes)) => nodes,
        _ => return Err(FbasError::ParseError("nodes field missing or not an array")),
    };

    let mut quorum_map = QuorumSetMap::new();
    for node in nodes {
        let node = match node {
            JsonValue::Object(n) => n,
            _ => return Err(FbasError::ParseError("node is not an object")),
        };

        let public_key = node
            .get("node")
            .and_then(|n| n.as_str())
            .ok_or(FbasError::ParseError("node field missing or not a string"))?
            .to_string();

        let qset = parse_internal_quorum_set(&node["qset"])?;
        quorum_map.insert(public_key, Rc::new(qset));
    }

    Ok(quorum_map)
}

fn parse_internal_quorum_set(json_qset: &JsonValue) -> Result<InternalScpQuorumSet, FbasError> {
    let threshold = json_qset["t"].as_u32().ok_or(FbasError::ParseError(
        "threshold field missing or not a number",
    ))?;

    let v = match &json_qset["v"] {
        JsonValue::Array(v) => v,
        _ => return Err(FbasError::ParseError("v field missing or not an array")),
    };

    let mut validators = vec![];
    let mut inner_sets = vec![];

    for item in v {
        match item {
            JsonValue::String(validator) => {
                validators.push(validator.to_string());
            }
            JsonValue::Object(obj) if obj.get("t").is_some() => {
                inner_sets.push(parse_internal_quorum_set(item)?);
            }
            _ => {
                return Err(FbasError::ParseError(
                    "validator entry must be either a string (PublicKey) or an object (QuorumSet)",
                ))
            }
        }
    }

    Ok(InternalScpQuorumSet {
        threshold,
        validators,
        inner_sets,
    })
}

fn parse_stellarbeats_internal_quorum_set(
    json_qset: &JsonValue,
) -> Result<InternalScpQuorumSet, FbasError> {
    let threshold = json_qset["threshold"]
        .as_u32()
        .ok_or(FbasError::ParseError(
            "threshold field missing or not a number",
        ))?;

    let mut validators = vec![];
    let mut inner_sets = vec![];

    match &json_qset["validators"] {
        JsonValue::Array(validator_arr) => {
            for validator in validator_arr {
                match validator.as_str() {
                    Some(validator_str) => validators.push(validator_str.to_string()),
                    None => return Err(FbasError::ParseError("validator entry must be a string")),
                }
            }
        }
        _ => {
            return Err(FbasError::ParseError(
                "validators field missing or not an array",
            ))
        }
    }

    match &json_qset["innerQuorumSets"] {
        JsonValue::Array(inner_arr) => {
            for inner_qset in inner_arr {
                inner_sets.push(parse_stellarbeats_internal_quorum_set(inner_qset)?);
            }
        }
        _ => {
            return Err(FbasError::ParseError(
                "innerQuorumSets field missing or not an array",
            ))
        }
    }

    Ok(InternalScpQuorumSet {
        threshold,
        validators,
        inner_sets,
    })
}

fn try_parse_quorum_set_map_from_stellarbeats_json(
    nodes: Vec<JsonValue>,
) -> Result<QuorumSetMap, FbasError> {
    let mut quorum_map = QuorumSetMap::new();
    for node in nodes {
        let node = match node {
            JsonValue::Object(n) => n,
            _ => return Err(FbasError::ParseError("node is not an object")),
        };

        let public_key = node
            .get("publicKey")
            .and_then(|n| n.as_str())
            .ok_or(FbasError::ParseError(
                "publicKey field missing or not a string",
            ))?
            .to_string();

        let qset = parse_stellarbeats_internal_quorum_set(&node["quorumSet"])?;
        quorum_map.insert(public_key, Rc::new(qset));
    }

    Ok(quorum_map)
}
