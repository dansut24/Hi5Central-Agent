//! Native Windows GDI -> BGRA/I420 -> libvpx -> Rust WebRTC pipeline.
//! Deliberately independent of FFmpeg and the production C++ agent.
use super::{Settings, Outbound, tell};
use bytes::Bytes;
use std::sync::{Arc, atomic::{AtomicU64,Ordering}};
use tokio::sync::mpsc;
use webrtc::track::track_local::track_local_static_rtp::TrackLocalStaticRTP;
#[cfg(windows)]
use rtp::{
    codecs::{vp8::Vp8Payloader, vp9::Vp9Payloader},
    packetizer::{new_packetizer, Packetizer, Payloader},
    sequence::new_random_sequencer,
};
use std::time::Duration;

#[cfg(windows)]
use std::time::Instant;

#[cfg(windows)]
struct EncodedFrame {
    payload: Vec<u8>,
    slot: u64,
    keyframe: bool,
}

#[cfg(windows)]
fn resize_nearest(src:&[u8], sw:usize, sh:usize, dst:&mut[u8], dw:usize, dh:usize) {
    debug_assert!(src.len() >= sw*sh*4 && dst.len()>=dw*dh*4);
    for y in 0..dh {
        let src_y=(y*sh/dh)*sw*4;
        let dst_y=y*dw*4;
        for x in 0..dw {
            let sx=x*sw/dw;
            dst[dst_y+x*4..dst_y+x*4+4]
                .copy_from_slice(&src[src_y+sx*4..src_y+sx*4+4]);
        }
    }
}

#[cfg(windows)]
fn p95(samples:&[f64])->f64 {
    if samples.is_empty(){return 0.0}
    let mut sorted=samples.to_vec();
    sorted.sort_by(|a,b|a.total_cmp(b));
    sorted[((sorted.len()-1) as f64*0.95).ceil() as usize]
}

