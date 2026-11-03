//! The host's HTTP(S) API: server info, app list, launch/resume, cancel, and pairing requests.
//!
//! Responses are XML documents whose root carries `status_code` (and `status_message` on error).

use anyhow::{anyhow, bail, ensure, Context, Result};
use serde::Serialize;
use std::collections::HashMap;
use std::time::Duration;

use crate::config::Paths;
use crate::identity::ClientIdentity;
use crate::tls;

#[derive(Debug, Clone)]
pub struct Host {
    pub address: String,
    pub http_port: u16,
}

impl Host {
    fn url_address(&self) -> String {
        if self.address.contains(':') && !self.address.starts_with('[') {
            format!("[{}]", self.address)
        } else {
            self.address.clone()
        }
    }
    /// A stable endpoint binding prevents unauthenticated discovery from selecting another pin.
    pub(crate) fn pin_id(&self) -> String {
        use sha2::{Digest, Sha256};
        format!(
            "endpoint-{}",
            hex::encode(Sha256::digest(format!(
                "{}:{}",
                self.address.to_ascii_lowercase(),
                self.http_port
            )))
        )
    }
    pub fn http_url(&self, path_and_query: &str) -> String {
        format!(
            "http://{}:{}{}",
            self.url_address(),
            self.http_port,
            path_and_query
        )
    }

    pub fn https_url(&self, https_port: u16, path_and_query: &str) -> String {
        format!(
            "https://{}:{}{}",
            self.url_address(),
            https_port,
            path_and_query
        )
    }
}

/// Query prefix every request carries: the client's persisted `uniqueid` and a fresh `uuid`.
/// Hosts identify paired clients by certificate, not by `uniqueid` (protocol-notes §13.1).
pub fn base_query(unique_id: &str) -> String {
    let mut query = reqwest::Url::parse("http://localhost").expect("constant URL");
    query
        .query_pairs_mut()
        .append_pair("uniqueid", unique_id)
        .append_pair("uuid", &uuid::Uuid::new_v4().simple().to_string());
    query.query().expect("query set").to_string()
}

/// A parsed response: the root's status plus the text of every element, keyed by tag name.
/// Repeated tags keep their first value; lists are extracted separately.
#[derive(Debug)]
pub struct XmlResponse {
    pub fields: HashMap<String, String>,
    body: String,
}

impl XmlResponse {
    pub fn parse(body: String) -> Result<Self> {
        ensure!(
            body.len() <= MAX_XML_BYTES,
            "host XML exceeds response limit"
        );
        let fields = {
            let doc = roxmltree::Document::parse(&body).context("host returned invalid XML")?;
            let root = doc.root_element();
            ensure!(root.has_tag_name("root"), "host XML lacks root element");
            let status = root
                .attribute("status_code")
                .context("host XML lacks status_code")?;
            if status != "200" {
                bail!(
                    "host returned status {status}: {}",
                    root.attribute("status_message").unwrap_or("(no message)")
                );
            }
            let mut fields = HashMap::new();
            for node in root.children().filter(|n| n.is_element()) {
                if let Some(text) = node.text() {
                    if node.children().any(|child| child.is_element()) {
                        continue;
                    }
                    ensure!(
                        fields
                            .insert(node.tag_name().name().to_string(), text.trim().to_string())
                            .is_none(),
                        "duplicate host field {}",
                        node.tag_name().name()
                    );
                }
            }
            fields
        };
        Ok(Self { fields, body })
    }

    pub fn get(&self, tag: &str) -> Option<&str> {
        self.fields.get(tag).map(String::as_str)
    }

    pub fn require(&self, tag: &str) -> Result<&str> {
        self.get(tag)
            .ok_or_else(|| anyhow!("response lacks <{tag}>"))
    }

