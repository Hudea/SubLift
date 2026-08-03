#!/bin/bash
# SubLift 标准产品验证门（日常开发门，非完整发布门）。
#
# 该脚本由旧的重型 init.sh 迁入。Harness 的 ./init.sh 只做秒级 L0；
# 不要把本脚本中的依赖同步、构建或测试重新塞回默认会话入口。
#
# 默认覆盖：
#   工具检查 → uv sync（vision+paddle extras）→ ruff / mypy →
#   C++ cmake+build+ctest → 单次 pytest（含 worker e2e，需先有二进制）→
#   cutover 正确性 parity（golden 列表）
#
# 默认 *不* 跑：cutover runtime 微基准、GT L3 live、二次 pytest、coverage。
# 完整发布门请显式打开（见下方环境变量）或单独调用 check_cutover_gate.py。
#
# 环境变量：
#   SUBLIFT_INIT_SKIP_VISION=1   Darwin 上不编 Vision（默认 macOS VISION=ON）
#   SUBLIFT_INIT_RUNTIME=1       额外跑 cutover runtime wall/cancel/restart/RSS
#   SUBLIFT_INIT_GT=1            额外跑 GT L3（有片则测；缺片则按脚本策略 WAIVE/FAIL）
#   SUBLIFT_INIT_REQUIRE_GT=1    GT 视频缺失则 cutover 失败（发布门）
#   SUBLIFT_INIT_SKIP_CUTOVER=1  跳过整个 cutover（仅应急；不推荐）
#
# 防膨胀：见 AGENTS.md「环境与标准命令」；新增步骤不得复制既有门或污染版本库产物。
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass=0
fail=0

# After uv sync --extra …, plain `uv run` uses the project env (extras already installed).
UV=(uv run)

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

