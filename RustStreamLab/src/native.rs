//! Native Windows GDI -> BGRA/I420 -> libvpx -> Rust WebRTC pipeline.
//! Deliberately independent of FFmpeg and the production C++ agent.
use super::{Settings, Outbound, tell};
use bytes::Bytes;
use std::sync::{Arc, atomic::{AtomicU64,Ordering}};
use tokio::sync::mpsc;
use webrtc::{media::Sample,track::track_local::track_local_static_sample::TrackLocalStaticSample};
use std::time::Duration;

#[cfg(windows)]
use std::time::Instant;

#[cfg(windows)]
struct EncodedFrame {
    payload: Vec<u8>,
    slot: u64,
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
    let mut capture=GdiCapture::new()?;
    let sw=capture.width;
    let sh=capture.height;
    let dw=if settings.max_width==0 || sw<=settings.max_width as usize {sw}
        else {(settings.max_width as usize)&!1};
    let dh=(((sh as f64*dw as f64/sw as f64).round() as usize)&!1).max(2);
    let scaled=dw!=sw || dh!=sh;
    let mut resized=if scaled {vec![0u8;dw*dh*4]} else {Vec::new()};
    let mut i420=vec![0u8;dw*dh*3/2];
    let mut encoder=SoftwareEncoder::new(dw,dh,settings.fps,settings.kbps,
                                        settings.codec=="vp9")?;
    tell(&tx,serde_json::json!({
        "type":"capture_started","codec":settings.codec,
        "width":dw,"height":dh,"sourceWidth":sw,"sourceHeight":sh,
         "backend":format!("Rust {} -> I420 -> native libvpx -> Rust WebRTC",capture.mode())
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
        let bgra=capture.grab()?;
        capture_ms.push(begin_capture.elapsed().as_secs_f64()*1000.0);
        let begin_convert=Instant::now();
        if scaled {
            resize_nearest(bgra,sw,sh,&mut resized,dw,dh);
            bgra_to_i420(&resized,dw,dh,&mut i420);
        } else {
            bgra_to_i420(bgra,dw,dh,&mut i420);
        }
        convert_ms.push(begin_convert.elapsed().as_secs_f64()*1000.0);
        let begin_encode=Instant::now();
        let (_packets,_bytes,payloads)=encoder.encode(next_slot as i64,&i420)?;
        encode_ms.push(begin_encode.elapsed().as_secs_f64()*1000.0);
        total_ms.push(begun.elapsed().as_secs_f64()*1000.0);
        for payload in payloads {
            if payload.is_empty(){continue}
            encoded_total+=1;
            window_total+=1;
            if sender.blocking_send(EncodedFrame{payload,slot:next_slot}).is_err() {
                return Ok(());
            }
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
                "captureBackend":format!("Rust {} + I420 + libvpx",capture.mode()),
                "sourceWidth":sw,"sourceHeight":sh,"width":dw,"height":dh,
                "captureMsAvg":avg(&capture_ms),
                "convertMsAvg":avg(&convert_ms),
                "encodeMsAvg":avg(&encode_ms),
                "frameAgeP95Ms":p95(&total_ms),
                "missedDeadlines":missed
            }}));
            window_total=0;
            window_start=Instant::now();
            capture_ms.clear();convert_ms.clear();encode_ms.clear();total_ms.clear();
        }
    }
    Ok(())
}

pub async fn produce(settings:Settings, track:Arc<TrackLocalStaticSample>,
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
        let mut previous_slot:Option<u64>=None;
        let mut write_error=None;
        while let Some(frame)=receiver.recv().await {
            let slots=previous_slot.map(|prev|frame.slot.saturating_sub(prev).max(1)).unwrap_or(1);
            let duration=Duration::from_secs_f64(slots as f64/settings.fps as f64);
            let size=frame.payload.len();
            if let Err(e)=track.write_sample(&Sample {
                data:Bytes::from(frame.payload),duration,..Default::default()
            }).await {
                write_error=Some(format!("WebRTC sample send: {e}"));
                break;
            }
            previous_slot=Some(frame.slot);
            frames.fetch_add(1,Ordering::Relaxed);
            bytes.fetch_add(size as u64,Ordering::Relaxed);
        }
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
