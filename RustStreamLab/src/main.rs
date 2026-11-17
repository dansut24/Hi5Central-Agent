mod color;
#[cfg(windows)] mod gdi;
mod vpx;
mod native;
// Hi5Central Rust StreamLab — isolated read-only video host.
// Runs no Windows service, does not enroll or receive remote input.
use std::{env, sync::{Arc, atomic::AtomicU64}};
use futures_util::{SinkExt, StreamExt};
use serde::Deserialize;
use serde_json::{json, Value};
use tokio::{sync::mpsc, task::JoinHandle};
use tokio_tungstenite::{connect_async, tungstenite::Message};
use webrtc::{
    api::{APIBuilder, media_engine::{MediaEngine, MIME_TYPE_VP8, MIME_TYPE_VP9}},
    ice_transport::{ice_candidate::RTCIceCandidateInit, ice_server::RTCIceServer},
    peer_connection::{configuration::RTCConfiguration, peer_connection_state::RTCPeerConnectionState,
        sdp::session_description::RTCSessionDescription, RTCPeerConnection},
    rtp_transceiver::rtp_codec::RTCRtpCodecCapability,
    track::track_local::{track_local_static_rtp::TrackLocalStaticRTP, TrackLocal},
};

const DEFAULT_RELAY: &str = "wss://rust-streamlab.hi5central.com/rust-streamlab/ws?role=host";
type Outbound = mpsc::UnboundedSender<Message>;

#[derive(Debug, Deserialize)]
struct IceServer { urls: Value, username: Option<String>, credential: Option<String> }
#[derive(Clone, Debug)]
struct Settings { codec: String, fps: u32, max_width: u32, kbps: u32, capture_mode: String }
impl Settings {
    fn parse(v: &Value) -> Result<Self, String> {
        let codec=v.get("codec").and_then(Value::as_str).unwrap_or("").to_lowercase();
        let fps=v.get("fps").and_then(Value::as_u64).unwrap_or(0) as u32;
        let max_width=v.get("maxWidth").and_then(Value::as_u64).unwrap_or(0) as u32;
        let kbps=v.get("bitrateKbps").and_then(Value::as_u64).unwrap_or(0) as u32;
        if codec!="vp8" && codec!="vp9" {return Err("Only VP8/VP9 video supported".into())}
        if ![15,30,45,60].contains(&fps) {return Err("FPS must be 15/30/45/60".into())}
        if ![0,1280,1600,1920].contains(&max_width) {return Err("Unsupported width".into())}
        if !(1000..=20000).contains(&kbps) {return Err("Bitrate out of range".into())}
        let capture_mode=v.get("captureMode").and_then(Value::as_str).unwrap_or("rust-gdi").to_string();
        if !["rust-gdi","rust-synthetic"].contains(&capture_mode.as_str()) {
            return Err("Unsupported capture mode".into())
        }
        Ok(Self {codec,fps,max_width,kbps,capture_mode})
    }
}
fn tell(tx:&Outbound, v:Value) {let _=tx.send(Message::Text(v.to_string().into()));}
fn status(tx:&Outbound, kind:&str, detail:impl AsRef<str>) {
    eprintln!("[{kind}] {}",detail.as_ref());
    tell(tx,json!({"type":kind,"message":detail.as_ref()}));
}
fn parse_ice(v:&Value)->Result<Vec<RTCIceServer>,String>{
    let a=v.as_array().ok_or("Missing TURN servers")?;
    if a.is_empty(){return Err("TURN servers unavailable".into())}
    a.iter().map(|item|{
        let s:IceServer=serde_json::from_value(item.clone()).map_err(|e|e.to_string())?;
        let urls=if let Some(x)=s.urls.as_str(){vec![x.to_string()]}
            else {s.urls.as_array().ok_or("Invalid TURN urls")?.iter()
                .map(|x|x.as_str().ok_or("Invalid TURN url").map(str::to_string))
                .collect::<Result<Vec<_>,_>>()?};
        if urls.is_empty() || urls.iter().any(|x| !x.starts_with("turn:") && !x.starts_with("turns:")){
            return Err("Relay did not provide TURN servers".into());
        }
        Ok(RTCIceServer {urls, username:s.username.unwrap_or_default(),
            credential:s.credential.unwrap_or_default(), ..Default::default()})
    }).collect()
}
use native::produce;
struct Active {pc:Arc<RTCPeerConnection>, track:Arc<TrackLocalStaticRTP>,
    pending:Vec<RTCIceCandidateInit>, settings:Settings,
    video:Option<JoinHandle<()>>}
