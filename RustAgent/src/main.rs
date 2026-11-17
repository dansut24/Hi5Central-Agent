mod color;
#[cfg(windows)]
mod gdi;
#[cfg(feature = "vpx")]
mod vpx;

use std::{
    env, fs, thread,
    time::{Duration, Instant},
};
#[cfg(feature = "vpx")]
use vpx::SoftwareEncoder;

#[derive(Clone, Copy, PartialEq, Eq)]
enum Codec {
    None,
    Vp8,
    Vp9,
}
impl Codec {
    fn parse(s: &str) -> Result<Self, String> {
        match s.to_ascii_lowercase().as_str() {
            "none" => Ok(Self::None),
            "vp8" => Ok(Self::Vp8),
            "vp9" => Ok(Self::Vp9),
            _ => Err(format!("Unsupported codec: {s}")),
        }
    }
    fn name(self) -> &'static str {
        match self {
            Self::None => "none",
            Self::Vp8 => "vp8",
            Self::Vp9 => "vp9",
        }
    }
}

struct Options {
    source: String,
    codec: Codec,
    width: usize,
    height: usize,
    fps: u32,
    seconds: u64,
    bitrate_kbps: u32,
    json: Option<String>,
}
impl Options {
    fn from_args() -> Result<Self, String> {
        let mut o = Self {
            source: "synthetic".into(),
            codec: Codec::None,
            width: 1280,
            height: 720,
            fps: 60,
            seconds: 10,
            bitrate_kbps: 8000,
            json: None,
        };
        let mut args = env::args().skip(1);
        let command = args.next().unwrap_or_else(|| "help".into());
        if command != "bench" {
            return Err("Usage: hi5-rust-agent-lab bench [--source synthetic|gdi] [--codec none|vp8|vp9] [--width 1280] [--height 720] [--fps 15|30|45|60] [--seconds 10] [--bitrate-kbps 8000] [--json results.json]".into());
        }
        while let Some(flag) = args.next() {
            let val = args
                .next()
                .ok_or_else(|| format!("Missing value for {flag}"))?;
            match flag.as_str() {
                "--source" => o.source = val,
                "--codec" => o.codec = Codec::parse(&val)?,
                "--width" => o.width = val.parse().map_err(|_| "Bad width")?,
                "--height" => o.height = val.parse().map_err(|_| "Bad height")?,
                "--fps" => o.fps = val.parse().map_err(|_| "Bad fps")?,
                "--seconds" => o.seconds = val.parse().map_err(|_| "Bad seconds")?,
                "--bitrate-kbps" => o.bitrate_kbps = val.parse().map_err(|_| "Bad bitrate")?,
                "--json" => o.json = Some(val),
                _ => return Err(format!("Unknown flag: {flag}")),
            }
        }
        if !["synthetic", "gdi"].contains(&o.source.as_str()) {
            return Err("Bad source".into());
        }
        if ![15, 30, 45, 60].contains(&o.fps) {
            return Err("FPS must be 15,30,45,60".into());
        }
        if o.source == "synthetic"
            && (o.width == 0
                || o.height == 0
                || o.width % 2 != 0
                || o.height % 2 != 0
                || o.width > 3840
                || o.height > 2160)
        {
            return Err("Synthetic width/height must be even and <=3840x2160".into());
        }
        if o.seconds == 0 || o.seconds > 120 {
            return Err("Seconds must be 1..120".into());
        }
        if !(100..=100000).contains(&o.bitrate_kbps) {
            return Err("Bad bitrate".into());
        }
        #[cfg(not(feature = "vpx"))]
        if o.codec != Codec::None {
            return Err(
                "Codec requires cargo build --release --features vpx, with libvpx installed".into(),
            );
        }
        #[cfg(not(windows))]
        if o.source == "gdi" {
            return Err("GDI capture requires interactive Windows".into());
        }
        Ok(o)
    }
}

fn make_synthetic(pixels: &mut [u8], width: usize, height: usize, tick: u64) {
    // Moving content across the entire frame makes this a repeatable motion workload,
    // not a static all-black encode that would misleadingly inflate codec FPS.
    let t = tick as usize;
    for y in 0..height {
        let offset = y * width * 4;
        for x in 0..width {
            let i = offset + x * 4;
            pixels[i] = ((x + t * 7) & 255) as u8;
            pixels[i + 1] = ((y + t * 3) & 255) as u8;
            pixels[i + 2] = (((x / 8 ^ y / 8) + t * 11) & 255) as u8;
            pixels[i + 3] = 255;
        }
    }
}

#[derive(Default)]
struct Timings(Vec<f64>);
impl Timings {
    fn record(&mut self, duration: Duration) {
        self.0.push(duration.as_secs_f64() * 1000.0);
    }
    fn pct(&self, percentile: f64) -> f64 {
        if self.0.is_empty() {
            return 0.0;
        }
        let mut data = self.0.clone();
        data.sort_by(|a, b| a.total_cmp(b));
        let idx = ((data.len() - 1) as f64 * percentile).ceil() as usize;
        data[idx]
    }
}
#[derive(Default)]
struct Stats {
    capture: Timings,
    color: Timings,
    encode: Timings,
    loop_ms: Timings,
    captured: u64,
    encoded: u64,
    packets: u64,
    bytes: u64,
    dropped: u64,
    slow: u64,
}

