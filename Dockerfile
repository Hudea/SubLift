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
# Stage 2: Build C++ Native Server with PaddleOCR & OpenCV
# -----------------------------------------------------------------------------
FROM ubuntu:22.04 AS cpp-builder

ARG TARGETARCH

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    libopencv-dev \
    git \
    ca-certificates \
    wget \
    tar \
    && rm -rf /var/lib/apt/lists/*

# Download and install ONNX Runtime for the target architecture
RUN if [ "$TARGETARCH" = "arm64" ]; then \
      ORT_ARCH="aarch64"; \
    else \
      ORT_ARCH="x64"; \
    fi && \
    ORT_URL="https://github.com/microsoft/onnxruntime/releases/download/v1.18.1/onnxruntime-linux-${ORT_ARCH}-1.18.1.tgz" && \
    wget -q "$ORT_URL" -O /tmp/ort.tgz && \
    mkdir -p /opt/onnxruntime && \
    tar -xzf /tmp/ort.tgz -C /opt/onnxruntime --strip-components=1 && \
    cp -r /opt/onnxruntime/include/* /usr/local/include/ && \
    cp -r /opt/onnxruntime/lib/* /usr/local/lib/ && \
    ldconfig && \
    rm -f /tmp/ort.tgz

WORKDIR /app
COPY cpp/ /app/cpp/

RUN cmake -S /app/cpp -B /app/build/cpp -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSUBLIFT_REQUIRE_OPENCV=ON \
    -DSUBLIFT_ENABLE_PADDLE=ON \
    -DSUBLIFT_REQUIRE_PADDLE=ON \
    -DSUBLIFT_ENABLE_VISION=OFF \
    -DSUBLIFT_BUILD_TESTS=OFF \
    && cmake --build /app/build/cpp --target sublift_server

# Prepare pre-warmed PP-OCR models and dictionary
RUN mkdir -p /opt/sublift/models && \
    (wget -q -T 10 https://github.com/RapidAI/RapidOCR/releases/download/v1.1.0/ch_PP-OCRv4_det_infer.onnx -O /opt/sublift/models/PP-OCRv6_det_small.onnx 2>/dev/null || true) && \
    (wget -q -T 10 https://github.com/RapidAI/RapidOCR/releases/download/v1.1.0/ch_ppocr_mobile_v2.0_cls_infer.onnx -O /opt/sublift/models/ch_ppocr_mobile_v2.0_cls_mobile.onnx 2>/dev/null || true) && \
    (wget -q -T 10 https://github.com/RapidAI/RapidOCR/releases/download/v1.1.0/ch_PP-OCRv4_rec_infer.onnx -O /opt/sublift/models/PP-OCRv6_rec_small.onnx 2>/dev/null || true) && \
    (wget -q -T 10 https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/release/2.7/ppocr/utils/ppocr_keys_v1.txt -O /opt/sublift/models/ppocrv6_dict.txt 2>/dev/null || true)

# -----------------------------------------------------------------------------
# Stage 3: Minimal Production Runtime
# -----------------------------------------------------------------------------
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
ENV SUBLIFT_HOST=0.0.0.0
ENV SUBLIFT_PORT=8080
ENV SUBLIFT_STATIC_DIR=/app/web/dist
ENV SUBLIFT_PADDLE_MODEL_DIR=/opt/sublift/models
ENV SUBLIFT_ALLOWED_MEDIA_ROOT=/media

RUN apt-get update && apt-get install -y --no-install-recommends \
    ffmpeg \
    libopencv-core4.5d \
    libopencv-imgproc4.5d \
    libgomp1 \
    ca-certificates \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Copy ONNX Runtime shared libraries from builder
COPY --from=cpp-builder /usr/local/lib/libonnxruntime* /usr/local/lib/
RUN ldconfig

# Create non-root user and media/model directories
RUN groupadd -g 10001 sublift && \
    useradd -u 10001 -g sublift -s /bin/bash -m sublift && \
    mkdir -p /opt/sublift/models /media /app/web/dist && \
    chown -R sublift:sublift /opt/sublift /media /app

WORKDIR /app

# Copy server binary and pre-warmed models
COPY --from=cpp-builder /app/build/cpp/bin/sublift_server /usr/local/bin/sublift_server
COPY --from=cpp-builder --chown=sublift:sublift /opt/sublift/models /opt/sublift/models

# Copy frontend static assets
COPY --from=web-builder --chown=sublift:sublift /app/apps/web/dist /app/web/dist

USER sublift:sublift
VOLUME ["/media", "/opt/sublift/models"]

EXPOSE 8080

HEALTHCHECK --interval=15s --timeout=3s --start-period=5s --retries=3 \
  CMD curl -fsS http://localhost:8080/api/system/info || exit 1

ENTRYPOINT ["/usr/local/bin/sublift_server"]
CMD []
