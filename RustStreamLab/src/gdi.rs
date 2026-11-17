//! Windows GDI/BitBlt capture. Never touches the system agent or services.
#![cfg(windows)]
use std::{
    ffi::c_void,
    ptr::{null, null_mut},
    slice,
};

type Hdc = *mut c_void;
type Hbitmap = *mut c_void;
type Handle = *mut c_void;

#[repr(C)]
struct BitmapInfoHeader {
    size: u32,
    width: i32,
    height: i32,
    planes: u16,
    bit_count: u16,
    compression: u32,
    size_image: u32,
    xppm: i32,
    yppm: i32,
    clr_used: u32,
    clr_important: u32,
}
#[repr(C)]
struct BitmapInfo {
    header: BitmapInfoHeader,
    color: [u32; 1],
}

#[link(name = "gdi32")]
unsafe extern "system" {
    fn CreateCompatibleDC(hdc: Hdc) -> Hdc;
    fn DeleteDC(hdc: Hdc) -> i32;
    fn CreateDIBSection(
        hdc: Hdc,
        info: *const BitmapInfo,
        usage: u32,
        bits: *mut *mut c_void,
        section: Handle,
        offset: u32,
    ) -> Hbitmap;
    fn SelectObject(hdc: Hdc, object: Handle) -> Handle;
    fn DeleteObject(object: Handle) -> i32;
    fn StretchBlt(dst:Hdc, x:i32, y:i32, w:i32, h:i32,
        src:Hdc, src_x:i32, src_y:i32, src_w:i32, src_h:i32, rop:u32)->i32;
    fn SetStretchBltMode(dc:Hdc, mode:i32)->i32;
    fn BitBlt(
        dst: Hdc,
        x: i32,
        y: i32,
        w: i32,
        h: i32,
        src: Hdc,
        src_x: i32,
        src_y: i32,
        rop: u32,
    ) -> i32;
}
#[link(name = "user32")]
unsafe extern "system" {
    fn GetDC(window: Handle) -> Hdc;
    fn ReleaseDC(window: Handle, hdc: Hdc) -> i32;
    fn GetSystemMetrics(index: i32) -> i32;
}

pub struct GdiCapture {
    screen: Hdc,
    memory: Hdc,
    bitmap: Hbitmap,
    previous: Handle,
    bits: *const u8,
    raster_op: u32,
    pub width: usize,
    pub height: usize,
    pub source_width:usize,
    pub source_height:usize,
}
impl GdiCapture {
    pub fn new() -> Result<Self, String> { Self::new_with_max_width(0) }
    pub fn new_with_max_width(max_width:u32) -> Result<Self, String> {
        unsafe {
            let width = GetSystemMetrics(0);
            let height = GetSystemMetrics(1);
            if width <= 0 || height <= 0 {
                return Err("No interactive desktop".into());
            }
            // I420 requires even dimensions; capture the even rectangle.
            let source_w=width & !1;
            let source_h=height & !1;
            let w=if max_width>0 && source_w>max_width as i32 {max_width as i32&!1}else{source_w};
            let h=(((source_h as f64*w as f64/source_w as f64).round() as i32)&!1).max(2);
            let screen = GetDC(null_mut());
            if screen.is_null() {
                return Err("GetDC failed".into());
            }
            let memory = CreateCompatibleDC(screen);
            if memory.is_null() {
                ReleaseDC(null_mut(), screen);
                return Err("CreateCompatibleDC failed".into());
            }
            let info = BitmapInfo {
                header: BitmapInfoHeader {
                    size: std::mem::size_of::<BitmapInfoHeader>() as u32,
                    width: w,
                    height: -h,
                    planes: 1,
                    bit_count: 32,
                    compression: 0,
                    size_image: 0,
                    xppm: 0,
                    yppm: 0,
                    clr_used: 0,
                    clr_important: 0,
                },
                color: [0],
            };
            let mut bits: *mut c_void = null_mut();
            let bitmap = CreateDIBSection(screen, &info, 0, &mut bits, null_mut(), 0);
            if bitmap.is_null() || bits.is_null() {
                DeleteDC(memory);
                ReleaseDC(null_mut(), screen);
                return Err("CreateDIBSection failed".into());
            }
            let _=SetStretchBltMode(memory,3); // COLORONCOLOR, fast fixed-function scaling
            let previous = SelectObject(memory, bitmap);
            if previous.is_null() {
                DeleteObject(bitmap);
                DeleteDC(memory);
                ReleaseDC(null_mut(), screen);
                return Err("SelectObject failed".into());
            }
            Ok(Self {
                screen,
                memory,
                bitmap,
                previous,
                bits: bits as *const u8,
                // Default fast SRCCOPY; CAPTUREBLT is optional for layered windows.
                raster_op: if std::env::var("HI5_RUST_CAPTUREBLT").as_deref()==Ok("1") {
                    0x00CC0020 | 0x40000000
                } else { 0x00CC0020 },
                width: w as usize,
                height: h as usize,
                source_width:source_w as usize,
                source_height:source_h as usize,
            })
        }
    }
    pub fn mode(&self)->&'static str{
        if self.raster_op & 0x40000000 != 0 {"GDI CAPTUREBLT (compatibility mode)"}
        else if self.width!=self.source_width {"GDI StretchBlt SRCCOPY (scaled readback)"}
        else {"GDI BitBlt SRCCOPY (full readback)"}
    }
    pub fn grab(&mut self) -> Result<&[u8], String> {
        // The DIB buffer belongs to the capture instance and stays valid until drop.
        unsafe {
            let ok = if self.width==self.source_width && self.height==self.source_height {
                BitBlt(self.memory,0,0,self.width as i32,self.height as i32,
                    self.screen,0,0,self.raster_op)
            } else {
                // GDI scales while blitting, rather than reading full-resolution
                // pixels back from the display and then scaling in Rust.
                StretchBlt(self.memory,0,0,self.width as i32,self.height as i32,
                    self.screen,0,0,self.source_width as i32,self.source_height as i32,
                    self.raster_op)
            };
            if ok==0 {
                return Err("GDI capture failed: desktop locked or not available".into());
            }
            Ok(slice::from_raw_parts(
                self.bits,
                self.width * self.height * 4,
            ))
        }
    }
}
impl Drop for GdiCapture {
    fn drop(&mut self) {
        unsafe {
            SelectObject(self.memory, self.previous);
            DeleteObject(self.bitmap);
            DeleteDC(self.memory);
            ReleaseDC(null_mut(), self.screen);
        }
    }
}
