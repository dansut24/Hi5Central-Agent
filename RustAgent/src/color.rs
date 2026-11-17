/// Converts tightly packed, top-down BGRA8 pixels to planar I420 (Y then U then V).
/// Dimensions must be even. Input/output buffers are reused by the benchmark loop.
pub fn bgra_to_i420(src: &[u8], width: usize, height: usize, out: &mut [u8]) {
    assert!(width > 0 && height > 0 && width % 2 == 0 && height % 2 == 0);
    assert!(src.len() >= width * height * 4);
    assert!(out.len() >= width * height * 3 / 2);
    let n = width * height;
    let (y_plane, chroma) = out.split_at_mut(n);
    let (u_plane, v_plane) = chroma.split_at_mut(n / 4);

    // BT.601 studio-range transform. Clamp explicitly to prevent wraparound.
    #[inline]
    fn yuv(px: &[u8]) -> (u8, i32, i32) {
        let b = px[0] as i32;
        let g = px[1] as i32;
        let r = px[2] as i32;
        let y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
        let u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
        let v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
        (y.clamp(16, 235) as u8, u, v)
    }

    for row in (0..height).step_by(2) {
        for col in (0..width).step_by(2) {
            let positions = [
                row * width + col,
                row * width + col + 1,
                (row + 1) * width + col,
                (row + 1) * width + col + 1,
            ];
            let mut su = 0;
            let mut sv = 0;
            for p in positions {
                let (y, u, v) = yuv(&src[p * 4..p * 4 + 4]);
                y_plane[p] = y;
                su += u;
                sv += v;
            }
            let chroma_p = (row / 2) * (width / 2) + col / 2;
            u_plane[chroma_p] = ((su + 2) / 4).clamp(16, 240) as u8;
            v_plane[chroma_p] = ((sv + 2) / 4).clamp(16, 240) as u8;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn black_2x2() {
        let src = vec![0; 16];
        let mut dst = vec![0; 6];
        bgra_to_i420(&src, 2, 2, &mut dst);
        assert_eq!(&dst[..4], &[16, 16, 16, 16]);
        assert_eq!(&dst[4..], &[128, 128]);
    }
    #[test]
    fn white_4x2() {
        let mut src = vec![255; 32];
        for a in src.chunks_exact_mut(4) {
            a[3] = 255;
        }
        let mut dst = vec![0; 12];
        bgra_to_i420(&src, 4, 2, &mut dst);
        assert!(dst[..8].iter().all(|&v| v == 235));
        assert!(dst[8..].iter().all(|&v| v == 128));
    }
    #[test]
    fn chroma_size() {
        let src = vec![70; 1280 * 720 * 4];
        let mut dst = vec![0; 1280 * 720 * 3 / 2];
        bgra_to_i420(&src, 1280, 720, &mut dst);
        assert_eq!(dst.len(), 1_382_400);
    }
}
