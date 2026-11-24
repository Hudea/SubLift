use std::borrow::Cow;
use std::io::SeekFrom;
use std::path::Path;

use http::{header, Method, Response, StatusCode};
use tokio::io::{AsyncReadExt, AsyncSeekExt};

use super::MediaError;

pub const MAX_NATIVE_CHUNK_BYTES: u64 = 2 * 1024 * 1024;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RangeError {
    InvalidSyntax,
    UnsatisfiableOrMulti,
}

fn parse_ascii_digits_u64(s: &str) -> Result<u64, RangeError> {
    if s.is_empty() || !s.bytes().all(|b| b.is_ascii_digit()) {
        return Err(RangeError::InvalidSyntax);
    }
    s.parse::<u64>().map_err(|_| RangeError::InvalidSyntax)
}

pub fn parse_single_byte_range(header_val: &str, total_len: u64) -> Result<(u64, u64), RangeError> {
    if total_len == 0 {
        return Err(RangeError::UnsatisfiableOrMulti);
    }
    let trimmed = header_val.trim();
    let Some(spec) = trimmed
        .strip_prefix("bytes=")
        .or_else(|| trimmed.strip_prefix("BYTES="))
        .or_else(|| {
            if trimmed.len() >= 6 && trimmed[..6].eq_ignore_ascii_case("bytes=") {
                Some(&trimmed[6..])
            } else {
                None
            }
        })
    else {
        return Err(RangeError::InvalidSyntax);
    };

    let spec = spec.trim();
    if spec.is_empty() || spec.contains(',') {
        return Err(RangeError::UnsatisfiableOrMulti);
    }

    let Some((start_str, end_str)) = spec.split_once('-') else {
        return Err(RangeError::InvalidSyntax);
    };
    let start_str = start_str.trim();
    let end_str = end_str.trim();

    if start_str.is_empty() {
        let suffix_len: u64 = parse_ascii_digits_u64(end_str)?;
        if suffix_len == 0 {
            return Err(RangeError::UnsatisfiableOrMulti);
        }
        let start = total_len.saturating_sub(suffix_len);
        return Ok((start, total_len - 1));
    }

    let start: u64 = parse_ascii_digits_u64(start_str)?;
    if start >= total_len {
        return Err(RangeError::UnsatisfiableOrMulti);
    }

    let end = if end_str.is_empty() {
        total_len - 1
    } else {
        let parsed_end: u64 = parse_ascii_digits_u64(end_str)?;
        if parsed_end < start {
            return Err(RangeError::UnsatisfiableOrMulti);
        }
        parsed_end.min(total_len - 1)
    };

    Ok((start, end))
}

pub fn attach_common_headers(
    builder: http::response::Builder,
    mime: &str,
    cache_control: &str,
) -> http::response::Builder {
    builder
        .header(header::CONTENT_TYPE, mime)
        .header(header::ACCEPT_RANGES, "bytes")
        .header(header::ACCESS_CONTROL_ALLOW_ORIGIN, "*")
        .header(
            header::ACCESS_CONTROL_EXPOSE_HEADERS,
            "Content-Range, Accept-Ranges, Content-Length",
        )
        .header(header::CACHE_CONTROL, cache_control)
}

pub fn build_416_response(total_len: u64) -> Response<Cow<'static, [u8]>> {
    Response::builder()
        .status(StatusCode::RANGE_NOT_SATISFIABLE)
        .header(header::CONTENT_RANGE, format!("bytes */{total_len}"))
        .header(header::ACCEPT_RANGES, "bytes")
        .header(header::ACCESS_CONTROL_ALLOW_ORIGIN, "*")
        .body(Cow::Borrowed(&[][..]))
        .expect("valid 416 response")
}