#[cfg(windows)]
fn worker(settings:Settings, sender:mpsc::Sender<EncodedFrame>, tx:Outbound)
    -> Result<(),String>{
    use super::{gdi::GdiCapture,color::bgra_to_i420,vpx::SoftwareEncoder};
    let diagnostic=settings.capture_mode=="rust-synthetic";
    let mut capture=if diagnostic {None}
        else {Some(GdiCapture::new_with_max_width(settings.max_width)?)};
    let (sw,sh,source_width,source_height)=if let Some(c)=capture.as_ref() {
        (c.width,c.height,c.source_width,c.source_height)
    } else {(960usize,540usize,960usize,540usize)};
    let dw=sw;
    let dh=sh;
    let mut synthetic=if diagnostic {vec![0u8;dw*dh*4]} else {Vec::new()};
    let mut i420=vec![0u8;dw*dh*3/2];
    let backend=if let Some(c)=capture.as_ref(){
        format!("Rust {} -> I420 -> native libvpx -> paced RTP",c.mode())
    } else {"Rust synthetic test pattern -> I420 -> native libvpx -> paced RTP".into()};
    let mut encoder=SoftwareEncoder::new(dw,dh,settings.fps,settings.kbps,
                                        settings.codec=="vp9")?;
    tell(&tx,serde_json::json!({
        "type":"capture_started","codec":settings.codec,
        "width":dw,"height":dh,"sourceWidth":source_width,"sourceHeight":source_height,
        "backend":backend
    }));
    let start=Instant::now();
    let mut next_slot=0_u64;
    let mut missed=0_u64;
    let mut encoded_total=0_u64;
    let mut window_total=0_u64;
    let mut capture_ms:Vec<f64>=Vec::new();
    let mut convert_ms:Vec<f64>=Vec::new();
    let mut encode_ms:Vec<f64>=Vec::new();
    let mut total_ms:Vec<f64>=Vec::new();
    let mut window_start=Instant::now();
    let mut last_keyframe=Instant::now()-Duration::from_secs(10);
    let mut keyframes_total:u64=0;
    let mut largest_frame_bytes:usize=0;
    loop {
        if sender.is_closed(){break}
        let due=start+Duration::from_secs_f64(next_slot as f64/settings.fps as f64);
        let now=Instant::now();
        if now<due {std::thread::sleep(due-now)}
        if sender.is_closed(){break}
        let begun=Instant::now();
        let current_slot=(start.elapsed().as_secs_f64()*settings.fps as f64).floor() as u64;
        if current_slot>next_slot {
            missed += current_slot-next_slot;
            next_slot=current_slot;
        }
        let begin_capture=Instant::now();
        let bgra:&[u8]=if let Some(c)=capture.as_mut(){c.grab()?}
            else {
                // Identifiable moving bars, enabling codec/RTP qualification without GDI.
                for (n,px) in synthetic.chunks_exact_mut(4).enumerate(){
                    let x=n%sw;
                    let y=n/sw;
                    px[0]=((x/8+next_slot as usize*5)&255) as u8;
                    px[1]=((y/7+next_slot as usize*2)&255) as u8;
                    px[2]=(((x/16^y/16)+next_slot as usize*7)&255) as u8;
                    px[3]=255;
                }
                &synthetic
            };
        capture_ms.push(begin_capture.elapsed().as_secs_f64()*1000.0);
        let begin_convert=Instant::now();
        bgra_to_i420(bgra,dw,dh,&mut i420);
        convert_ms.push(begin_convert.elapsed().as_secs_f64()*1000.0);
        let begin_encode=Instant::now();
        // Periodic wall-clock keyframes, even when GDI is far below the target FPS.
        // A late-joining decoder cannot recover from delta-only streams.
        let force_keyframe=encoded_total==0 || last_keyframe.elapsed()>=Duration::from_secs(2);
        let (_packets,_bytes,payloads)=encoder.encode(next_slot as i64,&i420,force_keyframe)?;
        encode_ms.push(begin_encode.elapsed().as_secs_f64()*1000.0);
        total_ms.push(begun.elapsed().as_secs_f64()*1000.0);
        for frame in payloads {
            if frame.payload.is_empty(){continue}
            if frame.is_keyframe {
                keyframes_total+=1;
                last_keyframe=Instant::now();
            }
            largest_frame_bytes=largest_frame_bytes.max(frame.payload.len());
            encoded_total+=1;
            window_total+=1;
            if sender.blocking_send(EncodedFrame{
                payload:frame.payload,slot:next_slot,keyframe:frame.is_keyframe
            }).is_err() {return Ok(())}
        }
        next_slot+=1;
        if window_start.elapsed()>=Duration::from_secs(1) {
            let elapsed=window_start.elapsed().as_secs_f64();
            let avg=|v:&[f64]| if v.is_empty() {0.0}
                else {v.iter().sum::<f64>()/v.len() as f64};
            tell(&tx,serde_json::json!({"type":"stats","stats":{
                "codec":settings.codec,
                "configuredFps":settings.fps,
                "lastSecondFps":window_total as f64/elapsed,
                "fpsMeasured":encoded_total as f64/start.elapsed().as_secs_f64().max(0.001),
                "captureBackend":backend,
                "sourceWidth":source_width,"sourceHeight":source_height,"width":dw,"height":dh,
                "captureMsAvg":avg(&capture_ms),
                "convertMsAvg":avg(&convert_ms),
                "encodeMsAvg":avg(&encode_ms),
                "frameAgeP95Ms":p95(&total_ms),
                "missedDeadlines":missed,
                "encodedKeyframes":keyframes_total,
                "largestEncodedFrameBytes":largest_frame_bytes
            }}));
            window_total=0;
            window_start=Instant::now();
            capture_ms.clear();convert_ms.clear();encode_ms.clear();total_ms.clear();
        }
    }
    Ok(())
}

