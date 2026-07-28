// phase-review — Phase implementation quality gate
// Review → fix → recheck until delivery quality, for SubLift phase tracking.
//
// Args (object):
//   phase       string  required — "6" | "phase6" | "6.1" | "6.2" | "phase5" ...
//   max_rounds  int     optional — review-fix-recheck loops (default 3, max 5)
//   fix         bool    optional — apply fixes (default true). false = review-only
//   focus       string  optional — extra focus note for reviewers
//
// Delivery quality (exit gate):
//   no open critical/high findings after triage (or after a recheck round),
//   and synthesizer sets delivery_ready=true.
//
// Does NOT git commit (repo rule). Does NOT touch Python product path unless
// a finding proves a phase deliverable requires it.

let meta = #{
    name: "phase-review",
    description: "Phase implementation quality gate: multi-dimension review, fix, recheck until delivery-ready",
    when_to_use: "After a SubLift phase or sub-phase (e.g. 6.1 / 6.2) claims done; before opening the next knife",
    phases: [
        #{ title: "Scope", detail: "Load phase tracking, design docs, map code surface" },
        #{ title: "Review", detail: "Parallel dimensions: goals, architecture, quality, redundancy, tests" },
        #{ title: "Triage", detail: "Merge findings, severity, delivery gate" },
        #{ title: "Fix", detail: "Apply actionable high-severity fixes (optional)" },
        #{ title: "Recheck", detail: "Verify fixes + residual scan; loop until dry or max_rounds" },
        #{ title: "Report", detail: "Write final delivery report to scratch" },
    ],
};

// ---------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------

let scope_schema = #{
    "type": "object",
    "required": ["phase_id", "subphase", "feature_ids", "design_docs", "code_roots", "status_summary", "acceptance_bullets"],
    "properties": #{
        "phase_id": #{ "type": "string" },
        "subphase": #{ "type": "string" },
        "feature_ids": #{
            "type": "array",
            "maxItems": 40,
            "items": #{ "type": "string" },
        },
        "design_docs": #{
            "type": "array",
            "maxItems": 20,
            "items": #{ "type": "string" },
        },
        "code_roots": #{
            "type": "array",
            "maxItems": 20,
            "items": #{ "type": "string" },
        },
        "status_summary": #{ "type": "string" },
        "acceptance_bullets": #{
            "type": "array",
            "maxItems": 20,
            "items": #{ "type": "string" },
        },
        "notes": #{ "type": "string" },
    },
};

let findings_schema = #{
    "type": "object",
    "required": ["dimension", "findings", "summary"],
    "properties": #{
        "dimension": #{ "type": "string" },
        "summary": #{ "type": "string" },
        "findings": #{
            "type": "array",
            "maxItems": 10,
            "items": #{
                "type": "object",
                "required": ["id", "severity", "title", "file", "issue", "suggestion", "blocks_delivery"],
                "properties": #{
                    "id": #{ "type": "string" },
                    "severity": #{ "type": "string" },
                    "title": #{ "type": "string" },
                    "file": #{ "type": "string" },
                    "issue": #{ "type": "string" },
                    "suggestion": #{ "type": "string" },
                    "blocks_delivery": #{ "type": "boolean" },
                },
            },
        },
    },
};

