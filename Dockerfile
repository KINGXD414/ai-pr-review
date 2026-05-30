# syntax=docker/dockerfile:1.6
#
# AI PR Review — multi-stage Dockerfile
#
# Build:
#   docker build -t ai-pr-review:dev .
#
# Run:
#   docker run --rm \
#       -e DEEPSEEK_API_KEY=$DEEPSEEK_API_KEY \
#       -e GITHUB_TOKEN=$GITHUB_TOKEN \
#       -v "$PWD":/work \
#       ai-pr-review:dev \
#       --pr https://github.com/chenshuo/muduo/pull/729 \
#       --persona linus --consensus 3 --self-critique \
#       --output /work/review.md
#
# Final image is based on ubuntu:22.04. Size ~600MB (libclang runtime is
# the heaviest dependency; alpine is not used because libclang has no
# musl-friendly build).

###############################################################################
# Stage 1: builder
###############################################################################
FROM ubuntu:22.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
# Optional GFW mirror for FetchContent — override with --build-arg GITHUB_MIRROR=...
ARG GITHUB_MIRROR=https://github.com

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        pkg-config \
        libcurl4-openssl-dev \
        libssl-dev \
        libsqlite3-dev \
        libspdlog-dev \
        libfmt-dev \
        nlohmann-json3-dev \
        libclang-14-dev \
        clang-14 \
        llvm-14-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY code/ /src/code/

RUN cmake -B /build -S /src/code \
        -DCMAKE_BUILD_TYPE=Release \
        -DGITHUB_MIRROR=${GITHUB_MIRROR} \
 && cmake --build /build -j"$(nproc)" \
        --target ai-pr-review ai-pr-eval

###############################################################################
# Stage 2: runtime
###############################################################################
FROM ubuntu:22.04 AS runtime

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libcurl4 \
        libssl3 \
        libsqlite3-0 \
        libspdlog1 \
        libfmt8 \
        libclang1-14 \
 && rm -rf /var/lib/apt/lists/* \
 && update-ca-certificates

COPY --from=builder /build/ai-pr-review /usr/local/bin/ai-pr-review
COPY --from=builder /build/ai-pr-eval   /usr/local/bin/ai-pr-eval

# Mount a host volume at /work to keep generated review.md outside the container.
WORKDIR /work

LABEL org.opencontainers.image.source="https://github.com/KINGXD414/ai-pr-review"
LABEL org.opencontainers.image.description="AI PR Review assistant — C++17 CLI for AI-powered Pull Request review"
LABEL org.opencontainers.image.licenses="MIT"
LABEL org.opencontainers.image.title="ai-pr-review"
LABEL org.opencontainers.image.vendor="KINGXD414"

ENTRYPOINT ["ai-pr-review"]
CMD ["--help"]