    /// Children of every `<parent>` element, as tag → text maps.
    pub fn records(&self, parent: &str) -> Result<Vec<HashMap<String, String>>> {
        let doc = roxmltree::Document::parse(&self.body).expect("parsed once already");
        doc.descendants()
            .filter(|n| n.has_tag_name(parent))
            .map(|node| {
                let mut fields = HashMap::new();
                for child in node.children().filter(|c| c.is_element()) {
                    ensure!(
                        !child.children().any(|c| c.is_element()),
                        "nested record field"
                    );
                    let text = child.text().unwrap_or_default();
                    // Moonlight Qt compares this app field to exact text "1" (see parse_apps).
                    let text = if parent == "App" && child.has_tag_name("IsHdrSupported") {
                        text
                    } else {
                        text.trim()
                    };
                    ensure!(
                        fields
                            .insert(child.tag_name().name().to_string(), text.to_string())
                            .is_none(),
                        "duplicate record field"
                    );
                }
                Ok(fields)
            })
            .collect()
    }
}

pub async fn get_xml(client: &reqwest::Client, url: &str) -> Result<XmlResponse> {
    let mut response = client
        .get(url)
        .send()
        .await
        .map_err(reqwest::Error::without_url)
        .with_context(|| format!("GET {}", redact(url)))?;
    let status = response.status();
    let body = bounded_body(&mut response).await?;
    ensure!(
        status.is_success(),
        "GET {} returned HTTP {status}",
        redact(url)
    );
    XmlResponse::parse(String::from_utf8(body).context("host response is not UTF-8")?)
}

async fn bounded_body(response: &mut reqwest::Response) -> Result<Vec<u8>> {
    ensure!(
        response
            .content_length()
            .is_none_or(|length| length <= MAX_XML_BYTES as u64),
        "host response exceeds limit"
    );
    let mut bytes = Vec::new();
    while let Some(chunk) = response
        .chunk()
        .await
        .map_err(reqwest::Error::without_url)?
    {
        ensure!(
            chunk.len() <= MAX_XML_BYTES.saturating_sub(bytes.len()),
            "host response exceeds limit"
        );
        bytes.extend_from_slice(&chunk);
    }
    Ok(bytes)
}

pub(crate) async fn json_response(mut response: reqwest::Response) -> Result<serde_json::Value> {
    ensure!(
        response.status().is_success(),
        "PIN API returned HTTP {}",
        response.status()
    );
    Ok(serde_json::from_slice(&bounded_body(&mut response).await?)?)
}

const MAX_XML_BYTES: usize = 1024 * 1024;

/// Strips query values that carry key material before a URL reaches logs or errors.
fn redact(url: &str) -> String {
    let Some((base, query)) = url.split_once('?') else {
        return url.to_string();
    };
    let query: Vec<String> = query
        .split('&')
        .map(|pair| match pair.split_once('=') {
            Some((k, _))
                if matches!(
                    k,
                    "rikey"
                        | "clientpairingsecret"
                        | "serverchallengeresp"
                        | "clientchallenge"
                        | "clientcert"
                        | "salt"
                ) =>
            {
                format!("{k}=<redacted>")
            }
            _ => pair.to_string(),
        })
        .collect();
    format!("{base}?{}", query.join("&"))
}

#[derive(Debug, Clone, Serialize)]
pub struct DisplayMode {
    pub width: u32,
    pub height: u32,
    pub refresh_rate: u32,
}

#[derive(Debug, Clone, Serialize)]
pub struct ServerInfo {
    pub hostname: String,
    pub app_version: String,
    pub uniqueid: String,
    pub http_port: u16,
    pub https_port: u16,
    pub codec_mode_support: u32,
    pub current_game: u32,
    pub state: Option<String>,
    /// Only reported over HTTPS, where the host knows which client is asking.
    pub pair_status: Option<bool>,
    pub display_modes: Vec<DisplayMode>,
}

impl ServerInfo {
    pub fn major_version(&self) -> Option<u32> {
        self.app_version.split('.').next()?.parse().ok()
    }
}

