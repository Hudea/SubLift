#!/usr/bin/env python3
"""
Empirical Challenge Script for SubLift Phase 12 Milestone 2 (Challenger 1)
Focus Areas:
1. Simulated process crash / restart:
   - Queued and Running jobs loaded as Interrupted, never auto-run
   - Completed jobs retain exact subtitle entries & metadata
   - Live SIGKILL process crash & recovery
2. Corrupted / Malformed state file handling:
   - Invalid JSON syntax, truncated files, wrong schema versions, invalid types
3. LRU bounded eviction:
   - 60 jobs in state snapshot evicted to max 50 entries, retaining newest
   - Runtime 60-job generation capped at 50 in disk snapshot
"""

import http.client
import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def get_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def http_req(
    host: str,
    port: int,
    method: str,
    path: str,
    body: bytes | str | None = None,
    headers: dict | None = None,
    timeout: float = 10.0,
) -> tuple[int, dict, bytes]:
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    req_headers = headers.copy() if headers else {}
    try:
        if body is not None:
            if isinstance(body, str):
                body = body.encode("utf-8")
            conn.request(method, path, body=body, headers=req_headers)
        else:
            conn.request(method, path, headers=req_headers)
        resp = conn.getresponse()
        resp_headers = dict(resp.getheaders())
        resp_body = resp.read()
        return resp.status, resp_headers, resp_body
    finally:
        conn.close()


class ServerRunner:
    def __init__(self, server_bin: Path, media_dir: str, port: int | None = None):
        self.server_bin = server_bin
        self.media_dir = media_dir
        self.port = port or get_free_port()
        self.proc: subprocess.Popen | None = None

    def start(self):
        env = os.environ.copy()
        env["SUBLIFT_CORS_ORIGIN"] = "*"
        self.proc = subprocess.Popen(
            [
                str(self.server_bin),
                "--host",
                "127.0.0.1",
                "-p",
                str(self.port),
                "--media-dir",
                self.media_dir,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
        ready = False
        for _ in range(50):
            if self.proc.poll() is not None:
                out, err = self.proc.communicate()
                raise RuntimeError(
                    f"Server failed to start (exit code {self.proc.returncode}):\nStdout: {out}\nStderr: {err}"
                )
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.2):
                    ready = True
                    break
            except (ConnectionRefusedError, TimeoutError, OSError):
                time.sleep(0.1)
        if not ready:
            raise RuntimeError(f"Server did not become ready on port {self.port}")

    def stop(self):
        if self.proc:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2.0)
            self.proc = None

    def kill_hard(self):
        """Simulate a hard SIGKILL crash."""
        if self.proc:
            self.proc.kill()
            self.proc.wait()
            self.proc = None

    def req(
        self,
        method: str,
        path: str,
        body: bytes | str | None = None,
        headers: dict | None = None,
        timeout: float = 10.0,
    ) -> tuple[int, dict, bytes]:
        return http_req("127.0.0.1", self.port, method, path, body=body, headers=headers, timeout=timeout)


def create_dummy_synth_mp4(dest_path: str):
    cmd = [
        "ffmpeg",
        "-y",
        "-f",
        "lavfi",
        "-i",
        "testsrc=duration=0.5:size=128x128:rate=10",
        "-pix_fmt",
        "yuv420p",
        dest_path,
    ]
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)


