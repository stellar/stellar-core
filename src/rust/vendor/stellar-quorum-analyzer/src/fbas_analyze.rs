use crate::{
    fbas::{Fbas, FbasError},
    resource_limiter::ResourceLimiter,
};
use batsat::{interface::SolveResult, lbool, theory, Lit, Solver, SolverInterface, Var};
use itertools::Itertools;
use log::{trace, warn};
use petgraph::graph::NodeIndex;
use std::collections::BTreeMap;

// Two imaginary quorums A and B, and we have FBAS system with V vertices. Note
// that a vertex can be either a validator or a qset. The relation of each
// vertex being in each quorum is represented with one atomic variable (also
// known as a literal or `lit`). A `true` value indicates the vertex is in the
// quorum. Therefore the entire system has `2 * length(V)` such native atomics.
// Then we build a set of constraints that must be satisfied in order to fail
// the quorum intersection property (there only needs to be one such instance to
// disprove the property). We hand the constraints to the solver, which tries to
// find a configuration in atomics such that these constraints are satisfied,
// which is sufficient to disprove quorum intersection.
//
// The constraints are derived from the following formal requirements:
//
// 1. Both quorums must contain at least one validator
//
// 2. Quorums must be disjoint (no validator in common)
//
// 3. Each quorum must satisfy its dependencies. For each vertex i in the graph,
//    if i is in a quorum Q, then i must have a slice in Q (i.e., tᵢ successors
//    of i must be in Q).
//
// Before sending these constraints to a SAT solver, they must be transformed
// into Conjunctive Normal Form (CNF). This is done using the Tseitin
// transformation, which introduces additional auxiliary variables to handle
// complex logical expressions. The transformation converts the constraints into
// "ANDs of ORs" form, which is required by SAT solvers.
//
// Once the solver produces a satisfiable result (result == SAT), that means a
// pair of disjoint quorums has been found, disproving the quorum intersection
// property. An `UNSAT` result means all quorums intersect, i.e. it has been
// proven there are no disjoint quorums.

#[derive(Default)]
struct VarManager {
    // stores variables representing nodes in quorums A and B
    node_quorum_membership: BTreeMap<NodeIndex, (Var, Var)>,
}

impl VarManager {
    fn quorum_a(&self, ni: &NodeIndex) -> Result<Var, FbasError> {
        Ok(self
            .node_quorum_membership
            .get(ni)
            .ok_or(FbasError::InternalError("Node index not found"))?
            .0)
    }
    fn quorum_b(&self, ni: &NodeIndex) -> Result<Var, FbasError> {
        Ok(self
            .node_quorum_membership
            .get(ni)
            .ok_or(FbasError::InternalError("Node index not found"))?
            .1)
    }
    // constructs and returns a Lit representing the node's membership of quorum A
    pub fn lit_in_quorum_a(&self, ni: &NodeIndex, is_member: bool) -> Result<Lit, FbasError> {
        self.quorum_a(ni).map(|var| Lit::new(var, is_member))
    }
    // constructs and returns a Lit representing the node's membership of quorum B
    pub fn lit_in_quorum_b(&self, ni: &NodeIndex, is_member: bool) -> Result<Lit, FbasError> {
        self.quorum_b(ni).map(|var| Lit::new(var, is_member))
    }
}

pub struct FbasAnalyzer {
    fbas: Fbas,
    solver: Solver<ResourceLimiter>,
    status: SolveStatus,
    vars: VarManager,
}

#[derive(Clone, Default, PartialEq)]
pub enum SolveStatus {
    UNSAT,
    SAT((Vec<NodeIndex>, Vec<NodeIndex>)),
    #[default]
    UNKNOWN,
}

impl std::fmt::Debug for SolveStatus {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SolveStatus::UNSAT => write!(f, "UNSAT"),
            SolveStatus::SAT((quorum_a, quorum_b)) => {
                write!(f, "SAT(quorum_a: {quorum_a:?}, quorum_b: {quorum_b:?})")
            }
            SolveStatus::UNKNOWN => write!(f, "UNKNOWN"),
        }
    }
}

impl std::fmt::Display for SolveStatus {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        <Self as std::fmt::Debug>::fmt(self, f)
    }
}