impl Active {
    async fn stop(mut self) {
        if let Some(task)=self.video.take(){task.abort();}
        let _=self.pc.close().await;
    }
}
async fn offer(settings:Settings, ice:&[RTCIceServer], tx:&Outbound)->Result<Active,String>{
    let mut engine=MediaEngine::default();
    engine.register_default_codecs().map_err(|e|e.to_string())?;
    let api=APIBuilder::new().with_media_engine(engine).build();
    let pc=Arc::new(api.new_peer_connection(RTCConfiguration{
        ice_servers:ice.to_vec(),..Default::default()
    }).await.map_err(|e|e.to_string())?);
    let mime=if settings.codec=="vp8" {MIME_TYPE_VP8} else {MIME_TYPE_VP9};
    let track=Arc::new(TrackLocalStaticRTP::new(
        RTCRtpCodecCapability {mime_type:mime.to_owned(),..Default::default()},
        "video".to_owned(), "hi5-rust-streamlab".to_owned()));
    let sender=pc.add_track(track.clone() as Arc<dyn TrackLocal + Send + Sync>)
        .await.map_err(|e|e.to_string())?;
    tokio::spawn(async move {
        let mut buf=vec![0u8;1500];
        while sender.read(&mut buf).await.is_ok(){}
    });
    let ice_tx=tx.clone();
    pc.on_ice_candidate(Box::new(move |candidate| {
        let out=ice_tx.clone();
        Box::pin(async move{
            if let Some(c)=candidate {
                if let Ok(v)=c.to_json(){
                    // webrtc-rs 0.13 supplies Some("") as the candidate MID.
                    // An invented MID like "video" is rejected by Safari.
                    tell(&out,json!({"type":"ice_candidate","candidate":v.candidate,
                        "sdpMid":v.sdp_mid.filter(|mid|!mid.is_empty()),
                        "sdpMLineIndex":v.sdp_mline_index.unwrap_or(0)}));
                }
            }
        })
    }));
    let state_tx=tx.clone();
    pc.on_peer_connection_state_change(Box::new(move |state:RTCPeerConnectionState|{
        let out=state_tx.clone();
        Box::pin(async move {status(&out,"diagnostic",format!("WebRTC {state:?}"));})
    }));
    let description=pc.create_offer(None).await.map_err(|e|e.to_string())?;
    pc.set_local_description(description.clone()).await.map_err(|e|e.to_string())?;
    tell(tx,json!({"type":"webrtc_offer","sdp":description.sdp}));
    Ok(Active {pc,track,pending:Vec::new(),settings,video:None})
}
async fn run() -> Result<(),String> {
    if env::args().any(|a|a=="--version"){ println!("Hi5Central Rust StreamLab 0.1.0 — native Windows GDI/libvpx/WebRTC"); return Ok(()) }
    if !cfg!(target_os="windows"){return Err("Rust StreamLab desktop capture currently requires Windows".into());}
    let mut relay=DEFAULT_RELAY.to_owned();
    let mut args=env::args().skip(1);
    while let Some(a)=args.next(){
        let v=args.next().ok_or_else(||format!("Missing argument value: {a}"))?;
        match a.as_str() {
            "--relay" if v.starts_with("wss://")=>relay=v,
            _=>return Err(format!("Unsupported option {a}")),
        }
    }
    println!("Hi5Central Rust StreamLab | native GDI + libvpx + WebRTC | read-only");
    println!("Connecting dedicated relay: {relay}");
    let (ws,_) = connect_async(&relay).await.map_err(|e|format!("Relay WebSocket: {e}"))?;
    let (mut sink,mut source)=ws.split();
    let (tx,mut rx)=mpsc::unbounded_channel::<Message>();
    let writer=tokio::spawn(async move{
        while let Some(item)=rx.recv().await {
            if sink.send(item).await.is_err(){break;}
        }
    });
    tell(&tx,json!({"type":"hello"}));
    let mut ice:Vec<RTCIceServer>=Vec::new();
    let mut active:Option<Active>=None;
    while let Some(item)=source.next().await {
        let incoming=item.map_err(|e|format!("Relay disconnected: {e}"))?;
        if incoming.is_ping(){let _=tx.send(Message::Pong(incoming.into_data()));continue}
        if !incoming.is_text(){continue}
        let v:Value=serde_json::from_str(incoming.to_text().unwrap_or(""))
            .map_err(|e|format!("Invalid signalling JSON: {e}"))?;
        let kind=v.get("type").and_then(Value::as_str).unwrap_or("");
        match kind {
            "hello_required"=>tell(&tx,json!({"type":"hello"})),
            "registered"=>{
                let code=v.get("code").and_then(Value::as_str).unwrap_or("(unknown)");
                println!("PAIRING CODE: {code}\nOpen https://rust-streamlab.hi5central.com and enter this code.\n");
            },
            "paired"=>{
                ice=parse_ice(v.get("iceServers").unwrap_or(&Value::Null))?;
                status(&tx,"diagnostic","Rust host paired; select VP8/VP9 and press Start");
            },
            "start"=>{
                if let Some(a)=active.take(){a.stop().await}
                match Settings::parse(&v) {
                    Ok(settings) if !ice.is_empty()=>{
                        match offer(settings,&ice,&tx).await {
                            Ok(a)=>active=Some(a),
                            Err(e)=>status(&tx,"error",e)
                        }
                    },
                    Ok(_)=>status(&tx,"error","Missing TURN credentials; pair again"),
                    Err(e)=>status(&tx,"error",e)
                }
            },
            "webrtc_answer"=>{
                if let Some(a)=active.as_mut() {
                    if a.video.is_some(){continue}
                    let sdp=v.get("sdp").and_then(Value::as_str).unwrap_or("").to_owned();
                    let res=async {
                        a.pc.set_remote_description(RTCSessionDescription::answer(sdp)
                            .map_err(|e|e.to_string())?).await.map_err(|e|e.to_string())?;
                        for c in a.pending.drain(..){a.pc.add_ice_candidate(c).await
                            .map_err(|e|e.to_string())?;}
                        Ok::<_,String>(())
                    }.await;
                    if let Err(e)=res{status(&tx,"error",e);continue}
                    let video_tx=tx.clone();
                    let config=a.settings.clone();
                    let video_track=a.track.clone();
                    let pc=a.pc.clone();
                    a.video=Some(tokio::spawn(async move{
                        // Important: an early keyframe sent before DTLS is connected
                        // can be dropped silently, leaving Safari unable to decode deltas.
                        let deadline=tokio::time::Instant::now()+std::time::Duration::from_secs(20);
                        loop {
                            match pc.connection_state() {
                                RTCPeerConnectionState::Connected=>break,
                                RTCPeerConnectionState::Failed|RTCPeerConnectionState::Closed=>{
                                    status(&video_tx,"error","WebRTC connection failed before first keyframe");
                                    return;
                                }
                                _=>{}
                            }
                            if tokio::time::Instant::now()>=deadline {
                                status(&video_tx,"error","WebRTC DTLS connection timeout");
                                return;
                            }
                            tokio::time::sleep(std::time::Duration::from_millis(100)).await;
                        }
                        status(&video_tx,"diagnostic","WebRTC connected; transmitting paced video from a fresh keyframe");
                        let frames=Arc::new(AtomicU64::new(0));
                        let bytes=Arc::new(AtomicU64::new(0));
                        if let Err(e)=produce(config,video_track,video_tx.clone(),frames,bytes).await{
                            status(&video_tx,"error",e)
                        }
                    }));
                }
            },
            "ice_candidate"=>{
                if let Some(a)=active.as_mut(){
                    if let Some(s)=v.get("candidate").and_then(Value::as_str) {
                        let init=RTCIceCandidateInit {
                            candidate:s.into(),
                            sdp_mid:v.get("sdpMid").and_then(Value::as_str)
                                .filter(|s|!s.is_empty()).map(str::to_string),
                            sdp_mline_index:v.get("sdpMLineIndex").and_then(Value::as_u64)
                                .and_then(|x|u16::try_from(x).ok()),
                            ..Default::default()
                        };
                        if a.pc.remote_description().await.is_some(){
                            if let Err(e)=a.pc.add_ice_candidate(init).await{
                                status(&tx,"diagnostic",format!("ICE: {e}"));
                            }
                        } else {a.pending.push(init)}
                    }
                }
            },
            "stop"=>{
                if let Some(a)=active.take(){a.stop().await}
                tell(&tx,json!({"type":"stopped"}));
            },
            "viewer_left"=>{
                if let Some(a)=active.take(){a.stop().await}
                ice.clear();
            },
            "error"=>eprintln!("Relay: {}",v.get("message").and_then(Value::as_str).unwrap_or("unknown error")),
            _=>{}
        }
    }
    if let Some(a)=active.take(){a.stop().await}
    writer.abort();
    Ok(())
}
#[tokio::main]
async fn main(){
    if let Err(e)=run().await{eprintln!("Rust StreamLab error: {e}");std::process::exit(1)}
}
#[cfg(test)]
mod tests{
    use super::*;
    #[test] fn profile_validation(){
        assert!(Settings::parse(&json!({"codec":"vp8","fps":60,"maxWidth":1280,"bitrateKbps":8000})).is_ok());
        assert!(Settings::parse(&json!({"codec":"jpeg","fps":60,"maxWidth":1280,"bitrateKbps":8000})).is_err());
        assert!(Settings::parse(&json!({"codec":"vp9","fps":999,"maxWidth":1280,"bitrateKbps":8000})).is_err());
    }
    #[test] fn parse_turn() {
        assert!(parse_ice(&json!([{"urls":["turn:example.org:3478"],"username":"x","credential":"y"}])).is_ok());
        assert!(parse_ice(&json!([{"urls":["https://example.org"]}])).is_err());
    }
}
