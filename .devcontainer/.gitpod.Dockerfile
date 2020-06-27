FROM gitpod/workspace-full-vnc:latest

# Work around https://github.com/sudo-project/sudo/issues/42
USER root
RUN echo "Set disable_coredump false" >> /etc/sudo.conf

# Switch back to non-root user
USER gitpod
ENV HOME=/home/gitpod
WORKDIR $HOME

# Avoid warnings by switching to noninteractive
ENV DEBIAN_FRONTEND=noninteractive
ENV DEBCONF_NONINTERACTIVE_SEEN=true

# Try to suppress any future interactive tzdata installation nonsense
RUN sudo ln -fs /usr/share/zoneinfo/UTC /etc/localtime \
    && sudo apt-get -y install tzdata \
    && sudo dpkg-reconfigure --frontend noninteractive tzdata

# Add test tool chain
# NOTE: newer version of the compilers are not
#    provided by stock distributions
#    and are provided by the /test toolchain
RUN sudo apt-get update \
    && sudo apt-get -y install --no-install-recommends apt-utils dialog 2>&1 \
    && sudo apt-get -y install software-properties-common \
    && sudo apt-get -y install git iproute2 procps lsb-release \
    && sudo apt-get update

# Install common compilation tools
RUN sudo apt-get -y install git build-essential pkg-config autoconf automake libtool bison flex libpq-dev parallel bear
RUN sudo apt-get -y install clang clangd clang-tools libc++-dev libc++abi-dev ccache neovim

# Install postgresql to enable tests under make check
RUN sudo apt-get -y install postgresql

# Install some Tracy dependencies
RUN sudo apt-get -y install libtbb-dev libmkl-tbb-thread

# Install electron dependencies for GUI apps over noVNC
RUN sudo apt-get install -y libasound2-dev libgtk-3-dev libnss3-dev

# Install some other potential dependencies for GUI apps
RUN sudo apt-get install -y libcapstone-dev libfreetype6-dev libglfw3-dev libgtk2.0-dev libgtk-3-0

# Set up locale
RUN sudo sed -i -e 's/# en_US.UTF-8 UTF-8/en_US.UTF-8 UTF-8/' /etc/locale.gen \
    && sudo locale-gen

ENV LANG en_US.UTF-8
ENV LANGUAGE en_US:en
ENV LC_ALL en_US.UTF-8

# Install clang-format for formatting, and link it into a directory in the $PATH
# (For now, our standard is an old version of clang-format, 5.0.2)
RUN sudo apt-get -y install curl
RUN sudo curl https://releases.llvm.org/5.0.2/clang+llvm-5.0.2-x86_64-linux-gnu-ubuntu-16.04.tar.xz | sudo tar -xJf - -C /usr/local
RUN sudo ln -sf /usr/local/clang+llvm-5.0.2-x86_64-linux-gnu-ubuntu-16.04/bin/clang-format /usr/bin

# git configuration
RUN sudo git config --system pull.rebase true

# Switch back to dialog for any ad-hoc use of apt-get
ENV DEBIAN_FRONTEND=
ENV DEBCONF_NONINTERACTIVE_SEEN=
