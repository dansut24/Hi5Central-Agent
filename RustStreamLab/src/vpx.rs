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
#[derive(Clone,Debug)]
pub struct EncodedVideoFrame {
    pub payload: Vec<u8>,
    pub is_keyframe: bool,
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
            cfg.kf_max_dist = fps; // Faster decoder recovery than 3-second baseline.
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
    fn packets(&mut self) -> (u64, u64, Vec<EncodedVideoFrame>) {
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
                    payloads.push(EncodedVideoFrame {
                        payload:payload.to_vec(),
                        is_keyframe: (frame.flags & vpx::VPX_FRAME_IS_KEY) != 0,
                    });
                }
            }
        }
        (count, bytes, payloads)
    }
    pub fn encode(&mut self, pts: i64, data: &[u8], force_keyframe: bool)
        -> Result<(u64, u64, Vec<EncodedVideoFrame>), String> {
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
                    if force_keyframe {vpx::VPX_EFLAG_FORCE_KF as _} else {0},
                    vpx::VPX_DL_REALTIME as c_ulong,
                ),
                "encode",
            )?;
        }
        Ok(self.packets())
    }
    pub fn finish(&mut self) -> Result<(u64, u64, Vec<EncodedVideoFrame>), String> {
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

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn vp8_keyframe_is_decodable_by_libvpx() {
        let (w,h)=(320usize,180usize);
        let frame=vec![128_u8;w*h*3/2];
        let mut encoder=SoftwareEncoder::new(w,h,30,4000,false).expect("VP8 encoder");
        let (_,_,packets)=encoder.encode(0,&frame,true).expect("encode");
        assert!(!packets.is_empty(),"no frame produced");
        let first=&packets[0];
        assert!(first.is_keyframe,"first frame must be keyframe");
        assert_eq!(first.payload[0]&1,0,"VP8 keyframe header");
        unsafe {
            let mut ctx=mem::MaybeUninit::<vpx::vpx_codec_ctx_t>::zeroed();
            check(vpx::vpx_codec_dec_init_ver(
                ctx.as_mut_ptr(),
                vpx::vpx_codec_vp8_dx(),
                ptr::null(),
                0,
                vpx::VPX_DECODER_ABI_VERSION as i32,
            ),"VP8 decoder init").unwrap();
            let mut ctx=ctx.assume_init();
            check(vpx::vpx_codec_decode(
                &mut ctx,
                first.payload.as_ptr(),
                first.payload.len() as u32,
                ptr::null_mut(),
                0
            ),"local VP8 decode").unwrap();
            let mut it:vpx::vpx_codec_iter_t=ptr::null();
            let img=vpx::vpx_codec_get_frame(&mut ctx,&mut it);
            assert!(!img.is_null(),"libvpx returned no decoded frame");
            assert_eq!((*img).d_w as usize,w);
            assert_eq!((*img).d_h as usize,h);
            let _=vpx::vpx_codec_destroy(&mut ctx);
        }
    }
    #[test]
    fn vp8_rtp_fragmentation_reassembles_valid_keyframe() {
        use bytes::Bytes;
        use rtp::{
            codecs::vp8::{Vp8Packet,Vp8Payloader},
            packetizer::{new_packetizer,Packetizer,Depacketizer},
            sequence::new_fixed_sequencer,
        };
        let (w,h)=(640usize,360usize);
        let mut i420=vec![128u8;w*h*3/2];
        for (i,y) in i420[..w*h].iter_mut().enumerate(){
            *y=((i/8^i/w*7)&255) as u8;
        }
        let mut encoder=SoftwareEncoder::new(w,h,30,4000,false).expect("VP8 encoder");
        let (_,_,encoded)=encoder.encode(0,&i420,true).expect("VP8 frame");
        assert!(!encoded.is_empty());
        let f=&encoded[0];
        assert!(f.is_keyframe);
        let mut vp8=Vp8Payloader::default();
        vp8.enable_picture_id=true;
        let mut packetizer=new_packetizer(1200,96,12345,
            Box::new(vp8),
            Box::new(new_fixed_sequencer(1)),90000);
        let packets=packetizer.packetize(&Bytes::copy_from_slice(&f.payload),3000)
            .expect("packetize");
        assert!(packets.len()>1,"test must exercise fragmentation");
        assert!(packets.last().unwrap().header.marker);
        assert!(packets[..packets.len()-1].iter().all(|p|!p.header.marker));
        let mut reassembled=Vec::new();
        for p in packets {
            let mut depay=Vp8Packet::default();
            let chunk=depay.depacketize(&p.payload).expect("VP8 depacketize");
            reassembled.extend_from_slice(&chunk);
        }
        assert_eq!(reassembled,f.payload);
    }

}
