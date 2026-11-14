//! The port's look shared by the setup window and the launcher: the app icon (a cream fedora
//! with an oxblood band on a black tile) and the palette it is drawn in.

/// Tile, rim, hat and band colours of the icon.
const TILE: [f32; 3] = [20.0, 16.0, 14.0];
const RIM: [f32; 3] = [58.0, 44.0, 36.0];
const CREAM: [f32; 3] = [236.0, 227.0, 211.0];
const BAND: [f32; 3] = [158.0, 31.0, 28.0];

/// Colour at the normalized point (u, v) in 0..1, or None outside the rounded tile.
fn shade(u: f32, v: f32) -> Option<[f32; 3]> {
    let inside_rounded = |r: f32, m: f32| {
        let cx = u.clamp(r + m, 1.0 - r - m);
        let cy = v.clamp(r + m, 1.0 - r - m);
        ((u - cx).powi(2) + (v - cy).powi(2)).sqrt() <= r
    };
    if !inside_rounded(0.19, 0.0) {
        return None;
    }
    let mut col = if inside_rounded(0.17, 0.025) { TILE } else { RIM };
    // brim
    let brim = ((u - 0.5) / 0.40).powi(2) + ((v - 0.665) / 0.075).powi(2) <= 1.0;
    // crown, with a centre crease and rounded top corners
    let top = 0.29 + 0.06 * (-((u - 0.5) / 0.075).powi(2)).exp();
    let bot = 0.665;
    let t = ((v - 0.29) / (bot - 0.29)).clamp(0.0, 1.0);
    let hw = 0.185 + 0.05 * t;
    let dx = (u - 0.5).abs();
    let mut crown = v >= top && v <= bot && dx <= hw;
    let rc = 0.06;
    let ex = hw - rc;
    if crown && dx > ex && v < 0.29 + rc && ((dx - ex).powi(2) + (v - 0.29 - rc).powi(2)).sqrt() > rc
    {
        crown = false;
    }
    if crown || brim {
        col = if crown && (0.565..=0.625).contains(&v) { BAND } else { CREAM };
    }
    Some(col)
}

/// The app icon, `n` x `n` straight-alpha RGBA (4x4 supersampled).
pub fn icon_rgba(n: usize) -> Vec<u8> {
    const SS: usize = 4;
    let mut out = vec![0u8; n * n * 4];
    for y in 0..n {
        for x in 0..n {
            let mut acc = [0.0f32; 3];
            let mut hits = 0usize;
            for j in 0..SS {
                for i in 0..SS {
                    let u = (x as f32 + (i as f32 + 0.5) / SS as f32) / n as f32;
                    let v = (y as f32 + (j as f32 + 0.5) / SS as f32) / n as f32;
                    if let Some(c) = shade(u, v) {
                        acc[0] += c[0];
                        acc[1] += c[1];
                        acc[2] += c[2];
                        hits += 1;
                    }
                }
            }
            if hits > 0 {
                let p = (y * n + x) * 4;
                let h = hits as f32;
                out[p] = (acc[0] / h).round() as u8;
                out[p + 1] = (acc[1] / h).round() as u8;
                out[p + 2] = (acc[2] / h).round() as u8;
                out[p + 3] = (255.0 * h / (SS * SS) as f32).round() as u8;
            }
        }
    }
    out
}

#[cfg(test)]
mod tests {
    #[test]
    fn icon_has_tile_and_transparent_corner() {
        let n = 64;
        let px = super::icon_rgba(n);
        assert_eq!(px.len(), n * n * 4);
        assert_eq!(px[3], 0, "corner is transparent");
        let c = (n / 2 * n + n / 2) * 4;
        assert_eq!(px[c + 3], 255, "centre is opaque");
    }
}