pub fn serve_in_memory_bytes(
    method: &Method,
    range_header: Option<&str>,
    bytes: &[u8],
    mime: &str,
    cache_control: &str,
) -> Response<Cow<'static, [u8]>> {
    let total_len = bytes.len() as u64;
    if *method == Method::HEAD {
        return attach_common_headers(Response::builder().status(StatusCode::OK), mime, cache_control)
            .header(header::CONTENT_LENGTH, total_len.to_string())
            .body(Cow::Borrowed(&[][..]))
            .expect("valid HEAD response");
    }

    if let Some(raw_range) = range_header {
        match parse_single_byte_range(raw_range, total_len) {
            Ok((start, end)) => {
                let clamped_end = end.min(start.saturating_add(MAX_NATIVE_CHUNK_BYTES - 1));
                let slice = bytes[start as usize..=clamped_end as usize].to_vec();
                let len = slice.len();
                return attach_common_headers(
                    Response::builder().status(StatusCode::PARTIAL_CONTENT),
                    mime,
                    cache_control,
                )
                .header(
                    header::CONTENT_RANGE,
                    format!("bytes {start}-{clamped_end}/{total_len}"),
                )
                .header(header::CONTENT_LENGTH, len.to_string())
                .body(Cow::Owned(slice))
                .expect("valid 206 response");
            }
            Err(_) => return build_416_response(total_len),
        }
    }

    if total_len <= MAX_NATIVE_CHUNK_BYTES {
        attach_common_headers(Response::builder().status(StatusCode::OK), mime, cache_control)
            .header(header::CONTENT_LENGTH, total_len.to_string())
            .body(Cow::Owned(bytes.to_vec()))
            .expect("valid 200 response")
    } else {
        let clamped_end = MAX_NATIVE_CHUNK_BYTES - 1;
        let slice = bytes[0..=clamped_end as usize].to_vec();
        let len = slice.len();
        attach_common_headers(
            Response::builder().status(StatusCode::PARTIAL_CONTENT),
            mime,
            cache_control,
        )
        .header(
            header::CONTENT_RANGE,
            format!("bytes 0-{clamped_end}/{total_len}"),
        )
        .header(header::CONTENT_LENGTH, len.to_string())
        .body(Cow::Owned(slice))
        .expect("valid 206 bounded response")
    }
}

pub async fn serve_native_file(
    path: &Path,
    method: &Method,
    range_header: Option<&str>,
    mime: &str,
) -> Result<Response<Cow<'static, [u8]>>, MediaError> {
    let file = tokio::fs::File::open(path)
        .await
        .map_err(|e| MediaError::NotFound(format!("{}: {e}", path.display())))?;
    let meta = file
        .metadata()
        .await
        .map_err(|e| MediaError::Io(e.to_string()))?;
    serve_opened_native_file(file, meta.len(), method, range_header, mime).await
}

