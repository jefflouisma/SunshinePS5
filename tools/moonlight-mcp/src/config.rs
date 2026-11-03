//! On-disk state: the client identity and the host certificates pinned at pairing.
//!
//! The directory is separate from the homelab moonlight-mcp tool so this fork never alters that
//! tool's pairing state.

use anyhow::{anyhow, Context, Result};
use std::fs;
use std::path::{Path, PathBuf};

const DIR_NAME: &str = "sunshineps5-moonlight-mcp";

#[derive(Debug, Clone)]
pub struct Paths {
    root: PathBuf,
}

impl Paths {
    /// Uses `override_dir` when given, otherwise the platform config directory.
    pub fn resolve(override_dir: Option<&Path>) -> Result<Self> {
        let root = match override_dir {
            Some(dir) => dir.to_path_buf(),
            None => dirs::config_dir()
                .ok_or_else(|| anyhow!("no platform config directory; pass --config-dir"))?
                .join(DIR_NAME),
        };
        fs::create_dir_all(root.join("hosts"))
            .with_context(|| format!("creating {}", root.display()))?;
        Ok(Self { root })
    }

    pub fn client_cert(&self) -> PathBuf {
        self.root.join("client.crt")
    }

    pub fn client_key(&self) -> PathBuf {
        self.root.join("client.key")
    }

    pub fn client_unique_id(&self) -> PathBuf {
        self.root.join("uniqueid")
    }

    /// Certificate a host presented during pairing, keyed by the host's `uniqueid`.
    pub fn host_cert(&self, host_uniqueid: &str) -> Result<PathBuf> {
        // The uniqueid comes from the network; refuse anything that could escape the directory.
        if host_uniqueid.is_empty()
            || !host_uniqueid
                .chars()
                .all(|c| c.is_ascii_alphanumeric() || c == '-')
        {
            return Err(anyhow!(
                "host uniqueid {host_uniqueid:?} is not a safe file name"
            ));
        }
        Ok(self.root.join("hosts").join(format!("{host_uniqueid}.crt")))
    }

    /// DER certificate a host presented during pairing.
    pub fn load_host_cert(&self, host_uniqueid: &str) -> Result<Vec<u8>> {
        let path = self.host_cert(host_uniqueid)?;
        let text = fs::read_to_string(&path).with_context(|| {
            format!(
                "no pinned certificate at {} — pair with this host first",
                path.display()
            )
        })?;
        Ok(pem::parse(text)?.into_contents())
    }
}
