fn main() {
    // On Windows (build_rust.bat), soroban submodule revisions are written to
    // .soroban-revs before invoking cargo.  Watching this file causes cargo to
    // mark the crate dirty when a submodule changes, so it re-links against
    // the updated extern rlibs (which are passed via --extern after "--" and
    // not tracked by cargo's own dependency graph).
    //
    // On Unix (Makefile.am), the rebuild is instead driven by Make
    // prerequisites and -Cmetadata in RUSTFLAGS, so the file won't exist;
    // we only register it when it's actually present.
    if std::path::Path::new(".soroban-revs").exists() {
        println!("cargo:rerun-if-changed=.soroban-revs");
    }
}
