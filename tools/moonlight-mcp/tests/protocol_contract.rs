//! T013: synthetic wire fixtures, never reference-host or hardware evidence.
//! Activated: T020 (1 mutual-TLS case), T021 (1 loopback RTSP case), T022 (1 control case).
//! Activated: T026 (1 controller case).
//! Replace the respective absent adapter and remove
//! its ignore when the named task lands. `cargo test --test protocol_contract -- --ignored`
//! deliberately demonstrates the missing behavior before implementation.
//!
//! Source pins: moonlight-qt 8f2e52e54046fc942e2b80ffb88a71ff0f2300bd,
//! moonlight-common-c 41cedb89d6b8592f99c09975c764ca3875034726,
//! Sunshine d0b4a88ef727f8dabf80cc970a14739b55c885de; docs/protocol-notes.md §§3,9,12,13.
//! Official-client checks are the expected oracle; these generated responses are not a
//! recorded execution of an official client.

use aws_lc_rs::rsa::PublicKey as RsaPublicKey;
use aws_lc_rs::signature::{UnparsedPublicKey, RSA_PKCS1_2048_8192_SHA256};
use moonlight_mcp::config::Paths;
use moonlight_mcp::crypto::{aes_ecb_decrypt, aes_ecb_encrypt, aes_gcm_decrypt, aes_gcm_encrypt};
use moonlight_mcp::identity::ClientIdentity;
use moonlight_mcp::nvhttp::{base_query, Host, XmlResponse};
use moonlight_mcp::pairing::{pair, PinDelivery};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;

const NEUTRAL_GAMEPAD_HEX: &str =
    "0000001e0c0000001a0000000f0014000000000000000000000000009c0000005500";