let triage_schema = #{
    "type": "object",
    "required": ["delivery_ready", "critical_count", "high_count", "medium_count", "low_count", "actionable", "summary", "rationale"],
    "properties": #{
        "delivery_ready": #{ "type": "boolean" },
        "critical_count": #{ "type": "integer" },
        "high_count": #{ "type": "integer" },
        "medium_count": #{ "type": "integer" },
        "low_count": #{ "type": "integer" },
        "actionable": #{
            "type": "array",
            "maxItems": 12,
            "items": #{
                "type": "object",
                "required": ["id", "severity", "title", "file", "issue", "suggestion", "blocks_delivery"],
                "properties": #{
                    "id": #{ "type": "string" },
                    "severity": #{ "type": "string" },
                    "title": #{ "type": "string" },
                    "file": #{ "type": "string" },
                    "issue": #{ "type": "string" },
                    "suggestion": #{ "type": "string" },
                    "blocks_delivery": #{ "type": "boolean" },
                },
            },
        },
        "deferred": #{
            "type": "array",
            "maxItems": 12,
            "items": #{
                "type": "object",
                "required": ["id", "title", "reason"],
                "properties": #{
                    "id": #{ "type": "string" },
                    "title": #{ "type": "string" },
                    "reason": #{ "type": "string" },
                },
            },
        },
        "summary": #{ "type": "string" },
        "rationale": #{ "type": "string" },
    },
};

let fix_schema = #{
    "type": "object",
    "required": ["fixed", "skipped", "verification", "notes"],
    "properties": #{
        "fixed": #{
            "type": "array",
            "maxItems": 12,
            "items": #{
                "type": "object",
                "required": ["id", "what_changed"],
                "properties": #{
                    "id": #{ "type": "string" },
                    "what_changed": #{ "type": "string" },
                    "files": #{
                        "type": "array",
                        "maxItems": 20,
                        "items": #{ "type": "string" },
                    },
                },
            },
        },
        "skipped": #{
            "type": "array",
            "maxItems": 12,
            "items": #{
                "type": "object",
                "required": ["id", "reason"],
                "properties": #{
                    "id": #{ "type": "string" },
                    "reason": #{ "type": "string" },
                },
            },
        },
        "verification": #{ "type": "string" },
        "notes": #{ "type": "string" },
    },
};

let recheck_schema = #{
    "type": "object",
    "required": ["still_open", "resolved", "new_findings", "summary", "delivery_ready"],
    "properties": #{
        "still_open": #{
            "type": "array",
            "maxItems": 12,
            "items": #{
                "type": "object",
                "required": ["id", "severity", "title", "file", "issue", "blocks_delivery"],
                "properties": #{
                    "id": #{ "type": "string" },
                    "severity": #{ "type": "string" },
                    "title": #{ "type": "string" },
                    "file": #{ "type": "string" },
                    "issue": #{ "type": "string" },
                    "blocks_delivery": #{ "type": "boolean" },
                },
            },
        },
        "resolved": #{
            "type": "array",
            "maxItems": 12,
            "items": #{ "type": "string" },
        },
        "new_findings": #{
            "type": "array",
            "maxItems": 8,
            "items": #{
                "type": "object",
                "required": ["id", "severity", "title", "file", "issue", "suggestion", "blocks_delivery"],
                "properties": #{
                    "id": #{ "type": "string" },
                    "severity": #{ "type": "string" },
                    "title": #{ "type": "string" },
                    "file": #{ "type": "string" },
                    "issue": #{ "type": "string" },
                    "suggestion": #{ "type": "string" },
                    "blocks_delivery": #{ "type": "boolean" },
                },
            },
        },
        "summary": #{ "type": "string" },
        "delivery_ready": #{ "type": "boolean" },
        "evidence": #{ "type": "string" },
    },
};

// ---------------------------------------------------------------------------
// Args
// ---------------------------------------------------------------------------

let phase_arg = if args == () { () } else { args.phase };
if phase_arg == () {
    pause("verification", "Pass args.phase — e.g. \"6\", \"6.2\", \"phase6\", or \"5\".");
}

let max_rounds = 3;
if args != () && args.max_rounds != () {
    max_rounds = args.max_rounds;
}
if max_rounds < 1 { max_rounds = 1; }
if max_rounds > 5 { max_rounds = 5; }

let do_fix = true;
if args != () && args.fix != () {
    do_fix = args.fix;
}

let focus = "";
if args != () && args.focus != () {
    focus = args.focus;
}

