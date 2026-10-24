# Build on the development/provisioning workstation, never on the SSH test host.
# noble is the CEF/sanitizer baseline. For desktop, override BASE_IMAGE with an
# Ubuntu 26.04 digest and GCC_VERSION=16. The packer transports docker save output.
ARG BASE_IMAGE=ubuntu@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3
FROM ${BASE_IMAGE}
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
ARG GCC_VERSION=13
ARG LLVM_VERSION=23
ARG CMAKE_VERSION=4.4.4
# Supply verified CMake release contents in the build context; no mutable installer.
COPY cmake/ /opt/cmake/
ENV PATH="/opt/cmake/bin:${PATH}"
RUN test "$(cmake --version | head -1)" = "cmake version ${CMAKE_VERSION}"
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl gnupg git python3 ninja-build ccache lcov xvfb xauth \
    pkg-config gcc g++ gcc-${GCC_VERSION} g++-${GCC_VERSION} \
    libwebkitgtk-6.0-dev libgtk-3-dev libnss3 libasound2t64 \
    libgbm-dev libxss1 libxcomposite1 libxdamage1 libxrandr2 \
    libatk-bridge2.0-0 libdrm2 libxkbcommon0 && rm -rf /var/lib/apt/lists/*
RUN gcc --version && g++ --version && gcc-${GCC_VERSION} --version && g++-${GCC_VERSION} --version
RUN curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /tmp/llvm.asc \
    && gpg --show-keys --with-colons --fingerprint /tmp/llvm.asc \
       | grep -q 'fpr:::::::::6084F3CF814B57C1CF12EFD515CF4D18AF4F7421:' \
    && gpg --dearmor < /tmp/llvm.asc > /usr/share/keyrings/llvm.gpg \
    && . /etc/os-release \
    && echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] https://apt.llvm.org/${VERSION_CODENAME}/ llvm-toolchain-${VERSION_CODENAME}-${LLVM_VERSION} main" > /etc/apt/sources.list.d/llvm.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends clang-${LLVM_VERSION} clang-tidy-${LLVM_VERSION} lld-${LLVM_VERSION} \
       libclang-rt-${LLVM_VERSION}-dev libc++-${LLVM_VERSION}-dev libc++abi-${LLVM_VERSION}-dev \
    && rm -rf /var/lib/apt/lists/* /tmp/llvm.asc
# Host UID/GID override this default in docker run; no privileged runtime is used.
USER 1000:1000
WORKDIR /workspace
