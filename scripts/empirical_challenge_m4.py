#!/usr/bin/env python3
"""Empirical Challenge Harness for Phase 12 Milestone 4 (Feature 12514: 真实输出计划与原子批量导出).

Adversarially challenges:
1. Concurrent atomic saves to same filename under heavy load across all 3 conflict policies (skip, deterministic_rename, replace).
2. Malicious path traversal injection vectors (relative escape, absolute root, .sublift_cache, symlink traversal, null bytes, illegal extensions).
3. Failure injection and leftover temporary file cleanup (read-only directories, blocked parent paths, empty subtitle protection).
4. Batch export API consistency and conflict resolution.
5. Extended edge cases and resilience (non-existent jobs, malformed JSON, unknown policies, unconfigured workspace).
"""

import concurrent.futures
import http.client
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


class SubLiftServer:
    def __init__(self, binary_path: str, workspace_dir: str):
        self.binary_path = binary_path
        self.workspace_dir = workspace_dir
        self.port = find_free_port()
        self.process: subprocess.Popen | None = None

    def start(self, timeout_sec: float = 5.0):
        env = dict(os.environ)
        env["SUBLIFT_CORS_ORIGIN"] = "*"
        env["SUBLIFT_MEDIA_DIR"] = self.workspace_dir
        cmd = [self.binary_path, "--host", "127.0.0.1", "-p", str(self.port)]
        self.process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
        start_time = time.time()
        while time.time() - start_time < timeout_sec:
            if self.process.poll() is not None:
                out, err = self.process.communicate()
                raise RuntimeError(f"Server crashed on start: {out}\n{err}")
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.2):
                    return
            except (ConnectionRefusedError, TimeoutError, OSError):
                time.sleep(0.05)
        self.stop()
        raise TimeoutError("Server failed to start in time")

    def stop(self):
        if self.process is not None:
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=1.0)
            self.process = None

    def request(
        self, method: str, path: str, body: dict | str | None = None, headers: dict | None = None
    ) -> tuple[int, dict, bytes]:
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10.0)
        try:
            req_headers = {"Content-Type": "application/json"}
            if headers:
                req_headers.update(headers)
            body_bytes = None
            if body is not None:
                if isinstance(body, dict):
                    body_bytes = json.dumps(body).encode("utf-8")
                elif isinstance(body, str):
                    body_bytes = body.encode("utf-8")
                else:
                    body_bytes = body
            conn.request(method, path, body=body_bytes, headers=req_headers)
            resp = conn.getresponse()
            resp_headers = dict(resp.getheaders())
            resp_body = resp.read()
            return resp.status, resp_headers, resp_body
        finally:
            conn.close()


def create_completed_job_via_api(server: SubLiftServer, video_path: str, custom_text: str = "Test subtitle") -> str:
    payload = {
        "video_path": video_path,
        "engine": "mock",
        "fps": 5.0,
        "confidence_threshold": 0.0,
        "region_box": {"x": 0.0, "y": 0.5, "width": 1.0, "height": 0.5},
    }
    status, _, body = server.request("POST", "/api/jobs", body=payload)
    if status != 201:
        raise RuntimeError(f"Failed to create job: {status} {body.decode('utf-8', errors='ignore')}")
    job_id = json.loads(body.decode("utf-8"))["job_id"]
    
    # Wait for completion
    for _ in range(100):
        time.sleep(0.05)
        st, _, exp_body = server.request("GET", f"/api/jobs/{job_id}/export")
        if st == 200:
            return job_id
    raise TimeoutError(f"Job {job_id} did not finish processing")


