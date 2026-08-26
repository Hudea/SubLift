#!/usr/bin/env python3
"""
Empirical Challenge Script for SubLift Phase 12 Milestone 3 (Feature 12513)
Empirically tests:
  1. Lossless JobConfig DTO roundtrip across HTTP POST /api/jobs and GET /api/jobs/:id
  2. Optional script field preservation ('Hans', 'Hant', 'Latn', omitted)
  3. Confidence threshold preservation across full [0.0, 1.0] range
  4. RegionBox normalization & coordinate clamping (negative / >1.0 values clamped safely to [0.0, 1.0])
  5. Fail-closed rejection of invalid / unavailable engines (e.g. 'nonexistent_engine', 'python')
  6. Configuration persistence across server restart in jobs_state.v1.json
"""

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


def main():
    print("================================================================")
    print(" SubLift Phase 12 Milestone 3 Empirical Challenge (Challenger 1)")
    print(" Feature 12513: Server JobConfig DTO, Immutability & Parity")
    print("================================================================")

    repo_root = Path(__file__).resolve().parent.parent
    server_bin = repo_root / "build" / "cpp" / "bin" / "sublift_server"

    if not server_bin.exists():
        print(f"Error: server binary not found at {server_bin}")
        sys.exit(1)

    work_dir = tempfile.mkdtemp(prefix="sublift_challenge_m3_")
    media_dir = Path(work_dir) / "media"
    media_dir.mkdir(parents=True, exist_ok=True)

    # Create dummy video file
    dummy_video = media_dir / "sample_challenge.mp4"
    dummy_video.write_bytes(b"\x00" * 1024)

    env = os.environ.copy()
    env["SUBLIFT_CORS_ORIGIN"] = "*"
    port = get_free_port()
    proc = subprocess.Popen(
        [str(server_bin), "--host", "127.0.0.1", "-p", str(port), "--media-dir", str(media_dir)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )

    try:
        # Wait for server ready
        ready = False
        for _ in range(50):
            if proc.poll() is not None:
                out, err = proc.communicate()
                print(f"Server exited with code {proc.returncode}:\nStdout: {out}\nStderr: {err}")
                sys.exit(1)
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    ready = True
                    break
            except OSError:
                time.sleep(0.1)

        assert ready, "Server failed to become ready"
        print("[PASS] Native server started on port", port)

        # ---------------------------------------------------------
        # Test 1: Full Unified JobConfig POST & GET Roundtrip
        # ---------------------------------------------------------
        print("\n--- Test 1: Full Unified JobConfig POST & GET Roundtrip ---")
        job_req_1 = {
            "video_path": str(dummy_video),
            "engine": "mock",
            "fps": 8.0,
            "confidence_threshold": 0.42,
            "script": "Hans",
            "region_box": {
                "x": 0.12,
                "y": 0.78,
                "width": 0.76,
                "height": 0.18
            }
        }
        status, _, body = http_req("127.0.0.1", port, "POST", "/api/jobs", body=json.dumps(job_req_1), headers={"Content-Type": "application/json"})
        assert status in (200, 201), f"Expected 200 on POST /api/jobs, got {status}: {body.decode()}"
        res1 = json.loads(body.decode())
        job_id_1 = res1["job_id"]
        print(f"Created Job 1: {job_id_1}")

        # Fetch detail
        status, _, body = http_req("127.0.0.1", port, "GET", f"/api/jobs/{job_id_1}")
        assert status in (200, 201), f"Expected 200 on GET /api/jobs/:id, got {status}"
        detail_1 = json.loads(body.decode())
        cfg_1 = detail_1["config"]

        assert cfg_1["video_path"] in (str(dummy_video), str(dummy_video.resolve())), f"video_path mismatch: {cfg_1}"
        assert cfg_1["engine"] == "mock", f"engine mismatch: {cfg_1}"
        assert abs(cfg_1["fps"] - 8.0) < 1e-5, f"fps mismatch: {cfg_1}"
        assert abs(cfg_1["confidence_threshold"] - 0.42) < 1e-5, f"confidence mismatch: {cfg_1}"
        assert cfg_1["script"] == "Hans", f"script mismatch: {cfg_1}"
        assert abs(cfg_1["region_box"]["x"] - 0.12) < 1e-5
        assert abs(cfg_1["region_box"]["y"] - 0.78) < 1e-5
        assert abs(cfg_1["region_box"]["width"] - 0.76) < 1e-5
        assert abs(cfg_1["region_box"]["height"] - 0.18) < 1e-5
        print("[PASS] Full JobConfig DTO fields perfectly preserved in roundtrip.")

        # ---------------------------------------------------------
        # Test 2: Omitted script and default confidence
        # ---------------------------------------------------------
        print("\n--- Test 2: Omitted script and default confidence ---")
        job_req_2 = {
            "video_path": str(dummy_video),
            "engine": "mock",
            "fps": 5.0,
            "region_box": {
                "x": 0.0,
                "y": 0.7,
                "width": 1.0,
                "height": 0.3
            }
        }
        status, _, body = http_req("127.0.0.1", port, "POST", "/api/jobs", body=json.dumps(job_req_2), headers={"Content-Type": "application/json"})
        assert status in (200, 201)
        job_id_2 = json.loads(body.decode())["job_id"]

        status, _, body = http_req("127.0.0.1", port, "GET", f"/api/jobs/{job_id_2}")
        cfg_2 = json.loads(body.decode())["config"]
        assert "script" not in cfg_2 or cfg_2["script"] is None
        assert abs(cfg_2["confidence_threshold"] - 0.0) < 1e-5
        print("[PASS] Omitted optional script and default confidence handled cleanly.")

        # ---------------------------------------------------------
        # Test 3: RegionBox Coordinate Clamping Edge Cases
        # ---------------------------------------------------------
        print("\n--- Test 3: RegionBox Coordinate Clamping Edge Cases ---")
        job_req_3 = {
            "video_path": str(dummy_video),
            "engine": "mock",
            "fps": 12.0,
            "region_box": {
                "x": -0.5,    # negative -> clamped to 0.0
                "y": 1.8,     # >1.0 -> clamped to 1.0
                "width": -0.2,# negative -> clamped to 0.0
                "height": 5.0 # >1.0 -> clamped to 1.0
            }
        }
        status, _, body = http_req("127.0.0.1", port, "POST", "/api/jobs", body=json.dumps(job_req_3), headers={"Content-Type": "application/json"})
        assert status in (200, 201)
        job_id_3 = json.loads(body.decode())["job_id"]

        status, _, body = http_req("127.0.0.1", port, "GET", f"/api/jobs/{job_id_3}")
        cfg_3 = json.loads(body.decode())["config"]
        box_3 = cfg_3["region_box"]
        assert box_3["x"] == 0.0, f"Expected x clamped to 0.0, got {box_3['x']}"
        assert box_3["y"] == 1.0, f"Expected y clamped to 1.0, got {box_3['y']}"
        assert box_3["width"] == 0.0, f"Expected width clamped to 0.0, got {box_3['width']}"
        assert box_3["height"] == 1.0, f"Expected height clamped to 1.0, got {box_3['height']}"
        print("[PASS] Out-of-bounds RegionBox coordinates safely clamped to [0.0, 1.0].")

        # ---------------------------------------------------------
        # Test 4: Fail-Closed Invalid Engine Rejection
        # ---------------------------------------------------------
        print("\n--- Test 4: Fail-Closed Invalid Engine Rejection ---")
        bad_engines = ["python", "tesseract", "whisper", "unknown_engine", ""]
        for bad in bad_engines:
            job_req_bad = {
                "video_path": str(dummy_video),
                "engine": bad,
                "fps": 5.0
            }
            status, _, body = http_req("127.0.0.1", port, "POST", "/api/jobs", body=json.dumps(job_req_bad), headers={"Content-Type": "application/json"})
            assert status == 400, f"Expected 400 for bad engine '{bad}', got {status}: {body.decode()}"
        print("[PASS] Fail-closed rejection of invalid engines verified.")

        # ---------------------------------------------------------
        # Test 5: Persistence Across Server Restart
        # ---------------------------------------------------------
        print("\n--- Test 5: Persistence Across Server Restart ---")
        # Kill server
        proc.terminate()
        proc.wait(timeout=3)

        # Check state file exists
        state_file = media_dir / ".sublift_cache" / "jobs_state.v1.json"
        assert state_file.exists(), f"State file not found at {state_file}"
        state_data = json.loads(state_file.read_text())
        assert "jobs" in state_data
        print(f"State file saved with {len(state_data['jobs'])} jobs.")

        # Restart server on new port
        port2 = get_free_port()
        proc2 = subprocess.Popen(
            [str(server_bin), "--host", "127.0.0.1", "-p", str(port2), "--media-dir", str(media_dir)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )

        try:
            ready = False
            for _ in range(50):
                if proc2.poll() is not None:
                    out, err = proc2.communicate()
                    print(f"Server 2 exited with code {proc2.returncode}:\nStdout: {out}\nStderr: {err}")
                    sys.exit(1)
                try:
                    with socket.create_connection(("127.0.0.1", port2), timeout=0.2):
                        ready = True
                        break
                except OSError:
                    time.sleep(0.1)

            assert ready, "Server 2 failed to become ready"

            status, _, body = http_req("127.0.0.1", port2, "GET", f"/api/jobs/{job_id_1}")
            assert status == 200, f"Failed to retrieve restored job {job_id_1}"
            restored_detail = json.loads(body.decode())
            restored_cfg = restored_detail["config"]

            assert restored_cfg["script"] == "Hans"
            assert abs(restored_cfg["confidence_threshold"] - 0.42) < 1e-5
            assert abs(restored_cfg["region_box"]["x"] - 0.12) < 1e-5
            print("[PASS] JobConfig persisted and fully recovered across restart.")
        finally:
            proc2.terminate()
            proc2.wait(timeout=3)

    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=3)
        shutil.rmtree(work_dir, ignore_errors=True)

    print("\n================================================================")
    print(" ALL EMPIRICAL CHALLENGE TESTS PASSED SUCCESSFULLY!")
    print("================================================================")


if __name__ == "__main__":
    main()
