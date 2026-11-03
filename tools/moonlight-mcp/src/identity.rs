//! The client's long-lived identity: an RSA-2048 key and a self-signed certificate.
//!
//! Hosts identify paired clients by this certificate, so it is generated once and reused.

use anyhow::{ensure, Context, Result};
use aws_lc_rs::encoding::{AsDer, Pkcs8V1Der};
use aws_lc_rs::rand::SystemRandom;
use aws_lc_rs::rsa::{KeyPair as RsaKeyPair, KeySize, PublicKey as RsaPublicKey};
use aws_lc_rs::signature::{KeyPair, RSA_PKCS1_SHA256};
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use crate::config::Paths;

/// Common name Moonlight clients put in their certificate.
const CLIENT_CN: &str = "NVIDIA GameStream Client";

pub struct ClientIdentity {
    /// Random 64-bit value in lowercase hex, persisted, sent as `uniqueid` on every request as
    /// moonlight-qt does (protocol-notes §13.1). Hosts key pending pairings on it.
    pub unique_id: String,
    pub cert_pem: String,
    pub key_pem: String,
    pub cert_der: Vec<u8>,
    key: RsaKeyPair,
}

impl ClientIdentity {
    pub fn load_or_create(paths: &Paths) -> Result<Self> {
        let root = paths
            .client_key()
            .parent()
            .context("identity has no parent")?
            .to_path_buf();
        restrict_directory(&root)?;
        // Exclusive creation prevents concurrent first runs from mixing certificate/key/ID files.
        let lock_path = root.join("identity.lock");
        let lock_file = private_file(&lock_path)
            .context("identity locked; inspect interrupted writer before removing identity.lock")?;
        let _lock = FileGuard {
            path: Some(lock_path),
            _file: lock_file,
        };
        let files = [
            paths.client_unique_id(),
            paths.client_cert(),
            paths.client_key(),
        ];
        let mut present = 0;
        for path in &files {
            match fs::symlink_metadata(path) {
                Ok(meta) => {
                    ensure!(
                        meta.is_file() && !meta.file_type().is_symlink(),
                        "identity state must be regular files"
                    );
                    ensure!(meta.len() <= 65536, "identity file exceeds limit");
                    present += 1;
                }
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                Err(e) => return Err(e.into()),
            }
        }
        ensure!(
            present == 0 || present == 3,
            "partial client identity; restore state or select a new isolated config directory"
        );
        if present == 3 {
            restrict_permissions(&paths.client_key())?;
            return Self::from_pem(
                fs::read_to_string(&files[0])?.trim().to_owned(),
                fs::read_to_string(&files[1])?,
                fs::read_to_string(&files[2])?,
            );
        }
        let identity = Self::generate(format!("{:016x}", rand::random::<u64>()))?;
        // Commit marker (ID) goes last. A crash leaves an explicit partial-state error,
        // never an apparently paired identity silently regenerated on the next run.
        atomic_write(&paths.client_key(), identity.key_pem.as_bytes())?;
        atomic_write(&paths.client_cert(), identity.cert_pem.as_bytes())?;
        atomic_write(&paths.client_unique_id(), identity.unique_id.as_bytes())?;
        Ok(identity)
    }

    fn generate(unique_id: String) -> Result<Self> {
        let key = RsaKeyPair::generate(KeySize::Rsa2048).context("generating RSA identity")?;
        let der: Pkcs8V1Der<'_> = key.as_der().context("encoding RSA identity")?;
        let key_pem = pem::encode_config(
            &pem::Pem::new("PRIVATE KEY", der.as_ref()),
            pem::EncodeConfig::new().set_line_ending(pem::LineEnding::LF),
        );
        let key_pair = rcgen::KeyPair::from_pem(&key_pem)?;
        let mut params = rcgen::CertificateParams::new(Vec::<String>::new())?;
        params
            .distinguished_name
            .push(rcgen::DnType::CommonName, CLIENT_CN);
        params.not_before = rcgen::date_time_ymd(2020, 1, 1);
        params.not_after = rcgen::date_time_ymd(2045, 1, 1);
        let cert = params.self_signed(&key_pair)?;
        Self::from_pem(unique_id, cert.pem(), key_pem)
    }

