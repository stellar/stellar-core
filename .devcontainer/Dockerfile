#-------------------------------------------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License. See https://go.microsoft.com/fwlink/?linkid=2090316 for license information.
#-------------------------------------------------------------------------------------------------------------
FROM ubuntu:noble

# Avoid warnings by switching to noninteractive
ENV DEBIAN_FRONTEND=noninteractive

# This Dockerfile adds a non-root 'dev' user with sudo access. However, for Linux,
# this user's GID/UID must match your local user UID/GID to avoid permission issues
# with bind mounts. Update USER_UID / USER_GID if yours is not 1000. See
# https://aka.ms/vscode-remote/containers/non-root-user for details.
ARG USERNAME=dev
ARG USER_UID=1000
ARG USER_GID=$USER_UID

# setup apt / certificates
RUN apt-get update \
    && apt-get -y install --no-install-recommends apt-utils dialog ca-certificates 2>&1

# use apt mirror instead of default archives if specified
# to use, specify the build arg or as an env var on the host machine
# e.g.:
#   mirror://mirrors.ubuntu.com/mirrors.txt
#   mirror://mirrors.ubuntu.com/<country-code>.txt
#   http://<country-code>.archive.ubuntu.com/ubuntu/
#   http://<aws-region>.ec2.archive.ubuntu.com/ubuntu
ARG APT_MIRROR=
RUN if [ ! -z "${APT_MIRROR}" ]; then \
    sed -i \
        -e "s|http://archive.ubuntu.com/ubuntu/|${APT_MIRROR}|" \
        -e "s|http://security.ubuntu.com/ubuntu/|${APT_MIRROR}|" \
        /etc/apt/sources.list.d/ubuntu.sources \
    ; fi \
    ; grep "^[^#;]" /etc/apt/sources.list.d/ubuntu.sources

# install base container packages and prep for VSCode
RUN apt-get update \
    # Verify process tools, lsb-release (common in install instructions for CLIs) installed
    && apt-get -y install iproute2 procps lsb-release \
    #
    # Create a non-root user to use if preferred - see https://aka.ms/vscode-remote/containers/non-root-user.
    # If group/user with the specified GID/UID already exists, rename them to $USERNAME.
    && if getent group $USER_GID > /dev/null 2>&1; then \
           groupmod -n $USERNAME $(getent group $USER_GID | cut -d: -f1); \
       else \
           groupadd --gid $USER_GID $USERNAME; \
       fi \
    && if id -u $USER_UID > /dev/null 2>&1; then \
           existing_user=$(getent passwd $USER_UID | cut -d: -f1); \
           usermod -l $USERNAME -d /home/$USERNAME -m -s /bin/bash $existing_user; \
       else \
           useradd -s /bin/bash --uid $USER_UID --gid $USER_GID -m $USERNAME; \
       fi \
    # [Optional] Add sudo support for the non-root user
    && apt-get install -y sudo \
    && echo $USERNAME ALL=\(root\) NOPASSWD:ALL > /etc/sudoers.d/$USERNAME\
    && chmod 0440 /etc/sudoers.d/$USERNAME

# Install build and dev requirements
RUN apt-get update && \
    apt-get -y install \
    git build-essential pkg-config autoconf automake libtool bison flex sed perl \
    libpq-dev parallel curl ccache bear \
    cpp-14 gcc-14 g++-14 libstdc++-14-dev \
    clang-20 llvm-20 libc++-20-dev clang-format-20 clangd-20 libc++abi-20-dev libclang-rt-20-dev \
    postgresql \
    jq ripgrep vim emacs-nox uftrace

# Set up locale
RUN sed -i -e 's/# en_US.UTF-8 UTF-8/en_US.UTF-8 UTF-8/' /etc/locale.gen \
    && locale-gen

# Asan in llvm provided in ubuntu 22.04 is incompatible with
# high-entropy ASLR in much newer kernels that GitHub runners are
# using leading to random crashes: https://reviews.llvm.org/D148280
RUN sysctl vm.mmap_rnd_bits=28

### Finished root-user-requiring stuff, rest is non-root-user
USER ${USERNAME}
WORKDIR /home/${USERNAME}

# rust
ENV PATH=/home/${USERNAME}/.local/bin:/home/${USERNAME}/.cargo/bin:$PATH

RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
RUN rustup component add clippy rustfmt rust-src rust-analyzer llvm-tools
RUN rustup target add wasm32-unknown-unknown wasm32v1-none

# fuzzer support
RUN rustup toolchain install nightly
RUN rustup component add rust-src --toolchain nightly

# rust-based packages (using binstall to get binaries when possible)
RUN cargo install --locked cargo-binstall
RUN cargo binstall -y --locked cargo-llvm-cov lcov-summary cargo-acl cargo-hack cargo-deny cargo-semver-checks cargo-fuzz sccache

# clang by default
ENV CC=clang-20
ENV CXX=clang++-20

# install namespace CLI
RUN curl -fsSL https://get.namespace.so/cloud/install.sh | sh

# install opencode and claude code
RUN curl -fsSL https://opencode.ai/install | bash -s -- --version 1.2.20
RUN curl -fsSL https://claude.ai/install.sh | bash

ENV LANG en_US.UTF-8
ENV LANGUAGE en_US:en
ENV LC_ALL en_US.UTF-8

# Switch back to dialog for any ad-hoc use of apt-get
ENV DEBIAN_FRONTEND=
