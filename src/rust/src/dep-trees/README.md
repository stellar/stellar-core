This directory contains expected dependency trees for the different soroban
hosts linked into stellar-core. During a build the makefile will extract
the actual dependency tree of the configured soroban-env-host from their
submodule lockfiles and compare them. If they differ you need to decide
how to fix it: either update the expectation, or roll back the change
in the submodule.