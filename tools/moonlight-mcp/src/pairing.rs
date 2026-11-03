//! Client side of GameStream PIN pairing, with every check an official client performs.
//!
//! The PIN authenticates both sides: the host proves it knows the PIN by producing a hash the
//! client can verify, and signs its secret with the certificate it sent in the first step. A test
//! client that skipped those checks could not detect a broken host implementation, so every one
//! is enforced here. Sequence, layouts, and checks: docs/protocol-notes.md §13.

use anyhow::{anyhow, bail, ensure, Context, Result};
use aws_lc_rs::rsa::PublicKey as RsaPublicKey;
use aws_lc_rs::signature::{UnparsedPublicKey, RSA_PKCS1_2048_8192_SHA256};
use rand::{Rng, RngCore};
use sha2::{Digest, Sha256};
use std::time::Duration;

use crate::config::Paths;
use crate::crypto::{aes_ecb_decrypt, aes_ecb_encrypt};
use crate::http::{self as nvhttp, base_query, Host, ServerInfo, XmlResponse};
use crate::identity::{certificate_signature, ClientIdentity};
use crate::tls::{self, ServerTrust};

/// Device name moonlight-qt hardcodes in pairing requests (§13.2).
const DEVICE_NAME: &str = "roth";

/// Hosts hold the first pairing response open until the PIN is entered, for up to 5 minutes on
/// Sunshine (§13.6); wait a little longer than that.
const PIN_ENTRY_TIMEOUT: Duration = Duration::from_secs(330);

/// How the PIN reaches the host. With a real Moonlight client a person types the PIN the client
/// shows into the host's UI; these strategies automate or delegate that step.
#[derive(Debug, Clone)]
pub enum PinDelivery {
    /// Print the PIN and wait for a person to enter it on the host (SunshinePS5's web app).
    Manual,
    /// Run a shell command with the PIN in `MOONLIGHT_PIN`, for hosts with a scriptable PIN API.
    Command(String),
    /// Wolf's API (the homelab test host): read the pending secret, then post PIN and secret.
    Wolf,
    /// Explicitly trusted local web-UI certificate, separate from the GameStream pin.
    SunshineApiPinned {
        url: String,
        username: String,
        password: String,
        certificate_der: Vec<u8>,
    },
}

impl PinDelivery {
    fn validate(&self) -> Result<()> {
        match self {
            Self::Command(command) => {
                ensure!(!command.trim().is_empty(), "PIN helper command is empty")
            }
            Self::SunshineApiPinned {
                url,
                certificate_der,
                ..
            } => {
                let parsed = reqwest::Url::parse(url)?;
                ensure!(
                    parsed.scheme() == "https"
                        && parsed.host_str().is_some()
                        && parsed.username().is_empty()
                        && parsed.password().is_none()
                        && parsed.query().is_none()
                        && parsed.fragment().is_none(),
                    "Sunshine PIN API requires HTTPS without URL credentials/query/fragment"
                );
                certificate_signature(certificate_der)?;
            }
            _ => {}
        }
        Ok(())
    }
}

/// Generates a 4-digit PIN, as Moonlight clients do.
pub fn random_pin() -> String {
    format!("{:04}", rand::thread_rng().gen_range(0..10_000))
}