impl FbasAnalyzer {
    pub fn from_quorum_set_map_buf<T: AsRef<[u8]>, I: ExactSizeIterator<Item = T>>(
        nodes: I,
        quorum_set: I,
        resource_limiter: ResourceLimiter,
    ) -> Result<Self, FbasError> {
        let fbas = Fbas::from_quorum_set_map_buf(nodes, quorum_set, &resource_limiter)?;
        Self::from_fbas(fbas, resource_limiter)
    }

    #[cfg(any(feature = "json", test))]
    pub fn from_json_path(
        path: &str,
        resource_limiter: ResourceLimiter,
    ) -> Result<Self, FbasError> {
        let fbas = Fbas::from_json_path(path, &resource_limiter)?;
        Self::from_fbas(fbas, resource_limiter)
    }

    pub(crate) fn from_fbas(
        fbas: Fbas,
        resource_limiter: ResourceLimiter,
    ) -> Result<Self, FbasError> {
        let mut analyzer = Self {
            fbas,
            solver: Solver::new(Default::default(), resource_limiter),
            status: SolveStatus::UNKNOWN,
            vars: VarManager::default(),
        };
        analyzer.construct_vars()?;
        analyzer.construct_formula()?;
        Ok(analyzer)
    }

    fn construct_vars(&mut self) -> Result<(), FbasError> {
        // For each vertex in the graph, we add a variable representing it
        // belonging to quorum A and quorum B.
        let node_count = self.fbas.graph.node_count();
        // sanity check so that we can use `[]` safely
        if node_count != self.fbas.graph.node_indices().len() {
            return Err(FbasError::InternalError(
                "node count does not match internal nodes length",
            ));
        }
        let vars = (0..2 * node_count)
            .map(|_| self.solver.new_var_default())
            .collect::<Vec<_>>();
        for (i, ni) in self.fbas.graph.node_indices().enumerate() {
            self.solver.cb().measure_and_enforce_limits()?;
            self.vars
                .node_quorum_membership
                .insert(ni, (vars[i], vars[i + node_count]));
        }
        Ok(())
    }

    fn add_clause_limited(
        solver: &mut Solver<ResourceLimiter>,
        clause: &mut Vec<Lit>,
    ) -> Result<bool, FbasError> {
        solver.cb().measure_and_enforce_limits()?;
        Ok(solver.add_clause_reuse(clause))
    }

    fn construct_formula(&mut self) -> Result<(), FbasError> {
        let fbas = &self.fbas;
        // vars representing quorum membership must be pre-constructed
        if self.solver.num_vars() as usize != fbas.graph.node_count() * 2 {
            return Err(FbasError::InternalError(
                "solver internal variables count does not match node count",
            ));
        }

        // formula 1: both quorums are non-empty -- at least one *validator* must
        // exist in each quorum
        let mut quorum_a_non_empty = fbas
            .validators
            .iter()
            .map(|ni| self.vars.lit_in_quorum_a(ni, true))
            .collect::<Result<Vec<Lit>, FbasError>>()?;
        Self::add_clause_limited(&mut self.solver, &mut quorum_a_non_empty)?;

        let mut quorum_b_non_empty = fbas
            .validators
            .iter()
            .map(|ni| self.vars.lit_in_quorum_b(ni, true))
            .collect::<Result<Vec<Lit>, FbasError>>()?;
        Self::add_clause_limited(&mut self.solver, &mut quorum_b_non_empty)?;

        // formula 2: two quorums do not intersect -- no *validator* can appear in
        // both quorums
        for ni in fbas.validators.iter() {
            Self::add_clause_limited(
                &mut self.solver,
                &mut vec![
                    self.vars.lit_in_quorum_a(ni, false)?,
                    self.vars.lit_in_quorum_b(ni, false)?,
                ],
            )?;
        }

        // formula 3: qset relation for each vertex must be satisfied. Variable
        // naming follows "Final formula encoding that A and B are quorums" in
        // `method.md`, assuming quorum A.
        let mut add_clauses_for_quorum_relations = |node_in_quorum: &dyn Fn(
            &NodeIndex,
            bool,
        ) -> Result<
            Lit,
            FbasError,
        >|
         -> Result<(), FbasError> {
            fbas.graph.node_indices().try_for_each(|n_i| {
                let threshold = fbas
                    .graph
                    .node_weight(n_i)
                    .ok_or(FbasError::InternalError("Node index not found"))?
                    .get_threshold();
                let successors = fbas.graph.neighbors(n_i);
                let comb_of_successors = successors.into_iter().combinations(threshold as usize);

                let mut first_term = Vec::with_capacity(comb_of_successors.size_hint().0 + 1);
                first_term.push(node_in_quorum(&n_i, false)?);
                for pi_i in comb_of_successors {
                    // Create a new variable as per Tseitin transformation for each
                    // combination. These are internal variables for facilitation of
                    // SAT solving. There is no need to store their indices.
                    let alpha_i_j = Lit::new(self.solver.new_var_default(), true);
                    // 1st term
                    first_term.push(alpha_i_j);

                    let mut third_term = Vec::with_capacity(pi_i.len() + 1);
                    third_term.push(alpha_i_j);
                    for n_k in pi_i.iter() {
                        // 2nd term
                        Self::add_clause_limited(
                            &mut self.solver,
                            &mut vec![!alpha_i_j, node_in_quorum(n_k, true)?],
                        )?;
                        // 3rd term
                        third_term.push(node_in_quorum(n_k, false)?);
                    }
                    Self::add_clause_limited(&mut self.solver, &mut third_term)?;
                }
                Self::add_clause_limited(&mut self.solver, &mut first_term)?;
                Ok(())
            })
        };
        add_clauses_for_quorum_relations(&|ni, is_member| {
            self.vars.lit_in_quorum_a(ni, is_member)
        })?;
        add_clauses_for_quorum_relations(&|ni, is_member| {
            self.vars.lit_in_quorum_b(ni, is_member)
        })?;

        trace!(
            target: "SCP",
            "FbasAnalyzer num_vars = {}, num_clauses = {}",
            self.solver.num_vars(),
            self.solver.num_clauses()
        );
        Ok(())
    }

