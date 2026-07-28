#!/bin/bash
# SubLift 项目环境初始化与验证脚本
# 检出 uv / python / ffmpeg / cmake，运行 Python 基线 + C++ 构建测试 + cutover 门禁
#
# 环境变量（可选加速 / 收紧）：
#   SUBLIFT_INIT_SKIP_GT=1       跳过 GT L3 live（有 debug/Zootopia 片时省 ~20s+）
#   SUBLIFT_INIT_SKIP_RUNTIME=1  跳过 cutover runtime 微基准
#   SUBLIFT_INIT_SKIP_VISION=1   Darwin 上不编 Vision（默认 macOS 开 VISION=ON）
#   SUBLIFT_INIT_REQUIRE_GT=1    GT 视频缺失则 cutover 失败（发布门）
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass=0
fail=0

# Shared extras for product path (Vision + Paddle optional deps).
UV=(uv run --extra vision --extra paddle)

check() {
    local name="$1"
    local cmd="$2"
    local required="${3:-true}"
    if command -v "$cmd" >/dev/null 2>&1; then
        printf "${GREEN}[OK]${NC}  %-12s -> %s\n" "$name" "$(command -v "$cmd")"
        pass=$((pass + 1))
    else
        if $required; then
            printf "${RED}[MISS]${NC} %-12s -> 未找到，请安装 ${cmd}${NC}\n" "$name"
            fail=$((fail + 1))
        else
            printf "${YELLOW}[SKIP]${NC} %-12s -> 未安装（可选）\n" "$name"
        fi
    fi
}

echo "=============================="
echo " SubLift 环境检查"
echo "=============================="
echo

# --- 必需工具 ---
check "uv"      uv      true
check "python3" python3 true
check "ffmpeg"  ffmpeg  true

# Python 版本检查（>=3.12，通过 uv 管理的版本）— 单次 uv run
echo
if command -v uv >/dev/null 2>&1; then
    py_line=$("${UV[@]}" python -c 'import sys; v=sys.version_info; print(f"{v.major}.{v.minor}"); print(1 if v >= (3, 12) else 0)')
    py_version=$(printf '%s\n' "$py_line" | sed -n '1p')
    py_ok=$(printf '%s\n' "$py_line" | sed -n '2p')
    if [ "$py_ok" = "1" ]; then
        printf "${GREEN}[OK]${NC}  python 版本 -> %s (>=3.12, via uv)\n" "$py_version"
        pass=$((pass + 1))
    else
        printf "${RED}[FAIL]${NC} python 版本 %s 不满足 >=3.12\n" "$py_version"
        fail=$((fail + 1))
    fi
fi

echo
echo "=============================="
echo " 依赖同步"
echo "=============================="
echo
# Phase 2 GUI 默认使用 Apple Vision；Phase 5 加 PaddleOCR；带上 optional extras，避免普通 uv sync 修剪可选依赖。
if uv sync --extra vision --extra paddle 2>&1; then
    printf "${GREEN}[OK]${NC}  uv sync --extra vision --extra paddle 成功\n"
    pass=$((pass + 1))
else
    printf "${RED}[FAIL]${NC} uv sync --extra vision --extra paddle 失败\n"
    fail=$((fail + 1))
fi

echo
echo "=============================="
echo " 验证（ruff / mypy / pytest）"
echo "=============================="
echo

run_check() {
    local label="$1"
    shift
    if "$@" >/tmp/sublift_init.log 2>&1; then
        printf "${GREEN}[OK]${NC}  %s\n" "$label"
        pass=$((pass + 1))
    else
        local code=$?
        # pytest 退出码 5 = 无测试收集，不视为失败
        if [ "$code" = "5" ] && [ "$label" = "pytest" ]; then
            printf "${YELLOW}[SKIP]${NC} %s（无测试用例）\n" "$label"
            pass=$((pass + 1))
        else
            printf "${RED}[FAIL]${NC} %s\n" "$label"
            cat /tmp/sublift_init.log
            fail=$((fail + 1))
        fi
    fi
}

run_check "ruff check ."   "${UV[@]}" ruff check .
run_check "mypy src tests" "${UV[@]}" mypy src tests
# 默认基线排除集成测试：其中 Paddle 集成测试首次构造模型可能触发下载。
# 需外部资源的验证由开发者显式运行：uv run pytest -m integration。
run_check "pytest"         "${UV[@]}" pytest -m "not integration"

