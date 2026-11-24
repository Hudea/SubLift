use std::collections::HashMap;
use std::path::Path;
use std::sync::Arc;
use std::time::Instant;

use crate::dto::SourceBox;

use super::font::draw_text_ascii;
use super::image::{
    crop_horizontal, encode_png_fast, fit_strip_with_black_pad, RgbImage,
};

pub const DEFAULT_SLOTS_PER_SHEET: usize = 10;
pub const DEFAULT_STRIP_WIDTH: u32 = 960;
pub const DEFAULT_MIN_STRIP_HEIGHT: u32 = 90;
pub const DEFAULT_MAX_STRIP_HEIGHT: u32 = 180;
pub const DEFAULT_HEADER_TAG: &str = "SubLift VLM OCR";

const PAD_X: u32 = 8;
const LABEL_W: u32 = 180;
const SEP_W: u32 = 4;
const ROW_GAP: u32 = 6;
const HEADER_H: u32 = 44;
const BOTTOM_PAD: u32 = 8;

const BG_COLOR: [u8; 3] = [18, 20, 24];
const HEADER_BG_COLOR: [u8; 3] = [28, 32, 40];
const HEADER_TEXT_COLOR: [u8; 3] = [235, 240, 248];
const LABEL_EVEN_BG: [u8; 3] = [245, 247, 250];
const LABEL_ODD_BG: [u8; 3] = [228, 234, 242];
const SEP_COLOR: [u8; 3] = [56, 189, 248];
const ID_TEXT_COLOR: [u8; 3] = [15, 23, 42];
const TIME_TEXT_COLOR: [u8; 3] = [100, 116, 139];

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TimingSlot {
    pub slot_id: u64,
    pub start_ms: i64,
    pub end_ms: i64,
    pub mid_ms: i64,
}