fn run(o: Options) -> Result<(), String> {
    #[cfg(windows)]
    let mut gdi = if o.source == "gdi" {
        Some(gdi::GdiCapture::new()?)
    } else {
        None
    };
    #[cfg(windows)]
    let (width, height) = if let Some(g) = &gdi {
        (g.width, g.height)
    } else {
        (o.width, o.height)
    };
    #[cfg(not(windows))]
    let (width, height) = (o.width, o.height);

    let mut synthetic = vec![0u8; width * height * 4];
    let mut i420 = vec![0u8; width * height * 3 / 2];
    #[cfg(feature = "vpx")]
    let mut encoder = if o.codec == Codec::None {
        None
    } else {
        Some(SoftwareEncoder::new(
            width,
            height,
            o.fps,
            o.bitrate_kbps,
            o.codec == Codec::Vp9,
        )?)
    };

    let mut stats = Stats::default();
    let frame_duration = Duration::from_secs_f64(1.0 / o.fps as f64);
    let limit = Duration::from_secs(o.seconds);
    let start = Instant::now();
    let mut next_slot = 0u64;
    while start.elapsed() < limit {
        // Deadline scheduling skips stale frames; it never queues an ever-growing backlog.
        let planned = start + frame_duration.mul_f64(next_slot as f64);
        let now = Instant::now();
        if now < planned {
            thread::sleep(planned - now);
        }
        if start.elapsed() >= limit {
            break;
        }
        let frame_start = Instant::now();
        let elapsed_slots =
            (frame_start.duration_since(start).as_secs_f64() * o.fps as f64).floor() as u64;
        if elapsed_slots > next_slot {
            stats.dropped += elapsed_slots - next_slot;
            next_slot = elapsed_slots;
        }

        let cap_start = Instant::now();
        #[cfg(windows)]
        let bgra: &[u8] = if let Some(g) = &mut gdi {
            g.grab()?
        } else {
            make_synthetic(&mut synthetic, width, height, next_slot);
            &synthetic
        };
        #[cfg(not(windows))]
        let bgra: &[u8] = {
            make_synthetic(&mut synthetic, width, height, next_slot);
            &synthetic
        };
        stats.capture.record(cap_start.elapsed());
        stats.captured += 1;

        let cvt_start = Instant::now();
        color::bgra_to_i420(bgra, width, height, &mut i420);
        stats.color.record(cvt_start.elapsed());

        let enc_start = Instant::now();
        #[cfg(feature = "vpx")]
        if let Some(enc) = &mut encoder {
            let (packets, bytes) = enc.encode(next_slot as i64, &i420)?;
            stats.packets += packets;
            stats.bytes += bytes;
        }
        stats.encode.record(enc_start.elapsed());
        stats.encoded += 1;
        stats.loop_ms.record(frame_start.elapsed());
        if frame_start.elapsed() > frame_duration {
            stats.slow += 1;
        }
        next_slot += 1;
    }
    #[cfg(feature = "vpx")]
    if let Some(enc) = &mut encoder {
        let (packets, bytes) = enc.finish()?;
        stats.packets += packets;
        stats.bytes += bytes;
    }
    let elapsed = start.elapsed().as_secs_f64();
    let json = format!(
        concat!(
            "{{\n  \"schema\": \"hi5-rust-agent-benchmark-v1\",\n",
            "  \"source\": \"{}\", \"codec\": \"{}\",\n",
            "  \"width\": {}, \"height\": {}, \"target_fps\": {},\n",
            "  \"duration_s\": {:.3}, \"captured_frames\": {}, \"encoded_frames\": {},\n",
            "  \"actual_fps\": {:.3}, \"missed_deadlines\": {}, \"frames_over_budget\": {},\n",
            "  \"output_packets\": {}, \"output_bytes\": {},\n",
            "  \"capture_p50_ms\": {:.3}, \"capture_p95_ms\": {:.3},\n",
            "  \"color_p50_ms\": {:.3}, \"color_p95_ms\": {:.3},\n",
            "  \"encode_p50_ms\": {:.3}, \"encode_p95_ms\": {:.3},\n",
            "  \"total_p50_ms\": {:.3}, \"total_p95_ms\": {:.3}\n}}\n"
        ),
        o.source,
        o.codec.name(),
        width,
        height,
        o.fps,
        elapsed,
        stats.captured,
        stats.encoded,
        stats.encoded as f64 / elapsed,
        stats.dropped,
        stats.slow,
        stats.packets,
        stats.bytes,
        stats.capture.pct(0.5),
        stats.capture.pct(0.95),
        stats.color.pct(0.5),
        stats.color.pct(0.95),
        stats.encode.pct(0.5),
        stats.encode.pct(0.95),
        stats.loop_ms.pct(0.5),
        stats.loop_ms.pct(0.95),
    );
    print!("{json}");
    if let Some(path) = o.json {
        fs::write(&path, json).map_err(|e| format!("Cannot write {path}: {e}"))?;
    }
    Ok(())
}
fn main() {
    match Options::from_args().and_then(run) {
        Ok(()) => (),
        Err(error) => {
            eprintln!("{error}");
            std::process::exit(2);
        }
    }
}