run_check() {
    local label="$1"
    shift
    if "$@" >/tmp/sublift_init.log 2>&1; then
        printf "${GREEN}[OK]${NC}  %s\n" "$label"
        pass=$((pass + 1))
    else
        local code=$?
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

echo "=============================="
echo " SubLift 环境检查"
echo "=============================="
echo

check "uv"      uv      true
check "python3" python3 true
check "ffmpeg"  ffmpeg  true

echo
if command -v uv >/dev/null 2>&1; then
    # One-shot version probe without forcing unused extras on the CLI line.
    py_line=$(uv run python -c 'import sys; v=sys.version_info; print(f"{v.major}.{v.minor}"); print(1 if v >= (3, 12) else 0)')
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
# Product optional engines: keep both extras so sync does not prune them.
if uv sync --extra vision --extra paddle 2>&1; then
    printf "${GREEN}[OK]${NC}  uv sync --extra vision --extra paddle 成功\n"
    pass=$((pass + 1))
else
    printf "${RED}[FAIL]${NC} uv sync --extra vision --extra paddle 失败\n"
    fail=$((fail + 1))
fi

echo
echo "=============================="
echo " Python 静态检查"
echo "=============================="
echo

run_check "ruff check ."   "${UV[@]}" ruff check .
run_check "mypy src tests" "${UV[@]}" mypy src tests

echo
echo "=============================="
echo " C++ / Phase 6 基线"
echo "=============================="
echo

if command -v cmake >/dev/null 2>&1; then
    cmake_cmd=(cmake -S cpp -B build/cpp -DCMAKE_BUILD_TYPE=Debug -DSUBLIFT_REQUIRE_OPENCV=ON)
    if [ "$(uname -s)" = "Darwin" ] && [ "${SUBLIFT_INIT_SKIP_VISION:-0}" != "1" ]; then
        cmake_cmd+=(-DSUBLIFT_ENABLE_VISION=ON)
    fi
    if command -v ninja >/dev/null 2>&1; then
        cmake_cmd+=(-G Ninja)
    fi
    if "${cmake_cmd[@]}" >/tmp/sublift_cpp_cmake.log 2>&1 \
        && cmake --build build/cpp >/tmp/sublift_cpp_build.log 2>&1 \
        && ctest --test-dir build/cpp --output-on-failure >/tmp/sublift_cpp_ctest.log 2>&1; then
        printf "${GREEN}[OK]${NC}  cmake/ctest (build/cpp Debug)\n"
        pass=$((pass + 1))
    else
        printf "${RED}[FAIL]${NC} cmake/ctest (build/cpp Debug)\n"
        tail -n 40 /tmp/sublift_cpp_cmake.log /tmp/sublift_cpp_build.log /tmp/sublift_cpp_ctest.log 2>/dev/null || true
        fail=$((fail + 1))
    fi
else
    printf "${RED}[FAIL]${NC} cmake 未安装（默认 C++ Worker 路径需要该工具链）\n"
    fail=$((fail + 1))
fi

echo
echo "=============================="
echo " Python 测试（单次，含 post-build IPC）"
echo "=============================="
echo
# After C++ build so tests/ipc/test_cpp_worker.py can run (not skip for missing binary).
# --no-cov: init 是启动门，不是 coverage 门（完整 cov 由开发者显式 pytest 配置）。
# 不再二次单独跑 test_cpp_worker.py。
run_check "pytest" "${UV[@]}" pytest -m "not integration" --no-cov

echo
echo "=============================="
echo " Cutover 正确性门（parity）"
echo "=============================="
echo

if [ "${SUBLIFT_INIT_SKIP_CUTOVER:-0}" = "1" ]; then
    printf "${YELLOW}[SKIP]${NC} cutover（SUBLIFT_INIT_SKIP_CUTOVER=1）\n"
elif [ ! -x build/cpp/bin/sublift_worker ] && [ ! -x build/cpp-rel/bin/sublift_worker ]; then
    printf "${RED}[FAIL]${NC} sublift_worker 缺失，无法跑 cutover parity\n"
    fail=$((fail + 1))
else
    # 日常 init：parity goldens 必跑；runtime/GT 默认关（发布用 env 打开）。
    cutover_args=(python scripts/parity/check_cutover_gate.py --check)
    cutover_args+=(--report-out /tmp/sublift_cutover_gate.md)

    if [ "${SUBLIFT_INIT_RUNTIME:-0}" = "1" ]; then
        : # keep runtime
    else
        cutover_args+=(--skip-runtime)
    fi

    if [ "${SUBLIFT_INIT_REQUIRE_GT:-0}" = "1" ]; then
        cutover_args+=(--require-gt)
    elif [ "${SUBLIFT_INIT_GT:-0}" = "1" ]; then
        : # measure if asset present, else script WAIVE
    else
        cutover_args+=(--skip-gt)
    fi

    if "${UV[@]}" "${cutover_args[@]}" >/tmp/sublift_cutover_gate.log 2>&1; then
        printf "${GREEN}[OK]${NC}  cutover gate"
        if [ "${SUBLIFT_INIT_RUNTIME:-0}" != "1" ] || { [ "${SUBLIFT_INIT_GT:-0}" != "1" ] && [ "${SUBLIFT_INIT_REQUIRE_GT:-0}" != "1" ]; }; then
            printf " (parity"
            [ "${SUBLIFT_INIT_RUNTIME:-0}" = "1" ] || printf "; runtime skipped"
            if [ "${SUBLIFT_INIT_GT:-0}" != "1" ] && [ "${SUBLIFT_INIT_REQUIRE_GT:-0}" != "1" ]; then
                printf "; GT skipped"
            fi
            printf ")"
        fi
        printf "\n"
        pass=$((pass + 1))
    else
        printf "${RED}[FAIL]${NC} cutover gate\n"
        cat /tmp/sublift_cutover_gate.log 2>/dev/null || true
        fail=$((fail + 1))
    fi
fi

echo
echo "=============================="
echo " 汇总"
echo "=============================="
printf "通过: ${GREEN}%d${NC}  失败: ${RED}%d${NC}\n" "$pass" "$fail"
echo "提示: 发布门请 SUBLIFT_INIT_RUNTIME=1 SUBLIFT_INIT_REQUIRE_GT=1 ./scripts/verify-standard.sh"
echo "      或 uv run python scripts/parity/check_cutover_gate.py --check --require-gt"

if [ "$fail" -gt 0 ]; then
    printf "${RED}环境验证未通过，请修复上述失败项。${NC}\n"
    exit 1
fi

printf "${GREEN}环境验证通过，可开始开发。${NC}\n"
