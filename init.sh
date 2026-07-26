#!/bin/bash
# SubLift 项目环境初始化与验证脚本
# 检出 uv / python / ffmpeg，运行 ruff / mypy / pytest 验证基线
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass=0
fail=0

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

# Python 版本检查（>=3.12，通过 uv 管理的版本）
echo
if command -v uv >/dev/null 2>&1; then
    py_version=$(uv run --extra vision --extra paddle python -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
    py_ok=$(uv run --extra vision --extra paddle python -c 'import sys; print(1 if sys.version_info >= (3, 12) else 0)')
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

run_check "ruff check ."   uv run --extra vision --extra paddle ruff check .
run_check "mypy src"       uv run --extra vision --extra paddle mypy src
# 默认基线排除集成测试：其中 Paddle 集成测试首次构造模型可能触发下载。
# 需外部资源的验证由开发者显式运行：uv run pytest -m integration。
run_check "pytest"         uv run --extra vision --extra paddle pytest -m "not integration"

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