echo
echo "=============================="
echo " C++ / Phase 6 基线"
echo "=============================="
echo

if command -v cmake >/dev/null 2>&1; then
    # Prefer Ninja when present; otherwise use CMake default generator.
    # Phase 6.2+ Pipeline / signature parity requires OpenCV: hard-fail configure
    # if missing so init is not green with the entire deliverable compiled out.
    cmake_cmd=(cmake -S cpp -B build/cpp -DCMAKE_BUILD_TYPE=Debug -DSUBLIFT_REQUIRE_OPENCV=ON)
    # macOS product path uses Vision; opt out with SUBLIFT_INIT_SKIP_VISION=1
    if [ "$(uname -s)" = "Darwin" ] && [ "${SUBLIFT_INIT_SKIP_VISION:-0}" != "1" ]; then
        cmake_cmd+=(-DSUBLIFT_ENABLE_VISION=ON)
    fi
    if command -v ninja >/dev/null 2>&1; then
        cmake_cmd+=(-G Ninja)
    fi
    if "${cmake_cmd[@]}" >/tmp/sublift_cpp_cmake.log 2>&1 \
        && cmake --build build/cpp >/tmp/sublift_cpp_build.log 2>&1 \
        && ctest --test-dir build/cpp --output-on-failure >/tmp/sublift_cpp_ctest.log 2>&1; then
        printf "${GREEN}[OK]${NC}  cmake/ctest (cpp/)\n"
        pass=$((pass + 1))

        # Process-level C++ worker e2e (requires binary; fail not skip).
        if [ -x build/cpp/bin/sublift_worker ] || [ -x build/cpp-rel/bin/sublift_worker ]; then
            if "${UV[@]}" pytest tests/ipc/test_cpp_worker.py \
                >/tmp/sublift_cpp_worker_e2e.log 2>&1; then
                printf "${GREEN}[OK]${NC}  pytest tests/ipc/test_cpp_worker.py (post-build)\n"
                pass=$((pass + 1))
            else
                printf "${RED}[FAIL]${NC} pytest tests/ipc/test_cpp_worker.py (post-build)\n"
                cat /tmp/sublift_cpp_worker_e2e.log 2>/dev/null || true
                fail=$((fail + 1))
            fi

            # Single cutover gate: 10 goldens (one process) + runtime + GT.
            cutover_args=(python scripts/parity/check_cutover_gate.py --check)
            if [ "${SUBLIFT_INIT_SKIP_RUNTIME:-0}" = "1" ]; then
                cutover_args+=(--skip-runtime)
            fi
            if [ "${SUBLIFT_INIT_SKIP_GT:-0}" = "1" ]; then
                cutover_args+=(--skip-gt)
            fi
            if [ "${SUBLIFT_INIT_REQUIRE_GT:-0}" = "1" ]; then
                cutover_args+=(--require-gt)
            fi
            if "${UV[@]}" "${cutover_args[@]}" \
                >/tmp/sublift_cutover_gate.log 2>&1; then
                printf "${GREEN}[OK]${NC}  cutover gate (parity + runtime/GT)\n"
                pass=$((pass + 1))
            else
                printf "${RED}[FAIL]${NC} cutover gate check\n"
                cat /tmp/sublift_cutover_gate.log 2>/dev/null || true
                fail=$((fail + 1))
            fi
        else
            printf "${RED}[FAIL]${NC} sublift_worker binary missing after cmake/ctest\n"
            fail=$((fail + 1))
        fi
    else
        printf "${RED}[FAIL]${NC} cmake/ctest (cpp/)\n"
        tail -n 40 /tmp/sublift_cpp_cmake.log /tmp/sublift_cpp_build.log /tmp/sublift_cpp_ctest.log 2>/dev/null || true
        fail=$((fail + 1))
    fi
else
    printf "${RED}[FAIL]${NC} cmake 未安装（默认 C++ Worker 路径需要该工具链）\n"
    fail=$((fail + 1))
fi

echo
echo "=============================="
echo " 汇总"
echo "=============================="
printf "通过: ${GREEN}%d${NC}  失败: ${RED}%d${NC}\n" "$pass" "$fail"

if [ "$fail" -gt 0 ]; then
    printf "${RED}环境验证未通过，请修复上述失败项。${NC}\n"
    exit 1
fi

printf "${GREEN}环境验证通过，可开始开发。${NC}\n"