struct Scratch(PathBuf);
impl Scratch {
    fn new() -> Self {
        let path = std::env::temp_dir()
            .join("moonlight-contract-fixtures")
            .join(format!("protocol-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir_all(&path).unwrap();
        Self(path)
    }
    fn paths(&self, name: &str) -> Paths {
        Paths::resolve(Some(&self.0.join(name))).unwrap()
    }
}
impl Drop for Scratch {
    fn drop(&mut self) {
        std::fs::remove_dir_all(&self.0).unwrap();
    }
}

#[test]
fn independent_installations_have_isolated_persistent_identities() {
    // mqt app/backend/identitymanager.cpp:137-151; notes §13.1.
    let scratch = Scratch::new();
    let a = scratch.paths("a");
    let b = scratch.paths("b");
    let first = ClientIdentity::load_or_create(&a).unwrap();
    let again = ClientIdentity::load_or_create(&a).unwrap();
    let other = ClientIdentity::load_or_create(&b).unwrap();
    assert_eq!(first.unique_id, again.unique_id);
    assert_eq!(first.cert_der, again.cert_der);
    assert_ne!(first.unique_id, other.unique_id);
    assert_ne!(first.cert_der, other.cert_der);
    assert_eq!(first.unique_id.len(), 16);
    assert!(first.unique_id.bytes().all(|c| c.is_ascii_hexdigit()));
    assert_ne!(base_query(&first.unique_id), base_query(&first.unique_id));
    for unsafe_id in ["", "../host", "/host", "a/b", "a\\b"] {
        assert!(a.host_cert(unsafe_id).is_err());
    }
}

#[test]
fn http_xml_status_and_required_fields_are_not_transport_success() {
    // mqt nvpairingmanager.cpp:242-252,274-287,350-377; notes §§13.2,13.5.
    for malformed in ["", "<root>", "<root>&unknown;</root>"] {
        assert!(XmlResponse::parse(malformed.into()).is_err());
    }
    for status in [400, 401, 404, 409, 503] {
        assert!(XmlResponse::parse(format!(
            "<root status_code=\"{status}\"><paired>1</paired></root>"
        ))
        .is_err());
    }
    let missing = XmlResponse::parse("<root status_code=\"200\"/>".into()).unwrap();
    assert!(missing.require("paired").is_err());
}

#[derive(Clone, Copy, Debug)]
enum PairFault {
    Rejected,
    MissingCertificate,
    BadCertificate,
    ShortChallenge,
    NonHexChallenge,
    ShortSecret,
    BadSignature,
    WrongPinProof,
    FinalRejected,
}

/// Minimal bounded loopback HTTP host. It intentionally never claims to be a reference host.
async fn fixture_host(
    listener: TcpListener,
    identity: Arc<ClientIdentity>,
    fault: PairFault,
    requests: Arc<Mutex<Vec<String>>>,
) {
    let mut key = [0; 16];
    let mut client_cert_signature = Vec::new();
    let mut client_key = None;
    let mut client_answer = Vec::new();
    loop {
        let (mut socket, _) = listener.accept().await.unwrap();
        let mut bytes = Vec::new();
        while !bytes.windows(4).any(|w| w == b"\r\n\r\n") {
            let mut chunk = [0; 4096];
            let n = socket.read(&mut chunk).await.unwrap();
            if n == 0 {
                return;
            }
            bytes.extend_from_slice(&chunk[..n]);
            assert!(bytes.len() < 64 * 1024);
        }
        let request = String::from_utf8(bytes).unwrap();
        let target = request
            .lines()
            .next()
            .unwrap()
            .split_whitespace()
            .nth(1)
            .unwrap();
        let url = reqwest::Url::parse(&format!("http://127.0.0.1{target}")).unwrap();
        let q: HashMap<String, String> = url.query_pairs().into_owned().collect();
        requests.lock().unwrap().push(target.to_string());
        let fields = if url.path() == "/serverinfo" {
            "<hostname>synthetic</hostname><appversion>7.1.431.-1</appversion>\
             <uniqueid>synthetic-host</uniqueid><HttpsPort>47984</HttpsPort>"
                .to_string()
        } else if url.path() == "/unpair" {
            "<paired>1</paired>".into()
        } else if q.get("phrase").map(String::as_str) == Some("getservercert") {
            assert_eq!(q["devicename"], "roth");
            assert_eq!(q["updateState"], "1");
            let client_pem = hex::decode(&q["clientcert"]).unwrap();
            let client_der = pem::parse(client_pem).unwrap().into_contents();
            client_cert_signature =
                moonlight_mcp::identity::certificate_signature(&client_der).unwrap();
            let (_, certificate) = x509_parser::parse_x509_certificate(&client_der).unwrap();
            client_key = Some(RsaPublicKey::from_der(certificate.public_key().raw).unwrap());
            assert_eq!(hex::decode(&q["salt"]).unwrap().len(), 16);
            let digest = Sha256::new()
                .chain_update(hex::decode(&q["salt"]).unwrap())
                .chain_update(b"1234")
                .finalize();
            key.copy_from_slice(&digest[..16]);
            match fault {
                PairFault::Rejected => "<paired>0</paired>".into(),
                PairFault::MissingCertificate => "<paired>1</paired><plaincert/>".into(),
                PairFault::BadCertificate => "<paired>1</paired><plaincert>00</plaincert>".into(),
                _ => format!(
                    "<paired>1</paired><plaincert>{}</plaincert>",
                    hex::encode(identity.cert_pem.as_bytes())
                ),
            }
        } else if let Some(challenge) = q.get("clientchallenge") {
            assert!(!q.contains_key("phrase"));
            let challenge = aes_ecb_decrypt(&key, &hex::decode(challenge).unwrap()).unwrap();
            assert_eq!(challenge.len(), 16);
            let mut proof = Sha256::new()
                .chain_update(&challenge)
                .chain_update(identity.cert_signature().unwrap())
                .chain_update([0x53; 16])
                .finalize()
                .to_vec();
            if matches!(fault, PairFault::WrongPinProof) {
                proof[0] ^= 1;
            }
            if !matches!(fault, PairFault::ShortChallenge) {
                proof.extend([0x43; 16]);
            }
            let value = if matches!(fault, PairFault::NonHexChallenge) {
                "not-hex".into()
            } else {
                hex::encode(aes_ecb_encrypt(&key, &proof).unwrap())
            };
            format!("<paired>1</paired><challengeresponse>{value}</challengeresponse>")
        } else if let Some(answer) = q.get("serverchallengeresp") {
            assert!(!q.contains_key("phrase"));
            assert_eq!(hex::decode(answer).unwrap().len(), 32);
            client_answer = aes_ecb_decrypt(&key, &hex::decode(answer).unwrap()).unwrap();
            let secret = [0x53; 16];
            let mut signed = secret.to_vec();
            if !matches!(fault, PairFault::ShortSecret) {
                let mut signature = identity.sign_sha256(&secret).unwrap();
                if matches!(fault, PairFault::BadSignature) {
                    signature[0] ^= 1;
                }
                signed.extend(signature);
            }
            format!(
                "<paired>1</paired><pairingsecret>{}</pairingsecret>",
                hex::encode(signed)
            )
        } else if q.contains_key("clientpairingsecret") {
            assert!(matches!(fault, PairFault::FinalRejected));
            assert!(!q.contains_key("phrase"));
            let signed_secret = hex::decode(&q["clientpairingsecret"]).unwrap();
            assert_eq!(signed_secret.len(), 272);
            let (secret, signature) = signed_secret.split_at(16);
            UnparsedPublicKey::new(
                &RSA_PKCS1_2048_8192_SHA256,
                client_key.as_ref().unwrap().as_ref(),
            )
            .verify(secret, signature)
            .unwrap();
            let expected_answer = Sha256::new()
                .chain_update([0x43; 16])
                .chain_update(&client_cert_signature)
                .chain_update(secret)
                .finalize();
            assert_eq!(client_answer.as_slice(), expected_answer.as_slice());
            "<paired>0</paired>".into()
        } else {
            panic!("unexpected fixture request: {target}");
        };
        let body = format!("<root status_code=\"200\">{fields}</root>");
        let response = format!(
            "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
            body.len()
        );
        socket.write_all(response.as_bytes()).await.unwrap();
    }
}

#[tokio::test]
async fn pairing_rejects_every_official_client_proof_failure_before_pinning() {
    // mqt nvpairingmanager.cpp:254-348,350-377; notes §§13.2,13.5.
    let scratch = Scratch::new();
    let host_identity = Arc::new(ClientIdentity::load_or_create(&scratch.paths("host")).unwrap());
    for (fault, error, steps) in [
        (PairFault::Rejected, "host rejected pairing", 1),
        (
            PairFault::MissingCertificate,
            "response lacks <plaincert>",
            1,
        ),
        (PairFault::BadCertificate, "", 1),
        (PairFault::ShortChallenge, "expected at least 48", 2),
        (PairFault::NonHexChallenge, "", 2),
        (PairFault::ShortSecret, "too short to hold a signature", 3),
        (PairFault::BadSignature, "possible MITM", 3),
        (
            PairFault::WrongPinProof,
            "PIN entered on the host was wrong",
            3,
        ),
        (PairFault::FinalRejected, "host rejected pairing", 4),
    ] {
        let paths = scratch.paths(&format!("client-{fault:?}"));
        let client = ClientIdentity::load_or_create(&paths).unwrap();
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let host = Host {
            address: "127.0.0.1".into(),
            http_port: listener.local_addr().unwrap().port(),
        };
        let requests = Arc::new(Mutex::new(Vec::new()));
        let task = tokio::spawn(fixture_host(
            listener,
            host_identity.clone(),
            fault,
            requests.clone(),
        ));
        // No terminal prompt or operator dependency: the synthetic host already knows this PIN.
        let result = tokio::time::timeout(
            Duration::from_secs(5),
            pair(
                &paths,
                &client,
                &host,
                "1234",
                &PinDelivery::Command("true".into()),
            ),
        )
        .await
        .expect("fixture exchange must not hang");
        let actual_error = format!("{:#}", result.unwrap_err());
        assert!(actual_error.contains(error), "{fault:?}: {actual_error}");
        assert!(!paths.host_cert("synthetic-host").unwrap().exists());
        let seen = requests.lock().unwrap().clone();
        task.abort();
        assert!(task.await.unwrap_err().is_cancelled());
        assert_eq!(
            seen.iter().filter(|r| r.starts_with("/pair?")).count(),
            steps,
            "{fault:?}"
        );
        assert!(
            seen.last().unwrap().starts_with("/unpair?"),
            "failure must attempt cleanup"
        );
        assert!(seen
            .iter()
            .all(|r| r.contains(&format!("uniqueid={}", client.unique_id))));
    }
}

#[test]
fn control_v2_authentication_binds_direction_sequence_and_tag() {
    // mcc ControlStream.c:548-629,692-737; notes §9. This tests crypto, not ENet delivery.
    let key = [0x17; 16];
    let mut nonce = [0; 12];
    nonce[..4].copy_from_slice(&0x0102_0304u32.to_le_bytes());
    nonce[10..].copy_from_slice(b"CC");
    let ping = hex::decode("000208000400000000000000").unwrap();
    let (ct, tag) = aes_gcm_encrypt(&key, &nonce, &ping).unwrap();
    assert_eq!(aes_gcm_decrypt(&key, &nonce, &ct, &tag).unwrap(), ping);
    for changed in [0, 10, 11] {
        let mut bad_nonce = nonce;
        bad_nonce[changed] ^= 1;
        assert!(aes_gcm_decrypt(&key, &bad_nonce, &ct, &tag).is_err());
    }
    assert!(aes_gcm_decrypt(&key, &nonce, &ct, &tag[..15]).is_err());
    let mut bad_tag = tag;
    bad_tag[0] ^= 1;
    assert!(aes_gcm_decrypt(&key, &nonce, &ct, &bad_tag).is_err());
    for len in [1, 15, 17, 47] {
        assert!(aes_ecb_decrypt(&key, &vec![0; len]).is_err());
    }
}

#[derive(Debug, PartialEq)]
struct TransportFixture {
    bytes: Vec<u8>,
    channel: u8,
    reliable: bool,
}

// Explicit test-only absent adapters, approved for the TDD backlog. These are not production
// implementations: the activating tasks must wire the real public API, never return fixtures.
async fn pairing_tls_outcome(certificate_matches: bool, paired: bool) -> Option<(bool, bool)> {
    use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer};
    use std::io::{Read, Write};
    let scratch = Scratch::new();
    let paths = scratch.paths("tls-client");
    let client = ClientIdentity::load_or_create(&paths).unwrap();
    let proved = ClientIdentity::load_or_create(&scratch.paths("proved-host")).unwrap();
    let other = ClientIdentity::load_or_create(&scratch.paths("other-host")).unwrap();
    let presented = if certificate_matches { &proved } else { &other };
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let mut roots = rustls::RootCertStore::empty();
    roots
        .add(CertificateDer::from(client.cert_der.clone()))
        .unwrap();
    let verifier = rustls::server::WebPkiClientVerifier::builder_with_provider(
        Arc::new(roots),
        provider.clone(),
    )
    .build()
    .unwrap();
    let config = rustls::ServerConfig::builder_with_provider(provider)
        .with_safe_default_protocol_versions()
        .unwrap()
        .with_client_cert_verifier(verifier)
        .with_single_cert(
            vec![CertificateDer::from(presented.cert_der.clone())],
            PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(
                pem::parse(&presented.key_pem).unwrap().into_contents(),
            )),
        )
        .unwrap();
    let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let server = std::thread::spawn(move || {
        let (socket, _) = listener.accept().unwrap();
        socket
            .set_read_timeout(Some(Duration::from_secs(3)))
            .unwrap();
        socket
            .set_write_timeout(Some(Duration::from_secs(3)))
            .unwrap();
        let connection = rustls::ServerConnection::new(Arc::new(config)).unwrap();
        let mut stream = rustls::StreamOwned::new(connection, socket);
        let mut request = Vec::new();
        let mut buffer = [0; 2048];
        while request.len() <= 8192 && !request.windows(4).any(|w| w == b"\r\n\r\n") {
            match stream.read(&mut buffer) {
                Ok(0) | Err(_) => return false,
                Ok(count) => request.extend_from_slice(&buffer[..count]),
            }
        }
        assert!(
            stream.conn.peer_certificates().is_some(),
            "must present a client certificate"
        );
        assert!(String::from_utf8_lossy(&request).contains("phrase=pairchallenge"));
        let body = format!(
            "<root status_code=\"200\"><paired>{}</paired></root>",
            u8::from(paired)
        );
        write!(
            stream,
            "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            body.len(),
            body
        )
        .unwrap();
        stream.flush().unwrap();
        true
    });
    let host = Host {
        address: "127.0.0.1".into(),
        http_port: port,
    };
    let info = moonlight_mcp::http::ServerInfo {
        hostname: "synthetic TLS fixture".into(),
        app_version: "7.0".into(),
        uniqueid: "fixture-host".into(),
        http_port: port,
        https_port: port,
        codec_mode_support: 0,
        current_game: 0,
        state: None,
        pair_status: None,
        display_modes: vec![],
    };
    let result =
        moonlight_mcp::pairing::confirm_and_store(&paths, &client, &host, &info, &proved.cert_pem)
            .await;
    let reached_http = server.join().unwrap();
    assert_eq!(reached_http, certificate_matches);
    assert_eq!(
        std::fs::read_dir(scratch.0.join("tls-client/hosts"))
            .unwrap()
            .count(),
        if result.is_ok() { 2 } else { 0 },
        "neither host nor endpoint pin may be stored on failure"
    );
    Some((
        result.is_ok(),
        paths.host_cert("fixture-host").unwrap().exists(),
    ))
}
async fn rtsp_requests_from_client() -> Option<Vec<String>> {
    let listener = TcpListener::bind("127.0.0.1:0").await.ok()?;
    let port = listener.local_addr().ok()?.port();
    let udp = std::net::UdpSocket::bind("127.0.0.1:0").unwrap();
    let control_port = udp.local_addr().unwrap().port();
    let mut enet = rusty_enet::Host::new(
        udp,
        rusty_enet::HostSettings {
            peer_limit: 1,
            channel_limit: 0x30,
            ..rusty_enet::HostSettings::default()
        },
    )
    .unwrap();
    let enet_peer = tokio::spawn(async move {
        let until = std::time::Instant::now() + Duration::from_secs(5);
        let mut connected = false;
        loop {
            assert!(
                std::time::Instant::now() < until,
                "session teardown must reach peer"
            );
            match enet.service().unwrap() {
                Some(rusty_enet::Event::Connect { data, .. }) => {
                    assert_eq!(data, 16909060);
                    connected = true;
                }
                Some(rusty_enet::Event::Disconnect { .. }) => {
                    assert!(connected);
                    break;
                }
                _ => tokio::time::sleep(Duration::from_millis(1)).await,
            }
        }
    });
    let host = tokio::spawn(async move {
        let mut requests = Vec::new();
        for cseq in 1..=7 {
            let (mut socket, _) = listener.accept().await.unwrap();
            let mut raw = Vec::new();
            let header_len = loop {
                raw.push(socket.read_u8().await.unwrap());
                if raw.ends_with(b"\r\n\r\n") {
                    break raw.len();
                }
                assert!(raw.len() < 8192);
            };
            let header = String::from_utf8(raw).unwrap();
            let length = header
                .lines()
                .find_map(|l| l.strip_prefix("Content-length: "))
                .map(|s| s.parse::<usize>().unwrap())
                .unwrap_or(0);
            let mut body = vec![0; length];
            socket.read_exact(&mut body).await.unwrap();
            assert!(header_len > 0);
            let mut words = header.lines().next().unwrap().split_whitespace();
            let method = words.next().unwrap();
            let target = words.next().unwrap();
            assert!(header.contains(&format!("CSeq: {cseq}\r\n")));
            let display = match method {
                "SETUP" | "ANNOUNCE" => {
                    format!("{method} {}", target.strip_prefix("streamid=").unwrap())
                }
                "PLAY" => format!("PLAY {target}"),
                _ => method.to_string(),
            };
            requests.push(format!("{display} CSeq:{cseq}"));
            let control_header =
                format!("Transport: server_port={control_port}\r\nX-SS-Connect-Data: 16909060\r\n");
            let extra = match cseq {
                3 => "Session: FIXTURE;timeout = 90\r\nTransport: server_port=51000\r\nX-SS-Ping-Payload: 0123456789abcdef\r\n",
                4 => "Transport: server_port=51001\r\nX-SS-Ping-Payload: fedcba9876543210\r\n",
                5 => control_header.as_str(),
                _ => "",
            };
            let sdp = if cseq == 2 {
                "v=0\r\na=x-ss-general.encryptionSupported:1\r\n"
            } else {
                ""
            };
            let response = format!(
                "RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\n{extra}Content-Length: {}\r\n\r\n{sdp}",
                sdp.len()
            );
            socket.write_all(response.as_bytes()).await.unwrap();
        }
        requests
    });
    let settings = moonlight_mcp::rtsp::StreamSettings {
        width: 1920,
        height: 1080,
        fps: 60,
        bitrate_kbps: 20_000,
        codec: moonlight_mcp::codec::Codec::H264,
        hdr: false,
        packet_size: 1392,
        encrypt_av: false,
    };
    let mut active = moonlight_mcp::rtsp::establish(
        "127.0.0.1",
        &format!("rtsp://127.0.0.1:{port}"),
        [0x17; 16],
        &settings,
    )
    .await
    .unwrap();
    let session = &active.negotiation;
    assert_eq!(
        (session.audio_port, session.video_port, session.control_port),
        (51000, 51001, control_port)
    );
    assert_eq!(session.connect_data, 16909060);
    assert_eq!(session.requested, settings);
    assert_eq!(session.announced, settings);
    assert_eq!(session.host_chosen_bitrate_kbps, None);
    assert_ne!(session.ping_payload, session.video_ping_payload);
    active.teardown().await.unwrap();
    active.teardown().await.unwrap();
    enet_peer.await.unwrap();
    Some(host.await.unwrap())
}
fn rtsp_response_accepted(response: &[u8]) -> Option<bool> {
    Some(moonlight_mcp::rtsp::SetupResponse::parse(response, 3).is_ok())
}
fn control_packet_accepted(packet: &[u8]) -> Option<bool> {
    Some(
        moonlight_mcp::control::ControlCipher::new([0x17; 16], true)
            .decode_host(packet)
            .is_ok(),
    )
}
fn control_from_client(
    key: &[u8; 16],
    sequence: u32,
    kind: u16,
    payload: &[u8],
) -> Option<TransportFixture> {
    let mut cipher = moonlight_mcp::control::ControlCipher::new(*key, true);
    for _ in 0..sequence {
        cipher.encode(kind, payload, 0, true).ok()?;
    }
    let packet = cipher.encode(kind, payload, 0, true).ok()?;
    Some(TransportFixture {
        bytes: packet.bytes,
        channel: packet.channel,
        reliable: packet.reliable,
    })
}
fn control_sequence_from_client(count: usize) -> Option<Vec<u32>> {
    let mut cipher = moonlight_mcp::control::ControlCipher::new([0x17; 16], true);
    (0..count)
        .map(|_| {
            let packet = cipher
                .encode(
                    moonlight_mcp::control::PING,
                    &moonlight_mcp::control::PING_PAYLOAD,
                    0,
                    true,
                )
                .ok()?;
            Some(u32::from_le_bytes(packet.bytes[4..8].try_into().ok()?))
        })
        .collect()
}
fn controller_from_client(controller: u8, active_mask: u16) -> Option<TransportFixture> {
    let packet =
        moonlight_mcp::input::gamepad_packet(controller, active_mask, &Default::default()).ok()?;
    Some(TransportFixture {
        bytes: packet.bytes,
        channel: packet.channel,
        reliable: packet.reliable,
    })
}

