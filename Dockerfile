FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    curl \
    git \
    libssl-dev \
    software-properties-common \
    python3 \
    python3-pip \
    wget \
    gnupg

# Installation de GCC 13 (comme dans GitHub Actions)
RUN add-apt-repository -y ppa:ubuntu-toolchain-r/test \
    && apt-get update \
    && apt-get install -y gcc-13 g++-13 \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100

# Installation de LLVM et Clang 17 (comme dans GitHub Actions)
RUN wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | apt-key add - \
    && echo "deb http://apt.llvm.org/jammy/ llvm-toolchain-jammy-17 main" >> /etc/apt/sources.list \
    && apt-get update \
    && apt-get install -y clang-17 lldb-17 lld-17 libclang-17-dev \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-17 100 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-17 100 \
    && update-alternatives --install /usr/bin/lldb lldb /usr/bin/lldb-17 100

# Installation d'autres outils requis
RUN apt-get install -y \
    cppcheck \
    valgrind \
    ninja-build \
    libfmt-dev \
    libspdlog-dev \
    gdb \
    llvm-17 \
    make

# Installation de vcpkg
RUN git clone https://github.com/Microsoft/vcpkg.git /opt/vcpkg \
    && /opt/vcpkg/bootstrap-vcpkg.sh \
    && ln -s /opt/vcpkg/vcpkg /usr/local/bin/vcpkg

# Installation des extensions VSCode pour le développement C/C++
RUN mkdir -p /root/.vscode-server/extensions

# Définition des variables d'environnement
ENV PATH="/opt/vcpkg:${PATH}"
ENV VCPKG_ROOT="/opt/vcpkg"

WORKDIR /workspaces/WebFront