fn parse_server_info(xml: &XmlResponse, http_port: u16) -> Result<ServerInfo> {
    let num = |tag: &str| -> Result<Option<u64>> {
        xml.get(tag)
            .map(|v| {
                v.parse::<u64>()
                    .with_context(|| format!("<{tag}> is not a number: {v:?}"))
            })
            .transpose()
    };
    Ok(ServerInfo {
        hostname: xml.require("hostname")?.to_string(),
        app_version: xml.require("appversion")?.to_string(),
        uniqueid: xml.require("uniqueid")?.to_string(),
        http_port: num("ExternalPort")?.map_or(Ok(http_port), u16::try_from)?,
        https_port: u16::try_from(num("HttpsPort")?.ok_or_else(|| anyhow!("no <HttpsPort>"))?)?,
        codec_mode_support: num("ServerCodecModeSupport")?.map_or(Ok(0), u32::try_from)?,
        current_game: num("currentgame")?.map_or(Ok(0), u32::try_from)?,
        state: xml.get("state").map(str::to_owned),
        pair_status: xml.get("PairStatus").map(parse_bool).transpose()?,
        display_modes: xml
            .records("DisplayMode")?
            .iter()
            .map(|m| -> Result<DisplayMode> {
                Ok(DisplayMode {
                    width: m.get("Width").context("mode lacks Width")?.parse()?,
                    height: m.get("Height").context("mode lacks Height")?.parse()?,
                    refresh_rate: m
                        .get("RefreshRate")
                        .context("mode lacks RefreshRate")?
                        .parse()?,
                })
            })
            .collect::<Result<_>>()?,
    })
}

/// Server info over plain HTTP; works before pairing.
pub async fn server_info_http(host: &Host, unique_id: &str) -> Result<ServerInfo> {
    let client = tls::http_client(Duration::from_secs(10))?;
    let xml = get_xml(
        &client,
        &host.http_url(&format!("/serverinfo?{}", base_query(unique_id))),
    )
    .await?;
    parse_server_info(&xml, host.http_port)
}

#[derive(Debug, Clone, Serialize)]
pub struct App {
    pub id: u32,
    pub title: String,
    pub hdr_supported: bool,
}

fn parse_bool(value: &str) -> Result<bool> {
    match value {
        "0" => Ok(false),
        "1" => Ok(true),
        _ => bail!("invalid protocol boolean"),
    }
}
fn parse_apps(xml: &XmlResponse) -> Result<Vec<App>> {
    let mut ids = std::collections::HashSet::new();
    xml.records("App")?
        .iter()
        .map(|record| {
            let id = record.get("ID").context("app lacks ID")?.parse()?;
            ensure!(ids.insert(id), "duplicate app ID");
            let title = record
                .get("AppTitle")
                .context("app lacks AppTitle")?
                .to_owned();
            ensure!(!title.is_empty(), "empty app title");
            Ok(App {
                id,
                title,
                // Exact upstream behavior: moonlight-qt f786e94c, app/backend/nvhttp.cpp:318-319.
                // Non-"1" text (including Wolf's "false") means SDR, not an invalid app list.
                hdr_supported: record.get("IsHdrSupported").is_some_and(|v| v == "1"),
            })
        })
        .collect()
}

/// Parameters for `/launch` and `/resume`, as moonlight-qt sends them (protocol-notes §2).
#[derive(Debug, Clone)]
pub struct LaunchRequest {
    pub app_id: u32,
    pub width: u32,
    pub height: u32,
    pub fps: u32,
    /// AES key for the session's control, RTSP, video, and audio encryption.
    pub rikey: [u8; 16],
    /// Audio IV base; sent as a signed decimal, used by the host as a big-endian u32.
    pub rikeyid: u32,
    /// Ask for HDR: sent when the client supports 10-bit formats.
    pub hdr: bool,
    /// Play audio on the host as well as streaming it.
    pub host_audio: bool,
    /// Client report session UUID carried only to a compatible host for
    /// session-bound host observation export. Other hosts ignore the query key.
    pub session_id: String,
}