    fn from_pem(unique_id: String, cert_pem: String, key_pem: String) -> Result<Self> {
        ensure!(
            unique_id.len() == 16 && unique_id.bytes().all(|b| b.is_ascii_hexdigit()),
            "invalid persisted client uniqueid"
        );
        let cert_der = pem::parse(&cert_pem)
            .context("parsing client certificate PEM")?
            .into_contents();
        let key_blocks = pem::parse_many(&key_pem).context("parsing client key PEM")?;
        ensure!(
            key_blocks.len() == 1 && key_blocks[0].tag() == "PRIVATE KEY",
            "expected one PKCS#8 client private key"
        );
        // AWS-LC validates the RSA private key and enforces 2048..=8192 bits.
        let key = RsaKeyPair::from_pkcs8(key_blocks[0].contents())
            .context("parsing client RSA private key")?;
        let (remaining, cert) = x509_parser::parse_x509_certificate(&cert_der)?;
        ensure!(remaining.is_empty(), "trailing certificate data");
        let public = RsaPublicKey::from_der(cert.public_key().raw)?;
        ensure!(
            public.as_ref() == key.public_key().as_ref(),
            "client certificate and RSA key do not match"
        );
        Ok(Self {
            unique_id,
            cert_pem,
            key_pem,
            cert_der,
            key,
        })
    }

    /// The signature field of the client certificate, which pairing hashes into its proofs.
    pub fn cert_signature(&self) -> Result<Vec<u8>> {
        certificate_signature(&self.cert_der)
    }

    /// RSASSA-PKCS1-v1_5 with SHA-256 over `data`.
    pub fn sign_sha256(&self, data: &[u8]) -> Result<Vec<u8>> {
        let mut signature = vec![0; self.key.public_modulus_len()];
        self.key
            .sign(
                &RSA_PKCS1_SHA256,
                &SystemRandom::new(),
                data,
                &mut signature,
            )
            .context("signing pairing proof")?;
        Ok(signature)
    }
}

/// Extracts the raw signature bytes from a DER certificate.
pub fn certificate_signature(cert_der: &[u8]) -> Result<Vec<u8>> {
    let (remaining, cert) =
        x509_parser::parse_x509_certificate(cert_der).context("parsing X.509 certificate")?;
    ensure!(
        remaining.is_empty() && cert.signature_value.unused_bits == 0,
        "invalid certificate encoding"
    );
    Ok(cert.signature_value.data.to_vec())
}

#[cfg(unix)]
fn restrict_permissions(path: &std::path::Path) -> Result<()> {
    use std::os::unix::fs::PermissionsExt;
    fs::set_permissions(path, fs::Permissions::from_mode(0o600))?;
    Ok(())
}

#[cfg(not(unix))]
fn restrict_permissions(_path: &std::path::Path) -> Result<()> {
    Ok(())
}

struct FileGuard {
    path: Option<PathBuf>,
    _file: fs::File,
}
impl Drop for FileGuard {
    fn drop(&mut self) {
        if let Some(path) = &self.path {
            if let Err(error) = fs::remove_file(path) {
                eprintln!(
                    "cannot remove state lock/staging file {}: {error}",
                    path.display()
                );
            }
        }
    }
}
fn private_file(path: &Path) -> Result<fs::File> {
    let mut options = fs::OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }
    Ok(options.open(path)?)
}
#[cfg(unix)]
fn restrict_directory(path: &Path) -> Result<()> {
    use std::os::unix::fs::PermissionsExt;
    fs::set_permissions(path, fs::Permissions::from_mode(0o700))?;
    Ok(())
}
#[cfg(not(unix))]
fn restrict_directory(_path: &Path) -> Result<()> {
    Ok(())
}

