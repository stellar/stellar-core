#-------------------------------------------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License. See https://go.microsoft.com/fwlink/?linkid=2090316 for license information.
#-------------------------------------------------------------------------------------------------------------
FROM ubuntu:focal

# Avoid warnings by switching to noninteractive
ENV DEBIAN_FRONTEND=noninteractive

# This Dockerfile adds a non-root 'vscode' user with sudo access. However, for Linux,
# this user's GID/UID must match your local user UID/GID to avoid permission issues
# with bind mounts. Update USER_UID / USER_GID if yours is not 1000. See
# https://aka.ms/vscode-remote/containers/non-root-user for details.
ARG USERNAME=vscode
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
        /etc/apt/sources.list \
    ; fi \
    ; grep "^[^#;]" /etc/apt/sources.list

# install base container packages and prep for VSCode
RUN apt-get update \
    # Verify process tools, lsb-release (common in install instructions for CLIs) installed
    && apt-get -y install iproute2 procps lsb-release \
    #
    # Create a non-root user to use if preferred - see https://aka.ms/vscode-remote/containers/non-root-user.
    && groupadd --gid $USER_GID $USERNAME \
    && useradd -s /bin/bash --uid $USER_UID --gid $USER_GID -m $USERNAME \
    # [Optional] Add sudo support for the non-root user
    && apt-get install -y sudo \
    && echo $USERNAME ALL=\(root\) NOPASSWD:ALL > /etc/sudoers.d/$USERNAME\
    && chmod 0440 /etc/sudoers.d/$USERNAME

# Add test tool chain
# NOTE: newer version of the compilers are not
#    provided by stock distributions
#    and are provided by the /test toolchain
# RUN apt-get -y install software-properties-common \
#     && add-apt-repository ppa:ubuntu-toolchain-r/test \
#    && apt-get update

# Install common compilation tools
RUN apt-get -y install git build-essential pkg-config autoconf automake libtool bison flex libpq-dev parallel libunwind-dev

# Update compiler tools
RUN apt-get -y install libstdc++-8-dev clang-format-10 ccache

# gcc
RUN apt-get -y install cpp-8 gcc-8 g++-8
# clang
RUN apt-get -y install clang-10 llvm-10
# rust
RUN apt-get -y install cargo rustc

# clang by default
ENV CC=clang-10
ENV CXX=clang++-10

# Install postgresql to enable tests under make check
RUN apt-get -y install postgresql

# Set up locale
RUN sed -i -e 's/# en_US.UTF-8 UTF-8/en_US.UTF-8 UTF-8/' /etc/locale.gen \
    && locale-gen

ENV LANG en_US.UTF-8
ENV LANGUAGE en_US:en
ENV LC_ALL en_US.UTF-8

# Switch back to dialog for any ad-hoc use of apt-get
ENV DEBIAN_FRONTEND=
