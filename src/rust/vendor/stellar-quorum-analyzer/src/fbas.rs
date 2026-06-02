use log::{trace, warn};
use petgraph::graph::{DiGraph, NodeIndex};
use std::{
    collections::{BTreeMap, BTreeSet},
    fmt::Debug,
    rc::Rc,
};
use stellar_xdr::{Limits, NodeId, PublicKey, ReadXdr, ScpQuorumSet};

use crate::{resource_limiter::ResourceLimiter, ResourceQuantity};

const QUORUM_SET_MAX_DEPTH: u32 = 4;

pub(crate) type QuorumSetMap = BTreeMap<String, Rc<InternalScpQuorumSet>>;

/// This is the internal representation of a quorum set. The Qset structure must
/// be explicitly specified (by validator's declaration). You can't say my inner
/// qset is "another validator's qset". Because of that, the `Qset` structure
/// cannot contain a cycle. This is different from transitive qset, which is
/// obtained by extending the all dependent validators by including their
/// dependent qsets (a validator can only depend on a single qsat, not other
/// validators). Such transitive structure is described by the graph in `Fbas`.
/// A leaf in a `Qset` can only contain 1. validator or 2. vacuous qset (qset
/// with a threshold but empty validator list and inner-qset).
#[derive(Debug, Clone, PartialEq, PartialOrd, Eq, Ord, Default)]
pub(crate) struct Qset {
    pub threshold: u32,
    // Stores index of validators that have been parsed and already exists in
    // the graph.
    pub validators: BTreeSet<NodeIndex>,
    // Stores index of qsets that have been parsed. Because the Qset is parsed
    // in depth-first manner and cannot contain cycles, this is possible.
    pub inner_qsets: BTreeSet<NodeIndex>,
}

/// Same as `ScpQuorumSet` except it identifies validators with String instead
/// of `NodeId`, because we want to make it easier for testing by allowing nodes
/// to be random strings instead of requiring valid stellar strkeys
#[derive(Clone, Debug, Hash, PartialEq, Eq, PartialOrd, Ord)]
pub(crate) struct InternalScpQuorumSet {
    pub threshold: u32,
    pub validators: Vec<String>,
    pub inner_sets: Vec<InternalScpQuorumSet>,
}

#[derive(Debug)]
pub(crate) enum Vertex {
    Validator(String),
    QSet(Qset),
}

impl Vertex {
    pub fn get_threshold(&self) -> u32 {
        match self {
            Vertex::Validator(_) => 1,
            Vertex::QSet(qset) => qset.threshold,
        }
    }
}

#[derive(Debug)]
pub enum FbasError {
    ParseError(&'static str),
    MaxDepthExceeded,
    XdrDecodingError(&'static str),
    InternalError(&'static str),
    ResourcelimitExceeded(ResourceQuantity),
}

impl std::error::Error for FbasError {}

impl std::fmt::Display for FbasError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            FbasError::ParseError(msg) => write!(f, "Parse error: {msg}"),
            FbasError::MaxDepthExceeded => write!(f, "Maximum quorum set depth exceeded"),
            FbasError::XdrDecodingError(msg) => write!(f, "XDR decoding error: {msg}"),
            FbasError::InternalError(msg) => write!(f, "Internal error (likely a bug): {msg}"),
            FbasError::ResourcelimitExceeded(resource_quantity) => write!(
                f,
                "Resource limits exceeded -- Time elapsed: {} ms, Memory usage: {} bytes",
                resource_quantity.time.as_millis(),
                resource_quantity.mem_bytes
            ),
        }
    }
}

impl From<ScpQuorumSet> for InternalScpQuorumSet {
    fn from(qset: ScpQuorumSet) -> Self {
        InternalScpQuorumSet {
            threshold: qset.threshold,
            validators: qset
                .validators
                .iter()
                .map(|node_id| match &node_id.0 {
                    PublicKey::PublicKeyTypeEd25519(key) => {
                        stellar_strkey::ed25519::PublicKey(key.0).to_string()
                    }
                })
                .collect(),
            inner_sets: qset
                .inner_sets
                .iter()
                .map(|qs| InternalScpQuorumSet::from(qs.clone()))
                .collect(),
        }
    }
}

#[derive(Default, Debug)]
pub(crate) struct Fbas {
    pub graph: DiGraph<Vertex, ()>,
    pub validators: Vec<NodeIndex>,
}

impl Fbas {
    fn add_validator(&mut self, v: String) -> NodeIndex {
        let idx = self.graph.add_node(Vertex::Validator(v));
        self.validators.push(idx);
        idx
    }

    pub(crate) fn try_get_validator_string(&self, ni: &NodeIndex) -> Result<String, FbasError> {
        match self.graph.node_weight(*ni) {
            Some(Vertex::Validator(v)) => Ok(v.clone()),
            _ => Err(FbasError::InternalError("Node index is not a validator")),
        }
    }