impl LaunchRequest {
    pub fn with_random_keys(app_id: u32, width: u32, height: u32, fps: u32, hdr: bool) -> Self {
        Self {
            app_id,
            width,
            height,
            fps,
            rikey: rand::random(),
            rikeyid: rand::random(),
            hdr,
            host_audio: false,
            session_id: String::new(),
        }
    }

    fn query(&self) -> String {
        // Stereo: channel mask 0x3 in the high 16 bits, 2 channels in the low 16 bits.
        const SURROUND_AUDIO_INFO_STEREO: u32 = (0x3 << 16) | 2;
        let mut q = format!(
            "appid={}&mode={}x{}x{}&additionalStates=1&sops=0&rikey={}&rikeyid={}",
            self.app_id,
            self.width,
            self.height,
            self.fps,
            hex::encode(self.rikey),
            self.rikeyid as i32
        );
        if self.hdr {
            q.push_str(
                "&hdrMode=1&clientHdrCapVersion=0&clientHdrCapSupportedFlagsInUint32=0\
                 &clientHdrCapMetaDataId=NV_STATIC_METADATA_TYPE_1\
                 &clientHdrCapDisplayData=0x0x0x0x0x0x0x0x0x0x0",
            );
        }
        if !self.session_id.is_empty() {
            q.push_str(&format!("&x-sp5-session={}", self.session_id));
        }
        q.push_str(&format!(
            "&localAudioPlayMode={}&surroundAudioInfo={SURROUND_AUDIO_INFO_STEREO}\
             &remoteControllersBitmap=1&gcmap=1&gcpersist=0&corever=1",
            u8::from(self.host_audio)
        ));
        q
    }
}

fn load_pin(paths: &Paths, id: &str) -> Result<Vec<u8>> {
    let path = paths.host_cert(id)?;
    let metadata = std::fs::symlink_metadata(&path)
        .context("no stored host pin; pair with an isolated config directory first")?;
    ensure!(
        metadata.is_file() && !metadata.file_type().is_symlink() && metadata.len() <= 65536,
        "invalid stored host pin"
    );
    paths.load_host_cert(id)
}

/// The host's paired (HTTPS, mutual TLS) API.
pub struct PairedHost {
    pub host: Host,
    pub https_port: u16,
    pub unique_id: String,
    client: reqwest::Client,
}

impl PairedHost {
    /// Connects using the certificate pinned when this host was paired.
    pub async fn connect(paths: &Paths, identity: &ClientIdentity, host: &Host) -> Result<Self> {
        let info = server_info_http(host, &identity.unique_id).await?;
        let cert = load_pin(paths, &host.pin_id())?;
        ensure!(
            cert == load_pin(paths, &info.uniqueid)?,
            "discovered host identity differs from pinned endpoint"
        );
        // Launching can take a long time while the host starts the app.
        let client = tls::https_client(
            identity,
            tls::ServerTrust::Pinned(cert),
            Duration::from_secs(120),
        )?;
        let paired = Self {
            host: host.clone(),
            https_port: info.https_port,
            unique_id: identity.unique_id.clone(),
            client,
        };
        let authenticated = paired.server_info().await?;
        ensure!(
            authenticated.uniqueid == info.uniqueid,
            "authenticated host ID differs from discovery"
        );
        ensure!(
            authenticated.pair_status == Some(true),
            "host no longer authorizes this client; pair again"
        );
        Ok(paired)
    }

    async fn get(&self, path: &str, query: &str) -> Result<XmlResponse> {
        let mut url = self.host.https_url(
            self.https_port,
            &format!("{path}?{}", base_query(&self.unique_id)),
        );
        if !query.is_empty() {
            url.push('&');
            url.push_str(query);
        }
        get_xml(&self.client, &url).await
    }

