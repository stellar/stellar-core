#!/bin/sh
#
# This file should be run if you want to install the exact version of Rust that
# stellar-core is building its packages and testing with.
#
# You are of course welcome to install your own version of Rust, but this file
# is version-controlled and run in CI to make the dependency explicit (and to
# use a version newer than the debian packages).

# Fail on any error
set -e

# Print each step as we go
set -x

# Extracts the current release-channel (which in our case is a specific rust
# version) that we want to install. This value will change relatively often, as
# new Rust _compilers_ are released, but you should only have to change it in
# rust-toolchain.toml. This file will pick it up automatically.
RUST_VERSION=$(perl -ne 'if (/channel\s+=\s+"(\d+\.\d+)"/) { print $1 }' rust-toolchain.toml)

# The host triple should basically never change. But just in case.
HOST_TRIPLE=x86_64-unknown-linux-gnu

# This is the SHA256 if the rustup-init binary (which is the same as rustup --
# it renames itself) and should be retrieved from a trusted source (eg. the rust
# website and/or by running sha256sum on a local copy of rustup you believe to
# be legitimate). The canonical URL for the SHA256 checksum provided here is:
#
# https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/rustup-init.sha256
#
# Rustup is an installer, not the compiler, and the installer changes fairly
# rarely, often a year between releases or more. This checksum will only need
# to be updated when there's a new rustup (installer) release.
RUSTUP_SHA256=5cc9ffd1026e82e7fb2eec2121ad71f4b0f044e88bca39207b3f6b769aaa799c

# We download rustup-init from a URL adjacent to the SHA256 file above, and
# check that it matches the expected SHA256 wired-in to this file, and then run
# it.
#
# Rustup with then install Rust binary components (compiler, stdlib, etc.) and
# check their PGP signatures match the Rust project's signing key (the signing
# key is embedded in rustup).
rm -f rustup-init
curl --fail --output rustup-init "https://static.rust-lang.org/rustup/dist/${HOST_TRIPLE}/rustup-init"
echo "${RUSTUP_SHA256} rustup-init" | sha256sum --check
chmod 0755 rustup-init
./rustup-init -y --verbose --profile default --default-host "${HOST_TRIPLE}" --default-toolchain "${RUST_VERSION}"