/// Runs the full pairing exchange and stores the host certificate on success.
pub async fn pair(
    paths: &Paths,
    identity: &ClientIdentity,
    host: &Host,
    pin: &str,
    delivery: &PinDelivery,
) -> Result<ServerInfo> {
    ensure!(
        pin.len() == 4 && pin.bytes().all(|b| b.is_ascii_digit()),
        "PIN must contain exactly four ASCII digits"
    );
    delivery.validate()?;
    paths.host_cert(&host.pin_id())?;
    let info = nvhttp::server_info_http(host, &identity.unique_id).await?;
    match info.major_version() {
        Some(v) if v >= 7 => {}
        other => bail!(
            "host app version {:?} (major {other:?}) predates SHA-256 pairing; not supported",
            info.app_version
        ),
    }
    let result = async {
        let (server_cert_pem, ()) = tokio::time::timeout(PIN_ENTRY_TIMEOUT, async {
            tokio::try_join!(
                exchange(paths, identity, host, &info, pin),
                deliver_pin(host, pin, delivery)
            )
        })
        .await
        .context("pairing/PIN delivery deadline exceeded")??;
        confirm_and_store(paths, identity, host, &info, &server_cert_pem).await
    }
    .await;
    if result.is_err() {
        // moonlight-qt unpairs after any failure. Sunshine has no /unpair route and erases the
        // pending session itself, so this is best effort.
        let _ = unpair(host, &identity.unique_id).await;
    }
    result.map(|()| info)
}

fn pair_query(unique_id: &str, extra: &str) -> String {
    format!(
        "/pair?{}&devicename={DEVICE_NAME}&updateState=1&{extra}",
        base_query(unique_id)
    )
}

async fn exchange(
    paths: &Paths,
    identity: &ClientIdentity,
    host: &Host,
    info: &ServerInfo,
    pin: &str,
) -> Result<String> {
    let uid = identity.unique_id.as_str();
    let salt: [u8; 16] = random_bytes();
    let key = derive_key(&salt, pin);

    // Step 1: exchange certificates. The host holds this response until the PIN is entered on
    // its side. Sunshine's PIN API in turn blocks until steps 2-4 finish, so the delivery runs
    // as its own task and is joined at the end.
    let step1_url = host.http_url(&pair_query(
        uid,
        &format!(
            "phrase=getservercert&salt={}&clientcert={}",
            hex::encode(salt),
            hex::encode(identity.cert_pem.as_bytes())
        ),
    ));
    let long_client = tls::http_client(PIN_ENTRY_TIMEOUT)?;
    let xml = nvhttp::get_xml(&long_client, &step1_url).await?;
    require_paired(&xml, "getservercert")?;
    let plaincert = xml.require("plaincert")?;
    if plaincert.is_empty() {
        bail!("host returned no certificate; another pairing attempt is already in progress");
    }
    let server_cert_pem = String::from_utf8(hex::decode(plaincert)?)?;
    let server_cert_der = parse_host_certificate(&server_cert_pem)?;
    check_existing_pin(paths, &info.uniqueid, &server_cert_der)?;
    check_existing_pin(paths, &host.pin_id(), &server_cert_der)?;
    let server_cert_sig = certificate_signature(&server_cert_der)?;
    let server_key = server_public_key(&server_cert_der)?;

    let http = tls::http_client(Duration::from_secs(30))?;

    // Step 2: client challenge. Decrypted response: hash(32) ‖ server challenge(16).
    let client_challenge: [u8; 16] = random_bytes();
    let xml = nvhttp::get_xml(
        &http,
        &host.http_url(&pair_query(
            uid,
            &format!(
                "clientchallenge={}",
                hex::encode(aes_ecb_encrypt(&key, &client_challenge)?)
            ),
        )),
    )
    .await?;
    require_paired(&xml, "clientchallenge")?;
    let response = aes_ecb_decrypt(&key, &hex::decode(xml.require("challengeresponse")?)?)?;
    if response.len() < 48 {
        bail!(
            "challenge response is {} bytes, expected at least 48",
            response.len()
        );
    }
    let (server_hash, server_challenge) = (&response[..32], &response[32..48]);

    // Step 3: answer the server challenge. Response: server secret(16) ‖ signature.
    let client_secret: [u8; 16] = random_bytes();
    let answer = Sha256::new()
        .chain_update(server_challenge)
        .chain_update(identity.cert_signature()?)
        .chain_update(client_secret)
        .finalize();
    let xml = nvhttp::get_xml(
        &http,
        &host.http_url(&pair_query(
            uid,
            &format!(
                "serverchallengeresp={}",
                hex::encode(aes_ecb_encrypt(&key, &answer)?)
            ),
        )),
    )
    .await?;
    require_paired(&xml, "serverchallengeresp")?;
    let pairing_secret = hex::decode(xml.require("pairingsecret")?)?;
    if pairing_secret.len() <= 16 {
        bail!(
            "pairing secret is {} bytes, too short to hold a signature",
            pairing_secret.len()
        );
    }
    let (server_secret, server_signature) = pairing_secret.split_at(16);
    // Check A: the secret is signed by the certificate from step 1.
    UnparsedPublicKey::new(&RSA_PKCS1_2048_8192_SHA256, server_key.as_ref())
        .verify(server_secret, server_signature)
        .map_err(|_| {
            anyhow!("host's pairing secret is not signed by its certificate (possible MITM)")
        })?;
    // Check B: the host's hash proves it derived the same key, i.e. the PIN matched.
    let expected = Sha256::new()
        .chain_update(client_challenge)
        .chain_update(&server_cert_sig)
        .chain_update(server_secret)
        .finalize();
    if expected.as_slice() != server_hash {
        bail!("host's challenge hash does not match: the PIN entered on the host was wrong");
    }

    // Step 4: send our signed secret.
    let mut client_pairing_secret = client_secret.to_vec();
    client_pairing_secret.extend(identity.sign_sha256(&client_secret)?);
    let xml = nvhttp::get_xml(
        &http,
        &host.http_url(&pair_query(
            uid,
            &format!("clientpairingsecret={}", hex::encode(client_pairing_secret)),
        )),
    )
    .await?;
    require_paired(&xml, "clientpairingsecret")?;

    Ok(server_cert_pem)
}