    /// Includes this client's pairing status and running app, unlike the HTTP variant.
    pub async fn server_info(&self) -> Result<ServerInfo> {
        parse_server_info(&self.get("/serverinfo", "").await?, self.host.http_port)
    }

    pub async fn app_list(&self) -> Result<Vec<App>> {
        parse_apps(&self.get("/applist", "").await?)
    }

    /// Starts the app and returns the RTSP session URL.
    pub async fn launch(&self, req: &LaunchRequest) -> Result<String> {
        let xml = self.get("/launch", &req.query()).await?;
        if xml.get("gamesession").is_none_or(|v| v == "0") {
            bail!("host did not start a game session");
        }
        Ok(xml.require("sessionUrl0")?.to_string())
    }

    /// Reattaches to the app this client already has running.
    pub async fn resume(&self, req: &LaunchRequest) -> Result<String> {
        let xml = self.get("/resume", &req.query()).await?;
        if xml.get("resume").is_none_or(|v| v == "0") {
            bail!("host did not resume the session");
        }
        Ok(xml.require("sessionUrl0")?.to_string())
    }

    /// Ends the running app session on the host.
    pub async fn cancel(&self) -> Result<()> {
        // Moonlight Qt f786e94c nvhttp.cpp:239-258 checks status then current game, not a cancel tag.
        self.get("/cancel", "").await?;
        let info = self.server_info().await?;
        ensure!(
            !(info
                .state
                .as_deref()
                .is_some_and(|state| state.ends_with("_SERVER_BUSY"))
                && info.current_game != 0),
            "host still reports a running game after cancel"
        );
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SERVERINFO: &str = r#"<?xml version="1.0" encoding="utf-8"?>
<root status_code="200"><hostname>Direwolf</hostname><appversion>7.1.431.-1</appversion>
<uniqueid>dd7c60f6-4b88-4ef1-be07-eeec72f96080</uniqueid><ServerCodecModeSupport>65793</ServerCodecModeSupport>
<HttpsPort>47984</HttpsPort><ExternalPort>47989</ExternalPort><currentgame>0</currentgame>
<SupportedDisplayMode><DisplayMode><Width>1920</Width><Height>1080</Height><RefreshRate>60</RefreshRate></DisplayMode>
<DisplayMode><Width>3840</Width><Height>2160</Height><RefreshRate>120</RefreshRate></DisplayMode></SupportedDisplayMode></root>"#;

    #[test]
    fn parses_server_info() {
        let info =
            parse_server_info(&XmlResponse::parse(SERVERINFO.into()).unwrap(), 47989).unwrap();
        assert_eq!(info.hostname, "Direwolf");
        assert_eq!(info.major_version(), Some(7));
        assert_eq!(info.https_port, 47984);
        assert_eq!(info.codec_mode_support, 65793);
        assert_eq!(info.display_modes.len(), 2);
        assert_eq!(info.display_modes[1].refresh_rate, 120);
        assert_eq!(info.pair_status, None);
    }

    #[test]
    fn error_status_becomes_error() {
        let err = XmlResponse::parse(
            r#"<root status_code="401" status_message="The client is not authorized"/>"#.into(),
        )
        .unwrap_err();
        assert!(err.to_string().contains("401"));
    }

    #[test]
    fn launch_query_matches_moonlight() {
        let req = LaunchRequest {
            app_id: 1,
            width: 1920,
            height: 1080,
            fps: 60,
            rikey: [0xab; 16],
            rikeyid: 0xffff_fffe,
            hdr: true,
            host_audio: false,
            session_id: String::new(),
        };
        let q = req.query();
        assert!(q.starts_with("appid=1&mode=1920x1080x60&additionalStates=1&sops=0&rikey=abababab"));
        assert!(
            q.contains("&rikeyid=-2&"),
            "rikeyid is sent as a signed integer: {q}"
        );
        assert!(q.contains("&hdrMode=1&clientHdrCapVersion=0&"));
        assert!(q.contains("&surroundAudioInfo=196610&"));
        assert!(q.ends_with("&corever=1"));
    }

    #[test]
    fn launch_query_carries_session_binding() {
        let mut req = LaunchRequest {
            app_id: 1,
            width: 1920,
            height: 1080,
            fps: 60,
            rikey: [0xab; 16],
            rikeyid: 0,
            hdr: false,
            host_audio: false,
            session_id: String::new(),
        };
        req.session_id = "641aca5e-a657-4b87-afa6-b70eacd2d0f0".into();
        let q = req.query();
        assert!(q.contains("&x-sp5-session=641aca5e-a657-4b87-afa6-b70eacd2d0f0"));
        assert!(q.ends_with("&corever=1"));
    }

    #[test]
    fn redacts_key_material() {
        let url = "https://h:47984/launch?uniqueid=x&rikey=00112233&rikeyid=5";
        assert_eq!(
            redact(url),
            "https://h:47984/launch?uniqueid=x&rikey=<redacted>&rikeyid=5"
        );
    }
    #[test]
    fn bounded_xml_requires_unambiguous_status_and_fields() {
        for body in [
            "<root/>",
            "<other status_code=\"200\"/>",
            "<root status_code=\"200\"><paired>1</paired><paired>0</paired></root>",
        ] {
            assert!(XmlResponse::parse(body.into()).is_err());
        }
        assert!(XmlResponse::parse(" ".repeat(MAX_XML_BYTES + 1)).is_err());
        assert!(XmlResponse::parse(
            "<root status_code=\"200\"><App><ID>1</ID><ID>2</ID></App></root>".into()
        )
        .unwrap()
        .records("App")
        .is_err());
    }
    #[test]
    fn app_list_keeps_all_records_and_rejects_malformed_entries() {
        let xml = XmlResponse::parse("<root status_code=\"200\"><App><ID>1</ID><AppTitle>Desktop</AppTitle><IsHdrSupported>1</IsHdrSupported></App><App><ID>2</ID><AppTitle>Game &amp; Pad</AppTitle></App></root>".into()).unwrap();
        let apps = parse_apps(&xml).unwrap();
        assert_eq!(apps.len(), 2);
        assert!(apps[0].hdr_supported);
        assert_eq!(apps[1].title, "Game & Pad");
        assert!(!apps[1].hdr_supported);
        for entry in ["<ID>bad</ID><AppTitle>Game</AppTitle>", "<ID>1</ID>"] {
            let xml = XmlResponse::parse(format!(
                "<root status_code=\"200\"><App>{entry}</App></root>"
            ))
            .unwrap();
            assert!(parse_apps(&xml).is_err());
        }
    }
    #[test]
    fn wolf_app_hdr_text_matches_official_moonlight_without_rejecting_apps() {
        // Sanitized live Fenrir 5c8251c app-list fields observed during T038.
        let xml = XmlResponse::parse(r#"<root status_code="200"><App><AppTitle>Cove</AppTitle><ID>4</ID><IsHdrSupported>false</IsHdrSupported></App><App><AppTitle>EmuDeck</AppTitle><ID>2</ID><IsHdrSupported>false</IsHdrSupported></App></root>"#.into()).unwrap();
        let apps = parse_apps(&xml).unwrap();
        assert_eq!(apps.iter().map(|a| a.id).collect::<Vec<_>>(), [4, 2]);
        assert!(apps.iter().all(|a| !a.hdr_supported));
        // Upstream tests exact text equality, not a general boolean conversion.
        for (text, expected) in [
            ("1", true),
            ("0", false),
            ("true", false),
            ("false", false),
            ("maybe", false),
            (" 1 ", false),
            ("", false),
        ] {
            let xml = XmlResponse::parse(format!("<root status_code=\"200\"><App><ID>1</ID><AppTitle>Fixture</AppTitle><IsHdrSupported>{text}</IsHdrSupported></App></root>")).unwrap();
            assert_eq!(
                parse_apps(&xml).unwrap()[0].hdr_supported,
                expected,
                "{text:?}"
            );
        }
    }
    #[tokio::test]
    async fn cancel_accepts_wolf_status_response_and_verifies_current_game() {
        use std::io::{Read, Write};
        use std::sync::Arc;
        for (state, game, expected) in [
            ("SUNSHINE_SERVER_FREE", 0, true),
            ("SUNSHINE_SERVER_BUSY", 4, false),
        ] {
            let rcgen::CertifiedKey { cert, key_pair } =
                rcgen::generate_simple_self_signed(vec!["localhost".into()]).unwrap();
            let root = reqwest::Certificate::from_der(cert.der()).unwrap();
            let config = rustls::ServerConfig::builder_with_provider(Arc::new(
                rustls::crypto::ring::default_provider(),
            ))
            .with_safe_default_protocol_versions()
            .unwrap()
            .with_no_client_auth()
            .with_single_cert(
                vec![cert.der().clone()],
                rustls::pki_types::PrivatePkcs8KeyDer::from(key_pair.serialize_der()).into(),
            )
            .unwrap();
            let socket = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
            socket.set_nonblocking(true).unwrap();
            let port = socket.local_addr().unwrap().port();
            let worker = tokio::task::spawn_blocking(move || {
                let deadline = std::time::Instant::now() + Duration::from_secs(6);
                let mut count = 0;
                while count < 2 && std::time::Instant::now() < deadline {
                    let (tcp, _) = match socket.accept() {
                        Ok(pair) => pair,
                        Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                            std::thread::sleep(Duration::from_millis(5));
                            continue;
                        }
                        Err(e) => panic!("{e}"),
                    };
                    tcp.set_nonblocking(false).unwrap();
                    tcp.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
                    let mut tls = rustls::StreamOwned::new(
                        rustls::ServerConnection::new(Arc::new(config.clone())).unwrap(),
                        tcp,
                    );
                    let mut request = Vec::new();
                    let mut byte = [0];
                    while !request.ends_with(b"\r\n\r\n") {
                        tls.read_exact(&mut byte).unwrap();
                        request.push(byte[0]);
                    }
                    let request = String::from_utf8(request).unwrap();
                    let body = if request.starts_with("GET /cancel?") {
                        "<root status_code=\"200\"/>".to_string()
                    } else {
                        assert!(request.starts_with("GET /serverinfo?"));
                        SERVERINFO
                            .replace(
                                "<currentgame>0</currentgame>",
                                &format!("<currentgame>{game}</currentgame>"),
                            )
                            .replace("</root>", &format!("<state>{state}</state></root>"))
                    };
                    write!(
                        tls,
                        "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                        body.len(),
                        body
                    )
                    .unwrap();
                    tls.flush().unwrap();
                    count += 1;
                }
                count
            });
            let host = PairedHost {
                host: Host {
                    address: "localhost".into(),
                    http_port: 47989,
                },
                https_port: port,
                unique_id: "synthetic-cancel".into(),
                client: reqwest::Client::builder()
                    .add_root_certificate(root)
                    .timeout(Duration::from_secs(5))
                    .build()
                    .unwrap(),
            };
            let outcome = host.cancel().await;
            assert_eq!(outcome.is_ok(), expected, "{outcome:?}");
            assert_eq!(worker.await.unwrap(), 2);
        }
    }
    #[test]
    fn queries_encode_identifiers_and_ipv6_endpoints() {
        assert!(base_query("id&injected=1").starts_with("uniqueid=id%26injected%3D1&uuid="));
        let host = Host {
            address: "::1".into(),
            http_port: 47989,
        };
        assert_eq!(
            host.http_url("/serverinfo"),
            "http://[::1]:47989/serverinfo"
        );
        assert_ne!(
            host.pin_id(),
            Host {
                address: "::1".into(),
                http_port: 47990
            }
            .pin_id()
        );
    }
}
