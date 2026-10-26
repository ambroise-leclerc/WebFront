# Toolchain of the Linux desktop CI job (GCC 16, WebKitGTK, lcov, Xvfb), prebuilt so that a job downloads one image from
# ghcr.io instead of dozens of packages from an Ubuntu mirror. BuildAndTest.yml tags it with the hash of this file:
# changing the file (for example to refresh the packages) is what rebuilds it. CMake is not part of it: the
# setup-cmake action installs an exact, checksummed release.
FROM ubuntu:26.04@sha256:c098726dd9b9733f3e885f8de0ef375206a311b0a317eadc40d347b1115fb944

LABEL org.opencontainers.image.source="https://github.com/ambroise-leclerc/WebFront"
LABEL org.opencontainers.image.description="WebFront CI: GCC 16, WebKitGTK 6, lcov and Xvfb on Ubuntu 26.04"

ENV DEBIAN_FRONTEND=noninteractive

# sudo: the composite actions of the repository call it, as they do on a hosted runner. build-essential: the CTest
# fixtures configure sub-projects with CMake's default generator and compiler, which a hosted runner also provides.
# libjson-xs-perl: a recommendation of lcov, which falls back to a pure-Perl parser about eight times slower without it.
RUN apt-get update \
    && apt-get install -y --no-install-recommends -o Acquire::Retries=3 \
        ca-certificates curl git sudo tar xz-utils build-essential \
        ninja-build pkg-config python3 \
        gcc-16 g++-16 libwebkitgtk-6.0-dev lcov libjson-xs-perl \
        xvfb xauth \
    && rm -rf /var/lib/apt/lists/*

# The workspace is mounted from the host runner and owned by another user: let git (and the tests that call it) read it.
RUN git config --system --add safe.directory '*'

ENV CC=gcc-16 CXX=g++-16
