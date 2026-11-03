//! Distribution summaries used in every report.

use serde::Serialize;

#[derive(Debug, Clone, Copy, Serialize, PartialEq)]
pub struct Summary {
    pub count: usize,
    pub min: f64,
    pub mean: f64,
    pub p50: f64,
    pub p95: f64,
    pub p99: f64,
    pub max: f64,
}

impl Summary {
    /// Returns `None` for an empty sample set. Percentiles use the nearest-rank method, so a
    /// reported p99 is always a value that was actually observed.
    pub fn of(samples: &[f64]) -> Option<Self> {
        if samples.is_empty() {
            return None;
        }
        let mut sorted = samples.to_vec();
        sorted.sort_by(f64::total_cmp);
        let rank = |p: f64| {
            let idx = ((p / 100.0) * sorted.len() as f64).ceil() as usize;
            sorted[idx.clamp(1, sorted.len()) - 1]
        };
        Some(Self {
            count: sorted.len(),
            min: sorted[0],
            mean: sorted.iter().sum::<f64>() / sorted.len() as f64,
            p50: rank(50.0),
            p95: rank(95.0),
            p99: rank(99.0),
            max: sorted[sorted.len() - 1],
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nearest_rank_percentiles() {
        let samples: Vec<f64> = (1..=100).map(f64::from).collect();
        let s = Summary::of(&samples).unwrap();
        assert_eq!((s.p50, s.p95, s.p99, s.max), (50.0, 95.0, 99.0, 100.0));
        assert_eq!(s.mean, 50.5);
    }

    #[test]
    fn single_and_empty() {
        assert!(Summary::of(&[]).is_none());
        let s = Summary::of(&[7.0]).unwrap();
        assert_eq!((s.min, s.p99, s.max), (7.0, 7.0, 7.0));
    }
}

/// Exact nearest-rank summaries backed by bounded sorted disk runs. Only 4096 values
/// and one head per run are resident; neither raw frames nor all scores are retained.
/// Nonfinite values are rejected: callers represent perfect-plane PSNR separately.
pub struct DiskSummary {
    root: std::path::PathBuf,
    pending: Vec<f64>,
    runs: usize,
    count: usize,
    sum: f64,
}

impl DiskSummary {
    pub fn new(parent: &std::path::Path) -> anyhow::Result<Self> {
        let root = parent.join(format!("summary-{}", uuid::Uuid::new_v4()));
        let mut builder = std::fs::DirBuilder::new();
        #[cfg(unix)]
        {
            use std::os::unix::fs::DirBuilderExt;
            builder.mode(0o700);
        }
        builder.create(&root)?;
        Ok(Self {
            root,
            pending: Vec::with_capacity(4096),
            runs: 0,
            count: 0,
            sum: 0.0,
        })
    }

    pub fn push(&mut self, value: f64) -> anyhow::Result<()> {
        anyhow::ensure!(value.is_finite(), "nonfinite distribution sample");
        self.pending.push(value);
        self.count = self
            .count
            .checked_add(1)
            .ok_or_else(|| anyhow::anyhow!("sample count overflow"))?;
        self.sum += value;
        anyhow::ensure!(self.sum.is_finite(), "distribution sum overflow");
        if self.pending.len() == 4096 {
            self.flush()?;
        }
        Ok(())
    }

    fn flush(&mut self) -> anyhow::Result<()> {
        use std::io::Write;
        if self.pending.is_empty() {
            return Ok(());
        }
        // The recording reader currently bounds the population to 36,002 frames.
        // Keep a separate finite file/merge bound if that reader changes later.
        anyhow::ensure!(self.runs < 256, "exact summary disk-run budget exhausted");
        self.pending.sort_by(f64::total_cmp);
        let mut options = std::fs::OpenOptions::new();
        options.write(true).create_new(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.mode(0o600);
        }
        let mut file =
            std::io::BufWriter::new(options.open(self.root.join(self.runs.to_string()))?);
        for value in self.pending.drain(..) {
            file.write_all(&value.to_le_bytes())?;
        }
        file.flush()?;
        self.runs += 1;
        Ok(())
    }

    pub fn finish(mut self) -> anyhow::Result<Option<Summary>> {
        use std::io::Read;
        if self.count == 0 {
            return Ok(None);
        }
        self.flush()?;
        let mut readers = (0..self.runs)
            .map(|i| {
                std::fs::File::open(self.root.join(i.to_string())).map(std::io::BufReader::new)
            })
            .collect::<std::io::Result<Vec<_>>>()?;
        fn next(reader: &mut impl Read) -> std::io::Result<Option<f64>> {
            let mut bytes = [0; 8];
            if reader.read(&mut bytes[..1])? == 0 {
                return Ok(None);
            }
            reader.read_exact(&mut bytes[1..])?;
            Ok(Some(f64::from_le_bytes(bytes)))
        }
        let mut heads = readers
            .iter_mut()
            .map(next)
            .collect::<std::io::Result<Vec<_>>>()?;
        let ranks = [
            1,
            self.count.div_ceil(2),
            (self.count * 95).div_ceil(100),
            (self.count * 99).div_ceil(100),
            self.count,
        ];
        let mut values = [0.0; 5];
        for rank in 1..=self.count {
            let (i, value) = heads
                .iter()
                .enumerate()
                .filter_map(|(i, v)| v.map(|v| (i, v)))
                .min_by(|a, b| a.1.total_cmp(&b.1))
                .ok_or_else(|| anyhow::anyhow!("summary run truncated"))?;
            for (k, wanted) in ranks.iter().enumerate() {
                if rank == *wanted {
                    values[k] = value;
                }
            }
            heads[i] = next(&mut readers[i])?;
        }
        Ok(Some(Summary {
            count: self.count,
            min: values[0],
            mean: self.sum / self.count as f64,
            p50: values[1],
            p95: values[2],
            p99: values[3],
            max: values[4],
        }))
    }
}

impl Drop for DiskSummary {
    fn drop(&mut self) {
        if let Err(error) = std::fs::remove_dir_all(&self.root) {
            eprintln!("exact-summary scratch cleanup failed: {error}");
        }
    }
}

#[cfg(test)]
mod disk_tests {
    use super::*;
    #[test]
    fn disk_ranks_match_full_population_across_runs_and_reject_nonfinite() {
        let mut disk = DiskSummary::new(&std::env::temp_dir()).unwrap();
        assert!(disk.push(f64::NAN).is_err());
        let values: Vec<_> = (0..12511).rev().map(|v| f64::from(v % 1007)).collect();
        for &value in &values {
            disk.push(value).unwrap();
        }
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(
                std::fs::metadata(&disk.root).unwrap().permissions().mode() & 0o777,
                0o700
            );
            assert_eq!(
                std::fs::metadata(disk.root.join("0"))
                    .unwrap()
                    .permissions()
                    .mode()
                    & 0o777,
                0o600
            );
        }
        assert_eq!(disk.finish().unwrap(), Summary::of(&values));
        assert!(DiskSummary::new(&std::env::temp_dir())
            .unwrap()
            .finish()
            .unwrap()
            .is_none());
    }
}
