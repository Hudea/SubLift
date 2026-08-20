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
    if [ "$ORT_ARCH" = "aarch64" ]; then \
      echo "c1dcd8ab29e8d227d886b6ee415c08aea893956acf98f0758a42a84f27c02851  /tmp/ort.tgz"; \
    else \
      echo "a0994512ec1e1debc00c18bfc7a5f16249f6ebd6a6128ff2034464cc380ea211  /tmp/ort.tgz"; \
    fi | sha256sum -c && \
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

# Prepare pre-warmed PP-OCRv6 ONNX models and dictionary.
# Sources are the exact artifacts RapidOCR itself distributes (ModelScope v3.9.2),
# pinned by SHA256; any download or checksum failure must fail the build
# (fail-closed) instead of silently shipping an unusable engine.
RUN mkdir -p /opt/sublift/models && \
    wget -q -T 60 --tries=3 -O /opt/sublift/models/PP-OCRv6_det_small.onnx \
      "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/onnx/PP-OCRv6/det/PP-OCRv6_det_small.onnx" && \
    wget -q -T 60 --tries=3 -O /opt/sublift/models/PP-OCRv6_rec_small.onnx \
      "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/onnx/PP-OCRv6/rec/PP-OCRv6_rec_small.onnx" && \
    wget -q -T 60 --tries=3 -O /opt/sublift/models/ch_ppocr_mobile_v2.0_cls_mobile.onnx \
      "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/onnx/PP-OCRv4/cls/ch_ppocr_mobile_v2.0_cls_mobile.onnx" && \
    wget -q -T 60 --tries=3 -O /opt/sublift/models/ppocrv6_dict.txt \
      "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/paddle/PP-OCRv6/rec/PP-OCRv6_rec_small/ppocrv6_dict.txt" && \
    printf '%s\n' \
      "090f04abcd9d9a7498bc4ebf677e4cb9bdce1fe4197ddb7e529f1ef44e1ff94f  /opt/sublift/models/PP-OCRv6_det_small.onnx" \
      "6f327246b50388f3c176ae304bd95767ea6dc0c9ae92153ef8cbe210b3c14884  /opt/sublift/models/PP-OCRv6_rec_small.onnx" \
      "e47acedf663230f8863ff1ab0e64dd2d82b838fceb5957146dab185a89d6215c  /opt/sublift/models/ch_ppocr_mobile_v2.0_cls_mobile.onnx" \
      "b5f2bfe2bdd9448429e3e82b51c789775d9b42f2403d082b00662eb77e401c5d  /opt/sublift/models/ppocrv6_dict.txt" \
      > /tmp/sublift_model_sha256 && \
    sha256sum -c /tmp/sublift_model_sha256 && \
    rm -f /tmp/sublift_model_sha256

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

EXPOSE 8080

HEALTHCHECK --interval=15s --timeout=3s --start-period=15s --retries=3 \
  CMD curl -fsS http://localhost:8080/api/system/info || exit 1

ENTRYPOINT ["/usr/local/bin/sublift_server"]
CMD []