/// The final official-client check uses mutual TLS pinned to the proved certificate.
/// Persist only after both TLS authentication and `<paired>1</paired>` succeed (§13.5).
pub async fn confirm_and_store(
    paths: &Paths,
    identity: &ClientIdentity,
    host: &Host,
    info: &ServerInfo,
    server_cert_pem: &str,
) -> Result<()> {
    let server_cert_der = parse_host_certificate(server_cert_pem)?;
    check_existing_pin(paths, &info.uniqueid, &server_cert_der)?;
    check_existing_pin(paths, &host.pin_id(), &server_cert_der)?;
    let https = tls::https_client(
        identity,
        ServerTrust::Pinned(server_cert_der),
        Duration::from_secs(30),
    )?;
    let xml = nvhttp::get_xml(
        &https,
        &host.https_url(
            info.https_port,
            &pair_query(&identity.unique_id, "phrase=pairchallenge"),
        ),
    )
    .await?;
    require_paired(&xml, "pairchallenge")?;
    crate::identity::atomic_write(
        &paths.host_cert(&info.uniqueid)?,
        server_cert_pem.as_bytes(),
    )?;
    crate::identity::atomic_write(
        &paths.host_cert(&host.pin_id())?,
        server_cert_pem.as_bytes(),
    )?;
    Ok(())
}

