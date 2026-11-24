use std::f64::consts::PI;
use std::fs::OpenOptions;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

static PPM_TMP_SEQ: AtomicU64 = AtomicU64::new(1);

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RgbImage {
    pub width: u32,
    pub height: u32,
    pub data: Vec<u8>,
}

impl RgbImage {
    pub fn new_filled(width: u32, height: u32, color: [u8; 3]) -> Self {
        let len = (width as usize)
            .checked_mul(height as usize)
            .and_then(|n| n.checked_mul(3))
            .expect("image dimensions overflow");
        let mut data = vec![0u8; len];
        if color != [0, 0, 0] {
            for px in data.chunks_exact_mut(3) {
                px.copy_from_slice(&color);
            }
        }
        Self {
            width,
            height,
            data,
        }
    }

    pub fn from_rgb_bytes(width: u32, height: u32, data: Vec<u8>) -> Result<Self, String> {
        let expected = (width as usize)
            .checked_mul(height as usize)
            .and_then(|n| n.checked_mul(3))
            .ok_or_else(|| format!("image dimensions overflow: {width}x{height}"))?;
        if data.len() != expected {
            return Err(format!(
                "RGB buffer length {} does not match {}x{}x3 ({expected})",
                data.len(),
                width,
                height
            ));
        }
        Ok(Self {
            width,
            height,
            data,
        })
    }

    #[inline]
    pub fn put_pixel_blended(&mut self, x: u32, y: u32, color: [u8; 3], alpha: f32) {
        if x >= self.width || y >= self.height || alpha <= 0.0 {
            return;
        }
        let idx = ((y as usize) * (self.width as usize) + (x as usize)) * 3;
        if alpha >= 1.0 {
            self.data[idx] = color[0];
            self.data[idx + 1] = color[1];
            self.data[idx + 2] = color[2];
            return;
        }
        let inv = 1.0 - alpha;
        self.data[idx] = (self.data[idx] as f32 * inv + color[0] as f32 * alpha)
            .round()
            .clamp(0.0, 255.0) as u8;
        self.data[idx + 1] = (self.data[idx + 1] as f32 * inv + color[1] as f32 * alpha)
            .round()
            .clamp(0.0, 255.0) as u8;
        self.data[idx + 2] = (self.data[idx + 2] as f32 * inv + color[2] as f32 * alpha)
            .round()
            .clamp(0.0, 255.0) as u8;
    }

    pub fn fill_rect(&mut self, x0: u32, y0: u32, x1: u32, y1: u32, color: [u8; 3]) {
        let rx0 = x0.min(self.width);
        let ry0 = y0.min(self.height);
        let rx1 = x1.min(self.width);
        let ry1 = y1.min(self.height);
        if rx0 >= rx1 || ry0 >= ry1 {
            return;
        }
        for y in ry0..ry1 {
            let row_start = ((y as usize) * (self.width as usize) + (rx0 as usize)) * 3;
            let row_end = ((y as usize) * (self.width as usize) + (rx1 as usize)) * 3;
            for px in self.data[row_start..row_end].chunks_exact_mut(3) {
                px.copy_from_slice(&color);
            }
        }
    }

    pub fn blit(&mut self, src: &RgbImage, dst_x: u32, dst_y: u32) {
        if dst_x >= self.width || dst_y >= self.height {
            return;
        }
        let copy_w = src.width.min(self.width - dst_x) as usize;
        let copy_h = src.height.min(self.height - dst_y) as usize;
        let row_bytes = copy_w * 3;
        for row in 0..copy_h {
            let src_start = row * (src.width as usize) * 3;
            let dst_start = (((dst_y as usize) + row) * (self.width as usize) + (dst_x as usize)) * 3;
            self.data[dst_start..dst_start + row_bytes]
                .copy_from_slice(&src.data[src_start..src_start + row_bytes]);
        }
    }
}

pub struct TempFileUnlink {
    path: Option<PathBuf>,
}