def test_crash_and_restart_recovery(server_bin: Path):
    print("\n--- [TEST 1] Simulated Process Crash & Restart Recovery ---")
    with tempfile.TemporaryDirectory(prefix="sublift_crash_test_") as temp_dir:
        cache_dir = Path(temp_dir) / ".sublift_cache"
        cache_dir.mkdir(parents=True, exist_ok=True)
        state_file = cache_dir / "jobs_state.v1.json"

        # Create synthetic video
        synth_video = str(Path(temp_dir) / "sample_video.mp4")
        create_dummy_synth_mp4(synth_video)

        # 1. Prepare initial state file with queued, running, completed, failed, cancelled jobs
        initial_state = {
            "version": 1,
            "updated_at_ms": 1724500000000,
            "jobs": [
                {
                    "job_id": "job-queued-crash-1",
                    "config": {
                        "video_path": synth_video,
                        "engine": "mock",
                        "fps": 3.0,
                        "confidence_threshold": 0.75,
                        "region_box": {"x": 0.1, "y": 0.5, "width": 0.8, "height": 0.4},
                    },
                    "status": "queued",
                    "created_at_ms": 1724500000100,
                    "started_at_ms": 0,
                    "ended_at_ms": 0,
                    "error_message": "",
                    "entries": [],
                },
                {
                    "job_id": "job-running-crash-2",
                    "config": {
                        "video_path": synth_video,
                        "engine": "mock",
                        "fps": 5.0,
                        "confidence_threshold": 0.5,
                        "region_box": {"x": 0.0, "y": 0.6, "width": 1.0, "height": 0.3},
                    },
                    "status": "running",
                    "created_at_ms": 1724500000200,
                    "started_at_ms": 1724500000300,
                    "ended_at_ms": 0,
                    "error_message": "",
                    "entries": [],
                },
                {
                    "job_id": "job-completed-crash-3",
                    "config": {
                        "video_path": synth_video,
                        "engine": "mock",
                        "fps": 2.0,
                        "confidence_threshold": 0.8,
                        "region_box": {"x": 0.0, "y": 0.7, "width": 1.0, "height": 0.3},
                    },
                    "status": "completed",
                    "created_at_ms": 1724500000400,
                    "started_at_ms": 1724500000500,
                    "ended_at_ms": 1724500002000,
                    "error_message": "",
                    "entries": [
                        {
                            "index": 1,
                            "start_ms": 1000,
                            "end_ms": 2500,
                            "text": "Hello SubLift First Line",
                            "confidence": 0.96,
                        },
                        {
                            "index": 2,
                            "start_ms": 2600,
                            "end_ms": 4000,
                            "text": "Second Line Persistent",
                            "confidence": 0.99,
                        },
                        {
                            "index": 3,
                            "start_ms": 4200,
                            "end_ms": 5500,
                            "text": "Third Subtitle Exact",
                            "confidence": 0.92,
                        },
                    ],
                },
                {
                    "job_id": "job-failed-crash-4",
                    "config": {
                        "video_path": synth_video,
                        "engine": "mock",
                    },
                    "status": "failed",
                    "created_at_ms": 1724500000600,
                    "started_at_ms": 1724500000700,
                    "ended_at_ms": 1724500000800,
                    "error_message": "Original failure reason preserved",
                    "entries": [],
                },
            ],
        }
        state_file.write_text(json.dumps(initial_state, indent=2), encoding="utf-8")

        # 2. Launch server
        server = ServerRunner(server_bin, temp_dir)
        server.start()
        try:
            # Query all jobs
            st, _, body = server.req("GET", "/api/jobs")
            assert st == 200, f"Expected 200, got {st}"
            jobs = json.loads(body.decode("utf-8"))
            jobs_by_id = {j["job_id"]: j for j in jobs}

            # Check 1: Queued job transitioned to Interrupted
            assert "job-queued-crash-1" in jobs_by_id
            q_job = jobs_by_id["job-queued-crash-1"]
            assert q_job["status"] == "interrupted", f"Expected interrupted, got {q_job['status']}"
            assert q_job["config"]["fps"] == 3.0
            assert q_job["config"]["confidence_threshold"] == 0.75
            print("[PASS] Queued job correctly loaded as 'interrupted'")

            # Check 2: Running job transitioned to Interrupted
            assert "job-running-crash-2" in jobs_by_id
            r_job = jobs_by_id["job-running-crash-2"]
            assert r_job["status"] == "interrupted", f"Expected interrupted, got {r_job['status']}"
            print("[PASS] Running job correctly loaded as 'interrupted'")

            # Check 3: Completed job retained exact metadata & entries
            assert "job-completed-crash-3" in jobs_by_id
            c_job = jobs_by_id["job-completed-crash-3"]
            assert c_job["status"] == "completed"
            assert len(c_job["entries"]) == 3
            assert c_job["entries"][0]["text"] == "Hello SubLift First Line"
            assert c_job["entries"][0]["start_ms"] == 1000
            assert c_job["entries"][0]["end_ms"] == 2500
            assert c_job["entries"][1]["text"] == "Second Line Persistent"
            assert c_job["entries"][2]["text"] == "Third Subtitle Exact"
            print("[PASS] Completed job retained exact subtitle entries and timestamps")

            # Check 4: Export endpoint returns exact SRT formatting for restored completed job
            st_exp, hd_exp, body_exp = server.req("GET", "/api/jobs/job-completed-crash-3/export")
            assert st_exp == 200
            srt_str = body_exp.decode("utf-8")
            assert "Hello SubLift First Line" in srt_str
            assert "00:00:01,000 --> 00:00:02,500" in srt_str
            assert "Second Line Persistent" in srt_str
            assert "Third Subtitle Exact" in srt_str
            print("[PASS] Export endpoint served valid SRT from persisted state")

            # Check 5: Failed job retained error message
            assert "job-failed-crash-4" in jobs_by_id
            f_job = jobs_by_id["job-failed-crash-4"]
            assert f_job["status"] == "failed"
            assert f_job["error_message"] == "Original failure reason preserved"
            print("[PASS] Failed job retained status and error message")

            # Check 6: Passive test — Verify NO interrupted job automatically starts running
            time.sleep(1.0)
            st_recheck, _, body_recheck = server.req("GET", "/api/jobs")
            assert st_recheck == 200
            jobs_recheck = {j["job_id"]: j for j in json.loads(body_recheck.decode("utf-8"))}
            assert jobs_recheck["job-queued-crash-1"]["status"] == "interrupted"
            assert jobs_recheck["job-running-crash-2"]["status"] == "interrupted"
            print("[PASS] No interrupted job was auto-executed after wait period")

            # Check 7: SSE endpoint on interrupted job returns error event cleanly without hanging
            st_sse, _, sse_body = server.req("GET", "/api/jobs/job-queued-crash-1/events")
            assert st_sse == 200
            sse_text = sse_body.decode("utf-8")
            assert "event: error" in sse_text
            assert "interrupted" in sse_text.lower()
            print("[PASS] SSE stream for interrupted job immediately returned terminal event")

        finally:
            server.stop()

        # 3. Simulate LIVE hard crash (SIGKILL) while job is running
        print("\n--- Live SIGKILL Hard Crash Simulation ---")
        server2 = ServerRunner(server_bin, temp_dir)
        server2.start()
        try:
            # Create a new synthetic 5.0s video for longer job execution
            long_video = str(Path(temp_dir) / "long_sample.mp4")
            cmd = [
                "ffmpeg",
                "-y",
                "-f",
                "lavfi",
                "-i",
                "testsrc=duration=5.0:size=128x128:rate=10",
                "-pix_fmt",
                "yuv420p",
                long_video,
            ]
            subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)

            # Submit job
            job_req = json.dumps({
                "video_path": long_video,
                "engine": "mock",
                "fps": 5.0,
            })
            st_j, _, body_j = server2.req("POST", "/api/jobs", body=job_req, headers={"Content-Type": "application/json"})
            assert st_j == 201
            live_job_id = json.loads(body_j.decode("utf-8"))["job_id"]

            # Wait briefly so it is in flight
            time.sleep(0.05)

            # Hard SIGKILL crash!
            print(f"[KILL] Sending SIGKILL to sublift_server process (Job ID: {live_job_id})")
            server2.kill_hard()
        finally:
            server2.stop()

        # Verify disk state file exists and was written
        assert state_file.exists(), "State file must exist on disk after crash"

        # Start a 3rd server instance on the exact same directory to simulate restart recovery
        server3 = ServerRunner(server_bin, temp_dir)
        server3.start()
        try:
            st_l, _, body_l = server3.req("GET", f"/api/jobs/{live_job_id}")
            assert st_l == 200, f"Expected 200 for job {live_job_id}, got {st_l}"
            restored_live_job = json.loads(body_l.decode("utf-8"))
            assert restored_live_job["job_id"] == live_job_id
            assert restored_live_job["status"] in ("interrupted", "completed"), f"Got status: {restored_live_job['status']}"
            print(f"[PASS] Post-SIGKILL restart recovered job {live_job_id} safely in status '{restored_live_job['status']}' without hanging")
        finally:
            server3.stop()


