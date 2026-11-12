# Stage 1: Build
# Pinned: sqlx 0.7 breaks on newer Rust (see rust-toolchain.toml)
FROM rust:1.97-bookworm AS builder

WORKDIR /build

# Copy migrations first (needed at compile time by sqlx::migrate! macro)
COPY migrations/ migrations/

# Cache dependencies by building with empty source first
COPY Cargo.toml Cargo.lock* ./
RUN mkdir src && echo "fn main() {}" > src/main.rs && echo "" > src/lib.rs && \
    mkdir -p src/bin && echo "fn main() {}" > src/bin/healthcheck.rs && \
    cargo build --release 2>/dev/null || true && \
    rm -rf src

# Build the actual application
COPY src/ src/
COPY config/ config/
RUN touch src/main.rs src/lib.rs && cargo build --release

# Stage 2: Runtime
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libssl3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /build/target/release/voip-server /app/voip-server
COPY --from=builder /build/target/release/healthcheck /app/healthcheck
COPY config/ /app/config/
COPY migrations/ /app/migrations/

EXPOSE 9000/tcp
EXPOSE 9001/udp

ENTRYPOINT ["/app/voip-server"]