    fn from_quorum_set_map(
        qsm: QuorumSetMap,
        resource_limiter: &ResourceLimiter,
    ) -> Result<Self, FbasError> {
        let mut fbas = Fbas::default();
        let mut known_validators = BTreeMap::new();
        let mut known_qsets = BTreeMap::new();

        // First pass: add all validators
        for (node_str, _) in qsm.iter() {
            let idx = fbas.add_validator(node_str.clone());
            known_validators.insert(node_str, idx);
        }

        // Second pass: process quorum sets and create connections
        for (node_str, qset) in qsm.iter() {
            let v_idx = known_validators
                .get(node_str)
                .ok_or(FbasError::InternalError("key not found"))?;
            let q_idx = fbas.process_scp_quorum_set(
                qset,
                0,
                &known_validators,
                &mut known_qsets,
                resource_limiter,
            )?;
            let _ = fbas.graph.add_edge(*v_idx, q_idx, ());
        }

        trace!(
            target: "SCP",
            "#known validators {}, #graph nodes {}, #graph edges {}",
            known_validators.len(),
            fbas.graph.node_count(),
            fbas.graph.edge_count()
        );
        Ok(fbas)
    }

    fn process_scp_quorum_set(
        &mut self,
        qset: &InternalScpQuorumSet,
        curr_depth: u32,
        known_validators: &BTreeMap<&String, NodeIndex>,
        known_qsets: &mut BTreeMap<Qset, NodeIndex>,
        resource_limiter: &ResourceLimiter,
    ) -> Result<NodeIndex, FbasError> {
        resource_limiter.measure_and_enforce_limits()?;

        if curr_depth == QUORUM_SET_MAX_DEPTH {
            return Err(FbasError::MaxDepthExceeded);
        }

        let mut new_qset = Qset {
            threshold: qset.threshold,
            ..Default::default()
        };

        // Add validators
        for validator in &qset.validators {
            if let Some(&idx) = known_validators.get(validator) {
                new_qset.validators.insert(idx);
            } else {
                warn!(target: "SCP", "validator {} is unknown", validator);
            }
        }

        // Process inner quorum sets
        for inner_qset in &qset.inner_sets {
            let qidx = self.process_scp_quorum_set(
                inner_qset,
                curr_depth + 1,
                known_validators,
                known_qsets,
                resource_limiter,
            )?;
            new_qset.inner_qsets.insert(qidx);
        }

        // Create or reuse the quorum set node
        let idx = if let Some(&idx) = known_qsets.get(&new_qset) {
            idx
        } else {
            let idx = self.graph.add_node(Vertex::QSet(new_qset.clone()));
            known_qsets.insert(new_qset.clone(), idx);
            idx
        };

        // Add edges
        new_qset.validators.iter().for_each(|vi| {
            let _ = self.graph.update_edge(idx, *vi, ());
        });
        new_qset.inner_qsets.iter().for_each(|qi| {
            let _ = self.graph.update_edge(idx, *qi, ());
        });

        Ok(idx)
    }

    pub fn from_quorum_set_map_buf<T: AsRef<[u8]>, I: ExactSizeIterator<Item = T>>(
        nodes: I,
        quorum_sets: I,
        resource_limiter: &ResourceLimiter,
    ) -> Result<Self, FbasError> {
        if nodes.len() != quorum_sets.len() {
            return Err(FbasError::ParseError(
                "length in nodes and quorum_sets do not match",
            ));
        }

        let mut quorum_set_map = QuorumSetMap::new();

        for (node_buf, qset_buf) in nodes.zip(quorum_sets) {
            let node = NodeId::from_xdr(node_buf, Limits::none())
                .map_err(|_| FbasError::XdrDecodingError("NodeId cannot be decoded from xdr"))?;
            let node_str = match &node.0 {
                PublicKey::PublicKeyTypeEd25519(key) => {
                    stellar_strkey::ed25519::PublicKey(key.0).to_string()
                }
            };
            if !qset_buf.as_ref().is_empty() {
                let qset = ScpQuorumSet::from_xdr(qset_buf, Limits::none()).map_err(|_| {
                    FbasError::XdrDecodingError("ScpQuorumSet cannot be decoded from xdr")
                })?;
                quorum_set_map.insert(node_str, Rc::new(qset.into()));
            } else {
                warn!(target: "SCP", "Validator {}'s quorum set is empty", node_str);
            }
        }

        Self::from_quorum_set_map(quorum_set_map, resource_limiter)
    }

    #[cfg(any(feature = "json", test))]
    pub fn from_json_path(
        path: &str,
        resource_limiter: &ResourceLimiter,
    ) -> Result<Self, FbasError> {
        let quorum_set_map = crate::json_parser::quorum_set_map_from_json(path)?;
        Self::from_quorum_set_map(quorum_set_map, resource_limiter)
    }
}