impl TempFileUnlink {
    pub fn new(path: PathBuf) -> Self {
        Self { path: Some(path) }
    }

    pub fn disarm(&mut self) {
        self.path = None;
    }
}

impl Drop for TempFileUnlink {
    fn drop(&mut self) {
        if let Some(path) = self.path.take() {
            let _ = std::fs::remove_file(path);
        }
    }
}

pub fn read_ppm_and_unlink(path: &Path) -> Result<RgbImage, String> {
    let _unlink_guard = TempFileUnlink::new(path.to_path_buf());
    let bytes = std::fs::read(path)
        .map_err(|e| format!("failed to read PPM '{}': {e}", path.display()))?;
    parse_ppm_bytes(&bytes)
}

pub fn parse_ppm_bytes(bytes: &[u8]) -> Result<RgbImage, String> {
    let mut pos = 0usize;
    let magic = next_ppm_token(bytes, &mut pos)?;
    let channels = match magic.as_slice() {
        b"P6" => 3usize,
        b"P5" => 1usize,
        other => {
            return Err(format!(
                "unsupported Netpbm magic '{}'; expected P5 or P6",
                String::from_utf8_lossy(other)
            ));
        }
    };

    let width: u32 = parse_ppm_uint(&next_ppm_token(bytes, &mut pos)?, "width")?;
    let height: u32 = parse_ppm_uint(&next_ppm_token(bytes, &mut pos)?, "height")?;
    let maxval: u32 = parse_ppm_uint(&next_ppm_token(bytes, &mut pos)?, "maxval")?;

    if width == 0 || height == 0 {
        return Err(format!("invalid PPM dimensions {width}x{height}"));
    }
    if !(1..=255).contains(&maxval) {
        return Err(format!("unsupported PPM maxval {maxval}; expected 1..=255"));
    }

    // Standard Netpbm binary format: single whitespace byte (or CRLF) separates header from raster payload.
    // Only skip CR+LF as a 2-byte terminator when remaining bytes after CR exceed expected_payload,
    // so a CR delimiter followed by pixel byte 0x0A is preserved.
    if pos >= bytes.len() || !bytes[pos].is_ascii_whitespace() {
        return Err("malformed PPM header: missing separator byte after maxval".into());
    }
    let pixel_count = (width as usize)
        .checked_mul(height as usize)
        .ok_or_else(|| "PPM pixel count overflow".to_string())?;
    let expected_payload = pixel_count
        .checked_mul(channels)
        .ok_or_else(|| "PPM payload size overflow".to_string())?;
    if bytes[pos] == b'\r'
        && bytes.get(pos + 1) == Some(&b'\n')
        && bytes.len().saturating_sub(pos + 1) > expected_payload
    {
        pos += 2;
    } else {
        pos += 1;
    }

    let payload = bytes
        .get(pos..)
        .ok_or_else(|| "PPM payload truncated".to_string())?;
    if payload.len() < expected_payload {
        return Err(format!(
            "PPM payload truncated: got {} bytes, expected at least {expected_payload}",
            payload.len()
        ));
    }
    let payload = &payload[..expected_payload];

    let rescale = |v: u8| -> u8 {
        if maxval == 255 {
            v
        } else {
            (((v as u32) * 255) / maxval).min(255) as u8
        }
    };

    let rgb_len = pixel_count
        .checked_mul(3)
        .ok_or_else(|| "RGB buffer overflow".to_string())?;
    let mut data = Vec::with_capacity(rgb_len);

    if channels == 3 {
        if maxval == 255 {
            data.extend_from_slice(payload);
        } else {
            for &b in payload {
                data.push(rescale(b));
            }
        }
    } else {
        for &g in payload {
            let v = rescale(g);
            data.push(v);
            data.push(v);
            data.push(v);
        }
    }

    Ok(RgbImage {
        width,
        height,
        data,
    })
}