let phase_s = phase_arg;
if type_of(phase_s) != "string" {
    phase_s = json_encode(phase_s);
}
// normalize whitespace-ish without regex
phase_s = phase_s + "";

log("phase-review: phase=" + phase_s + " max_rounds=" + max_rounds.to_string() + " fix=" + do_fix.to_string());

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn arr_len(a) {
    if a == () { 0 } else { a.len() }
}

fn as_str(x) {
    if x == () {
        ""
    } else if type_of(x) == "string" {
        x
    } else {
        json_encode(x)
    }
}

fn as_bool(x, default_val) {
    if x == () {
        default_val
    } else if type_of(x) == "bool" {
        x
    } else {
        default_val
    }
}

fn project_context() {
    let s = "";
    s += "You are working in the SubLift repo (hard-subtitle OCR extraction). ";
    s += "Phase tracking is the source of truth for what a phase must deliver:\n";
    s += "- feature-list.json — project-level feature blocks by phase\n";
    s += "- docs/phases/phaseN.json — feature IDs, subtasks, status, evidence\n";
    s += "- docs/plans/ or docs/cpp/ — design source for the phase\n";
    s += "- docs/ARCHITECTURE.md, docs/REQUIREMENTS.md, AGENTS.md\n";
    s += "- progress.md — navigation only (not evidence store)\n\n";
    s += "Phase 6 (native C++ core) specifics when in scope:\n";
    s += "- docs/cpp/phase6-overview.md, architecture.md, parity-contract.md\n";
    s += "- docs/cpp/phase6.0-bootstrap.md, phase6.1-pure-pipeline.md, phase6.2-pipeline.md\n";
    s += "- Code under cpp/; scripts/parity/; goldens; CTest + Catch2\n";
    s += "- Product path stays Python until cutover (6.6); C++ is Candidate vs frozen Oracle\n";
    s += "- sublift_core must NOT depend on ObjC/Vision; public API no bare cv::Mat\n";
    s += "- Behavior parity > mechanical .py→.cpp translation; no silent algorithm changes\n\n";
    s += "Hard rules for this run:\n";
    s += "- Use tools (read_file, grep, list_dir, run_terminal_command as allowed). Never answer from memory alone.\n";
    s += "- Empty findings are valid ONLY after inspecting the real files/docs.\n";
    s += "- Prefer concrete file:line and evidence over vague style opinions.\n";
    s += "- Do NOT git commit, push, or amend. Do NOT force-push.\n";
    s += "- Do NOT expand scope into the next sub-phase (e.g. reviewing 6.2 must not implement 6.3).\n";
    s
}

fn severity_guide() {
    let s = "";
    s += "Severity scale:\n";
    s += "- critical: wrong behavior vs acceptance, broken architecture boundary, false done status, failing required verification\n";
    s += "- high: material gap vs design, missing tests for claimed parity, unsafe ownership, significant redundancy that hurts maintainability\n";
    s += "- medium: local quality issues, incomplete evidence, inconsistent naming, avoidable duplication\n";
    s += "- low: nits, pure style, optional polish\n";
    s += "blocks_delivery=true only for critical/high that prevent shipping this phase as done.\n";
    s
}

// ---------------------------------------------------------------------------
// Phase: Scope
// ---------------------------------------------------------------------------

phase("Scope");