impl TimingSlot {
    pub fn new(slot_id: u64, start_ms: i64, end_ms: i64) -> Self {
        let mid_ms = start_ms + (end_ms - start_ms) / 2;
        Self {
            slot_id,
            start_ms,
            end_ms,
            mid_ms,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SheetSpec {
    pub sheet_index: u32,
    pub sheet_name: String,
    pub sheet_path: String,
    pub expected_slot_ids: Vec<u64>,
}

#[derive(Clone, Debug)]
pub struct PreparedSheet {
    pub spec: SheetSpec,
    pub png_bytes: Arc<Vec<u8>>,
    pub batch_slots: Vec<TimingSlot>,
    pub strip_images: Arc<HashMap<u64, Arc<RgbImage>>>,
    pub decode_s: f64,
    pub encode_s: f64,
}

pub fn resolve_default_bottom_region(
    video_w: i32,
    video_h: i32,
    region_bottom_ratio: f64,
) -> SourceBox {
    let ratio = if region_bottom_ratio.is_finite() {
        region_bottom_ratio.clamp(0.05, 1.0)
    } else {
        0.30
    };
    let raw_h = ((video_h as f64) * ratio).round_ties_even() as i32;
    let even_h = (raw_h / 2) * 2;
    let crop_h = even_h.max(2).clamp(1, video_h.max(1));
    let crop_y = (video_h - crop_h).max(0);
    SourceBox {
        x: 0,
        y: crop_y,
        width: video_w.max(1),
        height: crop_h,
    }
}

pub fn compute_strip_crop_geometry(
    video_w: i32,
    video_h: i32,
    region: SourceBox,
    horiz_margin_ratio: f64,
    vert_pad_px: i32,
) -> Result<SourceBox, String> {
    if video_w <= 0 || video_h <= 0 {
        return Err(format!("invalid video dimensions: {video_w}x{video_h}"));
    }
    if region.x < 0
        || region.y < 0
        || region.width <= 0
        || region.height <= 0
        || region.x >= video_w
        || region.y >= video_h
    {
        return Err(format!(
            "region {:?} out of bounds for video dimensions {video_w}x{video_h}",
            region
        ));
    }

    let (crop_x, crop_w) = if region.x == 0 && region.width >= video_w {
        let cx = ((video_w as f64) * horiz_margin_ratio)
            .round_ties_even()
            .max(0.0) as i32;
        let cw = (((video_w as f64) * (1.0 - 2.0 * horiz_margin_ratio)).round_ties_even() as i32)
            .min(video_w - cx)
            .max(1);
        (cx, cw)
    } else {
        let cx = region.x.max(0);
        let cw = region.width.min(video_w - cx).max(1);
        (cx, cw)
    };

    let mut crop_y = (region.y - vert_pad_px).max(0);
    let crop_y2 =
        ((region.y as i64) + (region.height as i64) + (vert_pad_px as i64)).min(video_h as i64)
            as i32;
    let min_h = 16.min(video_h);
    let mut crop_h = (crop_y2 - crop_y).max(min_h);
    if crop_y + crop_h > video_h {
        crop_y = (video_h - crop_h).max(0);
        crop_h = crop_h.min(video_h - crop_y);
    }

    Ok(SourceBox {
        x: crop_x,
        y: crop_y,
        width: crop_w,
        height: crop_h,
    })
}

pub fn compute_target_strip_height(
    crop_box: SourceBox,
    strip_w: u32,
    min_h: u32,
    max_h: u32,
) -> u32 {
    let raw_h = ((strip_w as f64) * (crop_box.height as f64) / (crop_box.width.max(1) as f64))
        .round_ties_even() as i32;
    raw_h.clamp(min_h as i32, max_h as i32) as u32
}

pub fn apply_full_width_horizontal_trim(
    img: RgbImage,
    horiz_margin_ratio: f64,
) -> Result<RgbImage, String> {
    let iw = img.width;
    let tx = ((iw as f64) * horiz_margin_ratio)
        .round_ties_even()
        .max(0.0) as u32;
    let tw = (((iw as f64) * (1.0 - 2.0 * horiz_margin_ratio)).round_ties_even() as i32)
        .min((iw as i32) - (tx as i32))
        .max(1) as u32;
    if tw < iw {
        crop_horizontal(&img, tx, tw)
    } else {
        Ok(img)
    }
}

pub fn compose_contact_sheet_image(
    batch_slots: &[TimingSlot],
    strip_images_by_id: &HashMap<u64, Arc<RgbImage>>,
    sheet_index: u32,
    strip_w: u32,
    strip_h: u32,
    header_tag: &str,
) -> Result<RgbImage, String> {
    if batch_slots.is_empty() {
        return Err("cannot render an empty Contact Sheet".into());
    }

    let canvas_w = PAD_X + LABEL_W + SEP_W + strip_w + PAD_X;
    let canvas_h = HEADER_H + (batch_slots.len() as u32) * (strip_h + ROW_GAP) + BOTTOM_PAD;
    let mut canvas = RgbImage::new_filled(canvas_w, canvas_h, BG_COLOR);

    canvas.fill_rect(0, 0, canvas_w, HEADER_H, HEADER_BG_COLOR);

    let first_id = batch_slots[0].slot_id;
    let last_id = batch_slots[batch_slots.len() - 1].slot_id;
    let header_text = format!(
        "{header_tag} #{sheet_index:02} | Slots #{first_id:03}..#{last_id:03}"
    );
    draw_text_ascii(&mut canvas, 14, 12, &header_text, 16, HEADER_TEXT_COLOR);

    let sep_x0 = PAD_X + LABEL_W;
    let strip_x0 = sep_x0 + SEP_W;

    for (r, slot) in batch_slots.iter().enumerate() {
        let y0 = HEADER_H + (r as u32) * (strip_h + ROW_GAP);
        let label_bg = if r % 2 == 0 {
            LABEL_EVEN_BG
        } else {
            LABEL_ODD_BG
        };
        canvas.fill_rect(PAD_X, y0, sep_x0, y0 + strip_h, label_bg);
        canvas.fill_rect(sep_x0, y0, strip_x0, y0 + strip_h, SEP_COLOR);

        let id_text = format!("#{:03}", slot.slot_id);
        draw_text_ascii(&mut canvas, PAD_X + 10, y0 + 16, &id_text, 22, ID_TEXT_COLOR);

        let time_text = format!(
            "{:.2}s - {:.2}s",
            slot.start_ms as f64 / 1000.0,
            slot.end_ms as f64 / 1000.0
        );
        draw_text_ascii(&mut canvas, PAD_X + 10, y0 + 52, &time_text, 13, TIME_TEXT_COLOR);

        let raw_strip = strip_images_by_id
            .get(&slot.slot_id)
            .ok_or_else(|| format!("missing strip image for slot #{}", slot.slot_id))?;
        if raw_strip.width == strip_w && raw_strip.height == strip_h {
            canvas.blit(raw_strip, strip_x0, y0);
        } else {
            let fitted = fit_strip_with_black_pad(raw_strip, strip_w, strip_h);
            canvas.blit(&fitted, strip_x0, y0);
        }
    }

    Ok(canvas)
}

pub fn build_prepared_sheet(
    sheet_index: u32,
    batch_slots: Vec<TimingSlot>,
    strip_images: HashMap<u64, Arc<RgbImage>>,
    strip_w: u32,
    strip_h: u32,
    decode_s: f64,
) -> Result<PreparedSheet, String> {
    let t_enc = Instant::now();
    let canvas = compose_contact_sheet_image(
        &batch_slots,
        &strip_images,
        sheet_index,
        strip_w,
        strip_h,
        DEFAULT_HEADER_TAG,
    )?;
    let png_bytes = encode_png_fast(&canvas)?;
    let encode_s = t_enc.elapsed().as_secs_f64();

    let sheet_name = format!("sheet_{sheet_index:02}.png");
    let expected_slot_ids = batch_slots.iter().map(|s| s.slot_id).collect();
    let spec = SheetSpec {
        sheet_index,
        sheet_name: sheet_name.clone(),
        sheet_path: format!("memory://{sheet_name}"),
        expected_slot_ids,
    };

    Ok(PreparedSheet {
        spec,
        png_bytes: Arc::new(png_bytes),
        batch_slots,
        strip_images: Arc::new(strip_images),
        decode_s,
        encode_s,
    })
}

pub fn render_subsheet(
    parent: &SheetSpec,
    sub_slots: &[TimingSlot],
    strip_images: &HashMap<u64, Arc<RgbImage>>,
    strip_w: u32,
    strip_h: u32,
    depth: usize,
    part_idx: usize,
) -> Result<(SheetSpec, Arc<Vec<u8>>), String> {
    let stem = Path::new(&parent.sheet_name)
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("sheet");
    let sub_name = format!("{stem}_d{}_p{part_idx}.png", depth + 1);
    let canvas = compose_contact_sheet_image(
        sub_slots,
        strip_images,
        parent.sheet_index,
        strip_w,
        strip_h,
        DEFAULT_HEADER_TAG,
    )?;
    let png_bytes = Arc::new(encode_png_fast(&canvas)?);
    let spec = SheetSpec {
        sheet_index: parent.sheet_index,
        sheet_name: sub_name.clone(),
        sheet_path: format!("memory://{sub_name}"),
        expected_slot_ids: sub_slots.iter().map(|s| s.slot_id).collect(),
    };
    Ok((spec, png_bytes))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn compute_strip_crop_geometry_matches_python_parity() {
        let full = compute_strip_crop_geometry(
            1920,
            1080,
            SourceBox {
                x: 0,
                y: 778,
                width: 1920,
                height: 121,
            },
            0.10,
            12,
        )
        .unwrap();
        assert_eq!(
            full,
            SourceBox {
                x: 192,
                y: 766,
                width: 1536,
                height: 145
            }
        );
        assert_eq!(compute_target_strip_height(full, 960, 90, 180), 91);

        let near_bottom = compute_strip_crop_geometry(
            1920,
            1080,
            SourceBox {
                x: 200,
                y: 1000,
                width: 800,
                height: 75,
            },
            0.10,
            12,
        )
        .unwrap();
        assert_eq!(
            near_bottom,
            SourceBox {
                x: 200,
                y: 988,
                width: 800,
                height: 92
            }
        );

        // Odd width rounding uses round_ties_even
        let odd_full = compute_strip_crop_geometry(
            105,
            100,
            SourceBox {
                x: 0,
                y: 50,
                width: 105,
                height: 20,
            },
            0.10,
            12,
        )
        .unwrap();
        // 105 * 0.10 = 10.5 -> round_ties_even = 10; 105 * 0.80 = 84.0 -> 84
        assert_eq!(odd_full.x, 10);
        assert_eq!(odd_full.width, 84);
    }

    #[test]
    fn compute_strip_crop_geometry_tiny_video_and_oob_bounds() {
        // Tiny video < 16px height clamps safely
        let tiny = compute_strip_crop_geometry(
            100,
            10,
            SourceBox {
                x: 10,
                y: 2,
                width: 50,
                height: 4,
            },
            0.10,
            12,
        )
        .unwrap();
        assert_eq!(tiny.height, 10);
        assert!(tiny.y + tiny.height <= 10);

        // Out-of-bounds region fails closed
        assert!(compute_strip_crop_geometry(
            640,
            360,
            SourceBox {
                x: 640,
                y: 0,
                width: 10,
                height: 10
            },
            0.10,
            12
        )
        .is_err());

        // Height clamp bounds
        assert_eq!(
            compute_target_strip_height(
                SourceBox {
                    x: 0,
                    y: 0,
                    width: 1536,
                    height: 60
                },
                960,
                90,
                180
            ),
            90
        );
        assert_eq!(
            compute_target_strip_height(
                SourceBox {
                    x: 0,
                    y: 0,
                    width: 500,
                    height: 300
                },
                960,
                90,
                180
            ),
            180
        );
    }

    #[test]
    fn compose_contact_sheet_and_subsheet_render_exact_pixels() {
        let slots = vec![
            TimingSlot::new(1, 500, 1500),
            TimingSlot::new(2, 1600, 2900),
        ];
        let mut strips = HashMap::new();
        strips.insert(1, Arc::new(RgbImage::new_filled(960, 90, [20, 40, 60])));
        strips.insert(2, Arc::new(RgbImage::new_filled(960, 90, [80, 100, 120])));

        let prepared = build_prepared_sheet(1, slots.clone(), strips.clone(), 960, 90, 0.01).unwrap();
        assert_eq!(prepared.spec.sheet_name, "sheet_01.png");
        assert_eq!(prepared.spec.expected_slot_ids, vec![1, 2]);

        // Decode PNG back and check layout geometry and colors
        let decoder = png::Decoder::new(std::io::Cursor::new(&*prepared.png_bytes));
        let mut reader = decoder.read_info().unwrap();
        let mut buf = vec![0; reader.output_buffer_size().unwrap()];
        let info = reader.next_frame(&mut buf).unwrap();
        assert_eq!(info.width, 1160);
        assert_eq!(info.height, 44 + 2 * (90 + 6) + 8);

        let pixel = |x: u32, y: u32| -> [u8; 3] {
            let idx = ((y * info.width + x) * 3) as usize;
            [buf[idx], buf[idx + 1], buf[idx + 2]]
        };
        assert_eq!(pixel(0, 0), [28, 32, 40]); // Header bg
        assert_eq!(pixel(0, 44), [18, 20, 24]); // Canvas bg
        assert_eq!(pixel(8, 44), [245, 247, 250]); // Row 0 label bg
        assert_eq!(pixel(8, 44 + 90 + 6), [228, 234, 242]); // Row 1 label bg
        assert_eq!(pixel(188, 44), [56, 189, 248]); // Cyan separator
        assert_eq!(pixel(192, 44), [20, 40, 60]); // Row 0 strip
        assert_eq!(pixel(192, 44 + 90 + 6), [80, 100, 120]); // Row 1 strip

        // Sub-sheet split rendering from RAM keeps parent sheet_index
        let (sub_spec, sub_png) =
            render_subsheet(&prepared.spec, &slots[..1], &strips, 960, 90, 0, 1).unwrap();
        assert_eq!(sub_spec.sheet_name, "sheet_01_d1_p1.png");
        assert_eq!(sub_spec.sheet_index, 1);
        assert_eq!(sub_spec.expected_slot_ids, vec![1]);
        assert!(sub_png.starts_with(b"\x89PNG\r\n\x1a\n"));
    }
}