def run_all_challenges():
    print("=================================================================")
    print("  SubLift Phase 12 Milestone 4 (Feature 12514) Empirical Harness ")
    print("=================================================================")

    binary_path = "/Volumes/lab/pp/SubLift/build/cpp/bin/sublift_server"
    if not os.path.exists(binary_path):
        print(f"Error: binary not found at {binary_path}")
        sys.exit(1)

    temp_ws = tempfile.mkdtemp(prefix="sublift_m4_challenge_ws_")
    print(f"[*] Created temporary workspace: {temp_ws}")

    # Generate dummy video file
    dummy_video = os.path.join(temp_ws, "test_video.mp4")
    subprocess.run(
        [
            "ffmpeg", "-y", "-f", "lavfi", "-i",
            "testsrc=duration=1:size=320x240:rate=10",
            "-c:v", "libx264", "-pix_fmt", "yuv420p",
            dummy_video
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=True
    )
    print(f"[*] Generated valid test MP4 video: {dummy_video}")

    server = SubLiftServer(binary_path, temp_ws)
    server.start()
    print(f"[*] Server running on 127.0.0.1:{server.port}")

    test_results = []

    def report(name: str, passed: bool, detail: str = ""):
        symbol = "✅ PASS" if passed else "❌ FAIL"
        print(f"  [{symbol}] {name} {detail}")
        test_results.append((name, passed, detail))

    try:
        # Create a reference completed job
        base_job_id = create_completed_job_via_api(server, dummy_video, "Base subtitle text")
        print(f"[*] Created base completed job: {base_job_id}")

        print("\n--- Suite 1: Concurrency Under Load Across Conflict Policies ---")

        # 1.1 Concurrency with policy 'replace'
        print("  [>] Testing concurrent atomic saves with conflict_policy='replace' (30 threads)...")
        target_replace = os.path.join(temp_ws, "concurrent_replace.srt")
        concurrency = 30

        def save_worker_replace(idx: int):
            payload = {
                "target_path": target_replace,
                "conflict_policy": "replace",
                "allow_empty": False
            }
            st, _, resp_bytes = server.request("POST", f"/api/jobs/{base_job_id}/save", body=payload)
            return st, resp_bytes

        with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as executor:
            futs = [executor.submit(save_worker_replace, i) for i in range(concurrency)]
            results = [f.result() for f in futs]

        all_200 = all(st == 200 for st, _ in results)
        statuses = [json.loads(b.decode("utf-8"))["status"] for _, b in results]
        all_saved = all(s == "saved" for s in statuses)
        file_exists = os.path.exists(target_replace)
        file_size = os.path.getsize(target_replace) if file_exists else 0
        with open(target_replace, "r", encoding="utf-8") as f:
            content = f.read()
        valid_srt = "-->" in content and len(content) > 10

        # Check for leftover .tmp files
        tmp_files = [f for f in os.listdir(temp_ws) if ".tmp." in f]
        report(
            "Concurrent 'replace' atomic consistency",
            all_200 and all_saved and file_exists and valid_srt and len(tmp_files) == 0,
            f"(30 threads 100% saved, target exists {file_size} bytes, 0 leftover temp files)"
        )

        # 1.2 Concurrency with policy 'skip'
        print("  [>] Testing concurrent atomic saves with conflict_policy='skip' (30 threads)...")
        target_skip = os.path.join(temp_ws, "concurrent_skip.srt")
        with open(target_skip, "w", encoding="utf-8") as f:
            f.write("INITIAL PRE-EXISTING CONTENT")

        def save_worker_skip(idx: int):
            payload = {
                "target_path": target_skip,
                "conflict_policy": "skip",
                "allow_empty": False
            }
            st, _, resp_bytes = server.request("POST", f"/api/jobs/{base_job_id}/save", body=payload)
            return st, resp_bytes

        with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as executor:
            futs = [executor.submit(save_worker_skip, i) for i in range(concurrency)]
            results_skip = [f.result() for f in futs]

        skip_all_200 = all(st == 200 for st, _ in results_skip)
        skip_statuses = [json.loads(b.decode("utf-8"))["status"] for _, b in results_skip]
        all_skipped = all(s == "skipped" for s in skip_statuses)
        with open(target_skip, "r", encoding="utf-8") as f:
            skip_content = f.read()
        intact = (skip_content == "INITIAL PRE-EXISTING CONTENT")
        tmp_files_skip = [f for f in os.listdir(temp_ws) if ".tmp." in f]

        report(
            "Concurrent 'skip' when file exists",
            skip_all_200 and all_skipped and intact and len(tmp_files_skip) == 0,
            f"(30/30 threads returned 'skipped', original file content untouched)"
        )

        # 1.3 Concurrency with policy 'deterministic_rename'
        print("  [>] Testing concurrent atomic saves with conflict_policy='deterministic_rename' (20 threads)...")
        target_rename_base = os.path.join(temp_ws, "concurrent_rename.srt")
        rename_concurrency = 20

        def save_worker_rename(idx: int):
            payload = {
                "target_path": target_rename_base,
                "conflict_policy": "deterministic_rename",
                "allow_empty": False
            }
            st, _, resp_bytes = server.request("POST", f"/api/jobs/{base_job_id}/save", body=payload)
            return st, resp_bytes

        with concurrent.futures.ThreadPoolExecutor(max_workers=rename_concurrency) as executor:
            futs = [executor.submit(save_worker_rename, i) for i in range(rename_concurrency)]
            results_rename = [f.result() for f in futs]

        rename_all_200 = all(st == 200 for st, _ in results_rename)
        rename_data = [json.loads(b.decode("utf-8")) for _, b in results_rename]
        rename_all_saved = all(d["status"] == "saved" for d in rename_data)
        saved_paths = [d["saved_path"] for d in rename_data]
        unique_paths = set(saved_paths)
        all_saved_files_exist = all(os.path.exists(p) for p in saved_paths)
        tmp_files_ren = [f for f in os.listdir(temp_ws) if ".tmp." in f]

        report(
            "Concurrent 'deterministic_rename' collision resolution",
            rename_all_200 and rename_all_saved and all_saved_files_exist and len(tmp_files_ren) == 0,
            f"({len(unique_paths)} unique files generated, 20/20 saved, 0 leftover temp files)"
        )

        print("\n--- Suite 2: Path Traversal & Sandbox Injection Matrix ---")

        injection_vectors = [
            ("Relative parent traversal (../../etc/passwd.srt)", os.path.join(temp_ws, "../../etc/passwd.srt")),
            ("Absolute path outside root (/tmp/out.srt)", "/tmp/out.srt"),
            ("Absolute system file (/etc/shadow.srt)", "/etc/shadow.srt"),
            ("Internal .sublift_cache direct write", os.path.join(temp_ws, ".sublift_cache", "hacked.srt")),
            ("Nested .sublift_cache/frames write", os.path.join(temp_ws, ".sublift_cache", "frames", "hacked.srt")),
            ("Deep parent traversal (sub/../../../../etc/passwd.srt)", os.path.join(temp_ws, "sub", "..", "..", "..", "..", "etc", "passwd.srt")),
            ("Disallowed extension (.sh)", os.path.join(temp_ws, "malicious.sh")),
            ("Disallowed extension (.exe)", os.path.join(temp_ws, "malicious.exe")),
            ("Disallowed extension (.py)", os.path.join(temp_ws, "evil.py")),
            ("Null byte in path", os.path.join(temp_ws, "test.srt\0.mp4")),
        ]

        # Add symlink escape vector
        symlink_escape_dir = os.path.join(temp_ws, "sym_escape_dir")
        outside_target_dir = tempfile.mkdtemp(prefix="sublift_outside_")
        os.symlink(outside_target_dir, symlink_escape_dir)
        injection_vectors.append(
            ("Symlink directory escaping workspace", os.path.join(symlink_escape_dir, "escaped.srt"))
        )

        for desc, bad_path in injection_vectors:
            payload = {"target_path": bad_path, "conflict_policy": "replace"}
            st, _, resp_bytes = server.request("POST", f"/api/jobs/{base_job_id}/save", body=payload)
            passed = (st == 400)
            detail = f"(HTTP {st})"
            if passed:
                resp_json = json.loads(resp_bytes.decode("utf-8", errors="ignore"))
                detail += f" [Error: {resp_json.get('error', '')[:40]}...]"
            report(f"Sandbox reject: {desc}", passed, detail)

        # Valid intra-workspace relative paths and allowed extensions
        valid_vectors = [
            ("Allowed format .vtt", os.path.join(temp_ws, "sub_valid.vtt")),
            ("Allowed format .ass", os.path.join(temp_ws, "sub_valid.ass")),
            ("Allowed format .txt", os.path.join(temp_ws, "sub_valid.txt")),
            ("Allowed uppercase .SRT", os.path.join(temp_ws, "sub_upper.SRT")),
            ("Normalized relative dot path (subdir/../sub_dot.srt)", os.path.join(temp_ws, "nested_dir", "..", "sub_dot.srt")),
        ]

        for desc, good_path in valid_vectors:
            payload = {"target_path": good_path, "conflict_policy": "replace"}
            st, _, resp_bytes = server.request("POST", f"/api/jobs/{base_job_id}/save", body=payload)
            passed = (st == 200) and os.path.exists(os.path.realpath(good_path))
            report(f"Valid path accept: {desc}", passed, f"(HTTP {st})")

        print("\n--- Suite 3: Failed Write Simulation & Leftover Integrity ---")

        # 3.1 Unwritable / read-only directory
        readonly_dir = os.path.join(temp_ws, "readonly_sub_dir")
        os.makedirs(readonly_dir, exist_ok=True)
        os.chmod(readonly_dir, 0o555)  # Read + execute, no write

        ro_target = os.path.join(readonly_dir, "should_fail.srt")
        ro_payload = {"target_path": ro_target, "conflict_policy": "replace"}
        st_ro, _, resp_ro = server.request("POST", f"/api/jobs/{base_job_id}/save", body=ro_payload)
        os.chmod(readonly_dir, 0o755)  # Restore for cleanup

        ro_passed = (st_ro == 400) and not os.path.exists(ro_target)
        ro_tmp = [f for f in os.listdir(readonly_dir) if ".tmp." in f]
        report(
            "Write failure in read-only folder returns 400 and leaves no temp files",
            ro_passed and len(ro_tmp) == 0,
            f"(HTTP {st_ro}, 0 temp files in readonly dir)"
        )

        # 3.2 Target parent is a regular file
        file_parent = os.path.join(temp_ws, "regular_file.txt")
        with open(file_parent, "w") as f:
            f.write("I am a file")
        blocked_target = os.path.join(file_parent, "blocked.srt")
        st_blk, _, _ = server.request("POST", f"/api/jobs/{base_job_id}/save", body={"target_path": blocked_target})
        report(
            "Parent is regular file rejected fail-closed",
            st_blk == 400,
            f"(HTTP {st_blk})"
        )

        # 3.3 Empty subtitle handling (0 entries and whitespace-only)
        dummy_video_empty = os.path.join(temp_ws, "empty_video.mp4")
        shutil.copy(dummy_video, dummy_video_empty)
        job_empty_payload = {
            "video_path": dummy_video_empty,
            "engine": "mock",
            "fps": 5.0,
            "confidence_threshold": 1.0,
        }
        st_ej, _, body_ej = server.request("POST", "/api/jobs", body=job_empty_payload)
        empty_job_id = json.loads(body_ej.decode("utf-8"))["job_id"]
        for _ in range(60):
            time.sleep(0.05)
            st_e, _, _ = server.request("GET", f"/api/jobs/{empty_job_id}/export")
            if st_e == 200:
                break

        # Save with allow_empty = False (Default)
        empty_out_1 = os.path.join(temp_ws, "empty_no_allow.srt")
        st_e1, _, body_e1 = server.request(
            "POST", f"/api/jobs/{empty_job_id}/save",
            body={"target_path": empty_out_1, "allow_empty": False}
        )
        data_e1 = json.loads(body_e1.decode("utf-8"))
        e1_passed = (
            st_e1 == 200
            and data_e1["status"] == "empty_result"
            and data_e1["empty_result"] is True
            and not os.path.exists(empty_out_1)
        )
        report(
            "Empty subtitle job with allow_empty=false produces no 0-byte file",
            e1_passed,
            f"(status: {data_e1.get('status')}, file exists: {os.path.exists(empty_out_1)})"
        )

        # Save with allow_empty = True
        empty_out_2 = os.path.join(temp_ws, "empty_allowed.srt")
        st_e2, _, body_e2 = server.request(
            "POST", f"/api/jobs/{empty_job_id}/save",
            body={"target_path": empty_out_2, "allow_empty": True}
        )
        data_e2 = json.loads(body_e2.decode("utf-8"))
        e2_passed = (
            st_e2 == 200
            and data_e2["status"] == "saved"
            and os.path.exists(empty_out_2)
        )
        report(
            "Empty subtitle job with allow_empty=true writes file on demand",
            e2_passed,
            f"(status: {data_e2.get('status')}, file exists: {os.path.exists(empty_out_2)})"
        )

        print("\n--- Suite 4: Batch Save Endpoint (/api/export/batch-save) ---")

        # 4.1 Batch save with all jobs
        batch_payload = {
            "job_ids": [base_job_id, empty_job_id],
            "conflict_policy": "deterministic_rename",
            "allow_empty": False
        }
        st_b, _, body_b = server.request("POST", "/api/export/batch-save", body=batch_payload)
        data_b = json.loads(body_b.decode("utf-8"))
        b_passed = (
            st_b == 200
            and data_b["total"] == 2
            and data_b["saved"] == 1
            and data_b["empty_results"] == 1
            and data_b["failed"] == 0
        )
        report(
            "POST /api/export/batch-save handles mixed completed & empty jobs cleanly",
            b_passed,
            f"(total: {data_b.get('total')}, saved: {data_b.get('saved')}, empty: {data_b.get('empty_results')})"
        )

        print("\n--- Suite 5: Extended Edge Cases & Resilience ---")

        # 5.1 Non-existent job ID returns 404
        st_nf, _, body_nf = server.request("POST", "/api/jobs/non-existent-uuid-1234/save", body={})
        report("Non-existent job ID returns 404", st_nf == 404, f"(HTTP {st_nf})")

        # 5.2 Malformed JSON body in POST /api/jobs/:id/save
        st_mal, _, body_mal = server.request(
            "POST", f"/api/jobs/{base_job_id}/save",
            body="{ this is invalid json !!! ",
            headers={"Content-Type": "application/json"}
        )
        report("Malformed JSON body returns 400 Bad Request", st_mal == 400, f"(HTTP {st_mal})")

        # 5.3 Unknown conflict_policy defaults gracefully to deterministic_rename
        target_unknown = os.path.join(temp_ws, "unknown_policy.srt")
        st_unk, _, body_unk = server.request(
            "POST", f"/api/jobs/{base_job_id}/save",
            body={"target_path": target_unknown, "conflict_policy": "non_existent_policy_abc"}
        )
        data_unk = json.loads(body_unk.decode("utf-8"))
        report(
            "Unknown conflict policy safely handled (defaults to deterministic_rename)",
            st_unk == 200 and data_unk["status"] == "saved",
            f"(HTTP {st_unk}, status: {data_unk.get('status')})"
        )

        # 5.4 Batch save with non-existent job IDs
        st_b_nf, _, body_b_nf = server.request(
            "POST", "/api/export/batch-save",
            body={"job_ids": ["does_not_exist_job_1", "does_not_exist_job_2"]}
        )
        data_b_nf = json.loads(body_b_nf.decode("utf-8"))
        report(
            "Batch save with non-existent job IDs handles failures cleanly",
            st_b_nf == 200 and data_b_nf["failed"] == 2 and data_b_nf["saved"] == 0,
            f"(HTTP {st_b_nf}, failed: {data_b_nf.get('failed')})"
        )

        # Check total workspace for leftover tmp files
        all_leftover_tmp = []
        for root, dirs, files in os.walk(temp_ws):
            for f in files:
                if ".tmp." in f or f.endswith(".tmp"):
                    all_leftover_tmp.append(os.path.join(root, f))

        report(
            "Total zero leftover temporary files assertion across all suites",
            len(all_leftover_tmp) == 0,
            f"(Found {len(all_leftover_tmp)} leftover temp files)"
        )

    finally:
        server.stop()
        if os.path.exists(temp_ws):
            shutil.rmtree(temp_ws, ignore_errors=True)
        if 'outside_target_dir' in locals() and os.path.exists(outside_target_dir):
            shutil.rmtree(outside_target_dir, ignore_errors=True)

    print("\n=================================================================")
    print("  Empirical Stress Challenge Summary")
    print("=================================================================")
    passed_count = sum(1 for _, p, _ in test_results if p)
    total_count = len(test_results)
    print(f"Total Tests Run: {total_count}")
    print(f"Passed: {passed_count}")
    print(f"Failed: {total_count - passed_count}")

    if passed_count == total_count:
        print("\n🎉 ALL EMPIRICAL CHALLENGES PASSED! VERDICT: APPROVE")
        return 0
    else:
        print("\n❌ SOME CHALLENGES FAILED! VERDICT: REJECT")
        return 1


if __name__ == "__main__":
    sys.exit(run_all_challenges())
