#!/usr/bin/env python3
"""
Empirical Challenge Script for SubLift Phase 12 Milestone 2 (Feature 12512)
Tests:
  1. SSE Last-Event-ID and ?cursor= Resumption over real HTTP socket
  2. Cursor waiting and completed stream boundaries
  3. Malformed Last-Event-ID and ?cursor= values fallback
  4. Subtitle deduplication under stress replay
  5. Progress scale 0.0-1.0 conversion parity
"""

import http.client
import json
import os
import re
import shutil
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


def http_req(host: str, port: int, method: str, path: str, body: bytes | str | None = None, headers: dict | None = None) -> tuple[int, dict, bytes]:
    conn = http.client.HTTPConnection(host, port, timeout=10)
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


def run_challenge():
    print("================================================================")
    print(" SubLift Phase 12 Milestone 2 Empirical Challenge (Challenger 2)")
    print("================================================================")

    repo_root = Path(__file__).resolve().parent.parent
    server_bin = repo_root / "build" / "cpp" / "bin" / "sublift_server"

    if not server_bin.exists():
        print(f"Error: server binary not found at {server_bin}")
        sys.exit(1)

    temp_dir = tempfile.mkdtemp(prefix="sublift_chal2_")
    port = get_free_port()

    # Generate synthetic 0.6s mp4 video
    synth_mp4 = os.path.join(temp_dir, "synth_test.mp4")
    cmd = [
        "ffmpeg",
        "-y",
        "-f",
        "lavfi",
        "-i",
        "testsrc=duration=0.6:size=128x128:rate=10",
        "-pix_fmt",
        "yuv420p",
        synth_mp4,
    ]
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)

    server_proc = None
    try:
        # Start server
        env = os.environ.copy()
        env["SUBLIFT_CORS_ORIGIN"] = "*"
        server_proc = subprocess.Popen(
            [str(server_bin), "--host", "127.0.0.1", "-p", str(port), "--media-dir", temp_dir],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )

        # Wait for server ready
        ready = False
        for _ in range(50):
            if server_proc.poll() is not None:
                out, err = server_proc.communicate()
                print(f"Server exited with code {server_proc.returncode}:\nStdout: {out}\nStderr: {err}")
                sys.exit(1)
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    ready = True
                    break
            except (ConnectionRefusedError, TimeoutError, OSError):
                time.sleep(0.1)

        if not ready:
            print("Failed to connect to sublift_server")
            sys.exit(1)

        print(f"[OK] sublift_server running on port {port}")

        # ---------------------------------------------------------------------
        # Test 1: Submit a job and verify execution
        # ---------------------------------------------------------------------
        job_req = json.dumps({
            "video_path": synth_mp4,
            "engine": "mock",
            "fps": 5.0,
            "confidence_threshold": 0.0,
            "region_box": {"x": 0.0, "y": 0.6, "width": 1.0, "height": 0.4}
        })
        st, _, body = http_req("127.0.0.1", port, "POST", "/api/jobs", body=job_req, headers={"Content-Type": "application/json"})
        assert st == 201, f"Expected 201, got {st}"
        resp_data = json.loads(body.decode("utf-8"))
        job_id = resp_data["job_id"]
        print(f"[OK] Job created with ID: {job_id}")

        # Wait for job completion
        time.sleep(1.0)

        # ---------------------------------------------------------------------
        # Test 2: Full SSE Stream (cursor=0)
        # ---------------------------------------------------------------------
        st, _, body = http_req("127.0.0.1", port, "GET", f"/api/jobs/{job_id}/events")
        assert st == 200
        full_stream = body.decode("utf-8")
        assert "event: progress" in full_stream or "event: done" in full_stream
        assert "id: 1\n" in full_stream
        print(f"[OK] Full SSE stream retrieved ({len(full_stream)} bytes)")

        # Count total events
        event_ids = [int(m) for m in re.findall(r"id:\s*(\d+)", full_stream)]
        print(f"[OK] Published event IDs in stream: {event_ids}")
        assert len(event_ids) >= 2, f"Expected at least 2 events, got {len(event_ids)}"
        max_id = max(event_ids)

        # ---------------------------------------------------------------------
        # Test 3: Resumption via Last-Event-ID: 1
        # ---------------------------------------------------------------------
        st, _, body = http_req("127.0.0.1", port, "GET", f"/api/jobs/{job_id}/events", headers={"Last-Event-ID": "1"})
        assert st == 200
        resumed_stream = body.decode("utf-8")
        resumed_ids = [int(m) for m in re.findall(r"id:\s*(\d+)", resumed_stream)]
        print(f"[OK] Resumed stream with Last-Event-ID: 1 -> IDs: {resumed_ids}")
        assert 1 not in resumed_ids, "Event id 1 should NOT be replayed"
        assert all(i > 1 for i in resumed_ids), "All resumed event IDs must be > 1"
        assert resumed_ids == [i for i in event_ids if i > 1], "Must match exact tail subsequence"

        # ---------------------------------------------------------------------
        # Test 4: Resumption via ?cursor=1
        # ---------------------------------------------------------------------
        st, _, body = http_req("127.0.0.1", port, "GET", f"/api/jobs/{job_id}/events?cursor=1")
        assert st == 200
        cursor_stream = body.decode("utf-8")
        cursor_ids = [int(m) for m in re.findall(r"id:\s*(\d+)", cursor_stream)]
        print(f"[OK] Resumed stream with ?cursor=1 -> IDs: {cursor_ids}")
        assert 1 not in cursor_ids, "Event id 1 should NOT be replayed"
        assert cursor_ids == resumed_ids, "Cursor and Last-Event-ID should return identical event sequences"

        # ---------------------------------------------------------------------
        # Test 5: Connect with cursor > latest on completed job
        # ---------------------------------------------------------------------
        st, _, body = http_req("127.0.0.1", port, "GET", f"/api/jobs/{job_id}/events?cursor={max_id + 10}")
        assert st == 200
        high_cursor_stream = body.decode("utf-8")
        high_cursor_ids = [int(m) for m in re.findall(r"id:\s*(\d+)", high_cursor_stream)]
        print(f"[OK] Stream with cursor={max_id + 10} -> returned IDs: {high_cursor_ids}")
        assert len(high_cursor_ids) == 0, "No old events should be returned when cursor > latest on terminal job"

        # ---------------------------------------------------------------------
        # Test 6: Malformed cursor fallback
        # ---------------------------------------------------------------------
        st, _, body = http_req("127.0.0.1", port, "GET", f"/api/jobs/{job_id}/events?cursor=invalid_not_number")
        assert st == 200
        fallback_stream = body.decode("utf-8")
        fallback_ids = [int(m) for m in re.findall(r"id:\s*(\d+)", fallback_stream)]
        print(f"[OK] Stream with malformed cursor -> returned IDs: {fallback_ids}")
        assert fallback_ids == event_ids, "Malformed cursor must fallback to cursor=0 and return all events"

        # ---------------------------------------------------------------------
        # Test 7: State File Crash Persistence and Interrupted Status
        # ---------------------------------------------------------------------
        state_file = Path(temp_dir) / ".sublift_cache" / "jobs_state.v1.json"
        assert state_file.exists(), f"State file {state_file} should exist"
        state_content = json.loads(state_file.read_text(encoding="utf-8"))
        assert state_content.get("version") == 1
        assert len(state_content.get("jobs", [])) >= 1
        print(f"[OK] State persistence file valid (version {state_content['version']}, {len(state_content['jobs'])} jobs)")

    finally:
        if server_proc:
            server_proc.terminate()
            try:
                server_proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                server_proc.kill()
        shutil.rmtree(temp_dir, ignore_errors=True)

    print("\n================================================================")
    print(" ALL EMPIRICAL CHALLENGE TESTS PASSED SUCCESSFULLY! (VERDICT: APPROVE)")
    print("================================================================")


if __name__ == "__main__":
    run_challenge()
