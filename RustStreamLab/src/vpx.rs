//! Explicit libvpx settings, aligned with the isolated C++ StreamingLab baseline.
//! Native codec is C libvpx; Rust manages frames, lifetimes and pacing.
use vpx_sys as vpx;
use std::{mem, os::raw::c_ulong, ptr};

fn check(status: vpx::vpx_codec_err_t, action: &str) -> Result<(), String> {
    if status == vpx::VPX_CODEC_OK {
        Ok(())
    } else {
        Err(format!("libvpx {action} failed: {status:?}"))
    }
}
pub struct SoftwareEncoder {
    ctx: vpx::vpx_codec_ctx_t,
    width: usize,
    height: usize,
}
impl SoftwareEncoder {
    pub fn new(
        width: usize,
        height: usize,
        fps: u32,
        bitrate_kbps: u32,
        use_vp9: bool,
    ) -> Result<Self, String> {
        if width == 0 || height == 0 || width % 2 != 0 || height % 2 != 0 {
            return Err("I420 requires positive even dimensions".into());
        }
        unsafe {
            let interface = if use_vp9 {
                vpx::vpx_codec_vp9_cx()
            } else {
                vpx::vpx_codec_vp8_cx()
            };
            if interface.is_null() {
                return Err("Missing libvpx encoder".into());
            }
            let mut cfg = mem::MaybeUninit::<vpx::vpx_codec_enc_cfg_t>::uninit();
            check(
                vpx::vpx_codec_enc_config_default(interface, cfg.as_mut_ptr(), 0),
                "defaults",
            )?;
            // libvpx fills the entire config on success, including C enum members.
            let mut cfg = cfg.assume_init();
            cfg.g_w = width as u32;
            cfg.g_h = height as u32;
            cfg.g_timebase.num = 1;
            cfg.g_timebase.den = fps as i32;
            cfg.rc_target_bitrate = bitrate_kbps;
            cfg.g_threads = 4;
            cfg.g_lag_in_frames = 0;
            cfg.g_error_resilient = 1;
            cfg.rc_resize_allowed = 0;
            cfg.rc_dropframe_thresh = 0;
            cfg.kf_max_dist = fps * 3;
            let mut ctx = mem::MaybeUninit::<vpx::vpx_codec_ctx_t>::zeroed();
            check(
                vpx::vpx_codec_enc_init_ver(
                    ctx.as_mut_ptr(),
                    interface,
                    &cfg,
                    0,
                    vpx::VPX_ENCODER_ABI_VERSION as i32,
                ),
                "initialise",
            )?;
            let mut ctx = ctx.assume_init();

            let speed = if use_vp9 { 6 } else { 8 };
            let speed_status = vpx::vpx_codec_control_(
                &mut ctx,
                vpx::vp8e_enc_control_id::VP8E_SET_CPUUSED as i32,
                speed,
            );
            if speed_status != vpx::VPX_CODEC_OK {
                vpx::vpx_codec_destroy(&mut ctx);
                return Err(format!("libvpx rejected CPUUSED={speed}: {speed_status:?}"));
            }
            if use_vp9 {
                check(
                    vpx::vpx_codec_control_(
                        &mut ctx,
                        vpx::vp8e_enc_control_id::VP9E_SET_ROW_MT as i32,
                        1,
                    ),
                    "VP9 row_mt",
                )?;
            }
            Ok(Self { ctx, width, height })
        }
    }
    fn packets(&mut self) -> (u64, u64, Vec<Vec<u8>>) {
        let mut count = 0;
        let mut bytes = 0;
        let mut payloads = Vec::new();
        unsafe {
            let mut iter: vpx::vpx_codec_iter_t = ptr::null();
            loop {
                let packet = vpx::vpx_codec_get_cx_data(&mut self.ctx, &mut iter);
                if packet.is_null() {
                    break;
                }
                if (*packet).kind == vpx::vpx_codec_cx_pkt_kind::VPX_CODEC_CX_FRAME_PKT {
                    let frame = &(*packet).data.frame;
                    count += 1;
                    bytes += frame.sz as u64;
                    let payload = std::slice::from_raw_parts(frame.buf as *const u8, frame.sz as usize);
                    payloads.push(payload.to_vec());
                }
            }
        }
        (count, bytes, payloads)
    }
    pub fn encode(&mut self, pts: i64, data: &[u8]) -> Result<(u64, u64, Vec<Vec<u8>>), String> {
        if data.len() < self.width * self.height * 3 / 2 {
            return Err("I420 too small".into());
        }
        unsafe {
            let mut image = mem::MaybeUninit::<vpx::vpx_image_t>::uninit();
            let wrapped = vpx::vpx_img_wrap(
                image.as_mut_ptr(),
                vpx::vpx_img_fmt::VPX_IMG_FMT_I420,
                self.width as u32,
                self.height as u32,
                1,
                data.as_ptr() as *mut u8,
            );
            if wrapped.is_null() {
                return Err("vpx_img_wrap failed".into());
            }
            check(
                vpx::vpx_codec_encode(
                    &mut self.ctx,
                    wrapped,
                    pts,
                    1,
                    0,
                    vpx::VPX_DL_REALTIME as c_ulong,
                ),
                "encode",
            )?;
        }
        Ok(self.packets())
    }
    pub fn finish(&mut self) -> Result<(u64, u64, Vec<Vec<u8>>), String> {
        unsafe {
            check(
                vpx::vpx_codec_encode(
                    &mut self.ctx,
                    ptr::null(),
                    -1,
                    1,
                    0,
                    vpx::VPX_DL_REALTIME as c_ulong,
                ),
                "flush",
            )?;
        }
        Ok(self.packets())
    }
}
impl Drop for SoftwareEncoder {
    fn drop(&mut self) {
        unsafe {
            vpx::vpx_codec_destroy(&mut self.ctx);
        }
    }
}
