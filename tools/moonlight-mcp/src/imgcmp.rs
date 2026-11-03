//! Image comparison on 8-bit planes.
//!
//! SSIM follows Wang, Bovik, Sheikh and Simoncelli, "Image Quality Assessment: From Error
//! Visibility to Structural Similarity" (IEEE TIP, 2004), with uniform 8×8 windows sampled every
//! 4 pixels instead of the paper's 11×11 Gaussian window. That keeps the cost low enough to score
//! every frame of a recording; scores are comparable between runs of this tool, not with other
//! SSIM implementations.

/// A borrowed 8-bit plane (luma or chroma).
#[derive(Debug, Clone, Copy)]
pub struct Plane<'a> {
    pub data: &'a [u8],
    pub width: usize,
    pub height: usize,
}

impl<'a> Plane<'a> {
    pub fn new(data: &'a [u8], width: usize, height: usize) -> Self {
        assert_eq!(data.len(), width * height, "plane size mismatch");
        Self {
            data,
            width,
            height,
        }
    }
}

const WINDOW: usize = 8;
const STRIDE: usize = 4;
const C1: f64 = (0.01 * 255.0) * (0.01 * 255.0);
const C2: f64 = (0.03 * 255.0) * (0.03 * 255.0);

/// Mean SSIM in [-1, 1]; 1 means identical.
pub fn ssim(a: Plane<'_>, b: Plane<'_>) -> f64 {
    assert_eq!(
        (a.width, a.height),
        (b.width, b.height),
        "plane dimensions differ"
    );
    assert!(
        a.width >= WINDOW && a.height >= WINDOW,
        "plane smaller than SSIM window"
    );
    let n = (WINDOW * WINDOW) as f64;
    let mut total = 0.0;
    let mut windows = 0usize;
    let mut y = 0;
    while y + WINDOW <= a.height {
        let mut x = 0;
        while x + WINDOW <= a.width {
            let (mut sa, mut sb, mut saa, mut sbb, mut sab) = (0.0, 0.0, 0.0, 0.0, 0.0);
            for row in y..y + WINDOW {
                let off = row * a.width + x;
                for (&pa, &pb) in a.data[off..off + WINDOW]
                    .iter()
                    .zip(&b.data[off..off + WINDOW])
                {
                    let (pa, pb) = (f64::from(pa), f64::from(pb));
                    sa += pa;
                    sb += pb;
                    saa += pa * pa;
                    sbb += pb * pb;
                    sab += pa * pb;
                }
            }
            let (ma, mb) = (sa / n, sb / n);
            let va = saa / n - ma * ma;
            let vb = sbb / n - mb * mb;
            let cov = sab / n - ma * mb;
            total += ((2.0 * ma * mb + C1) * (2.0 * cov + C2))
                / ((ma * ma + mb * mb + C1) * (va + vb + C2));
            windows += 1;
            x += STRIDE;
        }
        y += STRIDE;
    }
    total / windows as f64
}

/// Peak signal-to-noise ratio in dB. Identical planes return `f64::INFINITY`.
pub fn psnr(a: Plane<'_>, b: Plane<'_>) -> f64 {
    let mse = mean_squared_error(a, b);
    if mse == 0.0 {
        f64::INFINITY
    } else {
        10.0 * (255.0 * 255.0 / mse).log10()
    }
}

/// Mean absolute difference, used as the cheap cost function for temporal alignment.
pub fn mean_abs_diff(a: Plane<'_>, b: Plane<'_>) -> f64 {
    assert_eq!(a.data.len(), b.data.len(), "plane sizes differ");
    let sum: u64 = a
        .data
        .iter()
        .zip(b.data)
        .map(|(&x, &y)| u64::from(x.abs_diff(y)))
        .sum();
    sum as f64 / a.data.len() as f64
}

fn mean_squared_error(a: Plane<'_>, b: Plane<'_>) -> f64 {
    assert_eq!(a.data.len(), b.data.len(), "plane sizes differ");
    let sum: u64 = a
        .data
        .iter()
        .zip(b.data)
        .map(|(&x, &y)| {
            let d = u64::from(x.abs_diff(y));
            d * d
        })
        .sum();
    sum as f64 / a.data.len() as f64
}

/// Box-filter downscale by an integer factor; used to make alignment cheap.
pub fn downscale(src: Plane<'_>, factor: usize) -> (Vec<u8>, usize, usize) {
    assert!(factor >= 1);
    let (w, h) = (src.width / factor, src.height / factor);
    let mut out = Vec::with_capacity(w * h);
    let area = (factor * factor) as u32;
    for y in 0..h {
        for x in 0..w {
            let mut sum = 0u32;
            for dy in 0..factor {
                let row = (y * factor + dy) * src.width + x * factor;
                sum += src.data[row..row + factor]
                    .iter()
                    .map(|&p| u32::from(p))
                    .sum::<u32>();
            }
            out.push((sum / area) as u8);
        }
    }
    (out, w, h)
}

/// Population standard deviation of pixel values. Near zero means a flat (blank) image.
pub fn std_dev(p: Plane<'_>) -> f64 {
    let n = p.data.len() as f64;
    let mean = p.data.iter().map(|&v| f64::from(v)).sum::<f64>() / n;
    (p.data
        .iter()
        .map(|&v| (f64::from(v) - mean).powi(2))
        .sum::<f64>()
        / n)
        .sqrt()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn gradient(w: usize, h: usize, shift: u8) -> Vec<u8> {
        (0..w * h)
            .map(|i| ((i % w) as u8).wrapping_mul(3).wrapping_add(shift))
            .collect()
    }

    #[test]
    fn identical_images_score_perfectly() {
        let img = gradient(64, 32, 0);
        let p = Plane::new(&img, 64, 32);
        assert!((ssim(p, p) - 1.0).abs() < 1e-12);
        assert_eq!(psnr(p, p), f64::INFINITY);
        assert_eq!(mean_abs_diff(p, p), 0.0);
    }

    #[test]
    fn distortion_lowers_scores() {
        let a = gradient(64, 32, 0);
        let noisy: Vec<u8> = a
            .iter()
            .enumerate()
            .map(|(i, &v)| {
                if i % 2 == 0 {
                    v.saturating_add(40)
                } else {
                    v.saturating_sub(40)
                }
            })
            .collect();
        let (pa, pn) = (Plane::new(&a, 64, 32), Plane::new(&noisy, 64, 32));
        assert!(ssim(pa, pn) < 0.9);
        assert!(psnr(pa, pn) < 20.0);
    }

    #[test]
    fn psnr_of_uniform_offset_matches_formula() {
        let a = vec![100u8; 64];
        let b = vec![110u8; 64];
        let expected = 10.0 * (255.0f64 * 255.0 / 100.0).log10();
        assert!((psnr(Plane::new(&a, 8, 8), Plane::new(&b, 8, 8)) - expected).abs() < 1e-9);
    }

    #[test]
    fn downscale_averages_blocks() {
        let src = [0u8, 10, 20, 30, 40, 50, 60, 70];
        let (out, w, h) = downscale(Plane::new(&src, 4, 2), 2);
        assert_eq!((w, h), (2, 1));
        assert_eq!(out, vec![(10 + 40 + 50) / 4, (20 + 30 + 60 + 70) / 4]);
    }

    #[test]
    fn flat_image_has_zero_std_dev() {
        let flat = vec![16u8; 100];
        assert_eq!(std_dev(Plane::new(&flat, 10, 10)), 0.0);
    }
}
