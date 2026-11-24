#!/usr/bin/env python3
"""
scripts/check_dependencies.py - Cross-Stack Layered Architecture Dependency Checker for SubLift

Enforces strict unidirectional dependency boundaries, mutual exclusions,
adapter isolation, worker isolation, zero 3rd-party type leakages in public headers,
and desktop frontend unidirectional layering.

Rules enforced:
  Rule 1 (Adapter Isolation):
    core, features, timing, text, ports, engine must NEVER include adapter headers.
  Rule 2 (Mutual Exclusion):
    timing must NEVER include text headers, and text must NEVER include timing headers.
  Rule 3 (Features Privacy):
    text, engine, ports, core, adapters, worker must NEVER include features headers.
    Only timing may include features headers.
  Rule 4 (3rd-Party Type Leakage):
    - Public headers (include/sublift/* and cpp/adapters/*/include/*) must NEVER include
      OpenCV (<opencv2/...>), ONNX Runtime (<onnxruntime/...>, onnxruntime_cxx_api.h),
      FFmpeg (libav*), nlohmann JSON (<nlohmann/...>), Apple system framework headers,
      or Objective-C syntax (#import, @interface, @protocol, etc.).
    - Source files in pure layers (core, timing, text, ports, engine) and worker must NEVER
      include OpenCV, ONNX Runtime, FFmpeg C headers, or Apple framework headers.
    - Only src/features/ and cpp/adapters/paddle/src/ are allowed to privately include OpenCV.
    - Only cpp/adapters/paddle/src/ is allowed to privately include ONNX Runtime.
    - Only cpp/adapters/apple_vision/src/ is allowed to privately include Apple frameworks
      and Objective-C syntax.
    - Only cpp/adapters/*/src/ and tools/worker/ are allowed to privately include nlohmann JSON.
    - All modules must NEVER include FFmpeg C headers (libav*).
  Rule 5 (Engine Cleanliness):
    engine must NEVER include features or adapters.
  Rule 6 (Inward Dependency):
    adapters must NEVER include engine, timing, text, or features.
  Rule 7 (Core Domain Purity):
    core must NEVER include any other module headers.
  Rule 8 (Ports Contract Purity):
    ports must NEVER include features, timing, text, engine, or adapters.
  Rule 9 (Namespace Ownership & Hygiene):
    - core, ports, engine must use `sublift` (or `sublift::<module>::detail`).
    - features, timing, text public headers must use `sublift::<module>`;
      private headers/sources may use `sublift::<module>` or `sublift::<module>::detail`.
    - adapters/<name> public headers must use `sublift::adapters::<name>`;
      private headers/sources may use `sublift::adapters::<name>` or `sublift::adapters::<name>::detail`.
    - tools/worker must use `sublift::worker` or `sublift::worker::detail`.
  Rule 10 (Worker Isolation):
    - cpp/ and other non-worker modules must NEVER reverse-depend on tools/.
    - tools/worker must NEVER include tools/cli/, adapters/*/src/, or network socket headers.
  Rule TS-1..4 (Desktop Frontend Layering, when desktop/src/ exists):
    - RULE-TS-1: desktop/src/core/ must remain pure TS (no React/DOM/Zustand/Tauri/store/ui/styles).
    - RULE-TS-2: desktop/src/store/ may depend on core/, zustand, bridge, NOT react/react-dom/ui/styles.
    - RULE-TS-3: desktop/src/ui/ must not escape desktop/src/ via relative imports or use Node built-ins.
    - RULE-TS-4: desktop/src/styles/ may contain only .css files.

Exit codes:
  0 - All checks passed without violations.
  1 - One or more dependency violations detected.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import sys
from typing import Iterator, List, Optional, Tuple


@dataclass
class Violation:
    file_path: Path
    line_number: int
    rule_id: str
    rule_name: str
    message: str
    offending_line: str

    def format_diagnostic(self) -> str:
        return (
            f"[VIOLATION] {self.rule_id} - {self.rule_name}\n"
            f"  Location: {self.file_path}:{self.line_number}\n"
            f"  Details:  {self.message}\n"
            f"  Line:     {self.offending_line}"
        )


# Known internal C++ modules
KNOWN_MODULES = {"core", "features", "timing", "text", "ports", "engine", "adapters"}

# Ignored directory names during traversal
IGNORED_DIRS = {
    "build",
    ".git",
    ".agents",
    ".vscode",
    ".idea",
    "__pycache__",
    "tests",
    "node_modules",
    ".worktree",
    "skills",
    "dist",
    "target",
}

# C/C++/ObjC++ source and header extensions
CPP_EXTENSIONS = {".hpp", ".h", ".cpp", ".cc", ".cxx", ".c", ".mm"}

# TypeScript / JavaScript source extensions
TS_EXTENSIONS = {".ts", ".tsx", ".mts", ".cts", ".js", ".jsx", ".mjs", ".cjs"}

# Include/import pattern: #include <path> / #include "path" / #import <path> / #import "path"
INCLUDE_PATTERN = re.compile(r'^\s*#\s*(?:include|import)\s*[<"]([^>"]+)[>"]')

# Objective-C syntax / directive patterns
OBJC_IMPORT_PATTERN = re.compile(r'^\s*#\s*import\b')
OBJC_SYNTAX_PATTERN = re.compile(r'@(?:interface|protocol|implementation|property|class|end)\b')

# Explicit namespace block declaration (ignores anonymous `namespace {`, `using namespace`, and aliases `namespace fs = ...`)
NAMESPACE_DECL_PATTERN = re.compile(r'^\s*namespace\s+([A-Za-z0-9_:]+)\s*(?:\{|$)')

# Regex matching references to internal sublift modules in include paths
MODULE_INCLUDE_PATTERN = re.compile(
    r'(?:^|/)(?:sublift/)?(core|features|timing|text|ports|engine|adapters)/'
)

# 3rd-party library patterns
RE_OPENCV = re.compile(r'(?:^|/)opencv2?/')
RE_ONNX = re.compile(r'(?:^|/)onnxruntime(?:_cxx_api\.h|/|$)|(?:^|/)onnx/')
RE_FFMPEG = re.compile(r'(?:^|/)libav[a-z]+/')
RE_NLOHMANN = re.compile(r'(?:^|/)nlohmann/')
RE_APPLE_FRAMEWORK = re.compile(
    r'(?:^|/)(?:Vision|CoreGraphics|CoreVideo|CoreFoundation|CoreMedia|CoreText|Foundation|AppKit|Cocoa|ImageIO)/'
)

# Worker isolation patterns
RE_SOCKET_HEADER = re.compile(
    r'^(?:sys/socket\.h|sys/un\.h|netinet/[^/]+\.h|arpa/inet\.h|netdb\.h|winsock2?\.h|ws2tcpip\.h|asio(?:\.hpp|/.*)|boost/asio(?:\.hpp|/.*))$'
)
RE_TOOLS_INCLUDE = re.compile(r'(?:^|/)tools/')
RE_TOOLS_CLI_INCLUDE = re.compile(r'(?:^|/)tools/cli/')
RE_ADAPTER_PRIVATE_SRC = re.compile(r'(?:^|/)adapters/[^/]+/src/')

# TypeScript import/export/require patterns (matched against comment-stripped text)
TS_FROM_PATTERN = re.compile(
    r'\b(?:import|export)(?:\s+|(?=[{*]))(?:type(?:\s+|(?=[{*])))?(?:[^;\'"{}]|\{[^}]*\})*?\bfrom\s*[\'"]([^\'"]+)[\'"]'
)
TS_SIDE_EFFECT_IMPORT_PATTERN = re.compile(r'\bimport\s*[\'"]([^\'"]+)[\'"]')
TS_DYNAMIC_IMPORT_PATTERN = re.compile(r'\b(?:import|require)\s*\(\s*[\'"]([^\'"]+)[\'"]\s*(?:,|\))')

TS_FORBIDDEN_CORE_PACKAGES = re.compile(
    r'^(?:react|react-dom|zustand|@tauri-apps|@tanstack|lucide-react|@radix-ui|class-variance-authority|clsx|tailwind-merge|@testing-library)(?:/|$)'
)
TS_FORBIDDEN_STORE_PACKAGES = re.compile(
    r'^(?:react|react-dom|lucide-react|@tanstack|@radix-ui|class-variance-authority|clsx|tailwind-merge|@testing-library)(?:/|$)'
)
TS_FORBIDDEN_NODE_BUILTINS = re.compile(
    r'^(?:node:|(?:fs|path|child_process|os|net|http|https|crypto|stream|url|util|buffer|events|assert|process|worker_threads|zlib|readline|tty|dgram|dns|tls)(?:/|$))'
)
TS_DOM_BOM_PATTERN = re.compile(
    r'(?<!\.)\b(?:window|document|navigator|globalThis|localStorage|sessionStorage|HTMLElement|HTMLVideoElement|HTMLCanvasElement|DOMRect)\b'
)


def _strip_c_strings(line: str) -> str:
    """Replaces double-quoted and single-quoted literal contents with empty literals."""
    out: List[str] = []
    i = 0
    n = len(line)
    while i < n:
        ch = line[i]
        if ch in ('"', "'"):
            quote = ch
            out.append(ch)
            i += 1
            while i < n:
                if line[i] == "\\" and i + 1 < n:
                    i += 2
                    continue
                if line[i] == quote:
                    out.append(quote)
                    i += 1
                    break
                i += 1
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def _iter_spliced_lines(lines: Iterator[str]) -> Iterator[Tuple[int, str]]:
    """
    Splices physical C/C++ lines ending with an odd number of trailing backslashes,
    preserving the 1-based starting line number of each logical line.
    """
    buf: List[str] = []
    start_line_no: Optional[int] = None
    for line_no, raw_line in enumerate(lines, 1):
        stripped_nl = raw_line.rstrip("\r\n")
        if start_line_no is None:
            start_line_no = line_no
        bs_count = len(stripped_nl) - len(stripped_nl.rstrip("\\"))
        if bs_count % 2 == 1:
            buf.append(stripped_nl[:-1])
            continue
        buf.append(stripped_nl)
        yield start_line_no, "".join(buf)
        buf.clear()
        start_line_no = None
    if buf and start_line_no is not None:
        yield start_line_no, "".join(buf)


def iter_stripped_lines(file_path: Path) -> Iterator[Tuple[int, str, str]]:
    """
    Yields active C/C++ lines as (line_number, stripped_line, raw_line),
    honoring backslash line continuations and ignoring line comments (//) and block comments (/* ... */).
    """
    in_block_comment = False

    try:
        with open(file_path, "r", encoding="utf-8", errors="replace") as f:
            for line_no, raw_line in _iter_spliced_lines(f):
                line = raw_line

                # Handle multi-line block comments
                if in_block_comment:
                    if "*/" in line:
                        line = line.split("*/", 1)[1]
                        in_block_comment = False
                    else:
                        continue

                while "/*" in line:
                    before, after = line.split("/*", 1)
                    if "*/" in after:
                        line = before + after.split("*/", 1)[1]
                    else:
                        line = before
                        in_block_comment = True
                        break

                # Strip single-line comments
                line = line.split("//", 1)[0].strip()
                if line:
                    yield (line_no, line, raw_line.rstrip())
    except (OSError, UnicodeDecodeError) as e:
        print(f"[WARNING] Could not read file {file_path}: {e}", file=sys.stderr)


def extract_includes(file_path: Path) -> Iterator[Tuple[int, str, str]]:
    """
    Extracts all active #include / #import directives from a C/C++/ObjC++ file.
    Yields (line_number, include_target, raw_line).
    """
    for line_no, line, raw_line in iter_stripped_lines(file_path):
        m = INCLUDE_PATTERN.match(line)
        if m:
            yield (line_no, m.group(1).strip(), raw_line)


def extract_namespace_decls(file_path: Path) -> Iterator[Tuple[int, str, str]]:
    """
    Extracts explicit named namespace block declarations from a C/C++ file.
    Yields (line_number, namespace_name, raw_line).
    """
    for line_no, line, raw_line in iter_stripped_lines(file_path):
        m = NAMESPACE_DECL_PATTERN.match(line)
        if m:
            yield (line_no, m.group(1).strip(), raw_line)


def classify_file(file_path: Path, root_dir: Path) -> Tuple[Optional[str], bool]:
    """
    Determines which module a file belongs to, and whether it is a public header.
    Returns (module_name, is_public_header).
    If the file is not part of the analyzed C++ architecture (e.g. tests or build),
    returns (None, False).
    """
    try:
        rel = file_path.resolve().relative_to(root_dir.resolve())
    except ValueError:
        rel = file_path

    parts = rel.parts

    # Ignore build artifacts, tests, and metadata
    for ign in IGNORED_DIRS:
        if ign in parts:
            return None, False

    if file_path.suffix not in CPP_EXTENSIONS:
        return None, False

    # Check for tools/worker
    for i in range(len(parts) - 1):
        if parts[i] == "tools" and parts[i + 1] == "worker":
            return "worker", False

    # Check for adapters
    if "adapters" in parts:
        idx = parts.index("adapters")
        if "include" in parts[idx:]:
            return "adapters", True
        if "src" in parts[idx:]:
            return "adapters", False
        return "adapters", file_path.suffix in {".hpp", ".h"}

    # Check for public headers: include/sublift/<module>/... or include/<module>/...
    if "include" in parts:
        idx = parts.index("include")
        after = parts[idx + 1 :]
        if after and after[0] == "sublift":
            after = after[1:]
        if after:
            mod = after[0]
            if mod in KNOWN_MODULES:
                return mod, True
            return mod, True

    # Check for source files: src/<module>/...
    if "src" in parts:
        idx = parts.index("src")
        after = parts[idx + 1 :]
        if after:
            mod = after[0]
            if mod in KNOWN_MODULES:
                return mod, False
            return mod, False

    # Fallback: check if any path component matches a known module
    for p in parts[:-1]:
        if p in KNOWN_MODULES:
            is_pub = file_path.suffix in {".hpp", ".h"} and "include" in str(file_path)
            return p, is_pub

    return None, False


def detect_target_module(include_target: str) -> Optional[str]:
    """
    Identifies if an include target references an internal module.
    """
    m = MODULE_INCLUDE_PATTERN.search(include_target)
    if m:
        return m.group(1)
    if re.search(r'(?:^|/)adapters/', include_target):
        return "adapters"
    return None


def _is_adapter_private_source(file_path: Path, adapter_name: str) -> bool:
    parts = file_path.parts
    for i in range(len(parts) - 2):
        if parts[i] == "adapters" and parts[i + 1] == adapter_name and parts[i + 2] == "src":
            return True
    return False


def is_paddle_private_source(file_path: Path) -> bool:
    """
    Checks if a file is in cpp/adapters/paddle/src/ (or adapters/paddle/src/).
    """
    return _is_adapter_private_source(file_path, "paddle")


def is_apple_vision_private_source(file_path: Path) -> bool:
    """
    Checks if a file is in cpp/adapters/apple_vision/src/ (or adapters/apple_vision/src/).
    """
    return _is_adapter_private_source(file_path, "apple_vision")


def _resolves_into_tools(
    file_path: Path, include_target: str, root_dir: Path
) -> Optional[str]:
    if RE_TOOLS_INCLUDE.search(include_target):
        return include_target
    if include_target.startswith("."):
        try:
            resolved = (file_path.parent / include_target).resolve()
            rel = resolved.relative_to(root_dir.resolve()).as_posix()
            if rel == "tools" or rel.startswith("tools/"):
                return rel
        except ValueError:
            pass
    return None


def _resolves_into_tools_cli(
    file_path: Path, include_target: str, root_dir: Path
) -> bool:
    if RE_TOOLS_CLI_INCLUDE.search(include_target):
        return True
    if include_target.startswith("."):
        try:
            resolved = (file_path.parent / include_target).resolve()
            rel = resolved.relative_to(root_dir.resolve()).as_posix()
            if rel == "tools/cli" or rel.startswith("tools/cli/"):
                return True
        except ValueError:
            pass
    return False


def _resolves_into_adapter_private_src(
    file_path: Path, include_target: str, root_dir: Path
) -> bool:
    if RE_ADAPTER_PRIVATE_SRC.search(include_target):
        return True
    if include_target.startswith("."):
        try:
            resolved = (file_path.parent / include_target).resolve()
            rel = resolved.relative_to(root_dir.resolve()).as_posix()
            if RE_ADAPTER_PRIVATE_SRC.search(rel):
                return True
        except ValueError:
            pass
    return False


def extract_adapter_name(file_path: Path, root_dir: Path) -> Optional[str]:
    """
    Extracts `<adapter_name>` from paths under `adapters/<adapter_name>/...`.
    """
    try:
        rel = file_path.resolve().relative_to(root_dir.resolve())
    except ValueError:
        rel = file_path
    parts = rel.parts
    if "adapters" not in parts:
        return None
    idx = parts.index("adapters")
    if idx + 1 < len(parts) - 1 and parts[idx + 1] not in {"include", "src"}:
        return parts[idx + 1]
    return None


def check_namespace_ownership(
    file_path: Path, root_dir: Path, module: str, is_public_header: bool
) -> List[Violation]:
    """
    Rule 9: Ensures every explicit namespace declaration matches the file's module ownership.
    """
    violations: List[Violation] = []
    adapter_name = extract_adapter_name(file_path, root_dir) if module == "adapters" else None

    for line_no, ns_name, raw_line in extract_namespace_decls(file_path):
        allowed: Optional[set[str]] = None
        valid = False
        expected_desc = ""

        if module in {"core", "ports", "engine"}:
            allowed = {"sublift", f"sublift::{module}::detail"}
            valid = ns_name in allowed
            expected_desc = f"one of {sorted(allowed)}"
        elif module in {"features", "timing", "text"}:
            if is_public_header:
                allowed = {f"sublift::{module}"}
            else:
                allowed = {f"sublift::{module}", f"sublift::{module}::detail"}
            valid = ns_name in allowed
            expected_desc = f"one of {sorted(allowed)}"
        elif module == "adapters":
            if adapter_name:
                base_ns = f"sublift::adapters::{adapter_name}"
                allowed = {base_ns} if is_public_header else {base_ns, f"{base_ns}::detail"}
                valid = ns_name in allowed
                expected_desc = f"one of {sorted(allowed)}"
            else:
                if is_public_header:
                    valid = ns_name.startswith("sublift::adapters::") and not ns_name.endswith("::detail")
                    expected_desc = "'sublift::adapters::<name>'"
                else:
                    valid = ns_name.startswith("sublift::adapters::")
                    expected_desc = "'sublift::adapters::<name>(::detail)'"
        elif module == "worker":
            allowed = {"sublift::worker", "sublift::worker::detail"}
            valid = ns_name in allowed
            expected_desc = f"one of {sorted(allowed)}"
        else:
            valid = True

        if not valid:
            role = "public header" if is_public_header else "implementation/private file"
            violations.append(
                Violation(
                    file_path=file_path,
                    line_number=line_no,
                    rule_id="RULE-9",
                    rule_name="Rule 9 (Namespace Ownership & Hygiene)",
                    message=(
                        f"Namespace '{ns_name}' is not allowed in {role} of module '{module}' "
                        f"(expected {expected_desc})."
                    ),
                    offending_line=raw_line,
                )
            )

    return violations


def check_file(file_path: Path, root_dir: Path) -> Tuple[List[Violation], int]:
    """
    Inspects a single C/C++/ObjC++ file against all architecture dependency rules.
    Returns (list_of_violations, count_of_includes_checked).
    """
    violations: List[Violation] = []
    module, is_public_header = classify_file(file_path, root_dir)

    if module is None:
        return violations, 0

    includes_count = 0

    # Check Objective-C syntax / #import leakage outside adapters/apple_vision/src/
    allow_objc = (
        not is_public_header
        and module == "adapters"
        and is_apple_vision_private_source(file_path)
    )
    if not allow_objc:
        for line_no, line, raw_line in iter_stripped_lines(file_path):
            if OBJC_IMPORT_PATTERN.match(line):
                violations.append(
                    Violation(
                        file_path=file_path,
                        line_number=line_no,
                        rule_id="RULE-4",
                        rule_name="Rule 4 (3rd-Party Type Leakage)",
                        message=(
                            f"Objective-C #import is forbidden in "
                            f"{'public header' if is_public_header else f'module {module!r}'}. "
                            "Zero Objective-C syntax may leak outside 'adapters/apple_vision/src/'."
                        ),
                        offending_line=raw_line,
                    )
                )
                continue
            code_only = _strip_c_strings(line)
            m_objc = OBJC_SYNTAX_PATTERN.search(code_only)
            if m_objc:
                violations.append(
                    Violation(
                        file_path=file_path,
                        line_number=line_no,
                        rule_id="RULE-4",
                        rule_name="Rule 4 (3rd-Party Type Leakage)",
                        message=(
                            f"Objective-C syntax '{m_objc.group(0)}' is forbidden in "
                            f"{'public header' if is_public_header else f'module {module!r}'}. "
                            "Zero Objective-C syntax may leak outside 'adapters/apple_vision/src/'."
                        ),
                        offending_line=raw_line,
                    )
                )

    for line_no, include_target, raw_line in extract_includes(file_path):
        includes_count += 1

        # ---------------------------------------------------------------------
        # Check Rule 4: Third-Party Type Leakage
        # ---------------------------------------------------------------------
        is_opencv = bool(RE_OPENCV.search(include_target))
        is_onnx = bool(RE_ONNX.search(include_target))
        is_ffmpeg = bool(RE_FFMPEG.search(include_target))
        is_nlohmann = bool(RE_NLOHMANN.search(include_target))
        is_apple_fw = bool(RE_APPLE_FRAMEWORK.search(include_target))

        if is_opencv:
            if is_public_header:
                violations.append(
                    Violation(
                        file_path=file_path,
                        line_number=line_no,
                        rule_id="RULE-4",
                        rule_name="Rule 4 (3rd-Party Type Leakage)",
                        message=(
                            f"Public header in module '{module}' must NEVER include OpenCV header "
                            f"('<{include_target}>'). Zero 3rd-party types may leak into public contracts."
                        ),
                        offending_line=raw_line,
                    )
                )
            elif module == "features":
                pass
            elif module == "adapters" and is_paddle_private_source(file_path):
                pass
            else:
                violations.append(
                    Violation(
                        file_path=file_path,
                        line_number=line_no,
                        rule_id="RULE-4",
                        rule_name="Rule 4 (3rd-Party Type Leakage)",
                        message=(
                            f"Implementation file in module '{module}' must NEVER include OpenCV header "
                            f"('<{include_target}>'). OpenCV is strictly PRIVATE to 'features' and 'adapters/paddle/src/'."
                        ),
                        offending_line=raw_line,
                    )
                )

        if is_onnx:
            if is_public_header:
                violations.append(
                    Violation(
                        file_path=file_path,
                        line_number=line_no,
                        rule_id="RULE-4",
                        rule_name="Rule 4 (3rd-Party Type Leakage)",
                        message=(
                            f"Public header in module '{module}' must NEVER include ONNX Runtime header "
                            f"('<{include_target}>'). Zero 3rd-party types may leak into public contracts."
                        ),
                        offending_line=raw_line,
                    )
                )
            elif module == "adapters" and is_paddle_private_source(file_path):
                pass
            else:
                violations.append(
                    Violation(
                        file_path=file_path,
                        line_number=line_no,
                        rule_id="RULE-4",
                        rule_name="Rule 4 (3rd-Party Type Leakage)",
                        message=(
                            f"Module '{module}' must NEVER include ONNX Runtime header "
                            f"('<{include_target}>'). ONNX Runtime is strictly PRIVATE to 'adapters/paddle/src/'."
                        ),
                        offending_line=raw_line,
                    )
                )

        if is_ffmpeg:
            if is_public_header:
                violations.append(
                    Violation(
                        file_path=file_path,
                        line_number=line_no,
                        rule_id="RULE-4",
                        rule_name="Rule 4 (3rd-Party Type Leakage)",
                        message=(
                            f"Public header in module '{module}' must NEVER include FFmpeg header "
                            f"('<{include_target}>'). Zero 3rd-party types may leak into public contracts."
                        ),
                        offending_line=raw_line,
                    )
                )
            else:
                violations.append(
                    Violation(
                        file_path=file_path,
                        line_number=line_no,
                        rule_id="RULE-4",
                        rule_name="Rule 4 (3rd-Party Type Leakage)",
                        message=(
                            f"Module '{module}' must NEVER include FFmpeg header "
                            f"('<{include_target}>'). FFmpeg CLI subprocess architecture prohibits libav* C library header inclusion."
                        ),
                        offending_line=raw_line,
                    )
                )

        if is_nlohmann:
            if is_public_header:
                violations.append(
                    Violation(
                        file_path=file_path,
                        line_number=line_no,
                        rule_id="RULE-4",
                        rule_name="Rule 4 (3rd-Party Type Leakage)",
                        message=(
                            f"Public header in module '{module}' must NEVER include nlohmann JSON header "
                            f"('<{include_target}>'). Zero 3rd-party types may leak into public contracts."
                        ),
                        offending_line=raw_line,
                    )
                )
            elif module in {"core", "features", "timing", "text", "ports", "engine"}:
                violations.append(
                    Violation(
                        file_path=file_path,
                        line_number=line_no,
                        rule_id="RULE-4",
                        rule_name="Rule 4 (3rd-Party Type Leakage)",
                        message=(
                            f"Pure layer module '{module}' must NEVER include nlohmann JSON header "
                            f"('<{include_target}>'). JSON is strictly PRIVATE to adapter implementations."
                        ),
                        offending_line=raw_line,
                    )
                )

        if is_apple_fw:
            if is_public_header:
                violations.append(
                    Violation(
                        file_path=file_path,
                        line_number=line_no,
                        rule_id="RULE-4",
                        rule_name="Rule 4 (3rd-Party Type Leakage)",
                        message=(
                            f"Public header in module '{module}' must NEVER include Apple framework header "
                            f"('<{include_target}>'). Zero Apple framework/ObjC types may leak into public contracts."
                        ),
                        offending_line=raw_line,
                    )
                )
            elif module == "adapters" and is_apple_vision_private_source(file_path):
                pass
            else:
                violations.append(
                    Violation(
                        file_path=file_path,
                        line_number=line_no,
                        rule_id="RULE-4",
                        rule_name="Rule 4 (3rd-Party Type Leakage)",
                        message=(
                            f"Module '{module}' must NEVER include Apple framework header "
                            f"('<{include_target}>'). Apple frameworks are strictly PRIVATE to 'adapters/apple_vision/src/'."
                        ),
                        offending_line=raw_line,
                    )
                )

        # ---------------------------------------------------------------------
        # Check Rule 10: Worker Isolation
        # ---------------------------------------------------------------------
        if module == "worker" and RE_SOCKET_HEADER.match(include_target):
            violations.append(
                Violation(
                    file_path=file_path,
                    line_number=line_no,
                    rule_id="RULE-10",
                    rule_name="Rule 10 (Worker Isolation)",
                    message=(
                        f"tools/worker must NEVER include network socket header ('{include_target}'). "
                        "sublift_worker communicates strictly over stdio JSON-Lines with zero network sockets."
                    ),
                    offending_line=raw_line,
                )
            )

        if module != "worker":
            tools_ref = _resolves_into_tools(file_path, include_target, root_dir)
            if tools_ref is not None:
                violations.append(
                    Violation(
                        file_path=file_path,
                        line_number=line_no,
                        rule_id="RULE-10",
                        rule_name="Rule 10 (Worker Isolation)",
                        message=(
                            f"cpp/ module '{module}' must NEVER reverse-depend on tools/ ('{include_target}')."
                        ),
                        offending_line=raw_line,
                    )
                )

        if module == "worker" and _resolves_into_tools_cli(file_path, include_target, root_dir):
            violations.append(
                Violation(
                    file_path=file_path,
                    line_number=line_no,
                    rule_id="RULE-10",
                    rule_name="Rule 10 (Worker Isolation)",
                    message=(
                        f"tools/worker must NEVER include tools/cli/ ('{include_target}')."
                    ),
                    offending_line=raw_line,
                )
            )

        if module == "worker" and _resolves_into_adapter_private_src(file_path, include_target, root_dir):
            violations.append(
                Violation(
                    file_path=file_path,
                    line_number=line_no,
                    rule_id="RULE-10",
                    rule_name="Rule 10 (Worker Isolation)",
                    message=(
                        f"tools/worker may only include public adapter headers, never adapters/*/src/ ('{include_target}')."
                    ),
                    offending_line=raw_line,
                )
            )

        # ---------------------------------------------------------------------
        # Check Internal Architecture Rules
        # ---------------------------------------------------------------------
        target_mod = detect_target_module(include_target)
        if not target_mod:
            continue

        # Rule 1: Adapter Isolation
        # core, features, timing, text, ports, engine must NEVER include adapters
        if target_mod == "adapters" and module in {"core", "features", "timing", "text", "ports", "engine"}:
            violations.append(
                Violation(
                    file_path=file_path,
                    line_number=line_no,
                    rule_id="RULE-1",
                    rule_name="Rule 1 (Adapter Isolation)",
                    message=(
                        f"Core algorithm module '{module}' must NEVER include adapter headers "
                        f"('{include_target}')."
                    ),
                    offending_line=raw_line,
                )
            )

        # Rule 2: Mutual Exclusion
        # timing must NEVER include text, and text must NEVER include timing
        if module == "timing" and target_mod == "text":
            violations.append(
                Violation(
                    file_path=file_path,
                    line_number=line_no,
                    rule_id="RULE-2",
                    rule_name="Rule 2 (Mutual Exclusion)",
                    message=(
                        f"Module 'timing' must NEVER include 'text' headers ('{include_target}'). "
                        "Timing and text pipelines are strictly orthogonal."
                    ),
                    offending_line=raw_line,
                )
            )
        if module == "text" and target_mod == "timing":
            violations.append(
                Violation(
                    file_path=file_path,
                    line_number=line_no,
                    rule_id="RULE-2",
                    rule_name="Rule 2 (Mutual Exclusion)",
                    message=(
                        f"Module 'text' must NEVER include 'timing' headers ('{include_target}'). "
                        "Text post-processing must not depend on timing."
                    ),
                    offending_line=raw_line,
                )
            )

        # Rule 3: Features Privacy
        # text, engine, ports, core, adapters, worker must NEVER include features. Only timing may include features.
        if target_mod == "features" and module in {"text", "engine", "ports", "core", "adapters", "worker"}:
            violations.append(
                Violation(
                    file_path=file_path,
                    line_number=line_no,
                    rule_id="RULE-3",
                    rule_name="Rule 3 (Features Privacy)",
                    message=(
                        f"Module '{module}' must NEVER include 'features' headers ('{include_target}'). "
                        "'features' is strictly private to 'timing'."
                    ),
                    offending_line=raw_line,
                )
            )

        # Rule 5: Engine Cleanliness
        # engine must NEVER include features or adapters
        if module == "engine" and target_mod in {"features", "adapters"}:
            violations.append(
                Violation(
                    file_path=file_path,
                    line_number=line_no,
                    rule_id="RULE-5",
                    rule_name="Rule 5 (Engine Cleanliness)",
                    message=(
                        f"Engine orchestrator must NEVER include '{target_mod}' headers ('{include_target}')."
                    ),
                    offending_line=raw_line,
                )
            )

        # Rule 6: Adapter Inward Dependency
        # adapters must NEVER include engine, timing, text, or features
        if module == "adapters" and target_mod in {"engine", "timing", "text", "features"}:
            violations.append(
                Violation(
                    file_path=file_path,
                    line_number=line_no,
                    rule_id="RULE-6",
                    rule_name="Rule 6 (Adapter Inward Dependency)",
                    message=(
                        f"Adapter must NEVER include '{target_mod}' headers ('{include_target}'). "
                        "Adapters are leaf plugins implementing ports contracts."
                    ),
                    offending_line=raw_line,
                )
            )

        # Rule 7: Core Domain Purity
        # core must NEVER include any other module
        if module == "core" and target_mod != "core":
            violations.append(
                Violation(
                    file_path=file_path,
                    line_number=line_no,
                    rule_id="RULE-7",
                    rule_name="Rule 7 (Core Domain Purity)",
                    message=(
                        f"Core domain module must NEVER include '{target_mod}' headers ('{include_target}'). "
                        "Core must remain a pure primitive layer."
                    ),
                    offending_line=raw_line,
                )
            )

        # Rule 8: Ports Contract Purity
        # ports must only include core
        if module == "ports" and target_mod not in {"ports", "core"}:
            violations.append(
                Violation(
                    file_path=file_path,
                    line_number=line_no,
                    rule_id="RULE-8",
                    rule_name="Rule 8 (Ports Contract Purity)",
                    message=(
                        f"Ports interface contract must NEVER include '{target_mod}' headers ('{include_target}'). "
                        "Ports may only depend on core domain primitives."
                    ),
                    offending_line=raw_line,
                )
            )

    violations.extend(check_namespace_ownership(file_path, root_dir, module, is_public_header))

    return violations, includes_count


def _scan_ts_preserving_newlines(text: str, strip_strings: bool) -> str:
    """
    Strips JS/TS // and /* */ comments while preserving newline counts.
    When strip_strings=True, also blanks out string and template literal contents.
    """
    out: List[str] = []
    i = 0
    n = len(text)
    state = "normal"

    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if state == "normal":
            if ch == "/" and nxt == "/":
                out.append("  ")
                i += 2
                state = "line_comment"
            elif ch == "/" and nxt == "*":
                out.append("  ")
                i += 2
                state = "block_comment"
            elif ch == "'":
                out.append("'" if not strip_strings else " ")
                i += 1
                state = "single_quote"
            elif ch == '"':
                out.append('"' if not strip_strings else " ")
                i += 1
                state = "double_quote"
            elif ch == "`":
                out.append("`" if not strip_strings else " ")
                i += 1
                state = "template"
            else:
                out.append(ch)
                i += 1
        elif state == "line_comment":
            if ch == "\n":
                out.append("\n")
                state = "normal"
            else:
                out.append(" ")
            i += 1
        elif state == "block_comment":
            if ch == "*" and nxt == "/":
                out.append("  ")
                i += 2
                state = "normal"
            elif ch == "\n":
                out.append("\n")
                i += 1
            else:
                out.append(" ")
                i += 1
        elif state == "single_quote":
            if ch == "\\" and i + 1 < n:
                if not strip_strings:
                    out.append(ch)
                    out.append(nxt)
                else:
                    out.append("\n" if nxt == "\n" else "  ")
                i += 2
            elif ch == "'":
                out.append("'" if not strip_strings else " ")
                i += 1
                state = "normal"
            elif ch == "\n":
                out.append("\n")
                i += 1
                state = "normal"
            else:
                out.append(ch if not strip_strings else " ")
                i += 1
        elif state == "double_quote":
            if ch == "\\" and i + 1 < n:
                if not strip_strings:
                    out.append(ch)
                    out.append(nxt)
                else:
                    out.append("\n" if nxt == "\n" else "  ")
                i += 2
            elif ch == '"':
                out.append('"' if not strip_strings else " ")
                i += 1
                state = "normal"
            elif ch == "\n":
                out.append("\n")
                i += 1
                state = "normal"
            else:
                out.append(ch if not strip_strings else " ")
                i += 1
        elif state == "template":
            if ch == "\\" and i + 1 < n:
                if not strip_strings:
                    out.append(ch)
                    out.append(nxt)
                else:
                    out.append("\n" if nxt == "\n" else "  ")
                i += 2
            elif ch == "`":
                out.append("`" if not strip_strings else " ")
                i += 1
                state = "normal"
            elif ch == "\n":
                out.append("\n")
                i += 1
            else:
                out.append(ch if not strip_strings else " ")
                i += 1

    return "".join(out)


def extract_ts_imports(stripped_text: str, raw_text: str) -> List[Tuple[int, str, str]]:
    """
    Extracts (line_number, import_target, offending_line) from comment-stripped TS/JS source.
    """
    raw_lines = raw_text.splitlines()
    found: List[Tuple[int, int, str, str]] = []
    seen_spans: set[Tuple[int, int]] = set()

    for pat in (TS_FROM_PATTERN, TS_SIDE_EFFECT_IMPORT_PATTERN, TS_DYNAMIC_IMPORT_PATTERN):
        for m in pat.finditer(stripped_text):
            span = m.span(1)
            if span in seen_spans:
                continue
            seen_spans.add(span)
            line_no = stripped_text.count("\n", 0, m.start(1)) + 1
            line_text = raw_lines[line_no - 1].rstrip() if line_no <= len(raw_lines) else ""
            found.append((m.start(1), line_no, m.group(1).strip(), line_text))

    found.sort(key=lambda item: item[0])
    return [(line_no, target, line_text) for _, line_no, target, line_text in found]


def _resolve_ts_relative_layer(
    file_path: Path, import_target: str, desktop_src: Path
) -> Tuple[bool, Optional[str]]:
    """
    Resolves a relative TS/JS import against desktop/src/.
    Returns (inside_desktop_src, target_layer_or_None).
    """
    resolved = (file_path.parent / import_target).resolve()
    try:
        rel = resolved.relative_to(desktop_src.resolve())
    except ValueError:
        return False, None
    parts = rel.parts
    return True, (parts[0] if parts else None)


def check_ts_tree(desktop_src: Path, verbose: bool = False) -> Tuple[List[Violation], int, int]:
    """
    Checks desktop/src/{core,store,ui,styles}/ unidirectional layering rules.
    Returns (violations, checked_files_count, checked_imports_count).
    """
    violations: List[Violation] = []
    files_count = 0
    imports_count = 0
    desktop_src_resolved = desktop_src.resolve()

    for root, dirs, files in os.walk(desktop_src_resolved):
        dirs[:] = [d for d in dirs if d not in IGNORED_DIRS]
        for fname in sorted(files):
            fpath = Path(root) / fname
            rel = fpath.relative_to(desktop_src_resolved)
            if not rel.parts:
                continue
            layer = rel.parts[0]
            if layer not in {"core", "store", "ui", "styles"}:
                continue

            if layer == "styles":
                files_count += 1
                if fpath.suffix.lower() != ".css":
                    violations.append(
                        Violation(
                            file_path=fpath,
                            line_number=1,
                            rule_id="RULE-TS-4",
                            rule_name="Rule TS-4 (Styles CSS-Only Allowance)",
                            message=(
                                f"File '{rel.as_posix()}' inside desktop/src/styles/ must be a .css file."
                            ),
                            offending_line=fname,
                        )
                    )
                continue

            if fpath.suffix not in TS_EXTENSIONS:
                continue

            files_count += 1
            try:
                raw_text = fpath.read_text(encoding="utf-8", errors="replace")
            except OSError as e:
                print(f"[WARNING] Could not read file {fpath}: {e}", file=sys.stderr)
                continue

            stripped_comments = _scan_ts_preserving_newlines(raw_text, strip_strings=False)
            code_no_strings = _scan_ts_preserving_newlines(raw_text, strip_strings=True)

            file_violations: List[Violation] = []
            ts_imports = extract_ts_imports(stripped_comments, raw_text)
            imports_count += len(ts_imports)

            for line_no, target, line_text in ts_imports:
                if layer == "core":
                    if target.startswith("."):
                        inside, target_layer = _resolve_ts_relative_layer(
                            fpath, target, desktop_src_resolved
                        )
                        if not inside or target_layer in {"store", "ui", "styles"}:
                            file_violations.append(
                                Violation(
                                    file_path=fpath,
                                    line_number=line_no,
                                    rule_id="RULE-TS-1",
                                    rule_name="Rule TS-1 (Core Purity)",
                                    message=(
                                        f"desktop/src/core/ must NEVER import '{target}' "
                                        "(cannot depend on store/, ui/, styles/, or escape desktop/src/)."
                                    ),
                                    offending_line=line_text,
                                )
                            )
                    elif re.match(r'^(?:@/|src/|@)(store|ui|styles)(?:/|$)', target):
                        file_violations.append(
                            Violation(
                                file_path=fpath,
                                line_number=line_no,
                                rule_id="RULE-TS-1",
                                rule_name="Rule TS-1 (Core Purity)",
                                message=(
                                    f"desktop/src/core/ must NEVER import '{target}'."
                                ),
                                offending_line=line_text,
                            )
                        )
                    elif TS_FORBIDDEN_CORE_PACKAGES.match(target):
                        file_violations.append(
                            Violation(
                                file_path=fpath,
                                line_number=line_no,
                                rule_id="RULE-TS-1",
                                rule_name="Rule TS-1 (Core Purity)",
                                message=(
                                    f"desktop/src/core/ must remain pure TypeScript and NEVER import '{target}'."
                                ),
                                offending_line=line_text,
                            )
                        )
                    elif TS_FORBIDDEN_NODE_BUILTINS.match(target):
                        file_violations.append(
                            Violation(
                                file_path=fpath,
                                line_number=line_no,
                                rule_id="RULE-TS-1",
                                rule_name="Rule TS-1 (Core Purity)",
                                message=(
                                    f"desktop/src/core/ must remain pure TypeScript and NEVER import Node built-in '{target}'."
                                ),
                                offending_line=line_text,
                            )
                        )
                elif layer == "store":
                    if target.startswith("."):
                        inside, target_layer = _resolve_ts_relative_layer(
                            fpath, target, desktop_src_resolved
                        )
                        if not inside or target_layer in {"ui", "styles"}:
                            file_violations.append(
                                Violation(
                                    file_path=fpath,
                                    line_number=line_no,
                                    rule_id="RULE-TS-2",
                                    rule_name="Rule TS-2 (Store Unidirectional Boundary)",
                                    message=(
                                        f"desktop/src/store/ must NEVER import '{target}' "
                                        "(cannot depend on ui/, styles/, or escape desktop/src/)."
                                    ),
                                    offending_line=line_text,
                                )
                            )
                    elif re.match(r'^(?:@/|src/|@)(ui|styles)(?:/|$)', target):
                        file_violations.append(
                            Violation(
                                file_path=fpath,
                                line_number=line_no,
                                rule_id="RULE-TS-2",
                                rule_name="Rule TS-2 (Store Unidirectional Boundary)",
                                message=(
                                    f"desktop/src/store/ must NEVER import '{target}'."
                                ),
                                offending_line=line_text,
                            )
                        )
                    elif TS_FORBIDDEN_STORE_PACKAGES.match(target):
                        file_violations.append(
                            Violation(
                                file_path=fpath,
                                line_number=line_no,
                                rule_id="RULE-TS-2",
                                rule_name="Rule TS-2 (Store Unidirectional Boundary)",
                                message=(
                                    f"desktop/src/store/ must NEVER import UI package '{target}'."
                                ),
                                offending_line=line_text,
                            )
                        )
                    elif TS_FORBIDDEN_NODE_BUILTINS.match(target):
                        file_violations.append(
                            Violation(
                                file_path=fpath,
                                line_number=line_no,
                                rule_id="RULE-TS-2",
                                rule_name="Rule TS-2 (Store Unidirectional Boundary)",
                                message=(
                                    f"desktop/src/store/ must NEVER import Node built-in '{target}'."
                                ),
                                offending_line=line_text,
                            )
                        )
                elif layer == "ui":
                    if target.startswith("."):
                        inside, _ = _resolve_ts_relative_layer(
                            fpath, target, desktop_src_resolved
                        )
                        if not inside:
                            file_violations.append(
                                Violation(
                                    file_path=fpath,
                                    line_number=line_no,
                                    rule_id="RULE-TS-3",
                                    rule_name="Rule TS-3 (UI Boundary)",
                                    message=(
                                        f"desktop/src/ui/ must NEVER escape desktop/src/ via relative import '{target}'."
                                    ),
                                    offending_line=line_text,
                                )
                            )
                    elif TS_FORBIDDEN_NODE_BUILTINS.match(target):
                        file_violations.append(
                            Violation(
                                file_path=fpath,
                                line_number=line_no,
                                rule_id="RULE-TS-3",
                                rule_name="Rule TS-3 (UI Boundary)",
                                message=(
                                    f"desktop/src/ui/ must NEVER import Node built-in '{target}'."
                                ),
                                offending_line=line_text,
                            )
                        )

            if layer == "core":
                raw_lines = raw_text.splitlines()
                for m_dom in TS_DOM_BOM_PATTERN.finditer(code_no_strings):
                    line_no = code_no_strings.count("\n", 0, m_dom.start()) + 1
                    line_text = raw_lines[line_no - 1].rstrip() if line_no <= len(raw_lines) else ""
                    file_violations.append(
                        Violation(
                            file_path=fpath,
                            line_number=line_no,
                            rule_id="RULE-TS-1",
                            rule_name="Rule TS-1 (Core Purity)",
                            message=(
                                f"desktop/src/core/ must NEVER reference DOM/BOM symbol '{m_dom.group(0)}'."
                            ),
                            offending_line=line_text,
                        )
                    )

            violations.extend(file_violations)
            if verbose:
                status = "[FAIL]" if file_violations else "[OK]"
                print(f"  {status} desktop/src/{rel.as_posix()} ({len(ts_imports)} imports)")

    return violations, files_count, imports_count


def check_dependencies(root_path: Path, verbose: bool = False) -> Tuple[List[Violation], int, int]:
    """
    Scans the given directory tree and checks all C/C++/ObjC++ and Desktop TS/CSS files.
    Returns (violations_list, scanned_files_count, checked_includes_count).
    """
    root_resolved = root_path.resolve()
    all_violations: List[Violation] = []
    total_files = 0
    total_includes = 0

    cpp_dir = root_resolved / "cpp"
    worker_dir = root_resolved / "tools" / "worker"
    desktop_src = root_resolved / "desktop" / "src"

    if cpp_dir.is_dir() or worker_dir.is_dir() or desktop_src.is_dir():
        scan_roots: List[Path] = []
        if cpp_dir.is_dir():
            scan_roots.append(cpp_dir)
        if worker_dir.is_dir():
            scan_roots.append(worker_dir)
    else:
        scan_roots = [root_resolved]

    for s_root in scan_roots:
        for root, dirs, files in os.walk(s_root):
            # Prune ignored directories
            dirs[:] = [d for d in dirs if d not in IGNORED_DIRS]

            for fname in sorted(files):
                fpath = Path(root) / fname
                if fpath.suffix not in CPP_EXTENSIONS:
                    continue

                mod, _ = classify_file(fpath, root_resolved)
                if mod is None:
                    continue

                total_files += 1
                violations, inc_count = check_file(fpath, root_resolved)
                total_includes += inc_count
                all_violations.extend(violations)

                if verbose:
                    status = "[FAIL]" if violations else "[OK]"
                    print(f"  {status} {fpath.relative_to(root_resolved)} ({inc_count} includes)")

    if desktop_src.is_dir():
        ts_violations, ts_files, ts_imports = check_ts_tree(desktop_src, verbose=verbose)
        all_violations.extend(ts_violations)
        total_files += ts_files
        total_includes += ts_imports

    return all_violations, total_files, total_includes


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="SubLift Layered Architecture Dependency Checker"
    )
    parser.add_argument(
        "path",
        nargs="?",
        default=None,
        help="Root path to inspect (defaults to project root).",
    )
    parser.add_argument(
        "-r",
        "--root",
        dest="root_flag",
        default=None,
        help="Explicit project root directory.",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Enable verbose output showing all inspected files.",
    )

    args = parser.parse_args(argv)

    # Determine target root directory
    if args.root_flag:
        target_root = Path(args.root_flag)
    elif args.path:
        target_root = Path(args.path)
    else:
        # Default: parent of scripts/ directory
        target_root = Path(__file__).resolve().parent.parent

    if not target_root.exists():
        print(f"[ERROR] Target directory does not exist: {target_root}", file=sys.stderr)
        return 1

    print("Checking layered architecture dependency rules...")
    print(f"Inspecting directory: {target_root.resolve()}")

    violations, total_files, total_includes = check_dependencies(target_root, verbose=args.verbose)

    if violations:
        print(f"\n[FAILED] Found {len(violations)} architectural dependency violation(s):\n")
        for v in violations:
            print(v.format_diagnostic())
            print()
        print(f"Summary: {len(violations)} violation(s) across {total_files} files ({total_includes} includes checked).")
        return 1
    else:
        print(f"[CLEAN] All dependency rules passed ({total_files} files, {total_includes} includes inspected, 0 violations).")
        return 0


if __name__ == "__main__":
    sys.exit(main())
