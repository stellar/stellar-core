# Description

This directory contains docker related files. It includes Dockerfiles,
helper scripts and Makefile for convenience

## Dockerfile

This file is used to build the official docker image published in dockerhub.
It uses the official stellar-core deb package for two reasons:
1. To ensure docker and non-docker environments run the same build
2. To allow binaries to be cryptographically verified

To build set `STELLAR_CORE_VERSION` to the base deb package version you want installed (without the distro suffix). `DISTRO` will default to Ubuntu 24.04 noble if not passed in.
For example:
```
export STELLAR_CORE_VERSION=23.0.2-2724.7b9e6c863
export DISTRO=jammy
export TAG=${USER}/stellar-core:${STELLAR_CORE_VERSION}
make docker-build
```

Note: The Dockerfile will automatically append `.${DISTRO}` to the version when installing the package.

## Dockerfile.testing

This is a Dockerfile for one-off builds of stellar-core during development.

It enables the protocol-next by default, assuming you're using it for
testing features that are not yet ready for production.

It's intended to be run from the stellar-core source directory of a
developer's workstation to make a test buid that can be run in kubernetes
without passing through any of the normal CI process. As such it is as small
and quick as possible, using ubuntu linux and neither building nor installing
an intermediate debian package.

This file does not even require the workspace to be pushed to github, much
less in an open PR, only committed to the local git repo so 'git clean' knows
which files to keep: it copies the contents of the directory it's invoked from
and then does 'git clean' and a rebuild in the container's build environment.
There's no audit trail in the resulting images and they should only ever go in
user repositories for testing purposes.

To use this file, run something like the following at the top of this repository:
```
export TAG=${USER}/stellar-core:$(git describe --always --tags --long)
docker build -f docker/Dockerfile.testing . -t $TAG
```

Or using Makefile
```
make -C docker docker-build-testing
```
