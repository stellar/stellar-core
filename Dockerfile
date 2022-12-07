FROM ubuntu:20.04
LABEL Description="Stellar-core Build environment"

ENV HOME /root

SHELL ["/bin/bash", "-c"]

RUN echo  "=== Install dependencies and checkout source code ==="

# install ssh commands such as ssh-keyscan and git
RUN  apt-get -yq update
RUN apt-get -yqq install ssh
RUN  printf "2" |  apt-get -yqq install git

# add host github to ssh known_hosts file and get its the public key
RUN mkdir -p /root/.ssh && \
    chmod 700 /root/.ssh && \
    /usr/bin/ssh-keyscan github.com > /root/.ssh/known_hosts
ARG SSH_PRV_KEY
RUN echo "${SSH_PRV_KEY}" > /root/.ssh/id_rsa && \
    chmod 600 /root/.ssh/id_rsa

RUN apt-get update

# C++ toolchain.  A toolchain is a set of tools (such as compiler, linker, and assembler) intended to build your project.
RUN printf "Y" | apt-get install software-properties-common
RUN add-apt-repository ppa:ubuntu-toolchain-r/test
RUN apt-get update    

# Common packages.
RUN printf "Y" | apt-get install git build-essential pkg-config autoconf dh-autoreconf automake libtool bison flex libpq-dev libunwind-dev parallel

# Clang-format-5.0 - tool used to automatically format c++ code so that other developers don’t need to worry about style issues.  
# It’s used to format your c++ code before creating a pull request.
RUN printf "Y" | apt-get install clang-format

WORKDIR /usr/src
ARG LIB_VERSION
RUN git clone --depth 1 -b ${LIB_VERSION} git@github.com:tbcasoft/stellar-core.git

WORKDIR /usr/src/stellar-core

RUN echo "=== About to run  autogen.sh ==="
./autogen.sh
RUN echo "=== About to run  make, output stored in make.output file ==="
make clean
make &> make.output