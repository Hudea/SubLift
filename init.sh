#!/usr/bin/env bash
# SubLift 初始化入口：确认 Harness 与进度索引具备可靠开工的最低条件。
# 完整职责和扩展边界见 .agent/rules/initialization.md。
# 不要将依赖安装、构建、测试或交付验证加入本脚本。
set -euo pipefail

echo "== SubLift init =="
echo "cwd: $(pwd)"

missing=0

required_files=(
  "AGENTS.md"
  "progress.md"
  "phases.json"
)

for path in "${required_files[@]}"; do
  if [[ ! -f "$path" ]]; then
    echo "missing: $path"
    echo "  next: restore this file from the repo or template"
    missing=1
  fi
done

if command -v jq >/dev/null 2>&1; then
  if [[ -f phases.json ]] && ! jq empty phases.json 2>/dev/null; then
    echo "invalid json: phases.json"
    echo "  next: fix JSON syntax in phases.json"
    missing=1
  elif [[ -f phases.json ]]; then
    while IFS= read -r detail; do
      [[ -z "$detail" || "$detail" == "null" ]] && continue
      if [[ ! -f "$detail" ]]; then
        echo "missing detail_file: $detail (from phases.json)"
        echo "  next: create $detail or fix detail_file in phases.json"
        missing=1
      elif ! jq empty "$detail" 2>/dev/null; then
        echo "invalid json: $detail"
        echo "  next: fix JSON syntax in $detail"
        missing=1
      fi
    done < <(jq -r '.phases[]?.detail_file // empty' phases.json)
  fi
else
  echo "warn: jq not found; skipped phases.json / detail_file parse"
fi

if [[ "$missing" -ne 0 ]]; then
  echo "FAIL: init baseline not met (see .agent/rules/initialization.md)"
  exit 1
fi

echo "OK"