pub(crate) fn atomic_write(path: &Path, bytes: &[u8]) -> Result<()> {
    let parent = path.parent().context("state path has no parent")?;
    if let Ok(meta) = fs::symlink_metadata(path) {
        ensure!(
            meta.is_file() && !meta.file_type().is_symlink(),
            "state path is not a regular file"
        );
    }
    let temporary = parent.join(format!(".state-{}", uuid::Uuid::new_v4()));
    let mut guard = FileGuard {
        _file: private_file(&temporary)?,
        path: Some(temporary),
    };
    guard._file.write_all(bytes)?;
    guard._file.sync_all()?;
    fs::rename(guard.path.as_ref().expect("staging path"), path)?;
    guard.path = None;
    fs::File::open(parent)?.sync_all()?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use aws_lc_rs::signature::{UnparsedPublicKey, RSA_PKCS1_2048_8192_SHA256};
    use sha2::{Digest, Sha256};

    const LEGACY_CERT: &str = include_str!("../tests/fixtures/rsa-legacy/certificate.pem.txt");
    const LEGACY_KEY: &str = include_str!("../tests/fixtures/rsa-legacy/private-key.pem.txt");
    const LEGACY_ID: &str = include_str!("../tests/fixtures/rsa-legacy/uniqueid");
    const LEGACY_MESSAGE: &[u8] = b"SunshinePS5 legacy RSA compatibility fixture\n";

    #[test]
    fn legacy_rustcrypto_identity_and_signature_are_unchanged() {
        let dir = std::env::temp_dir().join(format!("mlmcp-legacy-{}", uuid::Uuid::new_v4()));
        let paths = Paths::resolve(Some(&dir)).unwrap();
        atomic_write(&paths.client_key(), LEGACY_KEY.as_bytes()).unwrap();
        atomic_write(&paths.client_cert(), LEGACY_CERT.as_bytes()).unwrap();
        atomic_write(&paths.client_unique_id(), LEGACY_ID.as_bytes()).unwrap();
        let identity = ClientIdentity::load_or_create(&paths).unwrap();
        assert_eq!(identity.unique_id, LEGACY_ID);
        assert_eq!(identity.cert_pem, LEGACY_CERT);
        assert_eq!(identity.key_pem, LEGACY_KEY);
        assert_eq!(
            hex::encode(Sha256::digest(&identity.cert_der)),
            "cc709ba107aefac086781c58627ec068a8e54ff81b7b323e631207c08368df8e"
        );
        assert_eq!(
            identity.cert_der,
            pem::parse(LEGACY_CERT).unwrap().contents()
        );
        let expected =
            hex::decode(include_str!("../tests/fixtures/rsa-legacy/signature.hex").trim()).unwrap();
        assert_eq!(identity.sign_sha256(LEGACY_MESSAGE).unwrap(), expected);
        let verifier = UnparsedPublicKey::new(
            &RSA_PKCS1_2048_8192_SHA256,
            identity.key.public_key().as_ref(),
        );
        verifier.verify(LEGACY_MESSAGE, &expected).unwrap();
        assert!(verifier.verify(b"changed message", &expected).is_err());
        let mut changed = expected.clone();
        changed[0] ^= 1;
        assert!(verifier.verify(LEGACY_MESSAGE, &changed).is_err());
        assert!(verifier.verify(LEGACY_MESSAGE, &expected[..255]).is_err());
        assert!(verifier.verify(LEGACY_MESSAGE, &[]).is_err());
        let wrong = ClientIdentity::generate("0123456789abcdef".into()).unwrap();
        assert!(UnparsedPublicKey::new(
            &RSA_PKCS1_2048_8192_SHA256,
            wrong.key.public_key().as_ref()
        )
        .verify(LEGACY_MESSAGE, &expected)
        .is_err());
        assert_eq!(fs::read(paths.client_key()).unwrap(), LEGACY_KEY.as_bytes());
        assert_eq!(
            fs::read(paths.client_cert()).unwrap(),
            LEGACY_CERT.as_bytes()
        );
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn invalid_private_key_encodings_fail_closed() {
        for key in [
            LEGACY_KEY.replace("PRIVATE KEY", "RSA PRIVATE KEY"),
            format!("{LEGACY_KEY}{LEGACY_KEY}"),
            pem::encode(&pem::Pem::new("PRIVATE KEY", [0_u8; 32])),
            LEGACY_KEY[..LEGACY_KEY.len() / 2].to_owned(),
        ] {
            assert!(ClientIdentity::from_pem(LEGACY_ID.into(), LEGACY_CERT.into(), key).is_err());
        }
        let mut cert = pem::parse(LEGACY_CERT).unwrap().into_contents();
        cert.push(0);
        assert!(ClientIdentity::from_pem(
            LEGACY_ID.into(),
            pem::encode(&pem::Pem::new("CERTIFICATE", cert)),
            LEGACY_KEY.into()
        )
        .is_err());
    }

    #[test]
    fn undersized_rsa_identity_is_rejected() {
        let weak = include_str!("../tests/fixtures/rsa-legacy/weak-private-key.pem.txt");
        let error = ClientIdentity::from_pem(LEGACY_ID.into(), LEGACY_CERT.into(), weak.into())
            .err()
            .unwrap();
        assert!(error.to_string().contains("parsing client RSA private key"));
    }

    #[test]
    fn generated_identity_round_trips_and_signs() {
        let dir = std::env::temp_dir().join(format!("mlmcp-id-{}", uuid::Uuid::new_v4()));
        let paths = Paths::resolve(Some(&dir)).unwrap();
        let first = ClientIdentity::load_or_create(&paths).unwrap();
        let second = ClientIdentity::load_or_create(&paths).unwrap();
        assert_eq!(first.cert_der, second.cert_der, "identity must persist");
        assert_eq!(first.unique_id, second.unique_id);
        assert_eq!(first.unique_id.len(), 16);
        assert_eq!(
            first.cert_signature().unwrap().len(),
            256,
            "RSA-2048 signature"
        );
        assert_eq!(first.sign_sha256(b"x").unwrap().len(), 256);
        fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn damaged_or_partial_identity_is_never_silently_replaced() {
        let dir = std::env::temp_dir().join(format!("mlmcp-damaged-{}", uuid::Uuid::new_v4()));
        let paths = Paths::resolve(Some(&dir)).unwrap();
        let identity = ClientIdentity::load_or_create(&paths).unwrap();
        for path in [
            paths.client_cert(),
            paths.client_key(),
            paths.client_unique_id(),
        ] {
            let saved = fs::read(&path).unwrap();
            fs::remove_file(&path).unwrap();
            assert!(ClientIdentity::load_or_create(&paths).is_err());
            assert!(!path.exists());
            atomic_write(&path, &saved).unwrap();
            atomic_write(&path, b"corrupt").unwrap();
            assert!(ClientIdentity::load_or_create(&paths).is_err());
            assert_eq!(fs::read(&path).unwrap(), b"corrupt");
            atomic_write(&path, &saved).unwrap();
        }
        let other = ClientIdentity::generate("0123456789abcdef".into()).unwrap();
        atomic_write(&paths.client_key(), other.key_pem.as_bytes()).unwrap();
        assert!(ClientIdentity::load_or_create(&paths).is_err());
        atomic_write(&paths.client_key(), identity.key_pem.as_bytes()).unwrap();
        assert_eq!(
            ClientIdentity::load_or_create(&paths).unwrap().cert_der,
            identity.cert_der
        );
        let lock = private_file(&dir.join("identity.lock")).unwrap();
        assert!(ClientIdentity::load_or_create(&paths).is_err());
        drop(lock);
        fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    #[cfg(unix)]
    fn identity_files_are_private_and_symlinks_are_refused() {
        use std::os::unix::fs::PermissionsExt;
        let dir = std::env::temp_dir().join(format!("mlmcp-private-{}", uuid::Uuid::new_v4()));
        let paths = Paths::resolve(Some(&dir)).unwrap();
        ClientIdentity::load_or_create(&paths).unwrap();
        assert_eq!(
            fs::metadata(paths.client_key())
                .unwrap()
                .permissions()
                .mode()
                & 0o777,
            0o600
        );
        assert_eq!(
            fs::metadata(&dir).unwrap().permissions().mode() & 0o777,
            0o700
        );
        fs::remove_file(paths.client_key()).unwrap();
        std::os::unix::fs::symlink(paths.client_cert(), paths.client_key()).unwrap();
        assert!(ClientIdentity::load_or_create(&paths).is_err());
        fs::remove_dir_all(dir).unwrap();
    }
}