    pub fn solve(&mut self) -> Result<SolveStatus, FbasError> {
        let mut th = theory::EmptyTheory::new();
        // Note on resource limiting: the solver checks `ResourceLimiter::stop()` internally
        // on its inner loop. If resource limits are exceeds, it will discontinue and return
        // `SolveStatus::UNKNOWN`.
        // In order for the solver to return a `ResourcelimitExceeded` error, we need to
        // enforce the limit before returning.
        let resource_limiter = self.solver.cb().clone();
        let result = self.solver.solve_limited_th_full(&mut th, &[]);
        self.status = match result {
            SolveResult::Sat(model) => {
                let mut quorum_a = vec![];
                let mut quorum_b = vec![];
                for ni in self.fbas.validators.iter() {
                    let la = self.vars.lit_in_quorum_a(ni, true)?;
                    if model.value_lit(la) == lbool::TRUE {
                        quorum_a.push(*ni);
                    }
                    let lb = self.vars.lit_in_quorum_b(ni, true)?;
                    if model.value_lit(lb) == lbool::TRUE {
                        quorum_b.push(*ni);
                    }
                }
                warn!(
                    target: "SCP",
                    "FbasAnalyzer found quorum split! quorum A: {:?}, quorum B: {:?}",
                    quorum_a,
                    quorum_b
                );
                Ok(SolveStatus::SAT((quorum_a, quorum_b)))
            }
            SolveResult::Unsat(_) => Ok(SolveStatus::UNSAT),
            // most likely the resource limits have been exceeded
            SolveResult::Unknown(_) => Ok(SolveStatus::UNKNOWN),
        }?;
        // enforce the limit (produce `Err(ResourcelimitExceeded)` if needed) before returning
        resource_limiter.measure_and_enforce_limits()?;
        Ok(self.status.clone())
    }

    pub fn get_potential_split(&self) -> Result<(Vec<String>, Vec<String>), FbasError> {
        match &self.status {
            // Note: the model returns one valid potential split, there is no
            // guaruantee which one (there can be many permutations of the same
            // split), make sure to check the content of the result to see if
            // it's expected.
            SolveStatus::SAT((quorum_a, quorum_b)) => {
                let qa_strings = quorum_a
                    .iter()
                    .map(|ni| self.fbas.try_get_validator_string(ni))
                    .collect::<Result<Vec<_>, _>>()?;
                let qb_strings = quorum_b
                    .iter()
                    .map(|ni| self.fbas.try_get_validator_string(ni))
                    .collect::<Result<Vec<_>, _>>()?;
                Ok((qa_strings, qb_strings))
            }
            _ => Ok((vec![], vec![])),
        }
    }
}
