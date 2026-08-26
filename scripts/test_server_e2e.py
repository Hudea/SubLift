#!/usr/bin/env python3
"""SubLift Native Web Server (sublift_server) End-to-End Integration & Boundary Test Suite."""

from __future__ import annotations

import concurrent.futures
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
import unittest
from pathlib import Path
from typing import Any
from urllib.parse import quote


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
        extra_env: dict[str, str] | None = None,
        media_dir: str | None = None,
    ) -> None:
        self.binary_path = binary_path
        self.port = port
        self.host = host
        self.static_dir = static_dir
        self.extra_env = extra_env or {}
        self.media_dir = media_dir
        self.process: subprocess.Popen[str] | None = None

    def start(self, timeout_sec: float = 5.0) -> None:
        if not os.path.isfile(self.binary_path):
            raise FileNotFoundError(f"sublift_server binary not found at: {self.binary_path}")

        cmd = [self.binary_path, "--host", self.host, "-p", str(self.port)]
        if self.static_dir:
            cmd.extend(["--static-dir", self.static_dir])
        if self.media_dir:
            cmd.extend(["--media-dir", self.media_dir])

        # The E2E suite asserts permissive CORS headers (TC-SYS-01 / TC-CORS-01);
        # opt the self-spawned server into CORS so those assertions stay meaningful.
        spawn_env = dict(os.environ)
        spawn_env.setdefault("SUBLIFT_CORS_ORIGIN", "*")
        spawn_env.update(self.extra_env)

        self.process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=spawn_env,
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
            if self.process.stdout:
                self.process.stdout.close()
            if self.process.stderr:
                self.process.stderr.close()
            self.process = None