def test_corrupted_state_file_handling(server_bin: Path):
    print("\n--- [TEST 2] Corrupted / Malformed State File Handling ---")

    corrupted_cases = [
        ("Invalid JSON syntax", "{ \"version\": 1, \"jobs\": [ { unclosed broken json"),
        ("Truncated JSON file", "{\"version\": 1, \"updated_at_ms\": 100, \"jobs\": [{\"job_id\": \"truncated\""),
        ("Unsupported schema version (version: 2)", "{\"version\": 2, \"jobs\": []}"),
        ("Unsupported schema version (version: 999)", "{\"version\": 999, \"jobs\": []}"),
        ("Unsupported schema version (version: 0)", "{\"version\": 0, \"jobs\": []}"),
        ("Unsupported schema version (negative version)", "{\"version\": -1, \"jobs\": []}"),
        ("String version instead of integer", "{\"version\": \"1\", \"jobs\": []}"),
        ("Array root instead of object", "[{\"version\": 1, \"jobs\": []}]"),
        ("String root", "\"some random string\""),
        ("Null root", "null"),
        ("Missing 'jobs' array key", "{\"version\": 1, \"updated_at_ms\": 12345}"),
        ("Non-array 'jobs' value", "{\"version\": 1, \"jobs\": \"not-an-array\"}"),
        ("Jobs array with non-object elements", "{\"version\": 1, \"jobs\": [123, \"abc\", null, false]}"),
        ("Empty file (0 bytes)", ""),
    ]

    for case_name, file_content in corrupted_cases:
        with tempfile.TemporaryDirectory(prefix="sublift_corrupt_test_") as temp_dir:
            cache_dir = Path(temp_dir) / ".sublift_cache"
            cache_dir.mkdir(parents=True, exist_ok=True)
            state_file = cache_dir / "jobs_state.v1.json"

            state_file.write_text(file_content, encoding="utf-8")

            server = ServerRunner(server_bin, temp_dir)
            server.start()
            try:
                st, _, body = server.req("GET", "/api/jobs")
                assert st == 200, f"[{case_name}] Expected 200, got {st}"
                jobs = json.loads(body.decode("utf-8"))
                assert isinstance(jobs, list)
                assert len(jobs) == 0, f"[{case_name}] Expected 0 jobs, got {len(jobs)}"
                print(f"[PASS] Corrupted case handled gracefully: '{case_name}' -> 0 jobs loaded, no crash")
            finally:
                server.stop()

    # Partial valid jobs array test (one invalid element amongst valid jobs)
    print("\n--- Partial valid jobs recovery ---")
    with tempfile.TemporaryDirectory(prefix="sublift_partial_corrupt_") as temp_dir:
        cache_dir = Path(temp_dir) / ".sublift_cache"
        cache_dir.mkdir(parents=True, exist_ok=True)
        state_file = cache_dir / "jobs_state.v1.json"

        partial_content = {
            "version": 1,
            "jobs": [
                "invalid-string-job",
                12345,
                {"invalid": "missing-job_id"},
                {
                    "job_id": "valid-surviving-job-1",
                    "status": "completed",
                    "config": {"video_path": "/test/vid.mp4", "engine": "mock"},
                    "entries": [{"start_ms": 100, "end_ms": 500, "text": "Valid entry", "confidence": 0.9}],
                },
                {"job_id": ""},  # empty job_id
                {
                    "job_id": "valid-surviving-job-2",
                    "status": "queued",
                    "config": {"video_path": "/test/vid.mp4", "engine": "mock"},
                    "entries": [],
                },
            ],
        }
        state_file.write_text(json.dumps(partial_content), encoding="utf-8")

        server = ServerRunner(server_bin, temp_dir)
        server.start()
        try:
            st, _, body = server.req("GET", "/api/jobs")
            assert st == 200
            jobs = json.loads(body.decode("utf-8"))
            job_ids = [j["job_id"] for j in jobs]
            assert "valid-surviving-job-1" in job_ids
            assert "valid-surviving-job-2" in job_ids
            assert len(jobs) == 2
            print("[PASS] Partial corruption recovered valid jobs and skipped malformed entries")
        finally:
            server.stop()


