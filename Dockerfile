FROM ubuntu:24.04

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
    gnupg \
    ccache \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

# Installation de GCC 16 (dernière 16.x du PPA, comme dans GitHub Actions)
RUN add-apt-repository -y ppa:ubuntu-toolchain-r/test \
    && apt-get update \
    && apt-get install -y gcc-16 g++-16 \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-16 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-16 100 \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

# Installation de LLVM et Clang 23 (comme dans GitHub Actions)
RUN wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm-archive-keyring.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/llvm-archive-keyring.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-23 main" > /etc/apt/sources.list.d/llvm.list \
    && apt-get update \
    && apt-get install -y clang-23 lld-23 lldb-23 libclang-23-dev \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-23 100 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-23 100 \
    && update-alternatives --install /usr/bin/lldb lldb /usr/bin/lldb-23 100 \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

# Installation d'un CMake récent (le paquet apt de Ubuntu 24.04 est trop ancien pour le projet)
RUN wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc | gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ noble main" > /etc/apt/sources.list.d/kitware.list \
    && apt-get update \
    && apt-get install -y cmake \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

# Installation d'autres outils requis
RUN apt-get update && apt-get install -y \
    cppcheck \
    valgrind \
    ninja-build \
    libfmt-dev \
    libspdlog-dev \
    gdb \
    llvm-23 \
    make \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

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