#[tokio::test]
async fn rtsp_sequence_and_missing_session_rejection() {
    // mcc RtspConnection.c:944-1442; notes §3. CSeq starts at one, per-connection sequence.
    let expected = [
        "OPTIONS",
        "DESCRIBE",
        "SETUP audio/0/0",
        "SETUP video/0/0",
        "SETUP control/13/0",
        "ANNOUNCE control/13/0",
        "PLAY /",
    ]
    .iter()
    .enumerate()
    .map(|(i, method)| format!("{} CSeq:{}", method, i + 1))
    .collect();
    assert_eq!(rtsp_requests_from_client().await, Some(expected));
    // T021 adapter must additionally feed a SETUP response without Session and verify rejection.
    let missing_session = b"RTSP/1.0 200 OK\r\nCSeq: 3\r\nTransport: server_port=48000\r\n\r\n";
    assert_eq!(rtsp_response_accepted(missing_session), Some(false));
    for response in [
        b"RTSP/1.0 500 Error\r\n\r\n".as_slice(),
        b"garbage".as_slice(),
    ] {
        assert_eq!(rtsp_response_accepted(response), Some(false));
    }
}

#[test]
fn control_ping_has_inner_length_and_reliable_channel_zero() {
    // mcc ControlStream.c:297-298,548-591,1403-1455; notes §9.
    let key = [0x17; 16];
    let mut nonce = [0; 12];
    nonce[10..].copy_from_slice(b"CC");
    let inner = hex::decode("000208000400000000000000").unwrap();
    let (ciphertext, tag) = aes_gcm_encrypt(&key, &nonce, &inner).unwrap();
    let mut envelope = hex::decode("0100200000000000").unwrap();
    envelope.extend(tag);
    envelope.extend(ciphertext);
    assert_eq!(
        control_from_client(&key, 0, 0x0200, &[4, 0, 0, 0, 0, 0, 0, 0]),
        Some(TransportFixture {
            bytes: envelope,
            channel: 0,
            reliable: true
        })
    );
    assert_eq!(control_sequence_from_client(3), Some(vec![0, 1, 2]));
    // Incoming host sequence values are arbitrary in the pinned official client; do not
    // invent an ordering-rejection requirement (notes §9, Sunshine stream.cpp:296).
    // mcc ControlStream.c:603-615,1259-1263: runts, bad lengths, and plaintext are dropped.
    for malformed in [
        vec![],
        vec![0; 23],
        hex::decode("0100ffff00000000").unwrap(),
        hex::decode("000208000400000000000000").unwrap(),
    ] {
        assert_eq!(control_packet_accepted(&malformed), Some(false));
    }
}