let scope_prompt = project_context();
scope_prompt += "TASK: Discover the review scope for phase argument: " + json_encode(phase_s) + ".\n\n";
scope_prompt += "Steps (mandatory):\n";
scope_prompt += "1. Read feature-list.json; find the matching phase block.\n";
scope_prompt += "2. Read docs/phases/phaseN.json for that phase (phase6.json for Phase 6).\n";
scope_prompt += "3. If sub-phase given (e.g. 6.1 / 6.2), filter feature IDs by prefix:\n";
scope_prompt += "   - 6.0 → feat-060xx; 6.1 → feat-061xx; 6.2 → feat-062xx; etc.\n";
scope_prompt += "   - bare 6 / phase6 → all features in phase6.json that are done or in-progress for the active sub-stages.\n";
scope_prompt += "4. Read the design docs listed in phase plan_file and docs/cpp/ for that sub-stage.\n";
scope_prompt += "5. Map code_roots (directories/files) that implement those features.\n";
scope_prompt += "6. Extract acceptance bullets from design + feature descriptions + evidence expectations.\n";
scope_prompt += "7. Summarize claimed status vs what the tracker says.\n\n";
if focus != "" {
    scope_prompt += "Extra focus from user: " + focus + "\n\n";
}
scope_prompt += "Return JSON matching the schema. feature_ids must be real IDs from the tracker. ";
scope_prompt += "code_roots and design_docs must be real paths you opened or listed.\n";

let scope_r = agent(scope_prompt, #{
    label: "scope-discover",
    capability_mode: "read-only",
    output_schema: scope_schema,
});

if scope_r == () || !scope_r.success || scope_r.output == () {
    pause("infra", "Scope discovery agent failed. Resume after fixing environment, or re-run with a clearer args.phase.");
}

let scope = scope_r.output;
// Normalize canned/partial fields so later .len() / string concat never hit unit.
if scope.phase_id == () { scope.phase_id = phase_s; }
if scope.subphase == () { scope.subphase = phase_s; }
if scope.feature_ids == () { scope.feature_ids = []; }
if scope.design_docs == () { scope.design_docs = []; }
if scope.code_roots == () { scope.code_roots = []; }
if scope.status_summary == () { scope.status_summary = ""; }
if scope.acceptance_bullets == () { scope.acceptance_bullets = []; }
let scope_json = json_encode(scope);
log("Scope: phase_id=" + as_str(scope.phase_id) + " subphase=" + as_str(scope.subphase)
    + " features=" + arr_len(scope.feature_ids).to_string());

// ---------------------------------------------------------------------------
// Phase: Review (parallel dimensions)
// ---------------------------------------------------------------------------

phase("Review");

let dims = [
    "goals",
    "architecture",
    "code_quality",
    "redundancy",
    "tests_parity",
];

let dim_briefs = #{
    goals: "GOAL ATTAINMENT: Did each feature meet its description/subtasks? Is evidence in phaseN.json real and matching verification commands? False done / missing acceptance is critical.",
    architecture: "ARCHITECTURE CONFORMANCE: Match docs/cpp (or phase design) boundaries, target graph, interfaces, ownership, Oracle/parity rules, non-goals (no product cutover early, no libav, no silent algorithm changes). Flag violations of module boundaries.",
    code_quality: "CODE QUALITY: Naming, layering, error handling, API cleanliness, C++20/Python style consistency with existing modules, public header hygiene, comments only where needed. Prefer structural issues over pure style nits.",
    redundancy: "REDUNDANCY & DEAD WEIGHT: duplicated logic vs Python oracle that should be shared helpers, unused code, parallel implementations, over-abstraction, copy-paste goldens/tests, dead CMake options, leftover scaffolding.",
    tests_parity: "TESTS & PARITY: Unit/parity/golden coverage vs claimed behavior; L0–L4 levels if Phase 6; ctest/pytest/init.sh story; flaky or vacuous tests; missing negative paths (cancel, empty, quirk cases).",
};

let review_jobs = [];
for d in dims {
    let brief = dim_briefs[d];
    let p = project_context();
    p += severity_guide();
    p += "TASK: Independent code review dimension = " + d + ".\n";
    p += "Dimension focus: " + brief + "\n\n";
    p += "Scope (from discovery agent — treat as claims to re-verify, not gospel):\n";
    p += scope_json + "\n\n";
    p += "Mandatory procedure:\n";
    p += "1. Re-open design docs and phase tracker entries for the listed feature_ids.\n";
    p += "2. Inspect code under code_roots with read_file/grep (and list_dir).\n";
    p += "3. Cross-check claims in evidence strings against actual tests/files when possible.\n";
    p += "4. Report at most 10 findings for THIS dimension only. Use unique id like " + d + "-01.\n";
    p += "5. severity must be one of: critical, high, medium, low.\n";
    p += "6. file should be path:line when possible, or path if whole-file.\n";
    p += "7. summary: 2-4 sentences overall for this dimension.\n";
    if focus != "" {
        p += "User focus: " + focus + "\n";
    }
    review_jobs.push(#{
        prompt: p,
        label: "review:" + d,
        capability_mode: "read-only",
        output_schema: findings_schema,
    });
}