async fn serve_opened_native_file(
    mut file: tokio::fs::File,
    total_len: u64,
    method: &Method,
    range_header: Option<&str>,
    mime: &str,
) -> Result<Response<Cow<'static, [u8]>>, MediaError> {
    if *method == Method::HEAD {
        return Ok(
            attach_common_headers(Response::builder().status(StatusCode::OK), mime, "private, max-age=60")
                .header(header::CONTENT_LENGTH, total_len.to_string())
                .body(Cow::Borrowed(&[][..]))
                .expect("valid HEAD response"),
        );
    }

    if let Some(raw_range) = range_header {
        let (start, end) = match parse_single_byte_range(raw_range, total_len) {
            Ok(pair) => pair,
            Err(_) => return Ok(build_416_response(total_len)),
        };
        let clamped_end = end.min(start.saturating_add(MAX_NATIVE_CHUNK_BYTES - 1));
        let chunk_len = clamped_end - start + 1;
        let mut buf = Vec::with_capacity(chunk_len as usize);
        file.seek(SeekFrom::Start(start))
            .await
            .map_err(|e| MediaError::Io(e.to_string()))?;
        file.take(chunk_len)
            .read_to_end(&mut buf)
            .await
            .map_err(|e| MediaError::Io(e.to_string()))?;
        if buf.is_empty() {
            return Ok(build_416_response(total_len));
        }
        let actual_end = start + buf.len() as u64 - 1;
        return Ok(attach_common_headers(
            Response::builder().status(StatusCode::PARTIAL_CONTENT),
            mime,
            "private, max-age=60",
        )
        .header(
            header::CONTENT_RANGE,
            format!("bytes {start}-{actual_end}/{total_len}"),
        )
        .header(header::CONTENT_LENGTH, buf.len().to_string())
        .body(Cow::Owned(buf))
        .expect("valid 206 file response"));
    }

    if total_len <= MAX_NATIVE_CHUNK_BYTES {
        let mut buf = Vec::with_capacity(total_len as usize);
        file.read_to_end(&mut buf)
            .await
            .map_err(|e| MediaError::Io(e.to_string()))?;
        Ok(
            attach_common_headers(Response::builder().status(StatusCode::OK), mime, "private, max-age=60")
                .header(header::CONTENT_LENGTH, buf.len().to_string())
                .body(Cow::Owned(buf))
                .expect("valid 200 file response"),
        )
    } else {
        let chunk_len = MAX_NATIVE_CHUNK_BYTES;
        let mut buf = Vec::with_capacity(chunk_len as usize);
        file.take(chunk_len)
            .read_to_end(&mut buf)
            .await
            .map_err(|e| MediaError::Io(e.to_string()))?;
        if buf.is_empty() {
            return Ok(attach_common_headers(
                Response::builder().status(StatusCode::OK),
                mime,
                "private, max-age=60",
            )
            .header(header::CONTENT_LENGTH, "0")
            .body(Cow::Borrowed(&[][..]))
            .expect("valid 200 empty file response"));
        }
        let actual_end = buf.len() as u64 - 1;
        Ok(attach_common_headers(
            Response::builder().status(StatusCode::PARTIAL_CONTENT),
            mime,
            "private, max-age=60",
        )
        .header(
            header::CONTENT_RANGE,
            format!("bytes 0-{actual_end}/{total_len}"),
        )
        .header(header::CONTENT_LENGTH, buf.len().to_string())
        .body(Cow::Owned(buf))
        .expect("valid 206 bounded file response"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::TempDir;

    #[test]
    fn parses_rfc_9110_single_byte_ranges_and_rejects_invalid() {
        assert_eq!(parse_single_byte_range("bytes=0-1", 100), Ok((0, 1)));
        assert_eq!(parse_single_byte_range("Bytes=10-", 100), Ok((10, 99)));
        assert_eq!(parse_single_byte_range("bytes=-5", 100), Ok((95, 99)));
        assert_eq!(parse_single_byte_range("bytes=-500", 100), Ok((0, 99)));
        assert_eq!(parse_single_byte_range("bytes=50-9999", 100), Ok((50, 99)));

        assert_eq!(
            parse_single_byte_range("bytes=0-1, 4-5", 100),
            Err(RangeError::UnsatisfiableOrMulti)
        );
        assert_eq!(
            parse_single_byte_range("bytes=100-101", 100),
            Err(RangeError::UnsatisfiableOrMulti)
        );
        assert_eq!(
            parse_single_byte_range("bytes=0-0", 0),
            Err(RangeError::UnsatisfiableOrMulti)
        );
        assert_eq!(
            parse_single_byte_range("bytes=-0", 100),
            Err(RangeError::UnsatisfiableOrMulti)
        );
        assert_eq!(
            parse_single_byte_range("bytes=+0-1", 100),
            Err(RangeError::InvalidSyntax)
        );
        assert_eq!(
            parse_single_byte_range("bytes=0-+1", 100),
            Err(RangeError::InvalidSyntax)
        );
        assert_eq!(
            parse_single_byte_range("bytes=-+5", 100),
            Err(RangeError::InvalidSyntax)
        );

        let resp_plus = serve_in_memory_bytes(&Method::GET, Some("bytes=+0-1"), b"abcdef", "video/mp4", "no-store");
        assert_eq!(resp_plus.status(), StatusCode::RANGE_NOT_SATISFIABLE);
    }

    #[tokio::test]
    async fn unranged_large_file_truncated_to_empty_returns_200_without_underflow() {
        let dir = TempDir::new().unwrap();
        let path = dir.path().join("truncated.mp4");
        std::fs::write(&path, b"").unwrap();

        let file = tokio::fs::File::open(&path).await.unwrap();
        let resp = serve_opened_native_file(
            file,
            MAX_NATIVE_CHUNK_BYTES + 1024,
            &Method::GET,
            None,
            "video/mp4",
        )
        .await
        .unwrap();
        assert_eq!(resp.status(), StatusCode::OK);
        assert_eq!(resp.headers().get(header::CONTENT_LENGTH).unwrap(), "0");
        assert!(resp.body().is_empty());
    }

    #[tokio::test]
    async fn native_file_serving_bounds_chunks_to_2_mib_and_handles_416() {
        let dir = TempDir::new().unwrap();
        let path = dir.path().join("big.mp4");
        let size = (MAX_NATIVE_CHUNK_BYTES as usize) + 512 * 1024;
        let payload = vec![0xABu8; size];
        std::fs::write(&path, &payload).unwrap();

        // Unranged GET on >2 MiB file returns bounded 206
        let resp = serve_native_file(&path, &Method::GET, None, "video/mp4")
            .await
            .unwrap();
        assert_eq!(resp.status(), StatusCode::PARTIAL_CONTENT);
        assert_eq!(resp.body().len() as u64, MAX_NATIVE_CHUNK_BYTES);
        assert_eq!(
            resp.headers().get(header::CONTENT_RANGE).unwrap(),
            &format!("bytes 0-{}/{size}", MAX_NATIVE_CHUNK_BYTES - 1)
        );

        // Large requested range is also clamped to 2 MiB
        let resp2 = serve_native_file(&path, &Method::GET, Some("bytes=0-"), "video/mp4")
            .await
            .unwrap();
        assert_eq!(resp2.status(), StatusCode::PARTIAL_CONTENT);
        assert_eq!(resp2.body().len() as u64, MAX_NATIVE_CHUNK_BYTES);

        // Unsatisfiable range returns 416
        let resp416 = serve_native_file(&path, &Method::GET, Some("bytes=99999999-"), "video/mp4")
            .await
            .unwrap();
        assert_eq!(resp416.status(), StatusCode::RANGE_NOT_SATISFIABLE);
        assert_eq!(
            resp416.headers().get(header::CONTENT_RANGE).unwrap(),
            &format!("bytes */{size}")
        );
    }

    #[tokio::test]
    async fn head_empty_file_and_edge_range_syntax() {
        let dir = TempDir::new().unwrap();
        let file_path = dir.path().join("sample.mp4");
        std::fs::write(&file_path, b"0123456789").unwrap();

        // Method::HEAD on native file
        let head_native = serve_native_file(&file_path, &Method::HEAD, None, "video/mp4")
            .await
            .unwrap();
        assert_eq!(head_native.status(), StatusCode::OK);
        assert!(head_native.body().is_empty());
        assert_eq!(
            head_native.headers().get(header::CONTENT_LENGTH).unwrap(),
            "10"
        );
        assert_eq!(
            head_native.headers().get(header::ACCEPT_RANGES).unwrap(),
            "bytes"
        );

        // Method::HEAD on in-memory remux bytes
        let head_mem = serve_in_memory_bytes(&Method::HEAD, None, b"abcdef", "video/mp4", "no-store");
        assert_eq!(head_mem.status(), StatusCode::OK);
        assert!(head_mem.body().is_empty());
        assert_eq!(head_mem.headers().get(header::CONTENT_LENGTH).unwrap(), "6");

        // 0-byte native file unranged GET
        let empty_path = dir.path().join("empty.mp4");
        std::fs::write(&empty_path, b"").unwrap();
        let get_empty = serve_native_file(&empty_path, &Method::GET, None, "video/mp4")
            .await
            .unwrap();
        assert_eq!(get_empty.status(), StatusCode::OK);
        assert!(get_empty.body().is_empty());
        assert_eq!(
            get_empty.headers().get(header::CONTENT_LENGTH).unwrap(),
            "0"
        );

        // Inverted range, empty suffix `bytes=-`, and non-bytes unit -> RangeError + 416
        assert_eq!(
            parse_single_byte_range("bytes=10-5", 100),
            Err(RangeError::UnsatisfiableOrMulti)
        );
        assert_eq!(
            parse_single_byte_range("bytes=-", 100),
            Err(RangeError::InvalidSyntax)
        );
        assert_eq!(
            parse_single_byte_range("items=0-5", 100),
            Err(RangeError::InvalidSyntax)
        );
        for bad_range in ["bytes=10-5", "bytes=-", "items=0-5"] {
            let r = serve_native_file(&file_path, &Method::GET, Some(bad_range), "video/mp4")
                .await
                .unwrap();
            assert_eq!(r.status(), StatusCode::RANGE_NOT_SATISFIABLE);
            assert_eq!(r.headers().get(header::CONTENT_RANGE).unwrap(), "bytes */10");
        }
    }
}