#[test]
fn controller_packets_preserve_identity_mask_size_and_reliability() {
    // mcc Input.h:87-109, InputStream.c:1150-1153,1182-1205; notes §§12.2,12.5.
    for controller in 0..4u8 {
        let mut expected = hex::decode(NEUTRAL_GAMEPAD_HEX).unwrap();
        expected[10] = controller;
        assert_eq!(
            controller_from_client(controller, 0x000f),
            Some(TransportFixture {
                bytes: expected,
                channel: 0x10 + controller,
                reliable: true,
            })
        );
    }
}

#[tokio::test]
async fn final_https_confirmation_requires_pinned_host_identity_and_paired_success() {
    // mqt nvpairingmanager.cpp:261-272,367-377; notes §§13.2,13.5.
    // Expected tuple is (pairing accepted, host certificate persisted).
    for (certificate_matches, paired, expected) in [
        (true, true, (true, true)),
        (false, true, (false, false)),
        (true, false, (false, false)),
    ] {
        assert_eq!(
            pairing_tls_outcome(certificate_matches, paired).await,
            Some(expected)
        );
    }
}

#[test]
fn controller_fixture_has_exact_source_pinned_size_and_tail_offsets() {
    let bytes = hex::decode(NEUTRAL_GAMEPAD_HEX).unwrap();
    assert_eq!(bytes.len(), 34);
    assert_eq!(u32::from_be_bytes(bytes[..4].try_into().unwrap()), 30);
    assert_eq!(&bytes[28..], &[0x9c, 0, 0, 0, 0x55, 0]);
}