fn next_ppm_token(bytes: &[u8], pos: &mut usize) -> Result<Vec<u8>, String> {
    while *pos < bytes.len() {
        let b = bytes[*pos];
        if b == b'#' {
            while *pos < bytes.len() && bytes[*pos] != b'\n' {
                *pos += 1;
            }
            continue;
        }
        if b.is_ascii_whitespace() {
            *pos += 1;
            continue;
        }
        break;
    }

    if *pos >= bytes.len() {
        return Err("unexpected EOF in PPM header".into());
    }

    let mut tok = Vec::new();
    while *pos < bytes.len() {
        let b = bytes[*pos];
        if b.is_ascii_whitespace() || b == b'#' {
            break;
        }
        tok.push(b);
        *pos += 1;
    }
    if tok.is_empty() {
        return Err("empty token in PPM header".into());
    }
    Ok(tok)
}

fn parse_ppm_uint(tok: &[u8], field: &str) -> Result<u32, String> {
    if tok.is_empty() || !tok.iter().all(u8::is_ascii_digit) {
        let s = String::from_utf8_lossy(tok);
        return Err(format!(
            "invalid PPM {field} '{s}': expected unsigned digits"
        ));
    }
    let s = std::str::from_utf8(tok)
        .map_err(|_| format!("invalid UTF-8 in PPM {field} token"))?;
    s.parse::<u32>()
        .map_err(|e| format!("invalid PPM {field} '{s}': {e}"))
}

pub fn write_ppm_atomic(path: &Path, img: &RgbImage) -> Result<(), String> {
    let parent = path
        .parent()
        .ok_or_else(|| format!("PPM path '{}' has no parent directory", path.display()))?;
    std::fs::create_dir_all(parent)
        .map_err(|e| format!("failed to create PPM dir '{}': {e}", parent.display()))?;

    let fname = path
        .file_name()
        .and_then(|n| n.to_str())
        .ok_or_else(|| format!("invalid PPM filename '{}'", path.display()))?;
    let seq = PPM_TMP_SEQ.fetch_add(1, Ordering::Relaxed);
    let tmp_path = parent.join(format!(".tmp_{}_{seq}_{fname}", std::process::id()));
    let mut cleanup = TempFileUnlink::new(tmp_path.clone());

    let mut opts = OpenOptions::new();
    opts.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        opts.mode(0o600);
    }

    {
        let mut file = opts
            .open(&tmp_path)
            .map_err(|e| format!("failed to create temp PPM '{}': {e}", tmp_path.display()))?;
        let header = format!("P6\n{} {}\n255\n", img.width, img.height);
        file.write_all(header.as_bytes())
            .and_then(|_| file.write_all(&img.data))
            .and_then(|_| file.sync_all())
            .map_err(|e| format!("failed to write temp PPM '{}': {e}", tmp_path.display()))?;
    }

    std::fs::rename(&tmp_path, path)
        .map_err(|e| format!("failed to rename temp PPM to '{}': {e}", path.display()))?;
    cleanup.disarm();
    Ok(())
}

pub fn crop_horizontal(src: &RgbImage, x: u32, w: u32) -> Result<RgbImage, String> {
    if src.width == 0 || src.height == 0 {
        return Err("cannot crop empty image".into());
    }
    let sx = x.min(src.width.saturating_sub(1));
    let sw = w.max(1).min(src.width - sx);
    if sx == 0 && sw == src.width {
        return Ok(src.clone());
    }
    let mut out = Vec::with_capacity((sw as usize) * (src.height as usize) * 3);
    let src_stride = (src.width as usize) * 3;
    let x_off = (sx as usize) * 3;
    let copy_bytes = (sw as usize) * 3;
    for row in 0..(src.height as usize) {
        let start = row * src_stride + x_off;
        out.extend_from_slice(&src.data[start..start + copy_bytes]);
    }
    Ok(RgbImage {
        width: sw,
        height: src.height,
        data: out,
    })
}