pub async fn produce(settings:Settings, track:Arc<TrackLocalStaticRTP>,
    tx:Outbound, frames:Arc<AtomicU64>, bytes:Arc<AtomicU64>)
    -> Result<(),String> {
    #[cfg(not(windows))]
    {
        let _=(settings,track,tx,frames,bytes);
        return Err("Windows GDI capture required; no FFmpeg fallback".into());
    }
    #[cfg(windows)]
    {
        let (sender,mut receiver)=mpsc::channel::<EncodedFrame>(2);
        let worker_tx=tx.clone();
        let opts=settings.clone();
        let handle=tokio::task::spawn_blocking(move || worker(opts,sender,worker_tx));
        // Rust RTP payloader explicitly exposes packet boundaries for paced
        // transmission. StaticSample wrote all fragments of large VP8 keyframes
        // as a UDP burst, and the iPhone lost substantial parts of each keyframe.
        let payloader:Box<dyn Payloader+Send+Sync> = if settings.codec=="vp8" {
            let mut vp8=Vp8Payloader::default();
            vp8.enable_picture_id=true;
            Box::new(vp8)
        } else {Box::new(Vp9Payloader::default())};
        let mut packetizer=new_packetizer(
            1200,96,0,payloader,Box::new(new_random_sequencer()),90000
        );
        let mut previous_slot:Option<u64>=None;
        let mut write_error=None;
        let paced_bits_per_second=(settings.kbps as f64*1000.0*0.85).max(300_000.0);
        let mut next_packet_due=tokio::time::Instant::now();
        let mut sent_rtp_packets=0u64;
        let mut sent_keyframes=0u64;
        let mut largest_keyframe_bytes=0usize;
        while let Some(frame)=receiver.recv().await {
            let slots=previous_slot.map(|prev|frame.slot.saturating_sub(prev).max(1)).unwrap_or(1);
            let samples=((slots as f64/settings.fps as f64)*90000.0).round() as u32;
            let size=frame.payload.len();
            if frame.keyframe {
                sent_keyframes+=1;
                largest_keyframe_bytes=largest_keyframe_bytes.max(size);
                tell(&tx,serde_json::json!({"type":"diagnostic","message":format!(
                    "RTP paced keyframe #{}: {} bytes",sent_keyframes,size)}));
            }
            let packets=packetizer.packetize(&Bytes::from(frame.payload),samples)
                .map_err(|e|format!("VP8/VP9 RTP packetize: {e}"))?;
            for pkt in packets {
                let now=tokio::time::Instant::now();
                if next_packet_due>now {
                    tokio::time::sleep_until(next_packet_due).await;
                } else if now.duration_since(next_packet_due)>Duration::from_millis(100) {
                    next_packet_due=now; // Never accumulate a large delayed burst.
                }
                if let Err(e)=track.write_rtp_with_extensions(&pkt,&[]).await {
                    write_error=Some(format!("Write paced RTP: {e}"));
                    break;
                }
                sent_rtp_packets+=1;
                let packet_bits=(pkt.payload.len()+44) as f64*8.0;
                next_packet_due+=Duration::from_secs_f64(packet_bits/paced_bits_per_second);
            }
            if write_error.is_some(){break}
            previous_slot=Some(frame.slot);
            frames.fetch_add(1,Ordering::Relaxed);
            bytes.fetch_add(size as u64,Ordering::Relaxed);
        }
        tell(&tx,serde_json::json!({"type":"diagnostic","message":format!(
            "RTP sender stopped: packets={}, keyframes={}, max_keyframe_bytes={}",
            sent_rtp_packets,sent_keyframes,largest_keyframe_bytes)}));
        drop(receiver);
        // Closing the receiver causes the blocking worker to terminate without a service kill.
        let result=handle.await.map_err(|e|format!("Capture worker failure: {e}"))?;
        if let Some(e)=write_error{return Err(e)}
        result
    }
}

#[cfg(test)]
mod tests {
    #[cfg(windows)]
    #[test]
    fn nearest_resize() {
        let src=vec![42u8;4*4*4];
        let mut dst=vec![0u8;2*2*4];
        super::resize_nearest(&src,4,4,&mut dst,2,2);
        assert!(dst.iter().all(|x|*x==42));
    }
}