let review_results = parallel(review_jobs);

let all_findings = [];
let dim_summaries = [];
let ri = 0;
for r in review_results {
    let dim_name = dims[ri];
    if r != () && r.success && r.output != () {
        dim_summaries.push(dim_name + ": " + r.output.summary);
        if r.output.findings != () {
            for f in r.output.findings {
                // tag dimension if missing from id
                all_findings.push(f);
            }
        }
    } else {
        log("WARN: review dimension failed: " + dim_name + " (fail-closed: treat as incomplete coverage)");
        dim_summaries.push(dim_name + ": AGENT_FAILED");
    }
    ri += 1;
}

log("Review collected " + all_findings.len().to_string() + " raw findings across dimensions");

// ---------------------------------------------------------------------------
// Phase: Triage
// ---------------------------------------------------------------------------

phase("Triage");

let triage_prompt = project_context();
triage_prompt += severity_guide();
triage_prompt += "TASK: Triage and dedupe review findings for delivery gate.\n\n";
triage_prompt += "Scope:\n" + scope_json + "\n\n";
triage_prompt += "Dimension summaries:\n" + json_encode(dim_summaries) + "\n\n";
triage_prompt += "Raw findings:\n" + json_encode(all_findings) + "\n\n";
triage_prompt += "Rules:\n";
triage_prompt += "1. Merge duplicates (same root cause). Keep the strongest severity.\n";
triage_prompt += "2. actionable = findings that should be fixed before declaring delivery_ready, ";
triage_prompt += "capped at 12, ordered critical → high → medium. Include blocks_delivery flags.\n";
triage_prompt += "3. deferred = real but non-blocking items for later (with reason).\n";
triage_prompt += "4. delivery_ready=true ONLY if zero critical AND zero high that block delivery ";
triage_prompt += "(blocks_delivery true or severity critical/high with unresolved acceptance/arch/test gaps).\n";
triage_prompt += "5. Counts must match actionable+deferred classification after merge.\n";
triage_prompt += "6. Re-read 1-2 key files if a finding looks hallucinated; drop unsupported claims.\n";
triage_prompt += "7. rationale must explain the delivery_ready decision in plain language.\n";

let triage_r = agent(triage_prompt, #{
    label: "triage",
    capability_mode: "read-only",
    output_schema: triage_schema,
});

if triage_r == () || !triage_r.success || triage_r.output == () {
    // fail closed: not delivery ready
    let fail_report = "# Phase review — triage failed\n\nScope phase: " + phase_s + "\n\nTriage agent failed; cannot assert delivery quality.\n";
    let fail_path = write_scratch_file("phase-review-report.md", fail_report);
    complete(#{
        path: fail_path,
        delivery_ready: false,
        reason: "triage_failed",
        phase: phase_s,
    });
}

let triage = triage_r.output;
if triage.delivery_ready == () { triage.delivery_ready = false; }
if triage.critical_count == () { triage.critical_count = 0; }
if triage.high_count == () { triage.high_count = 0; }
if triage.medium_count == () { triage.medium_count = 0; }
if triage.low_count == () { triage.low_count = 0; }
if triage.actionable == () { triage.actionable = []; }
if triage.deferred == () { triage.deferred = []; }
if triage.summary == () { triage.summary = ""; }
if triage.rationale == () { triage.rationale = ""; }