#[inline]
fn lanczos3_weight(x: f64) -> f64 {
    let ax = x.abs();
    if ax < 1e-12 {
        return 1.0;
    }
    if ax >= 3.0 {
        return 0.0;
    }
    let px = PI * ax;
    let px3 = px / 3.0;
    (px.sin() / px) * (px3.sin() / px3)
}

struct ResampleContrib {
    start: usize,
    weights: Vec<f64>,
}

fn build_resample_table(src_len: u32, dst_len: u32) -> Vec<ResampleContrib> {
    let src_f = src_len as f64;
    let dst_f = dst_len as f64;
    let scale = src_f / dst_f;
    let filter_scale = if scale > 1.0 { scale } else { 1.0 };
    let support = 3.0 * filter_scale;

    let mut table = Vec::with_capacity(dst_len as usize);
    for dst_i in 0..dst_len {
        let center = (dst_i as f64 + 0.5) * scale - 0.5;
        let left = (center - support).ceil().max(0.0) as isize;
        let right = (center + support).floor().min(src_f - 1.0) as isize;

        let mut weights = Vec::new();
        let mut sum = 0.0;
        let start = if left <= right {
            for s in left..=right {
                let w = lanczos3_weight((s as f64 - center) / filter_scale);
                weights.push(w);
                sum += w;
            }
            left as usize
        } else {
            let clamped = center.round().clamp(0.0, src_f - 1.0) as usize;
            weights.push(1.0);
            sum = 1.0;
            clamped
        };

        if sum.abs() > 1e-12 {
            for w in &mut weights {
                *w /= sum;
            }
        }
        table.push(ResampleContrib { start, weights });
    }
    table
}

pub fn resize_lanczos3(src: &RgbImage, dst_w: u32, dst_h: u32) -> RgbImage {
    let dst_w = dst_w.max(1);
    let dst_h = dst_h.max(1);
    if src.width == dst_w && src.height == dst_h {
        return src.clone();
    }

    let horiz_table = build_resample_table(src.width, dst_w);
    let mut temp = vec![0.0f64; (dst_w as usize) * (src.height as usize) * 3];
    let src_stride = (src.width as usize) * 3;
    let temp_stride = (dst_w as usize) * 3;

    for y in 0..(src.height as usize) {
        let src_row = &src.data[y * src_stride..(y + 1) * src_stride];
        let temp_row = &mut temp[y * temp_stride..(y + 1) * temp_stride];
        for (dx, contrib) in horiz_table.iter().enumerate() {
            let mut r = 0.0;
            let mut g = 0.0;
            let mut b = 0.0;
            for (k, &w) in contrib.weights.iter().enumerate() {
                let sx = (contrib.start + k) * 3;
                r += (src_row[sx] as f64) * w;
                g += (src_row[sx + 1] as f64) * w;
                b += (src_row[sx + 2] as f64) * w;
            }
            let out_off = dx * 3;
            temp_row[out_off] = r;
            temp_row[out_off + 1] = g;
            temp_row[out_off + 2] = b;
        }
    }

    let vert_table = build_resample_table(src.height, dst_h);
    let mut out = vec![0u8; (dst_w as usize) * (dst_h as usize) * 3];

    for (dy, contrib) in vert_table.iter().enumerate() {
        let out_row = &mut out[dy * temp_stride..(dy + 1) * temp_stride];
        for x in 0..(dst_w as usize) {
            let mut r = 0.0;
            let mut g = 0.0;
            let mut b = 0.0;
            let x_off = x * 3;
            for (k, &w) in contrib.weights.iter().enumerate() {
                let t_off = (contrib.start + k) * temp_stride + x_off;
                r += temp[t_off] * w;
                g += temp[t_off + 1] * w;
                b += temp[t_off + 2] * w;
            }
            out_row[x_off] = r.round().clamp(0.0, 255.0) as u8;
            out_row[x_off + 1] = g.round().clamp(0.0, 255.0) as u8;
            out_row[x_off + 2] = b.round().clamp(0.0, 255.0) as u8;
        }
    }

    RgbImage {
        width: dst_w,
        height: dst_h,
        data: out,
    }
}