fn parse_host_certificate(pem_text: &str) -> Result<Vec<u8>> {
    let blocks = pem::parse_many(pem_text)?;
    ensure!(
        blocks.len() == 1 && blocks[0].tag() == "CERTIFICATE",
        "expected one host certificate"
    );
    let der = blocks[0].contents().to_vec();
    certificate_signature(&der)?;
    server_public_key(&der)?;
    Ok(der)
}
fn check_existing_pin(paths: &Paths, id: &str, der: &[u8]) -> Result<()> {
    let path = paths.host_cert(id)?;
    match std::fs::symlink_metadata(&path) {
        Ok(meta) => {
            ensure!(
                meta.is_file() && !meta.file_type().is_symlink() && meta.len() <= 65536,
                "invalid stored host pin"
            );
            ensure!(
                paths.load_host_cert(id)? == der,
                "host identity changed; explicitly remove old pairing state before pairing again"
            );
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
        Err(e) => return Err(e.into()),
    }
    Ok(())
}

/// Unattended callers must reject manual delivery before making any network request.
pub async fn pair_unattended(
    paths: &Paths,
    identity: &ClientIdentity,
    host: &Host,
    pin: &str,
    delivery: &PinDelivery,
) -> Result<ServerInfo> {
    ensure!(
        !matches!(delivery, PinDelivery::Manual),
        "manual PIN delivery is forbidden in unattended runs"
    );
    pair(paths, identity, host, pin, delivery).await
}

/// Resolve only separately supplied local configuration. Scenario text cannot carry commands,
/// credentials, or an implicit fallback to a person entering a PIN.
pub fn scenario_delivery(
    pairing: &crate::scenario::Pairing,
    providers: &std::collections::HashMap<String, PinDelivery>,
) -> Result<Option<PinDelivery>> {
    use crate::scenario::PinStrategy;
    if pairing.pin_delivery == PinStrategy::AlreadyPaired {
        return Ok(None);
    }
    let provider = providers
        .get(
            pairing
                .provider_ref
                .as_deref()
                .context("missing provider_ref")?,
        )
        .context("PIN provider is not configured")?;
    ensure!(
        matches!(
            (pairing.pin_delivery, provider),
            (PinStrategy::Command, PinDelivery::Command(_))
                | (PinStrategy::Wolf, PinDelivery::Wolf)
                | (PinStrategy::Sunshine, PinDelivery::SunshineApiPinned { .. })
        ),
        "PIN provider strategy mismatch or manual provider"
    );
    provider.validate()?;
    Ok(Some(provider.clone()))
}

// `ThreadRng` is not `Send`, so randomness is drawn here rather than held across awaits.
fn random_bytes<const N: usize>() -> [u8; N] {
    let mut buf = [0u8; N];
    rand::thread_rng().fill_bytes(&mut buf);
    buf
}

/// AES key for pairing: the first 16 bytes of SHA-256(salt ‖ PIN) (§13.4).
fn derive_key(salt: &[u8; 16], pin: &str) -> [u8; 16] {
    let digest = Sha256::new()
        .chain_update(salt)
        .chain_update(pin.as_bytes())
        .finalize();
    digest[..16].try_into().expect("16 bytes")
}

fn require_paired(xml: &XmlResponse, step: &str) -> Result<()> {
    match xml.get("paired") {
        Some("1") => Ok(()),
        other => bail!("host rejected pairing at {step} (paired={other:?})"),
    }
}

fn server_public_key(cert_der: &[u8]) -> Result<RsaPublicKey> {
    let (_, cert) = x509_parser::parse_x509_certificate(cert_der)?;
    RsaPublicKey::from_der(cert.public_key().raw)
        .context("host certificate does not carry an RSA public key")
}

pub async fn unpair(host: &Host, unique_id: &str) -> Result<()> {
    let client = tls::http_client(Duration::from_secs(10))?;
    nvhttp::get_xml(
        &client,
        &host.http_url(&format!("/unpair?{}", base_query(unique_id))),
    )
    .await?;
    Ok(())
}

async fn deliver_pin(host: &Host, pin: &str, delivery: &PinDelivery) -> Result<()> {
    match delivery {
        PinDelivery::Manual => {
            eprintln!("Enter PIN {pin} on the host to complete pairing.");
            Ok(())
        }
        PinDelivery::Command(cmd) => {
            // Give the host a moment to register the pending request before the PIN arrives.
            tokio::time::sleep(Duration::from_millis(500)).await;
            let status = tokio::process::Command::new("sh")
                .args(["-c", cmd])
                .env("MOONLIGHT_PIN", pin)
                .env("MOONLIGHT_HOST", &host.address)
                .kill_on_drop(true)
                .status()
                .await?;
            if !status.success() {
                bail!("PIN command exited with {status}");
            }
            Ok(())
        }
        PinDelivery::SunshineApiPinned {
            url,
            username,
            password,
            ..
        } => {
            let PinDelivery::SunshineApiPinned {
                certificate_der, ..
            } = delivery
            else {
                unreachable!()
            };
            let client = tls::pinned_server_client(certificate_der.clone(), PIN_ENTRY_TIMEOUT)?;
            let api = format!("{}/api/pin", url.trim_end_matches('/'));
            let pairing_id = poll_until(|| async {
                let list = nvhttp::json_response(
                    client
                        .get(&api)
                        .basic_auth(username, Some(password))
                        .send()
                        .await
                        .map_err(reqwest::Error::without_url)?,
                )
                .await?;
                let pending: Vec<&serde_json::Value> = list["pairings"]
                    .as_array()
                    .map(|a| a.iter().filter(|p| p["name"] == DEVICE_NAME).collect())
                    .unwrap_or_default();
                match pending.as_slice() {
                    [] => Ok(None),
                    [one] => Ok(one["id"].as_str().map(str::to_owned)),
                    _ => bail!(
                        "several pending pairings named {DEVICE_NAME:?}; cannot tell which is ours"
                    ),
                }
            })
            .await?;
            let response = client
                .post(&api)
                .basic_auth(username, Some(password))
                .json(&serde_json::json!({ "pairing_id": pairing_id, "pin": pin, "name": "moonlight-mcp" }))
                .send()
                .await?;
            let status = response.status();
            let body = nvhttp::json_response(response).await?;
            if !status.is_success() || body["status"] != true {
                bail!("Sunshine did not accept the PIN (HTTP {status})");
            }
            Ok(())
        }
        PinDelivery::Wolf => {
            let client = tls::http_client(Duration::from_secs(10))?;
            let secret = poll_until(|| async {
                let pending = nvhttp::json_response(client
                    .get(host.http_url("/pairing/pending"))
                    .send()
                    .await
                    .map_err(reqwest::Error::without_url)?).await?;
                let secrets = pending["secrets"].as_array().context("Wolf response lacks secrets array")?;
                ensure!(secrets.len() <= 1, "ambiguous Wolf pairing requests; use an isolated host or a configured command helper");
                Ok(secrets.first().and_then(|s| s.as_str()).map(str::to_owned))
            })
            .await?;
            let response = client
                .post(host.http_url("/pin/"))
                .json(&serde_json::json!({ "pin": pin, "secret": secret }))
                .send()
                .await?;
            if !response.status().is_success() {
                bail!("Wolf rejected the PIN: HTTP {}", response.status());
            }
            Ok(())
        }
    }
}

/// Polls every 500 ms for up to 10 s until `check` yields a value.
async fn poll_until<T, F, Fut>(mut check: F) -> Result<T>
where
    F: FnMut() -> Fut,
    Fut: std::future::Future<Output = Result<Option<T>>>,
{
    for _ in 0..20 {
        tokio::time::sleep(Duration::from_millis(500)).await;
        if let Some(value) = check().await? {
            return Ok(value);
        }
    }
    bail!("the host never listed the pending pairing request")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn key_derivation_is_sha256_prefix() {
        let salt = [0u8; 16];
        let mut input = salt.to_vec();
        input.extend_from_slice(b"1234");
        let full = Sha256::digest(&input);
        assert_eq!(derive_key(&salt, "1234"), full[..16]);
    }

    #[test]
    fn pins_are_four_digits() {
        for _ in 0..100 {
            let pin = random_pin();
            assert_eq!(pin.len(), 4);
            assert!(pin.chars().all(|c| c.is_ascii_digit()));
        }
    }
    #[tokio::test]
    async fn unattended_manual_delivery_and_bad_pins_fail_before_network() {
        let dir = std::env::temp_dir().join(format!("pin-policy-{}", uuid::Uuid::new_v4()));
        let paths = Paths::resolve(Some(&dir)).unwrap();
        let identity = ClientIdentity::load_or_create(&paths).unwrap();
        let host = Host {
            address: "invalid-do-not-resolve".into(),
            http_port: 1,
        };
        let error = pair_unattended(&paths, &identity, &host, "1234", &PinDelivery::Manual)
            .await
            .unwrap_err();
        assert!(error.to_string().contains("unattended"));
        for pin in ["", "123", "12345", "12a4"] {
            assert!(pair(&paths, &identity, &host, pin, &PinDelivery::Manual)
                .await
                .unwrap_err()
                .to_string()
                .contains("four ASCII digits"));
        }
        std::fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn scenario_providers_have_no_manual_or_strategy_fallback() {
        use crate::scenario::{Pairing, PinStrategy};
        let mut providers = std::collections::HashMap::new();
        let config = Pairing {
            pin_delivery: PinStrategy::Command,
            provider_ref: Some("helper".into()),
        };
        assert!(scenario_delivery(&config, &providers).is_err());
        providers.insert("helper".into(), PinDelivery::Manual);
        assert!(scenario_delivery(&config, &providers).is_err());
        providers.insert("helper".into(), PinDelivery::Wolf);
        assert!(scenario_delivery(&config, &providers).is_err());
        providers.insert("helper".into(), PinDelivery::Command("exit 0".into()));
        assert!(matches!(
            scenario_delivery(&config, &providers).unwrap(),
            Some(PinDelivery::Command(_))
        ));
        assert!(scenario_delivery(
            &Pairing {
                pin_delivery: PinStrategy::AlreadyPaired,
                provider_ref: None
            },
            &providers
        )
        .unwrap()
        .is_none());
    }
    #[tokio::test]
    async fn failed_pin_helper_cancels_a_host_waiting_for_pin() {
        use tokio::io::{AsyncReadExt, AsyncWriteExt};
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let port = listener.local_addr().unwrap().port();
        let server = tokio::spawn(async move {
            let mut handlers = tokio::task::JoinSet::new();
            loop {
                let (mut stream, _) = listener.accept().await.unwrap();
                handlers.spawn(async move {
                    let mut request = Vec::new();
                    let mut chunk = [0; 4096];
                    while !request.windows(4).any(|w| w == b"\r\n\r\n") {
                        let n = stream.read(&mut chunk).await.unwrap();
                        if n == 0 { return; }
                        request.extend_from_slice(&chunk[..n]);
                        assert!(request.len() <= 16384);
                    }
                    let request = String::from_utf8(request).unwrap();
                    let body = if request.starts_with("GET /serverinfo?") {
                        "<root status_code=\"200\"><hostname>fixture</hostname><appversion>7.0</appversion><uniqueid>fixture</uniqueid><HttpsPort>1</HttpsPort></root>"
                    } else if request.starts_with("GET /unpair?") {
                        "<root status_code=\"200\"><paired>1</paired></root>"
                    } else {
                        // A real host holds this response until the PIN provider completes.
                        tokio::time::sleep(Duration::from_secs(60)).await;
                        return;
                    };
                    let response = format!("HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}", body.len());
                    stream.write_all(response.as_bytes()).await.unwrap();
                });
            }
        });
        let dir = std::env::temp_dir().join(format!("pin-cancel-{}", uuid::Uuid::new_v4()));
        let paths = Paths::resolve(Some(&dir)).unwrap();
        let identity = ClientIdentity::load_or_create(&paths).unwrap();
        let host = Host {
            address: "127.0.0.1".into(),
            http_port: port,
        };
        let result = tokio::time::timeout(
            Duration::from_secs(3),
            pair_unattended(
                &paths,
                &identity,
                &host,
                "1234",
                &PinDelivery::Command("exit 17".into()),
            ),
        )
        .await
        .unwrap();
        assert!(result
            .unwrap_err()
            .to_string()
            .contains("PIN command exited"));
        assert!(!paths.host_cert("fixture").unwrap().exists());
        server.abort();
        assert!(server.await.unwrap_err().is_cancelled());
        std::fs::remove_dir_all(dir).unwrap();
    }
}
