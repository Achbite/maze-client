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
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF && \
    cmake --build /source/build --parallel --target maze_client

FROM python:3.11-slim

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libabsl20240722 \
    libgrpc++1.51t64 \
    libprotobuf32t64 \
    procps \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /source/build/maze_client /opt/rl/maze-client/bin/maze_client
COPY configs /opt/rl/maze-client/configs
COPY maps /opt/rl/maze-client/maps
COPY run.sh /opt/rl/maze-client/run.sh
COPY run_replay.sh /opt/rl/maze-client/run_replay.sh
COPY scripts /opt/rl/maze-client/scripts
COPY tools/viz_player /opt/rl/maze-client/tools/viz_player

RUN chmod +x /opt/rl/maze-client/bin/maze_client \
    /opt/rl/maze-client/run.sh \
    /opt/rl/maze-client/run_replay.sh \
    /opt/rl/maze-client/scripts/replay_service.py \
    /opt/rl/maze-client/scripts/entrypoint.sh \
    /opt/rl/maze-client/scripts/healthcheck.sh

WORKDIR /opt/rl/maze-client
EXPOSE 9004
HEALTHCHECK --interval=2s --timeout=2s --start-period=5s --retries=15 \
    CMD ["/opt/rl/maze-client/scripts/healthcheck.sh"]
ENTRYPOINT ["/opt/rl/maze-client/scripts/entrypoint.sh"]
CMD ["--config", "configs/client_config.yaml"]