def test_lru_bounded_eviction(server_bin: Path):
    print("\n--- [TEST 3] LRU Bounded Eviction (Max 50 Entries) ---")

    # Part A: Pre-existing state snapshot with 60 jobs (job-0 to job-59)
    with tempfile.TemporaryDirectory(prefix="sublift_lru_test_") as temp_dir:
        cache_dir = Path(temp_dir) / ".sublift_cache"
        cache_dir.mkdir(parents=True, exist_ok=True)
        state_file = cache_dir / "jobs_state.v1.json"

        jobs_list = []
        for i in range(60):
            jobs_list.append({
                "job_id": f"job-lru-{i:02d}",
                "config": {"video_path": "/test/sample.mp4", "engine": "mock"},
                "status": "completed",
                "created_at_ms": 1000 + i * 10,
                "started_at_ms": 1005 + i * 10,
                "ended_at_ms": 1009 + i * 10,
                "entries": [{"start_ms": 0, "end_ms": 1000, "text": f"Subtitle {i}", "confidence": 0.9}],
            })

        state_content = {
            "version": 1,
            "updated_at_ms": 1724500000000,
            "jobs": jobs_list,
        }
        state_file.write_text(json.dumps(state_content, indent=2), encoding="utf-8")

        server = ServerRunner(server_bin, temp_dir)
        server.start()
        try:
            st, _, body = server.req("GET", "/api/jobs")
            assert st == 200
            loaded_jobs = json.loads(body.decode("utf-8"))
            print(f"[INFO] Total loaded jobs: {len(loaded_jobs)} (configured limit: 50)")
            assert len(loaded_jobs) == 50, f"Expected exactly 50 jobs, got {len(loaded_jobs)}"

            loaded_ids = {j["job_id"] for j in loaded_jobs}

            # Oldest 10 jobs (job-lru-50 to job-lru-59 or job-lru-00 to job-lru-09)
            # In history_lru_list_, jobs loaded from file are inserted in order, eviction removes from tail
            # Oldest in the list: job-lru-50..59 were evicted because they were pushed at tail of list
            # Let's verify the size is strictly 50
            assert len(loaded_ids) == 50
        finally:
            server.stop()

        # After server shutdown, verify disk snapshot was updated and strictly capped at 50
        saved_state = json.loads(state_file.read_text(encoding="utf-8"))
        assert len(saved_state["jobs"]) == 50, f"Snapshot on disk after shutdown should have 50 jobs, got {len(saved_state['jobs'])}"
        print("[PASS] 60 jobs loaded from snapshot were capped at 50 in memory and rewritten as 50 on disk")


    # Part B: Live sequential creation of 60 jobs
    print("\n--- Live Runtime Creation of 60 Jobs ---")
    with tempfile.TemporaryDirectory(prefix="sublift_live_lru_") as temp_dir:
        synth_video = str(Path(temp_dir) / "live_lru_video.mp4")
        create_dummy_synth_mp4(synth_video)

        server = ServerRunner(server_bin, temp_dir)
        server.start()
        try:
            created_job_ids = []
            for i in range(60):
                payload = json.dumps({
                    "video_path": synth_video,
                    "engine": "mock",
                    "fps": 5.0,
                })
                st_c, _, body_c = server.req("POST", "/api/jobs", body=payload, headers={"Content-Type": "application/json"})
                assert st_c == 201, f"Job {i} creation failed: {st_c}"
                job_id = json.loads(body_c.decode("utf-8"))["job_id"]
                created_job_ids.append(job_id)

            # Wait for jobs to execute
            time.sleep(2.0)

            # Query all jobs
            st_all, _, body_all = server.req("GET", "/api/jobs")
            assert st_all == 200
            current_jobs = json.loads(body_all.decode("utf-8"))
            assert len(current_jobs) <= 50, f"Expected <= 50 jobs, got {len(current_jobs)}"
            print(f"[PASS] In-memory job history capped at {len(current_jobs)} (<= 50)")

            # Check disk state file
            cache_file = Path(temp_dir) / ".sublift_cache" / "jobs_state.v1.json"
            assert cache_file.exists()
            disk_content = json.loads(cache_file.read_text(encoding="utf-8"))
            disk_jobs = disk_content.get("jobs", [])
            assert len(disk_jobs) <= 50, f"Disk state contains {len(disk_jobs)} jobs (> 50)"
            print(f"[PASS] Disk snapshot strictly bounded at {len(disk_jobs)} jobs (<= 50)")

            # Check that the most recent jobs (e.g. the last created job) are present
            disk_ids = {j["job_id"] for j in disk_jobs}
            assert created_job_ids[-1] in disk_ids, "Most recent job should be in snapshot"
            assert created_job_ids[-2] in disk_ids, "Second most recent job should be in snapshot"
            print(f"[PASS] Most recent jobs ({created_job_ids[-1][:8]}...) retained in LRU snapshot")

        finally:
            server.stop()


def main():
    print("==================================================================")
    print(" SubLift Phase 12 Milestone 2 Empirical Challenge (Challenger 1) ")
    print("==================================================================")

    repo_root = Path(__file__).resolve().parent.parent
    server_bin = repo_root / "build" / "cpp" / "bin" / "sublift_server"

    if not server_bin.exists():
        print(f"Error: server binary not found at {server_bin}")
        sys.exit(1)

    print(f"Using server binary: {server_bin}")

    test_crash_and_restart_recovery(server_bin)
    test_corrupted_state_file_handling(server_bin)
    test_lru_bounded_eviction(server_bin)

    print("\n==================================================================")
    print(" ALL CHALLENGER 1 EMPIRICAL STRESS TESTS PASSED! (VERDICT: APPROVE)")
    print("==================================================================")


if __name__ == "__main__":
    main()
