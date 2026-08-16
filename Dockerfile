# syntax=docker/dockerfile:1

# -----------------------------------------------------------------------------
# Stage 1: Build Web Frontend (apps/web)
# -----------------------------------------------------------------------------
FROM node:20-slim AS web-builder

WORKDIR /app/apps/web
COPY apps/web/package*.json ./
RUN npm ci

COPY apps/web/ ./
RUN npm run build

# -----------------------------------------------------------------------------
# Stage 2: Build C++ Native Server (sublift_server)
# -----------------------------------------------------------------------------
FROM ubuntu:22.04 AS cpp-builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    libopencv-dev \
    git \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY cpp/ /app/cpp/

RUN cmake -S /app/cpp -B /app/build/cpp -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSUBLIFT_REQUIRE_OPENCV=ON \
    -DSUBLIFT_ENABLE_VISION=OFF \
    -DSUBLIFT_BUILD_TESTS=OFF \
    && cmake --build /app/build/cpp --target sublift_server

# -----------------------------------------------------------------------------
# Stage 3: Minimal Production Runtime
# -----------------------------------------------------------------------------
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
ENV SUBLIFT_HOST=0.0.0.0
ENV SUBLIFT_PORT=8080
ENV SUBLIFT_STATIC_DIR=/app/web/dist

RUN apt-get update && apt-get install -y --no-install-recommends \
    ffmpeg \
    libopencv-core4.5d \
    libopencv-imgproc4.5d \
    libgomp1 \
    ca-certificates \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user
RUN groupadd -g 10001 sublift && \
    useradd -u 10001 -g sublift -s /bin/bash -m sublift

WORKDIR /app

# Copy server binary
COPY --from=cpp-builder /app/build/cpp/bin/sublift_server /usr/local/bin/sublift_server

# Copy frontend static assets
COPY --from=web-builder --chown=sublift:sublift /app/apps/web/dist /app/web/dist

USER sublift:sublift
VOLUME ["/data"]

EXPOSE 8080

HEALTHCHECK --interval=15s --timeout=3s --start-period=5s --retries=3 \
  CMD curl -fsS http://localhost:8080/api/system/info || exit 1

ENTRYPOINT ["/usr/local/bin/sublift_server"]
CMD []