log("Triage: delivery_ready=" + triage.delivery_ready.to_string()
    + " C/H/M/L="
    + triage.critical_count.to_string() + "/"
    + triage.high_count.to_string() + "/"
    + triage.medium_count.to_string() + "/"
    + triage.low_count.to_string()
    + " actionable=" + arr_len(triage.actionable).to_string());

let round = 0;
let history = [];
history.push(#{
    round: 0,
    delivery_ready: triage.delivery_ready,
    actionable_n: arr_len(triage.actionable),
    summary: triage.summary,
});

let last_fingerprint = fingerprint(json_encode(triage.actionable));
let stall_count = 0;
let final_ready = as_bool(triage.delivery_ready, false);
let final_actionable = triage.actionable;
let final_deferred = triage.deferred;
let fix_log = [];
let recheck_log = [];

// Early exit if already ready or review-only with no fix loop needed
if final_ready || !do_fix {
    if !do_fix {
        log("fix=false: review-only mode, skipping fix/recheck loop");
    } else {
        log("Already delivery_ready after first triage");
    }
} else {
    // -----------------------------------------------------------------------
    // Loop: Fix → Recheck
    // -----------------------------------------------------------------------
    while round < max_rounds && !final_ready {
        round += 1;
        log("=== Fix/Recheck round " + round.to_string() + "/" + max_rounds.to_string() + " ===");

        // budget guard
        let b = budget();
        if b.remaining < 4 {
            log("Agent budget low (remaining=" + b.remaining.to_string() + "); stopping loop");
            break;
        }

        phase("Fix");
        let fix_prompt = project_context();
        fix_prompt += "TASK: Fix the actionable findings for this phase. Round " + round.to_string() + ".\n\n";
        fix_prompt += "Scope:\n" + scope_json + "\n\n";
        fix_prompt += "Actionable findings (fix these; priority order):\n";
        fix_prompt += json_encode(final_actionable) + "\n\n";
        fix_prompt += "Instructions:\n";
        fix_prompt += "1. Fix critical/high first. Skip medium/low if time/risk is high — list in skipped.\n";
        fix_prompt += "2. Stay inside phase scope. No drive-by refactors of unrelated modules.\n";
        fix_prompt += "3. Prefer minimal correct diffs. Match existing style.\n";
        fix_prompt += "4. After edits, run the relevant verification you can:\n";
        fix_prompt += "   - C++: cmake --build build/cpp && ctest --test-dir build/cpp --output-on-failure\n";
        fix_prompt += "     (if build/cpp missing: cmake -S cpp -B build/cpp -G Ninja -DCMAKE_BUILD_TYPE=Debug first)\n";
        fix_prompt += "   - Python: uv run ruff check . && uv run mypy src tests && uv run pytest (only if you touched Python)\n";
        fix_prompt += "5. Do NOT git commit/push. Do NOT update phaseN.json status to done unless evidence truly warrants and user asked — prefer notes only.\n";
        fix_prompt += "6. Do NOT expand into next sub-phase features.\n";
        fix_prompt += "7. Return fixed/skipped/verification/notes per schema. verification must quote commands run and pass/fail.\n";

        let fix_r = agent(fix_prompt, #{
            label: "fix-r" + round.to_string(),
            capability_mode: "all",
            output_schema: fix_schema,
        });

        let fix_out = #{ fixed: [], skipped: [], verification: "", notes: "" };
        if fix_r != () && fix_r.success && fix_r.output != () {
            fix_out = fix_r.output;
            if fix_out.fixed == () { fix_out.fixed = []; }
            if fix_out.skipped == () { fix_out.skipped = []; }
            if fix_out.verification == () { fix_out.verification = ""; }
            if fix_out.notes == () { fix_out.notes = ""; }
            fix_log.push(#{ round: round, output: fix_out });
            log("Fix round " + round.to_string() + ": fixed=" + arr_len(fix_out.fixed).to_string()
                + " skipped=" + arr_len(fix_out.skipped).to_string());
        } else {
            log("Fix agent failed on round " + round.to_string());
            fix_log.push(#{ round: round, output: #{}, error: "fix_agent_failed" });
            // still recheck — maybe partial edits happened
        }

        phase("Recheck");
        let re_prompt = project_context();
        re_prompt += severity_guide();
        re_prompt += "TASK: Adversarial recheck after fix round " + round.to_string() + ".\n\n";
        re_prompt += "Scope:\n" + scope_json + "\n\n";
        re_prompt += "Prior actionable findings:\n" + json_encode(final_actionable) + "\n\n";
        re_prompt += "Fix agent report:\n" + json_encode(fix_out) + "\n\n";
        re_prompt += "Procedure (mandatory):\n";
        re_prompt += "1. For each prior actionable id, independently read the code (and tests) to see if the issue is gone.\n";
        re_prompt += "2. Put still-broken items in still_open (do NOT trust the fix agent).\n";
        re_prompt += "3. Put truly fixed ids in resolved.\n";
        re_prompt += "4. Scan for new regressions introduced by the fix (new_findings), max 8.\n";
        re_prompt += "5. delivery_ready=true only if no still_open item is critical/high with blocks_delivery, ";
        re_prompt += "and no new critical/high blocks_delivery findings.\n";
        re_prompt += "6. evidence: what you read/ran; quote file:line or command results.\n";
        re_prompt += "7. Fail closed: if uncertain whether fixed, keep it in still_open.\n";

        let re_r = agent(re_prompt, #{
            label: "recheck-r" + round.to_string(),
            capability_mode: "execute",
            output_schema: recheck_schema,
        });

        if re_r == () || !re_r.success || re_r.output == () {
            log("Recheck agent failed on round " + round.to_string() + "; fail-closed");
            recheck_log.push(#{ round: round, error: "recheck_failed" });
            final_ready = false;
            break;
        }

        let re = re_r.output;
        if re.still_open == () { re.still_open = []; }
        if re.resolved == () { re.resolved = []; }
        if re.new_findings == () { re.new_findings = []; }
        if re.summary == () { re.summary = ""; }
        if re.delivery_ready == () { re.delivery_ready = false; }
        recheck_log.push(#{ round: round, output: re });
        final_ready = as_bool(re.delivery_ready, false);

        // rebuild actionable from still_open + new_findings
        let next_actionable = [];
        for f in re.still_open {
            next_actionable.push(#{
                id: as_str(f.id),
                severity: as_str(f.severity),
                title: as_str(f.title),
                file: as_str(f.file),
                issue: as_str(f.issue),
                suggestion: as_str(f.suggestion),
                blocks_delivery: as_bool(f.blocks_delivery, true),
            });
        }
        for f in re.new_findings {
            next_actionable.push(f);
        }
        final_actionable = next_actionable;

        history.push(#{
            round: round,
            delivery_ready: final_ready,
            actionable_n: arr_len(final_actionable),
            summary: re.summary,
            resolved_n: arr_len(re.resolved),
        });

        log("Recheck round " + round.to_string() + ": ready=" + final_ready.to_string()
            + " still_open+new=" + arr_len(final_actionable).to_string());

        if final_ready {
            break;
        }

        // stall detection
        let fp = fingerprint(json_encode(final_actionable));
        if fp == last_fingerprint {
            stall_count += 1;
            log("Stall fingerprint match streak=" + stall_count.to_string());
            if stall_count >= 2 {
                log("No progress for 2 consecutive rounds; pausing for user");
                await_user("no_progress", "phase-review stalled: same open findings for 2 rounds. Inspect /workflows, then resume to continue or stop.");
                stall_count = 0;
            }
        } else {
            stall_count = 0;
            last_fingerprint = fp;
        }

        if arr_len(final_actionable) == 0 {
            // no open items but delivery_ready false — inconsistent; force ready
            log("No actionable items left; treating as delivery_ready");
            final_ready = true;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Phase: Report
// ---------------------------------------------------------------------------

phase("Report");

let report = "";
report += "# Phase Review Report\n\n";
report += "- **phase arg:** " + phase_s + "\n";
report += "- **phase_id:** " + scope.phase_id + "\n";
report += "- **subphase:** " + scope.subphase + "\n";
report += "- **delivery_ready:** " + final_ready.to_string() + "\n";
report += "- **fix_enabled:** " + do_fix.to_string() + "\n";
report += "- **rounds_used:** " + round.to_string() + " / " + max_rounds.to_string() + "\n";
report += "- **features in scope:** " + json_encode(scope.feature_ids) + "\n";
report += "- **code_roots:** " + json_encode(scope.code_roots) + "\n\n";

report += "## Scope status\n\n";
report += scope.status_summary + "\n\n";
report += "### Acceptance bullets\n\n";
if scope.acceptance_bullets != () {
    for b in scope.acceptance_bullets {
        report += "- " + b + "\n";
    }
}
report += "\n";

report += "## Dimension summaries (initial review)\n\n";
for s in dim_summaries {
    report += "- " + s + "\n";
}
report += "\n";

report += "## Initial triage\n\n";
report += triage.summary + "\n\n";
report += triage.rationale + "\n\n";
report += "Counts C/H/M/L: "
    + triage.critical_count.to_string() + "/"
    + triage.high_count.to_string() + "/"
    + triage.medium_count.to_string() + "/"
    + triage.low_count.to_string() + "\n\n";

report += "## Loop history\n\n";
report += "```json\n" + json_encode(history) + "\n```\n\n";

report += "## Remaining actionable findings\n\n";
if arr_len(final_actionable) == 0 {
    report += "_None._\n\n";
} else {
    for f in final_actionable {
        let sev = as_str(f.severity);
        if sev == "" { sev = "?"; }
        let title = as_str(f.title);
        if title == "" { title = as_str(f.id); }
        let file = as_str(f.file);
        if file == "" { file = "?"; }
        let issue = as_str(f.issue);
        report += "### [" + sev + "] " + title + "\n";
        report += "- id: " + as_str(f.id) + "\n";
        report += "- file: " + file + "\n";
        report += "- issue: " + issue + "\n";
        let sug = as_str(f.suggestion);
        if sug != "" {
            report += "- suggestion: " + sug + "\n";
        }
        report += "\n";
    }
}

report += "## Deferred (non-blocking)\n\n";
if arr_len(final_deferred) == 0 {
    report += "_None recorded._\n\n";
} else {
    report += "```json\n" + json_encode(final_deferred) + "\n```\n\n";
}

report += "## Fix log\n\n";
report += "```json\n" + json_encode(fix_log) + "\n```\n\n";

report += "## Recheck log\n\n";
report += "```json\n" + json_encode(recheck_log) + "\n```\n\n";

report += "## Verdict\n\n";
if final_ready {
    report += "**DELIVERY READY** for the reviewed phase/sub-phase scope. ";
    report += "Residual low/deferred items may remain but do not block.\n";
} else {
    report += "**NOT DELIVERY READY.** ";
    report += "Open actionable findings remain after " + round.to_string() + " fix/recheck round(s). ";
    report += "Re-run with higher max_rounds, fix manually, or resume after addressing blockers.\n";
}

report += "\n---\n_Generated by workflow `phase-review`. Does not commit._\n";

let report_path = write_scratch_file("phase-review-report.md", report);
log("Report written: " + report_path);

complete(#{
    path: report_path,
    delivery_ready: final_ready,
    phase: phase_s,
    phase_id: scope.phase_id,
    subphase: scope.subphase,
    rounds: round,
    max_rounds: max_rounds,
    fix_enabled: do_fix,
    remaining_actionable: arr_len(final_actionable),
    history: history,
});
