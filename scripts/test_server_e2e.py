#!/usr/bin/env python3
"""SubLift Native Web Server (sublift_server) End-to-End Integration & Boundary Test Suite."""

from __future__ import annotations

import http.client
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from typing import Any


def find_free_port() -> int:
    """Finds an ephemeral free TCP port on localhost."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


class SubLiftServerProcess:
    """Manages lifecycle of a temporary sublift_server process."""

    def __init__(
        self,
        binary_path: str,
        port: int,
        host: str = "127.0.0.1",
        static_dir: str | None = None,
    ) -> None:
        self.binary_path = binary_path
        self.port = port
        self.host = host
        self.static_dir = static_dir
        self.process: subprocess.Popen[str] | None = None

    def start(self, timeout_sec: float = 5.0) -> None:
        if not os.path.isfile(self.binary_path):
            raise FileNotFoundError(f"sublift_server binary not found at: {self.binary_path}")

        cmd = [self.binary_path, "--host", self.host, "-p", str(self.port)]
        if self.static_dir:
            cmd.extend(["--static-dir", self.static_dir])

        self.process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        start_time = time.time()
        while time.time() - start_time < timeout_sec:
            if self.process.poll() is not None:
                out, err = self.process.communicate()
                raise RuntimeError(
                    f"Server process terminated prematurely (exit code {self.process.returncode})\n"
                    f"Stdout: {out}\nStderr: {err}"
                )
            try:
                with socket.create_connection((self.host, self.port), timeout=0.2):
                    return
            except (ConnectionRefusedError, TimeoutError, OSError):
                time.sleep(0.05)

        self.stop()
        raise TimeoutError(f"Server did not start listening on {self.host}:{self.port}")

    def stop(self) -> None:
        if self.process is not None:
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=1.0)
            self.process = None


def make_request(
    host: str,
    port: int,
    method: str,
    path: str,
    headers: dict[str, str] | None = None,
) -> tuple[int, dict[str, str], bytes]:
    """Performs an HTTP request and returns (status, headers_dict, body_bytes)."""
    conn = http.client.HTTPConnection(host, port, timeout=5.0)
    try:
        req_headers = headers or {}
        conn.request(method, path, headers=req_headers)
        resp = conn.getresponse()
        resp_headers = dict(resp.getheaders())
        body = resp.read()
        return resp.status, resp_headers, body
    finally:
        conn.close()


class TestSubLiftServerE2E(unittest.TestCase):
    server_proc: SubLiftServerProcess | None = None
    server_port: int = 0
    server_host: str = "127.0.0.1"
    temp_dir: str = ""
    dummy_video_path: str = ""
    dummy_video_data: bytes = b""
    synth_mp4_path: str = ""
    corrupt_file_path: str = ""
    ffmpeg_available: bool = False

    @classmethod
    def setUpClass(cls) -> None:
        project_root = Path(__file__).resolve().parents[1]
        binary_path = str(project_root / "build" / "cpp" / "bin" / "sublift_server")
        static_dir = str(project_root / "apps" / "web" / "dist")

        cls.server_port = find_free_port()
        cls.server_proc = SubLiftServerProcess(
            binary_path,
            cls.server_port,
            cls.server_host,
            static_dir=static_dir if os.path.exists(static_dir) else None,
        )
        cls.server_proc.start()

        # Create temporary working directory and test assets
        cls.temp_dir = tempfile.mkdtemp(prefix="sublift_e2e_")

        # 1. 10,240-byte deterministic binary file for streaming tests
        cls.dummy_video_data = bytes([i % 256 for i in range(10240)])
        cls.dummy_video_path = os.path.join(cls.temp_dir, "test_stream_video.mp4")
        with open(cls.dummy_video_path, "wb") as f:
            f.write(cls.dummy_video_data)

        # 2. Corrupt / non-video file
        cls.corrupt_file_path = os.path.join(cls.temp_dir, "corrupted_video.mp4")
        with open(cls.corrupt_file_path, "wb") as f:
            f.write(b"NOT_A_REAL_VIDEO_HEADER_1234567890")

        # 3. Synthetic real MP4 video (2.0 seconds) for frame extraction
        cls.synth_mp4_path = os.path.join(cls.temp_dir, "synth_test_video.mp4")
        ffmpeg_bin = shutil.which("ffmpeg") or "/opt/homebrew/bin/ffmpeg"
        if os.path.exists(ffmpeg_bin):
            try:
                cmd = [
                    ffmpeg_bin,
                    "-y",
                    "-f",
                    "lavfi",
                    "-i",
                    "testsrc=duration=2.0:size=128x128:rate=10",
                    "-pix_fmt",
                    "yuv420p",
                    cls.synth_mp4_path,
                ]
                res = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if res.returncode == 0 and os.path.exists(cls.synth_mp4_path):
                    cls.ffmpeg_available = True
            except Exception:
                cls.ffmpeg_available = False

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.server_proc:
            cls.server_proc.stop()
        if cls.temp_dir and os.path.exists(cls.temp_dir):
            shutil.rmtree(cls.temp_dir, ignore_errors=True)

    def req(
        self,
        method: str,
        path: str,
        headers: dict[str, str] | None = None,
    ) -> tuple[int, dict[str, str], bytes]:
        return make_request(self.server_host, self.server_port, method, path, headers)

    # -------------------------------------------------------------------------
    # 1. /api/system/info & OPTIONS CORS Tests
    # -------------------------------------------------------------------------

    def test_sys_01_system_info_status_and_cors(self) -> None:
        """TC-SYS-01: Verify GET /api/system/info returns 200 OK with valid CORS headers."""
        status, headers, _ = self.req("GET", "/api/system/info")
        self.assertEqual(status, 200)
        self.assertIn("application/json", headers.get("Content-Type", ""))
        self.assertEqual(headers.get("Access-Control-Allow-Origin"), "*")
        self.assertIn("GET", headers.get("Access-Control-Allow-Methods", ""))

    def test_sys_02_system_info_schema_and_fields(self) -> None:
        """TC-SYS-02: Verify system info JSON structure conforms to SystemInfoDTO schema."""
        status, _, body = self.req("GET", "/api/system/info")
        self.assertEqual(status, 200)
        data: dict[str, Any] = json.loads(body.decode("utf-8"))

        self.assertIn("runtime", data)
        self.assertEqual(data["runtime"], "cpp")

        self.assertIn("version", data)
        self.assertIsInstance(data["version"], str)
        self.assertTrue(len(data["version"]) > 0)

        self.assertIn("capabilities", data)
        self.assertIsInstance(data["capabilities"], list)
        expected_caps = {
            "path_mode",
            "frame_mode",
            "push_entry",
            "cancel",
            "video_stream",
            "video_frame",
        }
        for cap in expected_caps:
            self.assertIn(cap, data["capabilities"])

    def test_sys_03_system_info_engine_list(self) -> None:
        """TC-SYS-03: Verify engine list contains mock, paddle, vision with correct fields."""
        status, _, body = self.req("GET", "/api/system/info")
        self.assertEqual(status, 200)
        data: dict[str, Any] = json.loads(body.decode("utf-8"))

        self.assertIn("engines", data)
        self.assertIsInstance(data["engines"], list)
        engine_names = {e["name"]: e for e in data["engines"]}

        # Mock engine must always be present and available
        self.assertIn("mock", engine_names)
        self.assertTrue(engine_names["mock"]["available"])

        # Vision engine
        self.assertIn("vision", engine_names)
        self.assertIsInstance(engine_names["vision"]["available"], bool)

        # Paddle engine
        self.assertIn("paddle", engine_names)
        self.assertIsInstance(engine_names["paddle"]["available"], bool)

    def test_sys_04_system_info_ffmpeg_status(self) -> None:
        """TC-SYS-04: Verify FFmpeg toolchain status and binary path reporting."""
        status, _, body = self.req("GET", "/api/system/info")
        self.assertEqual(status, 200)
        data: dict[str, Any] = json.loads(body.decode("utf-8"))

        self.assertIn("ffmpeg", data)
        self.assertIsInstance(data["ffmpeg"], dict)
        self.assertIn("available", data["ffmpeg"])
        self.assertIsInstance(data["ffmpeg"]["available"], bool)
        if data["ffmpeg"]["available"]:
            self.assertIn("path", data["ffmpeg"])
            self.assertTrue(os.path.exists(data["ffmpeg"]["path"]))

    def test_cors_01_options_preflight_on_api_routes(self) -> None:
        """TC-CORS-01: Verify OPTIONS preflight returns 204 and CORS headers across routes."""
        endpoints = [
            "/api/system/info",
            "/api/video/stream",
            "/api/video/frame",
            "/api/nonexistent_route",
        ]
        for ep in endpoints:
            with self.subTest(endpoint=ep):
                status, headers, body = self.req("OPTIONS", ep)
                self.assertEqual(status, 204)
                self.assertEqual(headers.get("Access-Control-Allow-Origin"), "*")
                self.assertIn("OPTIONS", headers.get("Access-Control-Allow-Methods", ""))
                self.assertEqual(body, b"")

    # -------------------------------------------------------------------------
    # 2. /api/video/stream Streaming & Range Slicing Tests
    # -------------------------------------------------------------------------

    def test_stream_01_missing_path_parameter(self) -> None:
        """TC-STR-01: Missing path parameter returns 400 Bad Request."""
        status, _, body = self.req("GET", "/api/video/stream")
        self.assertEqual(status, 400)
        err = json.loads(body.decode("utf-8"))
        self.assertIn("error", err)

    def test_stream_02_nonexistent_file(self) -> None:
        """TC-STR-02: Nonexistent file path returns 404 Not Found."""
        fake_path = os.path.join(self.temp_dir, "does_not_exist.mp4")
        status, _, body = self.req("GET", f"/api/video/stream?path={fake_path}")
        self.assertEqual(status, 404)
        err = json.loads(body.decode("utf-8"))
        self.assertIn("error", err)

    def test_stream_03_directory_path(self) -> None:
        """TC-STR-03: Directory path instead of regular file returns 404 Not Found."""
        status, _, _ = self.req("GET", f"/api/video/stream?path={self.temp_dir}")
        self.assertEqual(status, 404)

    def test_stream_04_full_file_200_ok(self) -> None:
        """TC-STR-04: Full GET request returns 200 OK with full byte payload and Accept-Ranges."""
        status, headers, body = self.req("GET", f"/api/video/stream?path={self.dummy_video_path}")
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("Accept-Ranges"), "bytes")
        self.assertEqual(headers.get("Content-Type"), "video/mp4")
        self.assertEqual(int(headers.get("Content-Length", 0)), 10240)
        self.assertEqual(len(body), 10240)
        self.assertEqual(body, self.dummy_video_data)

    def test_stream_05_safari_probe_range(self) -> None:
        """TC-STR-05: Safari Range bytes=0-1 returns 206 Partial Content and 2 exact bytes."""
        status, headers, body = self.req(
            "GET",
            f"/api/video/stream?path={self.dummy_video_path}",
            headers={"Range": "bytes=0-1"},
        )
        self.assertEqual(status, 206)
        self.assertEqual(headers.get("Accept-Ranges"), "bytes")
        self.assertEqual(headers.get("Content-Range"), "bytes 0-1/10240")
        self.assertEqual(int(headers.get("Content-Length", 0)), 2)
        self.assertEqual(len(body), 2)
        self.assertEqual(body, self.dummy_video_data[0:2])

    def test_stream_06_slice_range(self) -> None:
        """TC-STR-06: Arbitrary range slice Range: bytes=100-299 returns 206 Partial Content."""
        status, headers, body = self.req(
            "GET",
            f"/api/video/stream?path={self.dummy_video_path}",
            headers={"Range": "bytes=100-299"},
        )
        self.assertEqual(status, 206)
        self.assertEqual(headers.get("Content-Range"), "bytes 100-299/10240")
        self.assertEqual(int(headers.get("Content-Length", 0)), 200)
        self.assertEqual(len(body), 200)
        self.assertEqual(body, self.dummy_video_data[100:300])

    def test_stream_07_open_ended_range(self) -> None:
        """TC-STR-07: Open-ended range Range: bytes=500- returns 206 Partial Content to EOF."""
        status, headers, body = self.req(
            "GET",
            f"/api/video/stream?path={self.dummy_video_path}",
            headers={"Range": "bytes=500-"},
        )
        self.assertEqual(status, 206)
        self.assertEqual(headers.get("Content-Range"), "bytes 500-10239/10240")
        self.assertEqual(int(headers.get("Content-Length", 0)), 10240 - 500)
        self.assertEqual(len(body), 10240 - 500)
        self.assertEqual(body, self.dummy_video_data[500:])

    def test_stream_08_suffix_range(self) -> None:
        """TC-STR-08: Suffix range Range: bytes=-100 returns 206 Partial Content last 100 bytes."""
        status, headers, body = self.req(
            "GET",
            f"/api/video/stream?path={self.dummy_video_path}",
            headers={"Range": "bytes=-100"},
        )
        self.assertEqual(status, 206)
        self.assertEqual(headers.get("Content-Range"), "bytes 10140-10239/10240")
        self.assertEqual(int(headers.get("Content-Length", 0)), 100)
        self.assertEqual(len(body), 100)
        self.assertEqual(body, self.dummy_video_data[-100:])

    def test_stream_09_out_of_bounds_range(self) -> None:
        """TC-STR-09: Out-of-bounds Range: bytes=9999999- returns 416 Range Not Satisfiable."""
        status, _, _ = self.req(
            "GET",
            f"/api/video/stream?path={self.dummy_video_path}",
            headers={"Range": "bytes=9999999-"},
        )
        self.assertEqual(status, 416)

    def test_stream_10_inverted_range(self) -> None:
        """TC-STR-10: Inverted invalid Range: bytes=500-200 returns 416 Range Not Satisfiable."""
        status, _, _ = self.req(
            "GET",
            f"/api/video/stream?path={self.dummy_video_path}",
            headers={"Range": "bytes=500-200"},
        )
        self.assertEqual(status, 416)

    def test_stream_11_mime_types(self) -> None:
        """TC-STR-11: Video streaming correctly maps MIME types based on file extension."""
        mime_map = {
            "test.mp4": "video/mp4",
            "test.m4v": "video/mp4",
            "test.webm": "video/webm",
            "test.mov": "video/quicktime",
            "test.mkv": "video/x-matroska",
            "test.avi": "video/x-msvideo",
            "test.flv": "video/x-flv",
        }
        for filename, expected_mime in mime_map.items():
            path = os.path.join(self.temp_dir, filename)
            with open(path, "wb") as f:
                f.write(b"DUMMY_VIDEO_HEADER_FOR_MIME_TEST")

            with self.subTest(filename=filename, mime=expected_mime):
                status, headers, _ = self.req("GET", f"/api/video/stream?path={path}")
                self.assertEqual(status, 200)
                self.assertEqual(headers.get("Content-Type"), expected_mime)

    # -------------------------------------------------------------------------
    # 3. /api/video/frame Frame Extraction Tests
    # -------------------------------------------------------------------------

    def test_frame_01_missing_path_parameter(self) -> None:
        """TC-FRM-01: Missing path parameter returns 400 Bad Request."""
        status, _, body = self.req("GET", "/api/video/frame")
        self.assertEqual(status, 400)
        err = json.loads(body.decode("utf-8"))
        self.assertIn("error", err)

    def test_frame_02_nonexistent_file(self) -> None:
        """TC-FRM-02: Nonexistent file returns 404 Not Found."""
        fake_path = os.path.join(self.temp_dir, "ghost_video.mp4")
        status, _, body = self.req("GET", f"/api/video/frame?path={fake_path}")
        self.assertEqual(status, 404)
        err = json.loads(body.decode("utf-8"))
        self.assertIn("error", err)

    def test_frame_03_corrupted_file(self) -> None:
        """TC-FRM-03: Corrupted non-video file returns 500 Internal Server Error."""
        if not self.ffmpeg_available:
            self.skipTest("FFmpeg toolchain not available for frame extraction test")
        status, _, body = self.req("GET", f"/api/video/frame?path={self.corrupt_file_path}")
        self.assertEqual(status, 500)
        err = json.loads(body.decode("utf-8"))
        self.assertIn("error", err)

    def test_frame_04_valid_extraction_at_specified_time(self) -> None:
        """TC-FRM-04: Valid video extraction returns 200 OK and JPEG SOI + EOI markers."""
        if not self.ffmpeg_available:
            self.skipTest("FFmpeg toolchain not available for frame extraction test")
        req_path = f"/api/video/frame?path={self.synth_mp4_path}&time_s=1.0"
        status, headers, body = self.req("GET", req_path)
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("Content-Type"), "image/jpeg")
        self.assertGreater(len(body), 100)
        # Verify JPEG SOI marker (0xFF, 0xD8)
        self.assertEqual(body[0:2], b"\xff\xd8")
        # Verify JPEG EOI marker (0xFF, 0xD9)
        self.assertEqual(body[-2:], b"\xff\xd9")

    def test_frame_05_default_time(self) -> None:
        """TC-FRM-05: Omitted time_s parameter defaults to 0.0s and returns 200 OK valid JPEG."""
        if not self.ffmpeg_available:
            self.skipTest("FFmpeg toolchain not available for frame extraction test")
        status, headers, body = self.req("GET", f"/api/video/frame?path={self.synth_mp4_path}")
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("Content-Type"), "image/jpeg")
        self.assertEqual(body[0:2], b"\xff\xd8")
        self.assertEqual(body[-2:], b"\xff\xd9")

    def test_frame_06_negative_time_clamped(self) -> None:
        """TC-FRM-06: Negative time_s=-5.0 clamps to 0.0s and returns 200 OK valid JPEG."""
        if not self.ffmpeg_available:
            self.skipTest("FFmpeg toolchain not available for frame extraction test")
        req_path = f"/api/video/frame?path={self.synth_mp4_path}&time_s=-5.0"
        status, headers, body = self.req("GET", req_path)
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("Content-Type"), "image/jpeg")
        self.assertEqual(body[0:2], b"\xff\xd8")
        self.assertEqual(body[-2:], b"\xff\xd9")

    def test_frame_07_fractional_subsecond_time(self) -> None:
        """TC-FRM-07: Sub-second fractional time_s=0.25 returns 200 OK valid JPEG."""
        if not self.ffmpeg_available:
            self.skipTest("FFmpeg toolchain not available for frame extraction test")
        req_path = f"/api/video/frame?path={self.synth_mp4_path}&time_s=0.25"
        status, headers, body = self.req("GET", req_path)
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("Content-Type"), "image/jpeg")
        self.assertEqual(body[0:2], b"\xff\xd8")
        self.assertEqual(body[-2:], b"\xff\xd9")

    # -------------------------------------------------------------------------
    # 4. /api/jobs Management, SSE Event Streaming & SRT Export Tests
    # -------------------------------------------------------------------------

    def test_jobs_01_missing_body_or_path(self) -> None:
        """TC-JOB-01: POST /api/jobs returns 400 when body or video_path is missing."""
        conn = http.client.HTTPConnection(self.server_host, self.server_port, timeout=5.0)
        try:
            # Invalid JSON
            conn.request(
                "POST",
                "/api/jobs",
                body="not-json",
                headers={"Content-Type": "application/json"},
            )
            resp = conn.getresponse()
            self.assertEqual(resp.status, 400)
            resp.read()

            # Empty object
            conn.request(
                "POST",
                "/api/jobs",
                body="{}",
                headers={"Content-Type": "application/json"},
            )
            resp2 = conn.getresponse()
            self.assertEqual(resp2.status, 400)
            resp2.read()
        finally:
            conn.close()

    def test_jobs_02_nonexistent_video_path(self) -> None:
        """TC-JOB-02: POST /api/jobs returns 404 when video_path does not exist."""
        conn = http.client.HTTPConnection(self.server_host, self.server_port, timeout=5.0)
        try:
            req_body = json.dumps({"video_path": "/path/to/nonexistent/video.mp4"})
            conn.request(
                "POST",
                "/api/jobs",
                body=req_body,
                headers={"Content-Type": "application/json"},
            )
            resp = conn.getresponse()
            self.assertEqual(resp.status, 404)
            resp.read()
        finally:
            conn.close()

    def test_jobs_03_unknown_job_endpoints(self) -> None:
        """TC-JOB-03: Unknown job ID returns 404 across cancel, export, and events."""
        for ep, method in [
            ("/api/jobs/unknown-id-12345/cancel", "POST"),
            ("/api/jobs/unknown-id-12345/export", "GET"),
            ("/api/jobs/unknown-id-12345/events", "GET"),
        ]:
            with self.subTest(endpoint=ep, method=method):
                status, _, _ = self.req(method, ep)
                self.assertEqual(status, 404)

    def test_jobs_04_create_job_sse_events_and_srt_export(self) -> None:
        """TC-JOB-04: Full lifecycle: create job with mock engine, consume SSE, export SRT."""
        if not self.ffmpeg_available:
            self.skipTest("FFmpeg toolchain not available for video extraction test")

        conn = http.client.HTTPConnection(self.server_host, self.server_port, timeout=10.0)
        try:
            job_payload = {
                "video_path": self.synth_mp4_path,
                "engine": "mock",
                "fps": 5.0,
                "confidence_threshold": 0.0,
                "region_box": {"x": 0.0, "y": 0.5, "width": 1.0, "height": 0.5},
            }
            conn.request(
                "POST",
                "/api/jobs",
                body=json.dumps(job_payload),
                headers={"Content-Type": "application/json"},
            )
            resp = conn.getresponse()
            self.assertEqual(resp.status, 201)
            create_data = json.loads(resp.read().decode("utf-8"))
            self.assertIn("job_id", create_data)
            self.assertIn("status", create_data)
            job_id = create_data["job_id"]
            self.assertTrue(len(job_id) > 10)

            # Test SSE Stream
            conn.request("GET", f"/api/jobs/{job_id}/events")
            sse_resp = conn.getresponse()
            self.assertEqual(sse_resp.status, 200)
            self.assertIn("text/event-stream", sse_resp.getheader("Content-Type", ""))

            # Read stream until done or timeout
            raw_stream = b""
            start_t = time.time()
            while time.time() - start_t < 6.0:
                chunk = sse_resp.read(1024)
                if not chunk:
                    break
                raw_stream += chunk
                if b"event: done" in raw_stream or b"event: error" in raw_stream:
                    break

            self.assertTrue(len(raw_stream) > 0)
            stream_text = raw_stream.decode("utf-8", errors="replace")
            # Verify event framing
            self.assertIn("event: ", stream_text)
            self.assertIn("data: ", stream_text)

            # Test SRT Export
            time.sleep(0.2)
            exp_status, exp_headers, exp_body = self.req("GET", f"/api/jobs/{job_id}/export")
            self.assertEqual(exp_status, 200)
            self.assertIn("text/plain", exp_headers.get("Content-Type", ""))
            disp = exp_headers.get("Content-Disposition", "")
            self.assertIn("attachment;", disp)
            srt_body = exp_body.decode("utf-8")
            self.assertIsInstance(srt_body, str)

        finally:
            conn.close()

    def test_jobs_05_cancel_job(self) -> None:
        """TC-JOB-05: POST /api/jobs/:id/cancel returns 200 and cancelled status."""
        if not self.ffmpeg_available:
            self.skipTest("FFmpeg toolchain not available for video extraction test")

        conn = http.client.HTTPConnection(self.server_host, self.server_port, timeout=10.0)
        try:
            job_payload = {
                "video_path": self.synth_mp4_path,
                "engine": "mock",
                "fps": 5.0,
            }
            conn.request(
                "POST",
                "/api/jobs",
                body=json.dumps(job_payload),
                headers={"Content-Type": "application/json"},
            )
            resp = conn.getresponse()
            self.assertEqual(resp.status, 201)
            create_data = json.loads(resp.read().decode("utf-8"))
            job_id = create_data["job_id"]

            # Trigger Cancel
            conn.request("POST", f"/api/jobs/{job_id}/cancel")
            cancel_resp = conn.getresponse()
            self.assertEqual(cancel_resp.status, 200)
            cancel_data = json.loads(cancel_resp.read().decode("utf-8"))
            self.assertEqual(cancel_data["job_id"], job_id)
            self.assertEqual(cancel_data["status"], "cancelled")
        finally:
            conn.close()

    # -------------------------------------------------------------------------
    # 5. Static Web Assets & Workbench Shell Hosting Tests
    # -------------------------------------------------------------------------

    def test_static_01_workbench_html_and_assets(self) -> None:
        """TC-WEB-01: Verify GET / serves index.html with SubLift Workbench shell."""
        status, headers, body = self.req("GET", "/")
        self.assertEqual(status, 200)
        self.assertIn("text/html", headers.get("Content-Type", ""))
        html = body.decode("utf-8", errors="replace")
        self.assertIn("SubLift", html)
        self.assertIn('<div id="app"></div>', html)


def print_banner(text: str) -> None:
    line = "=" * 70
    print(f"\n{line}\n {text}\n{line}")


def run_e2e_suite() -> int:
    print_banner("SubLift Native Web Server (sublift_server) E2E Test Suite")
    suite = unittest.TestLoader().loadTestsFromTestCase(TestSubLiftServerE2E)
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(run_e2e_suite())
