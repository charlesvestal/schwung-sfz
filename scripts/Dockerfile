# Dockerfile for building SFZ module for Move Anything (ARM64)
# Uses Debian bullseye for GCC 10 / GLIBCXX_3.4.29 compatibility with Move device
FROM debian:bullseye-slim

RUN apt-get update && apt-get install -y \
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
ENV RUSTUP_HOME=/usr/local/rustup \
    CARGO_HOME=/usr/local/cargo \
    PATH=/usr/local/cargo/bin:$PATH
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
        | sh -s -- -y --default-toolchain nightly --profile minimal \
                   --target aarch64-unknown-linux-gnu \
    && rustup component add rust-src --toolchain nightly \
    && chmod -R a+w /usr/local/rustup /usr/local/cargo

# Cargo cross-compile config: use aarch64-linux-gnu-gcc as the linker for the
# aarch64 target. Written at image build time so Docker users don't need a
# host-side .cargo/config.toml.
RUN mkdir -p /usr/local/cargo \
    && printf '[target.aarch64-unknown-linux-gnu]\nlinker = "aarch64-linux-gnu-gcc"\n' \
       > /usr/local/cargo/config.toml

WORKDIR /build
