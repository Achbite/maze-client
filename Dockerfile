FROM python:3.11-slim AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    protobuf-compiler \
    libprotobuf-dev \
    libgrpc++-dev \
    protobuf-compiler-grpc \
    libabsl-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

COPY . /source
RUN cmake -S /source -B /source/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release && \
    cmake --build /source/build --parallel

FROM python:3.11-slim

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libabsl20240722 \
    libgrpc++1.51t64 \
    libprotobuf32t64 \
    libssl3t64 \
    procps \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /source/build/maze_client /opt/rl/maze-client/bin/maze_client
COPY configs /opt/rl/maze-client/configs
COPY maps /opt/rl/maze-client/maps
COPY run.sh /opt/rl/maze-client/run.sh
COPY replay.sh /opt/rl/maze-client/replay.sh
COPY scripts /opt/rl/maze-client/scripts
COPY tools/viz_player /opt/rl/maze-client/tools/viz_player
COPY proto/manifest.json /opt/rl/identity/contracts.json

RUN chmod +x /opt/rl/maze-client/bin/maze_client \
    /opt/rl/maze-client/run.sh \
    /opt/rl/maze-client/replay.sh \
    /opt/rl/maze-client/scripts/entrypoint.sh

WORKDIR /opt/rl/maze-client
ENV MAZE_SESSION_POLICY_PATH=/tmp/maze-client-session-policy
EXPOSE 9004
HEALTHCHECK --interval=2s --timeout=2s --start-period=5s --retries=15 \
    CMD ["test", "-s", "/tmp/maze-client-session-policy"]
ENTRYPOINT ["/opt/rl/maze-client/scripts/entrypoint.sh"]
