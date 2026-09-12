# Dockerfile for building SFZ module for Move Anything (ARM64)
# Uses Debian bullseye for GCC 10 / GLIBCXX_3.4.29 compatibility with Move device
FROM debian:bullseye-slim

# Debian 11 reached END OF LTS on 2026-08-31 and its pool was withdrawn the
# same week — v0.6.4, built that day, was the last release to get through. A
# fresh image build now resolves the INDEX and 404s on every .deb, so
# `apt-get update` succeeds and `install` fails, which reads as a flaky
# mirror rather than as an EOL.
#
# Neither live host can serve this suite any more: archive.debian.org carries
# bullseye main but nothing newer than buster for SECURITY, while
# security.debian.org still publishes the bullseye-security index over a pool
# that is already gone. And archive's main alone does not satisfy the base
# image, whose libc6 is 2.31-13+deb11u14 from the security suite — apt reports
# that as "held broken packages", three layers away from the cause.
#
# snapshot.debian.org serves both suites as they stood on a given day. Pinning
# to one makes the toolchain reproducible instead of dependent on what a live
# suite happens to hold, which is what let this rot arrive unannounced.
# Bump the date deliberately, the way RUST_NIGHTLY_DATE below is bumped.
#
# The base image stays bullseye ON PURPOSE (see above): bookworm would move
# glibc 2.31 -> 2.36 and GLIBCXX past what Move's runtime provides, so "fix
# the build by bumping the base" yields a module that loads nowhere.
ENV DEBIAN_SNAPSHOT=20260825T000000Z
RUN printf 'deb http://snapshot.debian.org/archive/debian/%s bullseye main\n\
deb http://snapshot.debian.org/archive/debian-security/%s bullseye-security main\n' \
        "$DEBIAN_SNAPSHOT" "$DEBIAN_SNAPSHOT" > /etc/apt/sources.list \
    && apt-get -o Acquire::Check-Valid-Until=false update \
    && apt-get install -y \
    build-essential \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    cmake \
    git \
    pkg-config \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ENV CROSS_PREFIX=aarch64-linux-gnu-

# Rust toolchain for the xsynth shim crate (matches xsynth's rust-toolchain
# pin = nightly). Adds aarch64-unknown-linux-gnu target for cross-compilation.
#
# Pinned to a DATED nightly, not the floating "nightly" channel: the vendored
# Cargo.lock (fundsp 0.21.0, last touched 2026-05-18) stopped compiling under
# a later nightly's trait resolution for `An<X>: Mul<...>` in fundsp's
# combinator/net impls — the crate never changed, the compiler did. A fresh
# image build with a floating channel silently picks up whatever nightly is
# current that day, so this can break again with no local change to point
# at. Bump this pin deliberately (with a Cargo.lock update) if it needs to
# move, rather than letting it float.
ENV RUST_NIGHTLY_DATE=2026-05-18
ENV RUSTUP_HOME=/usr/local/rustup \
    CARGO_HOME=/usr/local/cargo \
    PATH=/usr/local/cargo/bin:$PATH
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
        | sh -s -- -y --default-toolchain "nightly-${RUST_NIGHTLY_DATE}" --profile minimal \
                   --target aarch64-unknown-linux-gnu \
    && rustup component add rust-src --toolchain "nightly-${RUST_NIGHTLY_DATE}" \
    && chmod -R a+w /usr/local/rustup /usr/local/cargo

# Cargo cross-compile config: use aarch64-linux-gnu-gcc as the linker for the
# aarch64 target. Written at image build time so Docker users don't need a
# host-side .cargo/config.toml.
RUN mkdir -p /usr/local/cargo \
    && printf '[target.aarch64-unknown-linux-gnu]\nlinker = "aarch64-linux-gnu-gcc"\n' \
       > /usr/local/cargo/config.toml

WORKDIR /build