def make_request(
    host: str,
    port: int,
    method: str,
    path: str,
    headers: dict[str, str] | None = None,
    body: str | bytes | None = None,
) -> tuple[int, dict[str, str], bytes]:
    """Performs an HTTP request and returns (status, headers_dict, body_bytes)."""
    conn = http.client.HTTPConnection(host, port, timeout=5.0)
    try:
        req_headers = headers or {}
        if body is not None:
            conn.request(method, path, body=body, headers=req_headers)
        else:
            conn.request(method, path, headers=req_headers)
        resp = conn.getresponse()
        resp_headers = dict(resp.getheaders())
        body_data = resp.read()
        return resp.status, resp_headers, body_data
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
    paddle_available: bool = False
    paddle_video_path: str = ""
    paddle_video_ready: bool = False
    expect_cors: bool = False
    workspace_root: str = ""
    api_path_prefix: str = ""
    server_binary: str = ""

    @classmethod
    def setUpClass(cls) -> None:
        project_root = Path(__file__).resolve().parents[1]
        binary_path = str(project_root / "build" / "cpp" / "bin" / "sublift_server")
        cls.server_binary = binary_path
        static_dir = str(project_root / "apps" / "web" / "dist")

        # Remote-server mode: fixtures may live in a client-side directory that
        # the server sees under a different filesystem path.
        cls.workspace_root = os.environ.get("SUBLIFT_E2E_WORKSPACE_DIR", "")
        cls.api_path_prefix = os.environ.get("SUBLIFT_E2E_API_PATH_PREFIX", "")
        if cls.workspace_root:
            os.makedirs(cls.workspace_root, exist_ok=True)
            cls.temp_dir = tempfile.mkdtemp(prefix="sublift_e2e_", dir=cls.workspace_root)
        else:
            cls.temp_dir = tempfile.mkdtemp(prefix="sublift_e2e_")

        external_url = os.environ.get("SUBLIFT_SERVER_URL")
        if external_url:
            from urllib.parse import urlparse
            parsed = urlparse(external_url)
            cls.server_host = parsed.hostname or "127.0.0.1"
            cls.server_port = parsed.port or 8080
            cls.server_proc = None
            cls.expect_cors = os.environ.get("SUBLIFT_E2E_EXPECT_CORS", "0") == "1"
        else:
            cls.server_port = find_free_port()
            cls.server_proc = SubLiftServerProcess(
                binary_path,
                cls.server_port,
                cls.server_host,
                static_dir=static_dir if os.path.exists(static_dir) else None,
                # 自建服务时把媒体沙箱根指向 fixture workspace：
                # 既约束 stream/jobs 的可访问范围，也让 TC-RSV 指纹反查有确定搜索域
                extra_env={"SUBLIFT_ALLOWED_MEDIA_ROOT": cls.temp_dir},
            )
            cls.server_proc.start()
            cls.expect_cors = True

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

        # 4. Probe server engine availability (real PaddleOCR support detection)
        try:
            st, _, info_body = make_request(
                cls.server_host, cls.server_port, "GET", "/api/system/info"
            )
            info = json.loads(info_body.decode("utf-8")) if st == 200 else {}
            cls.paddle_available = any(
                e.get("name") == "paddle" and e.get("available") is True
                for e in info.get("engines", [])
            )
        except Exception:
            cls.paddle_available = False

        # 5. Burned-in text video for real PaddleOCR extraction (TC-JOB-07).
        #    Single bottom-anchored token: the detector targets bottom subtitle
        #    bands, and multi-word spacing is not reliably merged into one entry
        #    by the current pipeline, so keep the fixture to one solid token.
        cls.paddle_video_path = os.path.join(cls.temp_dir, "paddle_text_video.mp4")
        if cls.ffmpeg_available:
            try:
                draw_cmd = [
                    ffmpeg_bin,
                    "-y",
                    "-f",
                    "lavfi",
                    "-i",
                    "color=c=black:s=640x360:d=2.0:r=10",
                    "-vf",
                    "drawtext=text='SUBLIFT2026':fontcolor=white:fontsize=48:"
                    "x=(w-text_w)/2:y=h-text_h-30",
                    "-pix_fmt",
                    "yuv420p",
                    cls.paddle_video_path,
                ]
                res = subprocess.run(draw_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if res.returncode == 0 and os.path.exists(cls.paddle_video_path):
                    cls.paddle_video_ready = True
            except Exception:
                cls.paddle_video_ready = False

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
        body: str | bytes | None = None,
    ) -> tuple[int, dict[str, str], bytes]:
        return make_request(self.server_host, self.server_port, method, path, headers, body)

    def api(self, host_path: str) -> str:
        """Maps a host-side fixture path to the path the server API should receive.

        When a remote server sees the fixtures under a different filesystem path,
        rewrite API paths with SUBLIFT_E2E_API_PATH_PREFIX before sending them.
        """
        if not (self.workspace_root and self.api_path_prefix):
            return quote(host_path, safe="/")
        rel = os.path.relpath(host_path, self.workspace_root).replace(os.sep, "/")
        return quote(self.api_path_prefix.rstrip("/") + "/" + rel, safe="/")

    def _require_local_binary(self) -> str:
        if not self.server_binary or not os.path.isfile(self.server_binary):
            self.skipTest("local sublift_server binary required for sidecar state tests")
        if os.environ.get("SUBLIFT_SERVER_URL"):
            self.skipTest("sidecar restart tests cannot run against SUBLIFT_SERVER_URL")
        return self.server_binary

    def _start_sidecar(self, media_dir: str) -> SubLiftServerProcess:
        sidecar = SubLiftServerProcess(
            self._require_local_binary(),
            find_free_port(),
            media_dir=media_dir,
            extra_env={"SUBLIFT_ALLOWED_MEDIA_ROOT": media_dir, "SUBLIFT_CORS_ORIGIN": "*"},
        )
        sidecar.start()
        return sidecar

    def _sidecar_req(
        self,
        sidecar: SubLiftServerProcess,
        method: str,
        path: str,
        headers: dict[str, str] | None = None,
        body: str | bytes | None = None,
    ) -> tuple[int, dict[str, str], bytes]:
        return make_request(sidecar.host, sidecar.port, method, path, headers, body)

    @staticmethod
    def _event_ids(body: bytes) -> list[int]:
        text = body.decode("utf-8", errors="replace")
        return [int(match) for match in re.findall(r"id:\s*(\d+)", text)]

    # -------------------------------------------------------------------------
    # 1. /api/system/info & OPTIONS CORS Tests
    # -------------------------------------------------------------------------

    def test_sys_01_system_info_status_and_cors(self) -> None:
        """TC-SYS-01: Verify GET /api/system/info returns 200 OK; CORS headers when opted in."""
        status, headers, _ = self.req("GET", "/api/system/info")
        self.assertEqual(status, 200)
        self.assertIn("application/json", headers.get("Content-Type", ""))
        if self.expect_cors:
            self.assertEqual(headers.get("Access-Control-Allow-Origin"), "*")
            self.assertIn("GET", headers.get("Access-Control-Allow-Methods", ""))
        else:
            self.assertNotIn(
                "Access-Control-Allow-Origin",
                headers,
                "CORS headers must be absent unless explicitly opted in",
            )

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
                if self.expect_cors:
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
        """TC-STR-02: Nonexistent file path rejected by path sandbox as 400 Bad Request."""
        fake_path = os.path.join(self.temp_dir, "does_not_exist.mp4")
        status, _, body = self.req("GET", f"/api/video/stream?path={self.api(fake_path)}")
        self.assertEqual(status, 400)
        err = json.loads(body.decode("utf-8"))
        self.assertIn("error", err)

    def test_stream_03_directory_path(self) -> None:
        """TC-STR-03: Directory path rejected by sandbox as 400 Bad Request."""
        status, _, _ = self.req("GET", f"/api/video/stream?path={self.api(self.temp_dir)}")
        self.assertEqual(status, 400)

    def test_stream_04_full_file_200_ok(self) -> None:
        """TC-STR-04: Full GET request returns 200 OK with full byte payload and Accept-Ranges."""
        uri = f"/api/video/stream?path={self.api(self.dummy_video_path)}"
        status, headers, body = self.req("GET", uri)
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
            f"/api/video/stream?path={self.api(self.dummy_video_path)}",
            headers={"Range": "bytes=0-1"},
        )
        self.assertEqual(status, 206)
        self.assertEqual(headers.get("Accept-Ranges"), "bytes")
        self.assertEqual(headers.get("Content-Range"), "bytes 0-1/10240")
        self.assertEqual(int(headers.get("Content-Length", 0)), 2)
        self.assertEqual(len(body), 2)
        self.assertEqual(body, self.dummy_video_data[0:2])

    def test_stream_06_slice_range(self) -> None:
        """TC-STR-06: Middle slice Range bytes=100-199 returns 206 Partial Content and 100 bytes."""
        status, headers, body = self.req(
            "GET",
            f"/api/video/stream?path={self.api(self.dummy_video_path)}",
            headers={"Range": "bytes=100-199"},
        )
        self.assertEqual(status, 206)
        self.assertEqual(headers.get("Content-Range"), "bytes 100-199/10240")
        self.assertEqual(int(headers.get("Content-Length", 0)), 100)
        self.assertEqual(len(body), 100)
        self.assertEqual(body, self.dummy_video_data[100:200])

    def test_stream_07_suffix_range(self) -> None:
        """TC-STR-07: Suffix Range bytes=-50 returns 206 Partial Content with last 50 bytes."""
        status, headers, body = self.req(
            "GET",
            f"/api/video/stream?path={self.api(self.dummy_video_path)}",
            headers={"Range": "bytes=-50"},
        )
        self.assertEqual(status, 206)
        self.assertEqual(headers.get("Content-Range"), "bytes 10190-10239/10240")
        self.assertEqual(int(headers.get("Content-Length", 0)), 50)
        self.assertEqual(len(body), 50)
        self.assertEqual(body, self.dummy_video_data[-50:])

    def test_stream_08_open_ended_range(self) -> None:
        """TC-STR-08: Open-ended Range bytes=10000- returns 206 Partial Content to EOF."""
        status, headers, body = self.req(
            "GET",
            f"/api/video/stream?path={self.api(self.dummy_video_path)}",
            headers={"Range": "bytes=10000-"},
        )
        self.assertEqual(status, 206)
        self.assertEqual(headers.get("Content-Range"), "bytes 10000-10239/10240")
        self.assertEqual(int(headers.get("Content-Length", 0)), 240)
        self.assertEqual(len(body), 240)
        self.assertEqual(body, self.dummy_video_data[10000:])

    def test_stream_09_invalid_range_416(self) -> None:
        """TC-STR-09: Unsatisfiable Range bytes=50000-60000 returns 416 Range Not Satisfiable."""
        status, _, _ = self.req(
            "GET",
            f"/api/video/stream?path={self.api(self.dummy_video_path)}",
            headers={"Range": "bytes=50000-60000"},
        )
        self.assertEqual(status, 416)

    def test_stream_10_head_request(self) -> None:
        """TC-STR-10: HEAD /api/video/stream returns 200 with headers but empty body."""
        uri = f"/api/video/stream?path={self.api(self.dummy_video_path)}"
        status, headers, body = self.req("HEAD", uri)
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("Accept-Ranges"), "bytes")
        self.assertEqual(headers.get("Content-Type"), "video/mp4")
        self.assertEqual(int(headers.get("Content-Length", 0)), 10240)
        self.assertEqual(body, b"")

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
                status, headers, _ = self.req("GET", f"/api/video/stream?path={self.api(path)}")
                self.assertEqual(status, 200)
                self.assertEqual(headers.get("Content-Type"), expected_mime)

    # -------------------------------------------------------------------------
    # 3. /api/video/frame Frame Extraction Tests
    # -------------------------------------------------------------------------

    def test_frame_01_missing_path(self) -> None:
        """TC-FRM-01: Missing path parameter returns 400 Bad Request."""
        status, _, body = self.req("GET", "/api/video/frame")
        self.assertEqual(status, 400)
        err = json.loads(body.decode("utf-8"))
        self.assertIn("error", err)

    def test_frame_02_nonexistent_file(self) -> None:
        """TC-FRM-02: Nonexistent file rejected by sandbox as 400 Bad Request."""
        fake_path = os.path.join(self.temp_dir, "ghost_video.mp4")
        status, _, body = self.req("GET", f"/api/video/frame?path={self.api(fake_path)}")
        self.assertEqual(status, 400)
        err = json.loads(body.decode("utf-8"))
        self.assertIn("error", err)

    def test_frame_03_corrupted_file(self) -> None:
        """TC-FRM-03: Corrupted non-video file returns 500 Internal Server Error."""
        if not self.ffmpeg_available:
            self.skipTest("FFmpeg toolchain not available for frame extraction test")
        uri = f"/api/video/frame?path={self.api(self.corrupt_file_path)}"
        status, _, body = self.req("GET", uri)
        self.assertEqual(status, 500)
        err = json.loads(body.decode("utf-8"))
        self.assertIn("error", err)

    def test_frame_04_valid_extraction_at_specified_time(self) -> None:
        """TC-FRM-04: Valid video extraction returns 200 OK and JPEG SOI + EOI markers."""
        if not self.ffmpeg_available:
            self.skipTest("FFmpeg toolchain not available for frame extraction test")
        req_path = f"/api/video/frame?path={self.api(self.synth_mp4_path)}&time_s=1.0"
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
        uri = f"/api/video/frame?path={self.api(self.synth_mp4_path)}"
        status, headers, body = self.req("GET", uri)
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("Content-Type"), "image/jpeg")
        self.assertEqual(body[0:2], b"\xff\xd8")
        self.assertEqual(body[-2:], b"\xff\xd9")

    def test_frame_06_negative_time_clamped(self) -> None:
        """TC-FRM-06: Negative time_s=-5.0 clamps to 0.0s and returns 200 OK valid JPEG."""
        if not self.ffmpeg_available:
            self.skipTest("FFmpeg toolchain not available for frame extraction test")
        req_path = f"/api/video/frame?path={self.api(self.synth_mp4_path)}&time_s=-5.0"
        status, headers, body = self.req("GET", req_path)
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("Content-Type"), "image/jpeg")
        self.assertEqual(body[0:2], b"\xff\xd8")
        self.assertEqual(body[-2:], b"\xff\xd9")

    def test_frame_07_fractional_subsecond_time(self) -> None:
        """TC-FRM-07: Sub-second fractional time_s=0.25 returns 200 OK valid JPEG."""
        if not self.ffmpeg_available:
            self.skipTest("FFmpeg toolchain not available for frame extraction test")
        req_path = f"/api/video/frame?path={self.api(self.synth_mp4_path)}&time_s=0.25"
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
        """TC-JOB-02: POST /api/jobs returns 400 when video_path is invalid or does not exist."""
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
            self.assertEqual(resp.status, 400)
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
                "video_path": self.api(self.synth_mp4_path),
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
            # Verify event framing and SSE id: contract
            self.assertIn("event: ", stream_text)
            self.assertIn("data: ", stream_text)
            self.assertIn("id: ", stream_text)
            self.assertIn("event: done", stream_text)

            # Test SRT Export with Exact Structure Assertion
            time.sleep(0.2)
            exp_status, exp_headers, exp_body = self.req("GET", f"/api/jobs/{job_id}/export")
            self.assertEqual(exp_status, 200)
            self.assertIn("text/plain", exp_headers.get("Content-Type", ""))
            disp = exp_headers.get("Content-Disposition", "")
            self.assertIn("attachment;", disp)
            srt_body = exp_body.decode("utf-8")
            self.assertIsInstance(srt_body, str)

            # Strict SRT Format & Content Verification (eliminating false-green assertions)
            self.assertTrue(
                srt_body.strip().startswith("1\n"),
                f"SRT should start with index 1:\n{srt_body}",
            )
            import re
            srt_time_pattern = re.compile(r"\d{2}:\d{2}:\d{2},\d{3} --> \d{2}:\d{2}:\d{2},\d{3}")
            matches = srt_time_pattern.findall(srt_body)
            self.assertGreater(len(matches), 0, f"No valid SRT timestamps found in:\n{srt_body}")
            self.assertIn("mock_ocr", srt_body)

        finally:
            conn.close()

    def test_jobs_05_cancel_job(self) -> None:
        """TC-JOB-05: POST /api/jobs/:id/cancel returns 200 and cancelled status."""
        if not self.ffmpeg_available:
            self.skipTest("FFmpeg toolchain not available for video extraction test")

        conn = http.client.HTTPConnection(self.server_host, self.server_port, timeout=10.0)
        try:
            job_payload = {
                "video_path": self.api(self.synth_mp4_path),
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

    def test_jobs_06_batch_queue_serial_execution(self) -> None:
        """TC-JOB-06: Submit 2 jobs to verify serial worker execution and convergence."""
        if not self.ffmpeg_available:
            self.skipTest("FFmpeg toolchain not available for queue test")

        job_ids = []
        for _ in range(2):
            payload = {
                "video_path": self.api(self.synth_mp4_path),
                "engine": "mock",
                "fps": 5.0,
            }
            status, _, body = self.req(
                "POST",
                "/api/jobs",
                body=json.dumps(payload),
                headers={"Content-Type": "application/json"},
            )
            self.assertEqual(status, 201)
            data = json.loads(body.decode("utf-8"))
            job_ids.append(data["job_id"])

        self.assertEqual(len(job_ids), 2)
        # Verify both jobs can be exported after execution completes
        for jid in job_ids:
            # Poll export for up to 12 seconds
            completed = False
            for _ in range(60):
                time.sleep(0.2)
                exp_st, _, _ = self.req("GET", f"/api/jobs/{jid}/export")
                if exp_st == 200:
                    completed = True
                    break
            self.assertTrue(completed, f"Job {jid} did not complete successfully")

    def test_jobs_07_paddle_real_extraction_golden(self) -> None:
        """TC-JOB-07: Real PaddleOCR extraction on burned-in text; golden SRT exact match."""
        if not (self.ffmpeg_available and self.paddle_video_ready):
            self.skipTest("FFmpeg drawtext unavailable for burned-text fixture video")
        if not self.paddle_available:
            self.skipTest("PaddleOCR engine not available on this server (models missing?)")

        job_payload = {
            "video_path": self.api(self.paddle_video_path),
            "engine": "paddle",
            "fps": 2.0,
        }
        status, _, body = self.req(
            "POST",
            "/api/jobs",
            body=json.dumps(job_payload),
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(status, 201)
        job_id = json.loads(body.decode("utf-8"))["job_id"]

        # Paddle model load + inference needs a wider window than mock.
        # 409 = job still running, keep polling until terminal (200) or evicted (404).
        srt_status, _, exp_body = 409, None, b""
        for _ in range(150):
            time.sleep(0.4)
            srt_status, _, exp_body = self.req("GET", f"/api/jobs/{job_id}/export")
            if srt_status in (200, 404):
                break
        self.assertEqual(
            srt_status, 200, f"Paddle job did not complete; last status {srt_status}"
        )
        srt_body = exp_body.decode("utf-8")

        # Format contract + burned text must be recovered by real OCR
        import re

        srt_time_pattern = re.compile(r"\d{2}:\d{2}:\d{2},\d{3} --> \d{2}:\d{2}:\d{2},\d{3}")
        self.assertGreater(
            len(srt_time_pattern.findall(srt_body)), 0, f"No valid timestamps:\n{srt_body}"
        )
        self.assertIn("SUBLIFT2026", srt_body.upper(), f"Burned text not recovered:\n{srt_body}")

        # Golden baseline: record on first run, exact-compare afterwards
        golden_path = os.environ.get("SUBLIFT_E2E_GOLDEN_SRT", "")
        if golden_path:
            if os.path.exists(golden_path):
                expected = Path(golden_path).read_text(encoding="utf-8").strip()
                self.assertEqual(
                    srt_body.strip(),
                    expected,
                    f"SRT differs from golden baseline {golden_path}",
                )
            else:
                os.makedirs(os.path.dirname(golden_path) or ".", exist_ok=True)
                Path(golden_path).write_text(srt_body, encoding="utf-8")
                print(f"\n[golden] recorded first-run baseline -> {golden_path}")

    def test_jobs_08_state_persistence_and_restart_recovery(self) -> None:
        """TC-JOB-08: Job state is persisted in cache dir and query endpoints return detail."""
        cache_dir = os.path.join(self.temp_dir, ".sublift_cache")
        state_file = os.path.join(cache_dir, "jobs_state.v1.json")

        # Submit a job
        job_payload = {
            "video_path": self.api(self.synth_mp4_path),
            "engine": "mock",
            "fps": 5.0,
        }
        status, _, body = self.req(
            "POST",
            "/api/jobs",
            body=json.dumps(job_payload),
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(status, 201)
        job_id = json.loads(body.decode("utf-8"))["job_id"]

        # Wait for execution and verify state file exists
        time.sleep(0.5)
        self.assertTrue(os.path.exists(state_file), f"State file {state_file} should exist")

        # Query GET /api/jobs and GET /api/jobs/:id
        st_list, _, body_list = self.req("GET", "/api/jobs")
        self.assertEqual(st_list, 200)
        jobs_list = json.loads(body_list.decode("utf-8"))
        self.assertTrue(any(j["job_id"] == job_id for j in jobs_list))

        st_detail, _, body_detail = self.req("GET", f"/api/jobs/{job_id}")
        self.assertEqual(st_detail, 200)
        detail = json.loads(body_detail.decode("utf-8"))
        self.assertEqual(detail["job_id"], job_id)

    def test_jobs_09_sse_resumption_with_cursor_and_last_event_id(self) -> None:
        """TC-JOB-09: SSE event stream supports resumption via cursor param and Last-Event-ID header."""
        if not self.ffmpeg_available:
            self.skipTest("FFmpeg toolchain not available for video extraction test")

        job_payload = {
            "video_path": self.api(self.synth_mp4_path),
            "engine": "mock",
            "fps": 5.0,
        }
        status, _, body = self.req(
            "POST",
            "/api/jobs",
            body=json.dumps(job_payload),
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(status, 201)
        job_id = json.loads(body.decode("utf-8"))["job_id"]

        # Wait for job to complete
        time.sleep(1.0)

        # Resume with cursor=1
        st1, _, body1 = self.req("GET", f"/api/jobs/{job_id}/events?cursor=1")
        self.assertEqual(st1, 200)
        stream_text1 = body1.decode("utf-8", errors="replace")
        self.assertIn("id: ", stream_text1)
        self.assertNotIn("id: 1\n", stream_text1)

        # Resume with Last-Event-ID: 1
        st2, _, body2 = self.req("GET", f"/api/jobs/{job_id}/events", headers={"Last-Event-ID": "1"})
        self.assertEqual(st2, 200)
        stream_text2 = body2.decode("utf-8", errors="replace")
        self.assertIn("id: ", stream_text2)
        self.assertNotIn("id: 1\n", stream_text2)

        full_st, _, full_body = self.req("GET", f"/api/jobs/{job_id}/events")
        self.assertEqual(full_st, 200)
        full_ids = self._event_ids(full_body)
        self.assertGreaterEqual(len(full_ids), 2)
        high_cursor = max(full_ids) + 10
        high_st, _, high_body = self.req(
            "GET", f"/api/jobs/{job_id}/events?cursor={high_cursor}"
        )
        self.assertEqual(high_st, 200)
        self.assertEqual(self._event_ids(high_body), [])

        bad_st, _, bad_body = self.req(
            "GET", f"/api/jobs/{job_id}/events?cursor=invalid_not_number"
        )
        self.assertEqual(bad_st, 200)
        self.assertEqual(self._event_ids(bad_body), full_ids)

    def test_jobs_10_unified_job_config_with_script_and_confidence(self) -> None:
        """TC-JOB-10: POST /api/jobs accepts full unified JobConfig (script, confidence, region_box) and preserves it."""
        job_payload = {
            "video_path": self.api(self.synth_mp4_path),
            "engine": "mock",
            "fps": 8.0,
            "confidence_threshold": 0.45,
            "script": "Hans",
            "region_box": {"x": 0.05, "y": 0.72, "width": 0.9, "height": 0.22},
        }
        status, _, body = self.req(
            "POST",
            "/api/jobs",
            body=json.dumps(job_payload),
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(status, 201)
        job_id = json.loads(body.decode("utf-8"))["job_id"]

        # Detail query
        st_detail, _, body_detail = self.req("GET", f"/api/jobs/{job_id}")
        self.assertEqual(st_detail, 200)
        detail = json.loads(body_detail.decode("utf-8"))
        cfg = detail.get("config", {})
        self.assertEqual(cfg.get("fps"), 8.0)
        self.assertEqual(cfg.get("confidence_threshold"), 0.45)
        self.assertEqual(cfg.get("script"), "Hans")
        self.assertEqual(cfg.get("region_box", {}).get("x"), 0.05)
        self.assertEqual(cfg.get("region_box", {}).get("y"), 0.72)

        omitted = {
            "video_path": self.api(self.synth_mp4_path),
            "engine": "mock",
            "fps": 5.0,
        }
        json_headers = {"Content-Type": "application/json"}
        st_om, _, body_om = self.req(
            "POST", "/api/jobs", body=json.dumps(omitted), headers=json_headers
        )
        self.assertEqual(st_om, 201)
        omit_id = json.loads(body_om.decode("utf-8"))["job_id"]
        st_od, _, body_od = self.req("GET", f"/api/jobs/{omit_id}")
        self.assertEqual(st_od, 200)
        omit_cfg = json.loads(body_od.decode("utf-8")).get("config", {})
        self.assertTrue(omit_cfg.get("script") in (None, ""))
        self.assertAlmostEqual(float(omit_cfg.get("confidence_threshold", 0.0)), 0.0)

        clamped = {
            "video_path": self.api(self.synth_mp4_path),
            "engine": "mock",
            "fps": 5.0,
            "region_box": {"x": -0.5, "y": 1.8, "width": -0.2, "height": 5.0},
        }
        st_c, _, body_c = self.req(
            "POST", "/api/jobs", body=json.dumps(clamped), headers=json_headers
        )
        self.assertEqual(st_c, 201)
        clamp_id = json.loads(body_c.decode("utf-8"))["job_id"]
        _, _, body_cd = self.req("GET", f"/api/jobs/{clamp_id}")
        box = json.loads(body_cd.decode("utf-8")).get("config", {}).get("region_box", {})
        self.assertEqual(box.get("x"), 0.0)
        self.assertEqual(box.get("y"), 1.0)
        self.assertEqual(box.get("width"), 0.0)
        self.assertEqual(box.get("height"), 1.0)

        for bad_engine in ("python", ""):
            bad_payload = {"video_path": self.api(self.synth_mp4_path), "engine": bad_engine}
            st_bad, _, _ = self.req(
                "POST", "/api/jobs", body=json.dumps(bad_payload), headers=json_headers
            )
            self.assertEqual(st_bad, 400, f"engine {bad_engine!r} must fail closed")

    def test_jobs_11_reload_interrupted_corrupt_and_lru(self) -> None:
        """Queued/running jobs become interrupted; corrupt state is fail-closed; LRU caps at 50."""
        media_dir = tempfile.mkdtemp(prefix="sublift_e2e_state_")
        cache_dir = Path(media_dir) / ".sublift_cache"
        cache_dir.mkdir(parents=True)
        state_file = cache_dir / "jobs_state.v1.json"
        video = str(Path(media_dir) / "sample.mp4")
        Path(video).write_bytes(b"not-a-real-mp4")

        try:
            state_file.write_text(
                json.dumps(
                    {
                        "version": 1,
                        "updated_at_ms": 1,
                        "jobs": [
                            {
                                "job_id": "job-queued-crash-1",
                                "config": {"video_path": video, "engine": "mock", "fps": 3.0},
                                "status": "queued",
                                "entries": [],
                            },
                            {
                                "job_id": "job-running-crash-2",
                                "config": {"video_path": video, "engine": "mock"},
                                "status": "running",
                                "entries": [],
                            },
                            {
                                "job_id": "job-completed-crash-3",
                                "config": {"video_path": video, "engine": "mock"},
                                "status": "completed",
                                "entries": [
                                    {
                                        "index": 1,
                                        "start_ms": 1000,
                                        "end_ms": 2500,
                                        "text": "Hello SubLift First Line",
                                        "confidence": 0.96,
                                    }
                                ],
                            },
                            {
                                "job_id": "job-failed-crash-4",
                                "config": {"video_path": video, "engine": "mock"},
                                "status": "failed",
                                "error_message": "Original failure reason preserved",
                                "entries": [],
                            },
                        ],
                    }
                ),
                encoding="utf-8",
            )
            sidecar = self._start_sidecar(media_dir)
            try:
                st, _, body = self._sidecar_req(sidecar, "GET", "/api/jobs")
                self.assertEqual(st, 200)
                jobs = {j["job_id"]: j for j in json.loads(body.decode("utf-8"))}
                self.assertEqual(jobs["job-queued-crash-1"]["status"], "interrupted")
                self.assertEqual(jobs["job-running-crash-2"]["status"], "interrupted")
                self.assertEqual(jobs["job-completed-crash-3"]["status"], "completed")
                completed = jobs["job-completed-crash-3"]
                self.assertEqual(completed["entries"][0]["text"], "Hello SubLift First Line")
                failed = jobs["job-failed-crash-4"]
                self.assertEqual(failed["error_message"], "Original failure reason preserved")
                export_path = "/api/jobs/job-completed-crash-3/export"
                st_exp, _, exp_body = self._sidecar_req(sidecar, "GET", export_path)
                self.assertEqual(st_exp, 200)
                self.assertIn("Hello SubLift First Line", exp_body.decode("utf-8"))
                queued_events = "/api/jobs/job-queued-crash-1/events"
                st_sse, _, sse_body = self._sidecar_req(sidecar, "GET", queued_events)
                self.assertEqual(st_sse, 200)
                self.assertIn("interrupted", sse_body.decode("utf-8").lower())
            finally:
                sidecar.stop()

            state_file.write_text("{ \"version\": 1, \"jobs\": [ { broken", encoding="utf-8")
            sidecar = self._start_sidecar(media_dir)
            try:
                st, _, body = self._sidecar_req(sidecar, "GET", "/api/jobs")
                self.assertEqual(st, 200)
                self.assertEqual(json.loads(body.decode("utf-8")), [])
            finally:
                sidecar.stop()

            mixed = {
                "version": 1,
                "jobs": [
                    "invalid",
                    {"invalid": True},
                    {
                        "job_id": "valid-surviving-job-1",
                        "status": "completed",
                        "config": {"video_path": video, "engine": "mock"},
                        "entries": [
                            {
                                "start_ms": 100,
                                "end_ms": 500,
                                "text": "Valid entry",
                                "confidence": 0.9,
                            }
                        ],
                    },
                    {
                        "job_id": "valid-surviving-job-2",
                        "status": "queued",
                        "config": {"video_path": video, "engine": "mock"},
                        "entries": [],
                    },
                ],
            }
            state_file.write_text(json.dumps(mixed), encoding="utf-8")
            sidecar = self._start_sidecar(media_dir)
            try:
                st, _, body = self._sidecar_req(sidecar, "GET", "/api/jobs")
                self.assertEqual(st, 200)
                ids = {j["job_id"] for j in json.loads(body.decode("utf-8"))}
                self.assertEqual(ids, {"valid-surviving-job-1", "valid-surviving-job-2"})
            finally:
                sidecar.stop()

            state_file.write_text(
                json.dumps(
                    {
                        "version": 1,
                        "jobs": [
                            {
                                "job_id": f"job-lru-{i:02d}",
                                "config": {"video_path": video, "engine": "mock"},
                                "status": "completed",
                                "created_at_ms": 1000 + i,
                                "entries": [],
                            }
                            for i in range(60)
                        ],
                    }
                ),
                encoding="utf-8",
            )
            sidecar = self._start_sidecar(media_dir)
            try:
                st, _, body = self._sidecar_req(sidecar, "GET", "/api/jobs")
                self.assertEqual(st, 200)
                self.assertEqual(len(json.loads(body.decode("utf-8"))), 50)
            finally:
                sidecar.stop()
            saved = json.loads(state_file.read_text(encoding="utf-8"))
            self.assertEqual(len(saved.get("jobs", [])), 50)
        finally:
            shutil.rmtree(media_dir, ignore_errors=True)

    def test_jobs_12_job_config_survives_restart(self) -> None:
        """JobConfig fields persist across a clean server restart."""
        if not self.ffmpeg_available:
            self.skipTest("FFmpeg toolchain not available for video extraction test")
        media_dir = tempfile.mkdtemp(prefix="sublift_e2e_restart_")
        try:
            video = os.path.join(media_dir, "restart.mp4")
            shutil.copy2(self.synth_mp4_path, video)
            sidecar = self._start_sidecar(media_dir)
            job_id = ""
            try:
                payload = {
                    "video_path": video,
                    "engine": "mock",
                    "fps": 8.0,
                    "confidence_threshold": 0.42,
                    "script": "Hans",
                    "region_box": {"x": 0.12, "y": 0.78, "width": 0.76, "height": 0.18},
                }
                st, _, body = self._sidecar_req(
                    sidecar, "POST", "/api/jobs", body=json.dumps(payload),
                    headers={"Content-Type": "application/json"},
                )
                self.assertEqual(st, 201)
                job_id = json.loads(body.decode("utf-8"))["job_id"]
                time.sleep(0.3)
            finally:
                sidecar.stop()

            sidecar = self._start_sidecar(media_dir)
            try:
                st, _, body = self._sidecar_req(sidecar, "GET", f"/api/jobs/{job_id}")
                self.assertEqual(st, 200)
                cfg = json.loads(body.decode("utf-8")).get("config", {})
                self.assertEqual(cfg.get("script"), "Hans")
                self.assertAlmostEqual(float(cfg.get("confidence_threshold", -1)), 0.42)
                self.assertAlmostEqual(float(cfg.get("region_box", {}).get("x", -1)), 0.12)
            finally:
                sidecar.stop()
        finally:
            shutil.rmtree(media_dir, ignore_errors=True)

    # -------------------------------------------------------------------------
    # 5. Security & Sandbox Boundary Tests (Feature 12502 / 12504)
    # -------------------------------------------------------------------------

    def test_security_01_lfi_and_path_traversal(self) -> None:
        """TC-SEC-01: Path sandbox blocks LFI targets (/etc/passwd, /etc/hosts) and traversal."""
        # .ts is a source-code extension colliding with MPEG-TS and must stay
        # outside the media whitelist (arbitrary code file read via CORS).
        ts_probe = os.path.join(self.temp_dir, "secret_source.ts")
        with open(ts_probe, "w", encoding="utf-8") as f:
            f.write("SECRET_TS_SHOULD_NOT_BE_READABLE")
        for target in [
            "/etc/passwd",
            "/etc/hosts",
            "../../../etc/shadow",
            "/dev/null",
            self.api(ts_probe),
        ]:
            with self.subTest(target=target):
                # Stream route
                st, _, _ = self.req("GET", f"/api/video/stream?path={target}")
                self.assertEqual(st, 400, f"Stream should reject {target} with 400")

                # Frame route
                st2, _, _ = self.req("GET", f"/api/video/frame?path={target}")
                self.assertEqual(st2, 400, f"Frame should reject {target} with 400")

                # Create Job route
                st3, _, _ = self.req(
                    "POST",
                    "/api/jobs",
                    body=json.dumps({"video_path": target, "engine": "mock"}),
                    headers={"Content-Type": "application/json"},
                )
                self.assertEqual(st3, 400, f"Job creation should reject {target} with 400")

    def test_security_02_invalid_ocr_engine_rejected(self) -> None:
        """TC-SEC-02: Invalid or unavailable OCR engine name is rejected with 400 Bad Request."""
        payload = {
            "video_path": self.api(self.synth_mp4_path),
            "engine": "invalid_engine_name_xyz",
            "fps": 2.0,
        }
        st, _, body = self.req(
            "POST",
            "/api/jobs",
            body=json.dumps(payload),
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(st, 400)
        err = json.loads(body.decode("utf-8"))
        self.assertIn("error", err)

    # -------------------------------------------------------------------------
    # 5.5 Local Fingerprint Resolve Tests (Feature 12507)
    # -------------------------------------------------------------------------

    def _fingerprint_body(self, name: str, data: bytes, size: int,
                          head_override: str | None = None,
                          tail_override: str | None = None) -> str:
        head = (head_override if head_override is not None else data[:4096].hex())
        tail = (tail_override if tail_override is not None else data[-4096:].hex())
        payload: dict[str, Any] = {
            "name": name,
            "size": size,
            "head_hex": head,
            "tail_hex": tail,
        }
        return json.dumps(payload)

    def test_resolve_01_exact_fingerprint_locates_and_streams(self) -> None:
        """TC-RSV-01: Exact name+size+edge-bytes fingerprint resolves the server path."""
        body = self._fingerprint_body(
            "test_stream_video.mp4", self.dummy_video_data, len(self.dummy_video_data)
        )
        st, _, resp_body = self.req(
            "POST", "/api/video/resolve", body=body,
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(st, 200)
        data = json.loads(resp_body.decode("utf-8"))
        self.assertIn("path", data)
        self.assertTrue(data["path"].endswith("test_stream_video.mp4"))

        # 返回的路径必须立即可用于流式播放（沙箱放行、字节完整）
        stream_uri = f"/api/video/stream?path={quote(data['path'], safe='/')}"
        st2, headers2, body2 = self.req("GET", stream_uri)
        self.assertEqual(st2, 200)
        self.assertEqual(int(headers2.get("Content-Length", 0)), len(self.dummy_video_data))
        self.assertEqual(body2, self.dummy_video_data)

    def test_resolve_02_tampered_or_unknown_fingerprints_miss(self) -> None:
        """TC-RSV-02: Tampered head/tail/size and unknown names all return 404."""
        data = self.dummy_video_data
        head_hex = data[:4096].hex()
        tail_hex = data[-4096:].hex()
        # 篡改首字节（原始 data[0]==0x00 与 data[-4096]==0x00，改 ff 必产生差异）
        bad_head = "ff" + head_hex[2:]
        bad_tail = "ff" + tail_hex[2:]
        for body in [
            self._fingerprint_body("test_stream_video.mp4", data, len(data),
                                   head_override=bad_head),
            self._fingerprint_body("test_stream_video.mp4", data, len(data),
                                   tail_override=bad_tail),
            json.dumps({"name": "test_stream_video.mp4", "size": 999, "head_hex": head_hex}),
            json.dumps({"name": "ghost_video.mp4", "size": len(data),
                        "head_hex": head_hex, "tail_hex": tail_hex}),
        ]:
            with self.subTest(body=body[:60]):
                st, _, _ = self.req(
                    "POST", "/api/video/resolve", body=body,
                    headers={"Content-Type": "application/json"},
                )
                self.assertEqual(st, 404)

    # -------------------------------------------------------------------------
    # 6. Workspace Configuration & .sublift_cache Tests (Feature 12508)
    # -------------------------------------------------------------------------

    def test_workspace_01_config_endpoints(self) -> None:
        """TC-WKS-01: GET and POST /api/config/workspace manage media directory."""
        # 1. Check workspace config
        st, _, body = self.req("GET", "/api/config/workspace")
        self.assertEqual(st, 200)
        cfg = json.loads(body.decode("utf-8"))
        self.assertIn("configured", cfg)
        self.assertIn("media_dir", cfg)
        self.assertIn("cache_dir", cfg)

        # 2. Configure media directory to self.temp_dir
        post_body = json.dumps({"media_dir": self.temp_dir})
        st, _, body = self.req(
            "POST",
            "/api/config/workspace",
            body=post_body,
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(st, 200)
        res = json.loads(body.decode("utf-8"))
        self.assertTrue(res.get("configured"))
        self.assertEqual(res.get("media_dir"), os.path.realpath(self.temp_dir))
        self.assertIn(".sublift_cache", res.get("cache_dir", ""))

        # 3. Invalid directory returns 400
        bad_body = json.dumps({"media_dir": "/nonexistent_folder_xyz_999"})
        st_bad, _, _ = self.req(
            "POST",
            "/api/config/workspace",
            body=bad_body,
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(st_bad, 400)

    def test_workspace_02_cache_directory_and_fingerprint(self) -> None:
        """TC-WKS-02: .sublift_cache is created and fingerprint resolves inside workspace."""
        # Ensure workspace is set to temp_dir
        post_body = json.dumps({"media_dir": self.temp_dir})
        st, _, _ = self.req(
            "POST",
            "/api/config/workspace",
            body=post_body,
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(st, 200)

        cache_path = os.path.join(self.temp_dir, ".sublift_cache")
        self.assertTrue(
            os.path.isdir(cache_path),
            ".sublift_cache directory should be automatically created",
        )
        self.assertTrue(
            os.path.isdir(os.path.join(cache_path, "remux")),
            "remux cache dir should exist",
        )
        self.assertTrue(
            os.path.isdir(os.path.join(cache_path, "frames")),
            "frames cache dir should exist",
        )

    def test_workspace_03_list_videos(self) -> None:
        """TC-WKS-03: GET /api/config/workspace/videos lists videos in workspace."""
        post_body = json.dumps({"media_dir": self.temp_dir})
        self.req(
            "POST",
            "/api/config/workspace",
            body=post_body,
            headers={"Content-Type": "application/json"},
        )
        st, _, body = self.req("GET", "/api/config/workspace/videos")
        self.assertEqual(st, 200)
        vids = json.loads(body.decode("utf-8"))
        self.assertIsInstance(vids, list)
        self.assertGreaterEqual(len(vids), 1)
        self.assertTrue(any(v.get("name") == "test_stream_video.mp4" for v in vids))

    # -------------------------------------------------------------------------
    # 7. Static Web Assets & Workbench Shell Hosting Tests
    # -------------------------------------------------------------------------

    def test_static_01_workbench_html_and_assets(self) -> None:
        """TC-WEB-01: Verify GET / serves index.html with SubLift Workbench shell."""
        status, headers, body = self.req("GET", "/")
        self.assertEqual(status, 200)
        self.assertIn("text/html", headers.get("Content-Type", ""))
        html = body.decode("utf-8", errors="replace")
        self.assertIn("SubLift", html)
        self.assertIn('<div id="app"></div>', html)

    # -------------------------------------------------------------------------
    # 8. Auto ROI Subtitle Region Detection Tests (Feature 12509)
    # -------------------------------------------------------------------------

    def test_detect_region_01_invalid_params_and_security(self) -> None:
        """TC-REG-01: POST /api/video/detect-region validates parameters and enforces sandbox."""
        # Missing video_path
        st, _, _ = self.req(
            "POST",
            "/api/video/detect-region",
            body=b"{}",
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(st, 400)

        # LFI / non-media file
        st_lfi, _, _ = self.req(
            "POST",
            "/api/video/detect-region",
            body=json.dumps({"video_path": "/etc/hosts"}),
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(st_lfi, 400)

    def test_detect_region_02_valid_video_detection(self) -> None:
        """TC-REG-02: POST /api/video/detect-region returns suggested_box and metadata."""
        if not self.synth_mp4_path or not os.path.exists(self.synth_mp4_path):
            self.skipTest("Synthesized test video not available")

        # Multi-point auto detection
        req_body = json.dumps({"video_path": self.synth_mp4_path, "engine": "mock"})
        st, _, body = self.req(
            "POST",
            "/api/video/detect-region",
            body=req_body,
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(st, 200)
        res = json.loads(body.decode("utf-8"))
        self.assertIn("detected", res)
        self.assertIn("sample_time_s", res)
        self.assertIn("suggested_box", res)
        box = res["suggested_box"]
        self.assertGreaterEqual(box["x"], 0.0)
        self.assertLessEqual(box["x"], 1.0)
        self.assertGreaterEqual(box["y"], 0.0)
        self.assertLessEqual(box["y"], 1.0)
        self.assertGreater(box["width"], 0.0)
        self.assertGreater(box["height"], 0.0)
        self.assertLessEqual(box["y"] + box["height"], 1.0001)

        # Single-point playhead detection
        req_time = json.dumps(
            {"video_path": self.synth_mp4_path, "time_s": 0.5, "engine": "mock"}
        )
        st2, _, body2 = self.req(
            "POST",
            "/api/video/detect-region",
            body=req_time,
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(st2, 200)
        res2 = json.loads(body2.decode("utf-8"))
        self.assertEqual(res2["sample_time_s"], 0.5)

    def test_scan_path_01_directory_expansion(self) -> None:
        """TC-SCN-01: POST /api/video/scan-path recursively expands server-side directory."""
        if not self.synth_mp4_path or not os.path.exists(self.synth_mp4_path):
            self.skipTest("Synthesized test video not available")

        # Scan directory containing synth video
        parent_dir = os.path.dirname(self.synth_mp4_path)
        req_body = json.dumps({"path": parent_dir})
        st, _, body = self.req(
            "POST",
            "/api/video/scan-path",
            body=req_body,
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(st, 200)
        res = json.loads(body.decode("utf-8"))
        self.assertIn("accepted", res)
        self.assertIn("skipped", res)
        self.assertIn("rejected", res)
        accepted_paths = [item["videoPath"] for item in res["accepted"]]
        self.assertIn(os.path.realpath(self.synth_mp4_path), accepted_paths)

    def test_scan_path_02_rejections_and_security(self) -> None:
        """TC-SCN-02: POST /api/video/scan-path properly rejects invalid paths."""
        # Non-existent path
        req_body = json.dumps({"path": "/nonexistent/random/directory_12345"})
        st, _, body = self.req(
            "POST",
            "/api/video/scan-path",
            body=req_body,
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(st, 200)
        res = json.loads(body.decode("utf-8"))
        self.assertEqual(len(res["accepted"]), 0)
        self.assertGreaterEqual(len(res["rejected"]), 1)
        self.assertEqual(res["rejected"][0]["reason"]["kind"], "unreadable")

        # Out-of-workspace path (/etc/hosts) rejected as unreadable / security restricted
        req_outside = json.dumps({"path": "/etc/hosts"})
        st_out, _, body_out = self.req(
            "POST",
            "/api/video/scan-path",
            body=req_outside,
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(st_out, 200)
        res_out = json.loads(body_out.decode("utf-8"))
        self.assertEqual(len(res_out["accepted"]), 0)
        self.assertGreaterEqual(len(res_out["rejected"]), 1)
        self.assertEqual(res_out["rejected"][0]["reason"]["kind"], "unreadable")

        # Non-media file inside workspace
        non_media_path = os.path.join(self.temp_dir, "notes.txt")
        with open(non_media_path, "w", encoding="utf-8") as f:
            f.write("text content")
        req_file = json.dumps({"path": non_media_path})
        st_file, _, body_file = self.req(
            "POST",
            "/api/video/scan-path",
            body=req_file,
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(st_file, 200)
        res_file = json.loads(body_file.decode("utf-8"))
        self.assertEqual(len(res_file["accepted"]), 0)
        self.assertGreaterEqual(len(res_file["rejected"]), 1)
        self.assertEqual(res_file["rejected"][0]["reason"]["kind"], "unsupportedFormat")

    # -------------------------------------------------------------------------
    # 9. Atomic Disk Save & Conflict Policies (Feature 12514)
    # -------------------------------------------------------------------------

    def test_save_01_single_job_atomic_save(self) -> None:
        """TC-SAV-01: POST /api/jobs/:id/save saves subtitle file atomically to disk."""
        if not self.synth_mp4_path or not os.path.exists(self.synth_mp4_path):
            self.skipTest("Synthesized test video not available")

        # 1. Create a job with region_box and confidence 0.0 to ensure entries
        job_payload = {
            "video_path": self.api(self.synth_mp4_path),
            "engine": "mock",
            "fps": 5.0,
            "confidence_threshold": 0.0,
            "region_box": {"x": 0.0, "y": 0.5, "width": 1.0, "height": 0.5},
        }
        status, _, body = self.req(
            "POST",
            "/api/jobs",
            body=json.dumps(job_payload),
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(status, 201)
        job_id = json.loads(body.decode("utf-8"))["job_id"]

        # Wait for completion
        for _ in range(60):
            time.sleep(0.1)
            exp_st, _, _ = self.req("GET", f"/api/jobs/{job_id}/export")
            if exp_st == 200:
                break

        # 2. Trigger atomic save
        target_srt = os.path.join(self.temp_dir, "saved_synth_test.srt")
        if os.path.exists(target_srt):
            os.remove(target_srt)

        save_payload = {
            "target_path": target_srt,
            "conflict_policy": "deterministic_rename",
            "allow_empty": False,
        }
        st_save, _, body_save = self.req(
            "POST",
            f"/api/jobs/{job_id}/save",
            body=json.dumps(save_payload),
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(st_save, 200)
        save_data = json.loads(body_save.decode("utf-8"))
        self.assertEqual(save_data["status"], "saved")
        self.assertEqual(save_data["job_id"], job_id)
        self.assertFalse(save_data["empty_result"])
        self.assertTrue(os.path.exists(target_srt))

        # Check saved SRT content
        with open(target_srt, "r", encoding="utf-8") as f:
            content = f.read()
            self.assertIn("mock_ocr", content)

    def test_save_02_conflict_policies(self) -> None:
        """TC-SAV-02: Test conflict policies: skip, deterministic_rename, replace."""
        if not self.synth_mp4_path or not os.path.exists(self.synth_mp4_path):
            self.skipTest("Synthesized test video not available")

        # Create completed job with entries
        job_payload = {
            "video_path": self.api(self.synth_mp4_path),
            "engine": "mock",
            "fps": 5.0,
            "confidence_threshold": 0.0,
            "region_box": {"x": 0.0, "y": 0.5, "width": 1.0, "height": 0.5},
        }
        st, _, body = self.req("POST", "/api/jobs", body=json.dumps(job_payload), headers={"Content-Type": "application/json"})
        self.assertEqual(st, 201)
        job_id = json.loads(body.decode("utf-8"))["job_id"]

        for _ in range(60):
            time.sleep(0.1)
            if self.req("GET", f"/api/jobs/{job_id}/export")[0] == 200:
                break

        base_file = os.path.join(self.temp_dir, "conflict_test.srt")
        with open(base_file, "w", encoding="utf-8") as f:
            f.write("ORIGINAL_CONTENT")

        # 1. Skip policy: should not overwrite
        skip_req = {"target_path": base_file, "conflict_policy": "skip"}
        st_skip, _, body_skip = self.req("POST", f"/api/jobs/{job_id}/save", body=json.dumps(skip_req), headers={"Content-Type": "application/json"})
        self.assertEqual(st_skip, 200)
        data_skip = json.loads(body_skip.decode("utf-8"))
        self.assertEqual(data_skip["status"], "skipped")
        with open(base_file, "r", encoding="utf-8") as f:
            self.assertEqual(f.read(), "ORIGINAL_CONTENT")

        # 2. Deterministic rename: should create conflict_test_1.srt
        rename_req = {"target_path": base_file, "conflict_policy": "deterministic_rename"}
        st_ren, _, body_ren = self.req("POST", f"/api/jobs/{job_id}/save", body=json.dumps(rename_req), headers={"Content-Type": "application/json"})
        self.assertEqual(st_ren, 200)
        data_ren = json.loads(body_ren.decode("utf-8"))
        self.assertEqual(data_ren["status"], "saved")
        renamed_file = os.path.join(self.temp_dir, "conflict_test_1.srt")
        self.assertTrue(os.path.exists(renamed_file))
        self.assertEqual(os.path.realpath(data_ren["saved_path"]), os.path.realpath(renamed_file))

        # 3. Replace policy: should overwrite base_file
        replace_req = {"target_path": base_file, "conflict_policy": "replace"}
        st_rep, _, body_rep = self.req("POST", f"/api/jobs/{job_id}/save", body=json.dumps(replace_req), headers={"Content-Type": "application/json"})
        self.assertEqual(st_rep, 200)
        data_rep = json.loads(body_rep.decode("utf-8"))
        self.assertEqual(data_rep["status"], "saved")
        with open(base_file, "r", encoding="utf-8") as f:
            self.assertIn("mock_ocr", f.read())

    def test_save_03_empty_subtitle_handling(self) -> None:
        """TC-SAV-03: Empty subtitle protection avoids writing 0-byte files unless allowed."""
        if not self.synth_mp4_path or not os.path.exists(self.synth_mp4_path):
            self.skipTest("Synthesized test video not available")

        # 1. Job without region box / high threshold produces 0 entries
        empty_payload = {"video_path": self.api(self.synth_mp4_path), "engine": "mock", "fps": 5.0}
        st_e, _, body_e = self.req("POST", "/api/jobs", body=json.dumps(empty_payload), headers={"Content-Type": "application/json"})
        job_id_empty = json.loads(body_e.decode("utf-8"))["job_id"]

        for _ in range(60):
            time.sleep(0.1)
            if self.req("GET", f"/api/jobs/{job_id_empty}/export")[0] == 200:
                break

        empty_target_1 = os.path.join(self.temp_dir, "empty_no_allow.srt")
        if os.path.exists(empty_target_1):
            os.remove(empty_target_1)

        # allow_empty: False -> returns status 'empty_result' and file is NOT created
        req_save_1 = {"target_path": empty_target_1, "allow_empty": False}
        st_s1, _, body_s1 = self.req("POST", f"/api/jobs/{job_id_empty}/save", body=json.dumps(req_save_1), headers={"Content-Type": "application/json"})
        self.assertEqual(st_s1, 200)
        data_s1 = json.loads(body_s1.decode("utf-8"))
        self.assertEqual(data_s1["status"], "empty_result")
        self.assertTrue(data_s1["empty_result"])
        self.assertFalse(os.path.exists(empty_target_1))

        # allow_empty: True -> writes empty file
        empty_target_2 = os.path.join(self.temp_dir, "empty_allowed.srt")
        if os.path.exists(empty_target_2):
            os.remove(empty_target_2)
        req_save_2 = {"target_path": empty_target_2, "allow_empty": True}
        st_s2, _, body_s2 = self.req("POST", f"/api/jobs/{job_id_empty}/save", body=json.dumps(req_save_2), headers={"Content-Type": "application/json"})
        self.assertEqual(st_s2, 200)
        data_s2 = json.loads(body_s2.decode("utf-8"))
        self.assertEqual(data_s2["status"], "saved")
        self.assertTrue(os.path.exists(empty_target_2))

    def test_save_04_batch_save_endpoint(self) -> None:
        """TC-SAV-04: POST /api/export/batch-save executes atomic batch save across multiple jobs."""
        if not self.synth_mp4_path or not os.path.exists(self.synth_mp4_path):
            self.skipTest("Synthesized test video not available")

        # Create two jobs with entries
        job_ids = []
        for _ in range(2):
            job_payload = {
                "video_path": self.api(self.synth_mp4_path),
                "engine": "mock",
                "fps": 5.0,
                "confidence_threshold": 0.0,
                "region_box": {"x": 0.0, "y": 0.5, "width": 1.0, "height": 0.5},
            }
            st, _, body = self.req("POST", "/api/jobs", body=json.dumps(job_payload), headers={"Content-Type": "application/json"})
            self.assertEqual(st, 201)
            job_ids.append(json.loads(body.decode("utf-8"))["job_id"])

        for jid in job_ids:
            for _ in range(60):
                time.sleep(0.1)
                if self.req("GET", f"/api/jobs/{jid}/export")[0] == 200:
                    break

        batch_req = {
            "job_ids": job_ids,
            "conflict_policy": "deterministic_rename",
            "allow_empty": False,
        }
        st_b, _, body_b = self.req("POST", "/api/export/batch-save", body=json.dumps(batch_req), headers={"Content-Type": "application/json"})
        self.assertEqual(st_b, 200)
        data_b = json.loads(body_b.decode("utf-8"))
        self.assertEqual(data_b["total"], 2)
        self.assertEqual(data_b["saved"], 2)
        self.assertEqual(data_b["failed"], 0)
        self.assertEqual(len(data_b["results"]), 2)

    def test_save_05_sandbox_security_rejection(self) -> None:
        """TC-SAV-05: POST /api/jobs/:id/save rejects path traversal, cache directory, and bad extensions."""
        if not self.synth_mp4_path or not os.path.exists(self.synth_mp4_path):
            self.skipTest("Synthesized test video not available")

        job_payload = {"video_path": self.api(self.synth_mp4_path), "engine": "mock", "fps": 5.0}
        st, _, body = self.req("POST", "/api/jobs", body=json.dumps(job_payload), headers={"Content-Type": "application/json"})
        job_id = json.loads(body.decode("utf-8"))["job_id"]

        for _ in range(60):
            time.sleep(0.1)
            if self.req("GET", f"/api/jobs/{job_id}/export")[0] == 200:
                break

        # 1. Directory traversal outside workspace
        st1, _, _ = self.req(
            "POST",
            f"/api/jobs/{job_id}/save",
            body=json.dumps({"target_path": os.path.join(self.temp_dir, "../escaped_save.srt")}),
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(st1, 400)

        # 2. Writing to .sublift_cache
        st2, _, _ = self.req(
            "POST",
            f"/api/jobs/{job_id}/save",
            body=json.dumps({"target_path": os.path.join(self.temp_dir, ".sublift_cache", "hack.srt")}),
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(st2, 400)

        # 3. Bad extension (.exe)
        st3, _, _ = self.req(
            "POST",
            f"/api/jobs/{job_id}/save",
            body=json.dumps({"target_path": os.path.join(self.temp_dir, "virus.exe")}),
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(st3, 400)

        nul_path = os.path.join(self.temp_dir, "test.srt") + "\x00.mp4"
        st_nul, _, _ = self.req(
            "POST",
            f"/api/jobs/{job_id}/save",
            body=json.dumps({"target_path": nul_path}),
            headers={"Content-Type": "application/json"},
        )
        self.assertIn(st_nul, (400, 404))

    def test_save_06_concurrent_replace_and_skip(self) -> None:
        """Concurrent save requests stay consistent and leave no leftover temp files."""
        if not self.synth_mp4_path or not os.path.exists(self.synth_mp4_path):
            self.skipTest("Synthesized test video not available")

        job_payload = {
            "video_path": self.api(self.synth_mp4_path),
            "engine": "mock",
            "fps": 5.0,
            "confidence_threshold": 0.0,
            "region_box": {"x": 0.0, "y": 0.5, "width": 1.0, "height": 0.5},
        }
        st, _, body = self.req(
            "POST",
            "/api/jobs",
            body=json.dumps(job_payload),
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(st, 201)
        job_id = json.loads(body.decode("utf-8"))["job_id"]
        for _ in range(60):
            time.sleep(0.1)
            if self.req("GET", f"/api/jobs/{job_id}/export")[0] == 200:
                break

        replace_target = os.path.join(self.temp_dir, "concurrent_replace.srt")

        def _save(policy: str, target: str) -> tuple[int, str]:
            payload = {"target_path": target, "conflict_policy": policy, "allow_empty": False}
            status, _, resp = self.req(
                "POST",
                f"/api/jobs/{job_id}/save",
                body=json.dumps(payload),
                headers={"Content-Type": "application/json"},
            )
            return status, resp.decode("utf-8", errors="replace")

        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(lambda _: _save("replace", replace_target), range(8)))
        self.assertTrue(all(status == 200 for status, _ in results))
        self.assertTrue(all(json.loads(body)["status"] == "saved" for _, body in results))
        self.assertTrue(os.path.exists(replace_target))
        self.assertIn("-->", Path(replace_target).read_text(encoding="utf-8"))
        leftovers = [name for name in os.listdir(self.temp_dir) if ".tmp." in name]
        self.assertEqual(leftovers, [])

        skip_target = os.path.join(self.temp_dir, "concurrent_skip.srt")
        Path(skip_target).write_text("INITIAL PRE-EXISTING CONTENT", encoding="utf-8")
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            skip_results = list(pool.map(lambda _: _save("skip", skip_target), range(8)))
        self.assertTrue(all(status == 200 for status, _ in skip_results))
        self.assertTrue(all(json.loads(body)["status"] == "skipped" for _, body in skip_results))
        skip_text = Path(skip_target).read_text(encoding="utf-8")
        self.assertEqual(skip_text, "INITIAL PRE-EXISTING CONTENT")


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
