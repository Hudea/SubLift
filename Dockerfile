# SubLift 局域网自托管镜像（Phase 14 / ADR-0040）
#
# 单容器同源部署：容器内 sublift_server 同时托管 Web 静态资源与 API。
# 媒体通过宿主卷挂载到 /media，作为唯一媒体授权根；镜像内不预置任何用户视频。
#
# 构建： docker build -t sublift:local .
# 运行： SUBLIFT_MEDIA_DIR=/media docker compose up --build
#
# ORT 与 PP-OCRv6 只从 resources/manifest.json 已冻结的 URL 获取并做 SHA-256 校验；
# 任一校验失败即构建失败，不降级为 mock。

# syntax=docker/dockerfile:1.7

ARG UBUNTU_VERSION=22.04
ARG NODE_VERSION=20

# ----------------------------------------------------------------------------
# Stage 1: Web 静态资源
# ----------------------------------------------------------------------------
FROM node:${NODE_VERSION}-bookworm-slim AS web-build

WORKDIR /src/apps/web
COPY apps/web/package.json apps/web/package-lock.json ./
RUN npm ci

COPY apps/web/ ./
# vue-tsc -b && vite build：严格类型检查后产出 dist
RUN npm run build

# ----------------------------------------------------------------------------
# Stage 2: Native 运行时资源（ORT + PP-OCRv6），按 manifest 校验
# ----------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS resources

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl jq \
 && rm -rf /var/lib/apt/lists/*

ARG TARGETARCH=amd64
WORKDIR /work
# 脚本按仓库布局用 ${SCRIPT_DIR}/../.. /resources/manifest.json 定位清单，
# 因此必须放在 /work/scripts/docker/ 而不是 /work/ 根下。
COPY resources/manifest.json /work/resources/manifest.json
COPY scripts/docker/fetch-native-resources.sh /work/scripts/docker/fetch-native-resources.sh

# manifest 使用 linux/x64；Docker 的 TARGETARCH 为 arm64 时映射为 aarch64 目标。
RUN chmod +x /work/scripts/docker/fetch-native-resources.sh \
 && case "${TARGETARCH}" in \
      arm64) ARCH=arm64 ;; \
      *) ARCH=x64 ;; \
    esac \
 && /work/scripts/docker/fetch-native-resources.sh /work/native-resources linux "${ARCH}" \
 && find /work/native-resources -type f | sort > /work/native-resources/MANIFEST.txt \
 && cat /work/native-resources/MANIFEST.txt

# ----------------------------------------------------------------------------
# Stage 3: C++ sublift_server 构建
# ----------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS server-build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      cmake \
      git \
      ninja-build \
      pkg-config \
      nlohmann-json3-dev \
      libopencv-dev \
      libopencv-imgcodecs-dev \
      ffmpeg \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# 依赖声明先行，最大化构建缓存命中率
COPY cpp/CMakeLists.txt cpp/
COPY cpp/cmake/ cpp/cmake/
COPY cpp/third_party/ cpp/third_party/

# 模型与 ORT 来自 Stage 2，这里只提供头库搜索路径
COPY --from=resources /work/native-resources /opt/sublift/resources
ENV ONNXRUNTIME_ROOTDIR=/opt/sublift/resources/onnxruntime

COPY cpp/src/ cpp/src/
COPY cpp/include/ cpp/include/
COPY resources/manifest.json /src/resources/manifest.json

# Debian/Ubuntu 把 OpenCVConfig.cmake 放在 cmake/opencv4/，需显式 OpenCV_DIR。
# Paddle 强依赖 OpenCV 与 ORT：任一缺失即配置失败，不静默降级为 mock 引擎。
RUN cmake -S cpp -B build/cpp -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DOpenCV_DIR=/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)/cmake/opencv4 \
      -DSUBLIFT_ENABLE_PADDLE=ON \
      -DSUBLIFT_REQUIRE_PADDLE=ON \
      -DSUBLIFT_ENABLE_OPENCV=ON \
      -DSUBLIFT_REQUIRE_OPENCV=ON \
      -DSUBLIFT_BUILD_TESTS=OFF \
 && cmake --build build/cpp --target sublift_server -j"$(nproc)"

# ----------------------------------------------------------------------------
# Stage 4: 运行镜像
# ----------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates \
      curl \
      ffmpeg \
      libopencv-core4.5d \
      libopencv-imgproc4.5d \
      libopencv-imgcodecs4.5d \
      libgomp1 \
      tini \
 && rm -rf /var/lib/apt/lists/*

# non-root 运行；UID/GID 供需要匹配宿主目录权限时覆盖
ARG SUBLIFT_UID=10001
ARG SUBLIFT_GID=10001
RUN groupadd --gid "${SUBLIFT_GID}" sublift \
 && useradd --uid "${SUBLIFT_UID}" --gid "${SUBLIFT_GID}" \
      --home-dir /var/lib/sublift --create-home sublift

COPY --from=resources /work/native-resources/models /opt/sublift/models
COPY --from=resources /work/native-resources/onnxruntime/lib /opt/sublift/lib
COPY --from=server-build /src/build/cpp/lib/ /opt/sublift/lib/
COPY --from=server-build /src/build/cpp/bin/sublift_server /opt/sublift/bin/sublift_server
COPY --from=web-build /src/apps/web/dist /opt/sublift/web
COPY resources/manifest.json /opt/sublift/share/sublift/manifest.json

# 媒体挂载点；工作区缓存与导出产物落在挂载目录内的 .sublift_cache
RUN mkdir -p /media \
 && chown -R sublift:sublift /media /opt/sublift

USER sublift
WORKDIR /var/lib/sublift

ENV SUBLIFT_HOST=0.0.0.0 \
    SUBLIFT_PORT=8080 \
    SUBLIFT_STATIC_DIR=/opt/sublift/web \
    SUBLIFT_MEDIA_DIR=/media \
    SUBLIFT_PADDLE_MODEL_DIR=/opt/sublift/models/ppocrv6-small \
    SUBLIFT_NATIVE_MANIFEST=/opt/sublift/share/sublift/manifest.json \
    SUBLIFT_CONFIG_DIR=/var/lib/sublift/config \
    LD_LIBRARY_PATH=/opt/sublift/lib

EXPOSE 8080

# tini 负责转发 SIGTERM，使 sublift_server 的优雅停机生效
ENTRYPOINT ["/usr/bin/tini", "--", "/opt/sublift/bin/sublift_server"]
