#!/usr/bin/env bash
# SubLift Harness L0：只回答“下一次会话能不能开工”。
#
# 默认必须在数秒内结束：只检查文件、JSON 与 phases.json → detail_file 链。
# 不安装依赖、不跑 lint/test、不构建、不请求网络。完整日常验证见
# ./scripts/verify-standard.sh。
set -euo pipefail

if [[ "${1:-}" == "--standard" ]]; then
  shift
  if [[ "$#" -ne 0 ]]; then
    echo "usage: ./init.sh [--standard]" >&2
    exit 2
  fi
  exec ./scripts/verify-standard.sh
fi

if [[ "$#" -ne 0 ]]; then
  echo "usage: ./init.sh [--standard]" >&2
  exit 2
fi

# Preserve callers of the former heavyweight init contract. A non-empty legacy
# flag intentionally opts into the migrated standard gate rather than making
# the default L0 path slow again.
legacy_standard_requested=0
for legacy_flag in \
  SUBLIFT_INIT_SKIP_VISION \
  SUBLIFT_INIT_RUNTIME \
  SUBLIFT_INIT_GT \
  SUBLIFT_INIT_REQUIRE_GT \
  SUBLIFT_INIT_SKIP_CUTOVER; do
  if [[ -n "${!legacy_flag:-}" ]]; then
    legacy_standard_requested=1
  fi
done
if [[ "$legacy_standard_requested" -eq 1 ]]; then
  echo "[Harness] 检测到旧 SUBLIFT_INIT_* 标志，转发到 scripts/verify-standard.sh。" >&2
  exec ./scripts/verify-standard.sh
fi

echo "== SubLift Harness baseline (L0) =="
echo "cwd: $(pwd)"

missing=0

required_files=(
  "AGENTS.md"
  "README.md"
  "progress.md"
  "feature-list.json"
  "phases.json"
  "phases.schema.json"
  "docs/ARCHITECTURE.md"
  "docs/REQUIREMENTS.md"
  "docs/DECISIONS.md"
  "docs/HURDLES.md"
  "docs/phases/phaseN.schema.json"
  "scripts/verify-standard.sh"
  ".agent/rules/project-continuity.md"
  ".agent/rules/planning.md"
  ".agent/rules/testing.md"
  ".agent/rules/review.md"
  ".agent/rules/verification.md"
  ".agent/rules/feature-development.md"
  ".agent/rules/phase-development.md"
  ".agent/schemas/change-snapshot.schema.json"
  ".agent/schemas/test-evidence-snapshot.schema.json"
  ".agent/schemas/capability-run.schema.json"
  ".agent/schemas/feature-development-state.schema.json"
  ".agent/schemas/phase-development-state.schema.json"
  ".agent/scripts/validate-orchestration.py"
  ".agent/scripts/validate-result.py"
  ".agent/agents/planner.md"
  ".agent/agents/reviewer.md"
  ".agent/skills/session-bootstrap/SKILL.md"
  ".agent/skills/session-handoff/SKILL.md"
  ".agent/skills/commit/SKILL.md"
  ".agent/skills/commit-message/SKILL.md"
  ".agent/skills/plan/SKILL.md"
  ".agent/skills/test/SKILL.md"
  ".agent/skills/review/SKILL.md"
  ".agent/skills/verify/SKILL.md"
  ".agent/skills/develop-feature/SKILL.md"
  ".agent/skills/develop-phase/SKILL.md"
)

for path in "${required_files[@]}"; do
  if [[ ! -f "$path" ]]; then
    echo "missing: $path"
    missing=1
  fi
done

json_files=(
  "phases.schema.json"
  "phases.json"
  "docs/phases/phaseN.schema.json"
  ".agent/schemas/change-snapshot.schema.json"
  ".agent/schemas/test-evidence-snapshot.schema.json"
  ".agent/schemas/capability-run.schema.json"
  ".agent/schemas/feature-development-state.schema.json"
  ".agent/schemas/phase-development-state.schema.json"
  ".agent/skills/test/assets/test-result.schema.json"
  ".agent/skills/review/assets/review-result.schema.json"
  ".agent/skills/verify/assets/verification-result.schema.json"
)

check_detail_file() {
  local detail="$1"
  if [[ -z "$detail" || "$detail" == "null" ]]; then
    return
  fi
  if [[ ! -f "$detail" ]]; then
    echo "missing detail_file: $detail (from phases.json)"
    missing=1
    return
  fi
  if command -v jq >/dev/null 2>&1; then
    if ! jq empty "$detail" >/dev/null 2>&1; then
      echo "invalid json: $detail"
      missing=1
    fi
  elif ! python3 -c 'import json, pathlib, sys; json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))' "$detail" >/dev/null 2>&1; then
    echo "invalid json: $detail"
    missing=1
  fi
}

if command -v jq >/dev/null 2>&1; then
  if ! jq empty "${json_files[@]}" >/dev/null 2>&1; then
    echo "invalid json: one or more schema/index files failed to parse"
    missing=1
  else
    while IFS= read -r detail; do
      check_detail_file "$detail"
    done < <(jq -r '.phases[]?.detail_file // empty' phases.json)
  fi
elif command -v python3 >/dev/null 2>&1; then
  if ! python3 -c 'import json, pathlib, sys; [json.loads(pathlib.Path(path).read_text(encoding="utf-8")) for path in sys.argv[1:]]' "${json_files[@]}" >/dev/null 2>&1; then
    echo "invalid json: one or more schema/index files failed to parse"
    missing=1
  else
    while IFS= read -r detail; do
      check_detail_file "$detail"
    done < <(python3 -c 'import json, pathlib; print("\n".join(item.get("detail_file", "") for item in json.loads(pathlib.Path("phases.json").read_text(encoding="utf-8")).get("phases", [])))')
  fi
else
  echo "missing: jq or python3 is required to parse Harness JSON"
  missing=1
fi

if [[ "$missing" -ne 0 ]]; then
  echo "FAIL"
  exit 1
fi

echo "OK"