pub fn fit_strip_with_black_pad(src: &RgbImage, strip_w: u32, strip_h: u32) -> RgbImage {
    let strip_w = strip_w.max(1);
    let strip_h = strip_h.max(1);
    let sw = src.width.max(1) as f64;
    let sh = src.height.max(1) as f64;
    let scale = (strip_w as f64 / sw).min(strip_h as f64 / sh);

    let new_w = ((sw * scale).round_ties_even() as i64).clamp(1, strip_w as i64) as u32;
    let new_h = ((sh * scale).round_ties_even() as i64).clamp(1, strip_h as i64) as u32;

    let resized = if src.width != new_w || src.height != new_h {
        resize_lanczos3(src, new_w, new_h)
    } else {
        src.clone()
    };

    if new_w == strip_w && new_h == strip_h {
        return resized;
    }

    let mut canvas = RgbImage::new_filled(strip_w, strip_h, [0, 0, 0]);
    let dx = (strip_w - new_w) / 2;
    let dy = (strip_h - new_h) / 2;
    canvas.blit(&resized, dx, dy);
    canvas
}

pub fn encode_png_fast(img: &RgbImage) -> Result<Vec<u8>, String> {
    let mut out = Vec::with_capacity((img.width as usize) * (img.height as usize));
    {
        let mut encoder = png::Encoder::new(&mut out, img.width, img.height);
        encoder.set_color(png::ColorType::Rgb);
        encoder.set_depth(png::BitDepth::Eight);
        encoder.set_compression(png::Compression::Fast);
        let mut writer = encoder
            .write_header()
            .map_err(|e| format!("PNG header write failed: {e}"))?;
        writer
            .write_image_data(&img.data)
            .map_err(|e| format!("PNG image write failed: {e}"))?;
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::TempDir;

    #[test]
    fn read_ppm_and_unlink_handles_p6_p5_comments_and_corrupt_cleanup() {
        let dir = TempDir::new().unwrap();

        // 1. P6 with comments and maxval=255
        let p6_path = dir.path().join("sample_p6.ppm");
        let mut p6 = b"P6\n# leading comment\n2 1 # inline comment\n255\n".to_vec();
        p6.extend_from_slice(&[10, 20, 30, 200, 210, 220]);
        std::fs::write(&p6_path, &p6).unwrap();

        let img6 = read_ppm_and_unlink(&p6_path).unwrap();
        assert!(!p6_path.exists(), "PPM must be unlinked after decode");
        assert_eq!(img6.width, 2);
        assert_eq!(img6.height, 1);
        assert_eq!(img6.data, vec![10, 20, 30, 200, 210, 220]);

        // 2. P5 Gray8 rescaled from maxval=15
        let p5_path = dir.path().join("sample_p5.pgm");
        let mut p5 = b"P5\n2 1\n15\n".to_vec();
        p5.extend_from_slice(&[0, 15]);
        std::fs::write(&p5_path, &p5).unwrap();

        let img5 = read_ppm_and_unlink(&p5_path).unwrap();
        assert!(!p5_path.exists());
        assert_eq!(img5.data, vec![0, 0, 0, 255, 255, 255]);

        // 3. Corrupt PPM still unlinks file immediately
        let bad_path = dir.path().join("corrupt.ppm");
        std::fs::write(&bad_path, b"P6\n10 10\n255\nshort").unwrap();
        assert!(read_ppm_and_unlink(&bad_path).is_err());
        assert!(!bad_path.exists(), "corrupt PPM must still be unlinked");
    }

    #[test]
    fn write_ppm_atomic_round_trips_and_leaves_no_tmp() {
        let dir = TempDir::new().unwrap();
        let target = dir.path().join("leaf_0001.ppm");
        let img = RgbImage::from_rgb_bytes(2, 2, vec![1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]).unwrap();
        write_ppm_atomic(&target, &img).unwrap();
        assert!(target.is_file());
        let decoded = read_ppm_and_unlink(&target).unwrap();
        assert_eq!(decoded, img);
        assert!(std::fs::read_dir(dir.path()).unwrap().count() == 0);
    }

    #[test]
    fn fit_strip_with_black_pad_preserves_aspect_ratio_and_centers() {
        // 1600x100 fitted to 960x180 -> scale=0.6 -> 960x60 centered vertically at dy=60
        let white = RgbImage::new_filled(1600, 100, [255, 255, 255]);
        let fitted = fit_strip_with_black_pad(&white, 960, 180);
        assert_eq!(fitted.width, 960);
        assert_eq!(fitted.height, 180);
        // Top padding row (y=0) is black (0,0,0)
        assert_eq!(&fitted.data[0..3], &[0, 0, 0]);
        // Center row (y=90, x=480) is white (255,255,255)
        let center_idx = (90 * 960 + 480) * 3;
        assert_eq!(&fitted.data[center_idx..center_idx + 3], &[255, 255, 255]);
        // Bottom padding row (y=179) is black (0,0,0)
        let bot_idx = (179 * 960) * 3;
        assert_eq!(&fitted.data[bot_idx..bot_idx + 3], &[0, 0, 0]);
    }

    #[test]
    fn encode_png_fast_produces_valid_png() {
        let img = RgbImage::new_filled(32, 16, [56, 189, 248]);
        let bytes = encode_png_fast(&img).unwrap();
        assert!(bytes.starts_with(b"\x89PNG\r\n\x1a\n"));
        let decoder = png::Decoder::new(std::io::Cursor::new(&bytes));
        let reader = decoder.read_info().unwrap();
        assert_eq!(reader.info().width, 32);
        assert_eq!(reader.info().height, 16);
    }

    #[test]
    fn parse_ppm_bytes_handles_crlf_and_rejects_malformed_headers() {
        let mut crlf = b"P6\n2 1\n255\r\n".to_vec();
        crlf.extend_from_slice(&[10, 20, 30, 40, 50, 60]);
        let img = parse_ppm_bytes(&crlf).unwrap();
        assert_eq!(img.width, 2);
        assert_eq!(img.height, 1);
        assert_eq!(img.data, vec![10, 20, 30, 40, 50, 60]);

        // Single CR delimiter followed by pixel byte 0x0A must NOT be misparsed as CRLF
        let mut cr_pixel_0a = b"P6\n2 1\n255\r".to_vec();
        cr_pixel_0a.extend_from_slice(&[10, 20, 30, 40, 50, 60]);
        let img_cr = parse_ppm_bytes(&cr_pixel_0a).unwrap();
        assert_eq!(img_cr.width, 2);
        assert_eq!(img_cr.height, 1);
        assert_eq!(img_cr.data, vec![10, 20, 30, 40, 50, 60]);

        let mut comment_after_max = b"P6\n2 1\n255#c\n".to_vec();
        comment_after_max.extend_from_slice(&[10, 20, 30, 40, 50, 60]);
        assert!(parse_ppm_bytes(&comment_after_max).is_err());

        let mut partial_int = b"P6\n2abc 1\n255\n".to_vec();
        partial_int.extend_from_slice(&[10, 20, 30, 40, 50, 60]);
        assert!(parse_ppm_bytes(&partial_int).is_err());

        let mut plus_int = b"P6\n+2 1\n255\n".to_vec();
        plus_int.extend_from_slice(&[10, 20, 30, 40, 50, 60]);
        assert!(parse_ppm_bytes(&plus_int).is_err());

        assert!(parse_ppm_bytes(b"P6\n100000 100000\n255\nshort").is_err());
        assert!(parse_ppm_bytes(b"P6\n0 10\n255\n").is_err());
        assert!(parse_ppm_bytes(b"P6\n2 1\n256\n\x01\x02\x03\x04\x05\x06").is_err());
        assert!(parse_ppm_bytes(b"P6\n2 1\n0\n\x01\x02\x03\x04\x05\x06").is_err());
    }
}
