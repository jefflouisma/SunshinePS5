//! Moonlight protocol test client for SunshinePS5.
//!
//! Drives a GameStream host the way an official Moonlight client does (pairing, launch, RTSP,
//! control, video, audio, input) and measures what it receives. An HDMI capture of the host's
//! own output can be recorded alongside a stream to score fidelity and delay without any
//! cooperation from the host.

use anyhow::{anyhow, bail, Context, Result};
use clap::{Args, Parser, Subcommand};
use std::net::SocketAddr;
use std::path::PathBuf;

use moonlight_mcp::config::Paths;
use moonlight_mcp::identity::ClientIdentity;
use moonlight_mcp::nvhttp::{Host, PairedHost, ServerInfo};
use moonlight_mcp::pairing::PinDelivery;
use moonlight_mcp::reference::{CaptureDevice, ReferenceSource};
use moonlight_mcp::{nvhttp, pairing, reference, runner};

#[derive(Parser)]
#[command(name = "moonlight-mcp", version, about)]
struct Cli {
    /// Directory for the client identity and pinned host certificates.
    #[arg(long, global = true)]
    config_dir: Option<PathBuf>,
    #[command(subcommand)]
    command: Command,
}

#[derive(Args, Clone)]
struct HostArgs {
    /// Host address (IP or name).
    #[arg(short = 'H', long)]
    host: String,
    /// The host's plain-HTTP port; the HTTPS port is read from its server info.
    #[arg(long, default_value_t = 47989)]
    http_port: u16,
}

impl HostArgs {
    fn host(&self) -> Host {
        Host {
            address: self.host.clone(),
            http_port: self.http_port,
        }
    }
}

#[derive(Args, Clone)]
struct DeviceArgs {
    /// AVFoundation name of the HDMI capture device.
    #[arg(long, default_value = "Elgato 4K X")]
    device: String,
    /// Capture mode as WIDTHxHEIGHT.
    #[arg(long, default_value = "1920x1080", value_parser = parse_size)]
    device_size: (u32, u32),
    #[arg(long, default_value_t = 60)]
    device_fps: u32,
}

impl DeviceArgs {
    fn device(&self) -> CaptureDevice {
        CaptureDevice {
            name: self.device.clone(),
            width: self.device_size.0,
            height: self.device_size.1,
            fps: self.device_fps,
        }
    }
}

#[derive(Args)]
struct RunBindings {
    #[arg(long, default_value_t = 47989)]
    http_port: u16,
    #[arg(long)]
    app_id: Option<u32>,
    /// Bind a scenario provider_ref to separately supplied PIN delivery configuration.
    #[arg(long, requires = "pin_delivery")]
    pin_provider_ref: Option<String>,
    #[arg(long, requires = "pin_provider_ref")]
    pin_delivery: Option<String>,
    #[arg(long)]
    sunshine_user: Option<String>,
    #[arg(long)]
    sunshine_password: Option<String>,
    #[arg(long)]
    sunshine_certificate: Option<PathBuf>,
    /// Explicit device binding; no capture device opens without this selection.
    #[arg(long)]
    reference_device_ref: Option<String>,
    #[arg(long)]
    capture_server: Option<SocketAddr>,
    #[command(flatten)]
    device: DeviceArgs,
    #[arg(long, requires = "reference_device_ref")]
    hdr_helper: Option<PathBuf>,
    #[arg(long, requires_all = ["reference_device_ref", "hdr_helper"])]
    hdr_source_evidence: Option<PathBuf>,
    /// Trusted JSON-lines host observations written by an independent observer on the
    /// reference host for this run's session (REF-controller); never agent-supplied.
    #[arg(long)]
    host_observations: Option<PathBuf>,
    /// Read-only console host-observation JSONL endpoint fetched after teardown.
    #[arg(long, conflicts_with = "host_observations")]
    host_observations_url: Option<String>,
    /// Trusted independent reference-host pairing/teardown snapshot with hashed source files.
    /// Read after host cancellation; never accepted as an MCP tool argument.
    #[arg(long)]
    reference_lifecycle: Option<PathBuf>,
    /// Trusted native recording and report-correctness snapshot with hashed sources.
    #[arg(long)]
    reference_recording: Option<PathBuf>,
    /// Wait up to ten seconds for this observer READY file before launching the host app.
    /// The run publishes observer-binding.json in its artifact directory first.
    #[arg(long)]
    observer_ready: Option<PathBuf>,
    /// Publish an immutable cancellation result for the trusted observer supervisor.
    #[arg(long, requires = "observer_ready")]
    observer_finish_signal: bool,
    /// Wait for a bound runtime-attachment envelope after launch and before RTSP/input.
    #[arg(long, requires = "observer_ready")]
    observer_attachment: Option<PathBuf>,
}

#[derive(Subcommand)]
enum Command {
    /// Serve bounded local tools over stdio MCP; outputs stay beneath this root.
    Mcp {
        #[arg(long)]
        output_root: Option<PathBuf>,
        #[command(flatten)]
        bindings: RunBindings,
    },
    /// Execute an immutable scenario and always retain its acceptance report.
    Run {
        #[arg(long)]
        scenario: PathBuf,
        #[arg(long)]
        report: PathBuf,
        #[command(flatten)]
        bindings: RunBindings,
    },
    /// Print the host's server info as JSON.
    Serverinfo(HostArgs),
    /// Pair with the host.
    Pair {
        #[command(flatten)]
        host: HostArgs,
        /// PIN to use; a random 4-digit PIN by default, as Moonlight clients generate.
        #[arg(long)]
        pin: Option<String>,
        /// How the PIN reaches the host: `manual`, `wolf`, `command:<shell command>`
        /// (PIN in $MOONLIGHT_PIN), or `sunshine:<web UI URL>` with --sunshine-user/-password.
        #[arg(long, default_value = "manual")]
        pin_delivery: String,
        #[arg(long)]
        sunshine_user: Option<String>,
        #[arg(long)]
        sunshine_password: Option<String>,
        /// Trusted Sunshine web-UI certificate (PEM), obtained through a trusted local channel.
        /// Required for sunshine: PIN delivery; separate from the GameStream pairing pin.
        #[arg(long)]
        sunshine_certificate: Option<PathBuf>,
    },
    /// Remove this client's pairing from the host.
    Unpair(HostArgs),
    /// List the host's apps.
    Apps(HostArgs),
    /// End the running app session on the host.
    Quit(HostArgs),
    /// Record the HDMI reference for a number of seconds.
    RecordReference {
        #[arg(long)]
        seconds: f64,
        #[arg(long)]
        out: PathBuf,
        /// Record through a capture server at this address instead of opening the device here.
        #[arg(long)]
        capture_server: Option<SocketAddr>,
        #[arg(long, value_enum, default_value_t = SignalRange::Sdr)]
        dynamic_range: SignalRange,
        /// Explicit local ten-bit acquisition helper executable.
        #[arg(long)]
        hdr_helper: Option<PathBuf>,
        #[arg(long)]
        hdr_source_evidence: Option<PathBuf>,
        #[command(flatten)]
        device: DeviceArgs,
    },
    /// Compare a decoded stream recording with an HDMI reference recording.
    Compare {
        #[arg(long)]
        stream: PathBuf,
        #[arg(long)]
        reference: PathBuf,
        /// Directory for sample PNGs.
        #[arg(long)]
        out: PathBuf,
        #[arg(long)]
        profile: Option<PathBuf>,
        #[arg(long)]
        report: Option<PathBuf>,
    },
    /// Analyze recorded capture cadence offline from frame timestamps already on disk.
    /// Diagnostic only: it opens no device, contacts nothing, and is not acceptance evidence.
    AnalyzeCadence {
        /// Capture output directory holding frames.json, recording.json and any capture summary.
        #[arg(long)]
        capture: PathBuf,
        /// Classify an interval as a gap when it exceeds this multiple of the nominal interval.
        #[arg(long, default_value_t = 1.5)]
        gap_threshold: f64,
        /// Explicit nominal capture rate; overrides whatever rate the capture metadata declares.
        #[arg(long)]
        nominal_fps: Option<f64>,
        /// Declare leading frames as a startup window; the full population is still reported.
        #[arg(long, requires = "startup_exclude_reason")]
        startup_exclude_frames: Option<usize>,
        /// Stated reason for the declared startup window; exclusions are never silent.
        #[arg(long, requires = "startup_exclude_frames")]
        startup_exclude_reason: Option<String>,
        /// Also write the JSON report to this new path; an existing path is never overwritten.
        #[arg(long)]
        report: Option<PathBuf>,
    },
    /// Serve HDMI recordings over localhost; run this from a terminal with camera access.
    CaptureServer {
        #[arg(long, default_value = "127.0.0.1:8766")]
        listen: SocketAddr,
        #[arg(long)]
        output_root: Option<PathBuf>,
        #[command(flatten)]
        device: DeviceArgs,
    },
}

#[derive(Clone, Copy, clap::ValueEnum)]
enum SignalRange {
    Sdr,
    Hdr,
}

fn parse_size(s: &str) -> Result<(u32, u32), String> {
    let (w, h) = s
        .split_once('x')
        .ok_or_else(|| format!("expected WIDTHxHEIGHT, got {s:?}"))?;
    Ok((
        w.parse().map_err(|_| format!("bad width in {s:?}"))?,
        h.parse().map_err(|_| format!("bad height in {s:?}"))?,
    ))
}

fn print_json(value: &impl serde::Serialize) -> Result<()> {
    println!("{}", serde_json::to_string_pretty(value)?);
    Ok(())
}

fn pin_delivery(
    spec: &str,
    user: Option<String>,
    password: Option<String>,
    certificate: Option<PathBuf>,
) -> Result<PinDelivery> {
    Ok(match spec.split_once(':') {
        None if spec == "manual" => PinDelivery::Manual,
        None if spec == "wolf" => PinDelivery::Wolf,
        Some(("command", cmd)) => PinDelivery::Command(cmd.to_string()),
        Some(("sunshine", url)) => {
            let path = certificate.context(
                "--sunshine-certificate is required for automated Sunshine PIN delivery",
            )?;
            let metadata =
                std::fs::metadata(&path).context("reading Sunshine certificate metadata")?;
            if !metadata.is_file() || metadata.len() > 65536 {
                bail!("invalid Sunshine certificate file");
            }
            let blocks =
                pem::parse_many(std::fs::read(&path).context("reading Sunshine certificate")?)?;
            if blocks.len() != 1 || blocks[0].tag() != "CERTIFICATE" {
                bail!("expected one PEM Sunshine certificate");
            }
            let certificate_der = blocks[0].contents().to_vec();
            moonlight_mcp::identity::certificate_signature(&certificate_der)
                .context("invalid Sunshine X.509 certificate")?;
            PinDelivery::SunshineApiPinned {
                url: url.to_string(),
                username: user.ok_or_else(|| anyhow!("--sunshine-user is required"))?,
                password: password.ok_or_else(|| anyhow!("--sunshine-password is required"))?,
                certificate_der,
            }
        }
        _ => bail!("unknown --pin-delivery {spec:?}"),
    })
}

fn read_hdr_source(path: Option<PathBuf>) -> Result<Option<moonlight_mcp::hdr_capture::HdrSource>> {
    path.map(|path| {
        let file = std::fs::File::open(path)?;
        if file.metadata()?.len() > 65536 {
            bail!("HDR source evidence exceeds metadata bound");
        }
        Ok(serde_json::from_reader(file)?)
    })
    .transpose()
}

fn bind_options(options: &mut runner::RunOptions, bindings: RunBindings) -> Result<()> {
    let RunBindings {
        http_port,
        app_id,
        pin_provider_ref,
        pin_delivery: delivery,
        sunshine_user,
        sunshine_password,
        sunshine_certificate,
        reference_device_ref,
        capture_server,
        device,
        hdr_helper,
        hdr_source_evidence,
        host_observations,
        host_observations_url,
        reference_lifecycle,
        reference_recording,
        observer_ready,
        observer_finish_signal,
        observer_attachment,
    } = bindings;
    options.http_port = http_port;
    options.host_observations = host_observations;
    options.host_observations_url = host_observations_url;
    options.reference_lifecycle = reference_lifecycle;
    options.reference_recording = reference_recording;
    options.observer_ready = observer_ready;
    options.observer_finish_signal = observer_finish_signal;
    options.observer_attachment = observer_attachment;
    options.app_id = app_id;
    if let (Some(id), Some(delivery)) = (pin_provider_ref, delivery) {
        moonlight_mcp::evidence::validate_id(&id)?;
        let delivery = pin_delivery(
            &delivery,
            sunshine_user,
            sunshine_password,
            sunshine_certificate,
        )?;
        if matches!(delivery, PinDelivery::Manual) {
            bail!("manual PIN delivery is unavailable in scenarios");
        }
        options.providers.insert(id, delivery);
    }
    if let Some(id) = reference_device_ref {
        moonlight_mcp::evidence::validate_id(&id)?;
        let binding = if let Some(executable) = hdr_helper {
            if capture_server.is_some() {
                bail!("choose HDR helper or SDR capture server");
            }
            runner::ReferenceBinding::Hdr(moonlight_mcp::hdr_capture::HdrCapture {
                executable,
                source: read_hdr_source(hdr_source_evidence)?,
                max_bytes: 8 * 1024 * 1024 * 1024,
            })
        } else {
            let source = match capture_server {
                Some(addr) => {
                    moonlight_mcp::capture_server::validate_loopback(addr)?;
                    ReferenceSource::Server(addr)
                }
                None => ReferenceSource::Direct(device.device()),
            };
            runner::ReferenceBinding::Sdr(source)
        };
        options.references.insert(id, binding);
    }
    Ok(())
}

#[tokio::main]
async fn main() {
    let cli = match Cli::try_parse() {
        Ok(cli) => cli,
        Err(error) => {
            let code = if error.use_stderr() {
                runner::INVALID_INVOCATION
            } else {
                0
            };
            let _ = error.print();
            std::process::exit(code);
        }
    };
    let invalid_code = if matches!(
        cli.command,
        Command::Run { .. } | Command::Compare { .. } | Command::RecordReference { .. }
    ) {
        runner::INVALID_INVOCATION
    } else {
        1
    };
    match execute(cli).await {
        Ok(()) => (),
        Err(error) => {
            eprintln!("{error:#}");
            std::process::exit(invalid_code);
        }
    }
}

async fn execute(cli: Cli) -> Result<()> {
    let paths = || Paths::resolve(cli.config_dir.as_deref());
    match cli.command {
        Command::Mcp {
            output_root,
            bindings,
        } => {
            let mut config = moonlight_mcp::mcp::Config::new(
                output_root.unwrap_or(std::env::current_dir()?),
                cli.config_dir,
            )?;
            bind_options(&mut config.options, bindings)?;
            moonlight_mcp::mcp::stdio(config).await
        }
        Command::Run {
            scenario,
            report,
            bindings,
        } => {
            let scenario =
                moonlight_mcp::scenario::Scenario::from_reader(std::fs::File::open(scenario)?)?;
            let mut options = runner::RunOptions::new(std::env::current_dir()?, report);
            options.config_dir = cli.config_dir;
            bind_options(&mut options, bindings)?;
            let (stop, cancel) = tokio::sync::watch::channel(false);
            let signal = tokio::spawn(async move {
                if tokio::signal::ctrl_c().await.is_ok() {
                    let _ = stop.send(true);
                }
            });
            let report = runner::run(&scenario, &options, cancel).await?;
            signal.abort();
            print_json(
                &serde_json::json!({"overall_status":report.overall_status,"report":options.report}),
            )?;
            std::process::exit(runner::exit_code(report.overall_status));
        }
        Command::Serverinfo(h) => {
            let host = h.host();
            let paths = paths()?;
            let identity = ClientIdentity::load_or_create(&paths)?;
            let info = nvhttp::server_info_http(&host, &identity.unique_id).await?;
            // Over HTTPS the host also reports whether this client is paired.
            match paths.load_host_cert(&info.uniqueid) {
                Ok(_) => {
                    let paired = PairedHost::connect(&paths, &identity, &host).await?;
                    let info: ServerInfo = paired.server_info().await?;
                    print_json(&info)
                }
                Err(_) => print_json(&info),
            }
        }
        Command::Pair {
            host,
            pin,
            pin_delivery: spec,
            sunshine_user,
            sunshine_password,
            sunshine_certificate,
        } => {
            let paths = paths()?;
            let identity = ClientIdentity::load_or_create(&paths)?;
            let delivery = pin_delivery(
                &spec,
                sunshine_user,
                sunshine_password,
                sunshine_certificate,
            )?;
            let pin = pin.unwrap_or_else(pairing::random_pin);
            let info = pairing::pair(&paths, &identity, &host.host(), &pin, &delivery).await?;
            eprintln!("Paired with {} ({}).", info.hostname, info.uniqueid);
            Ok(())
        }
        Command::Unpair(h) => {
            let paths = paths()?;
            let identity = ClientIdentity::load_or_create(&paths)?;
            pairing::unpair(&h.host(), &identity.unique_id).await
        }
        Command::Apps(h) => {
            let paths = paths()?;
            let identity = ClientIdentity::load_or_create(&paths)?;
            let host = h.host();
            let paired = PairedHost::connect(&paths, &identity, &host).await?;
            print_json(&paired.app_list().await?)
        }
        Command::Quit(h) => {
            let paths = paths()?;
            let identity = ClientIdentity::load_or_create(&paths)?;
            let host = h.host();
            let paired = PairedHost::connect(&paths, &identity, &host).await?;
            paired.cancel().await
        }
        Command::RecordReference {
            seconds,
            out,
            capture_server,
            dynamic_range,
            hdr_helper,
            hdr_source_evidence,
            device,
        } => {
            if matches!(dynamic_range, SignalRange::Hdr) {
                let source = read_hdr_source(hdr_source_evidence)?;
                let capture = moonlight_mcp::hdr_capture::HdrCapture {
                    executable: hdr_helper.unwrap_or_default(),
                    source,
                    max_bytes: 8 * 1024 * 1024 * 1024,
                };
                let result = capture.acquire(seconds, &out).await;
                print_json(
                    &serde_json::json!({"overall_status":result.status,"reason":result.reason,"recording":result.recording}),
                )?;
                std::process::exit(runner::exit_code(result.status));
            }
            let source = match capture_server {
                Some(addr) => ReferenceSource::Server(addr),
                None => ReferenceSource::Direct(device.device()),
            };
            let index = source.record(seconds, &out).await?;
            eprintln!(
                "Recorded {} frames to {}",
                index.timestamps.len(),
                out.display()
            );
            Ok(())
        }
        Command::Compare {
            stream,
            reference,
            out,
            profile,
            report,
        } => {
            let acceptance = profile.is_some() || report.is_some();
            let (stop, cancel) = tokio::sync::watch::channel(false);
            let signal = tokio::spawn(async move {
                if tokio::signal::ctrl_c().await.is_ok() {
                    let _ = stop.send(true);
                }
            });
            let result = runner::compare_in_root(
                &std::env::current_dir()?,
                &stream,
                &reference,
                &out,
                profile.as_deref(),
                report.as_deref(),
                cancel,
            )
            .await?;
            signal.abort();
            if acceptance {
                print_json(&result)?;
                std::process::exit(runner::exit_code_name(
                    result["overall_status"]
                        .as_str()
                        .context("missing comparison status")?,
                )?);
            }
            print_json(&result["comparison"])
        }
        Command::AnalyzeCadence {
            capture,
            gap_threshold,
            nominal_fps,
            startup_exclude_frames,
            startup_exclude_reason,
            report,
        } => {
            use std::io::Write as _;
            let declared_startup_exclusion = match (startup_exclude_frames, startup_exclude_reason)
            {
                (Some(frames), Some(reason)) => {
                    Some(moonlight_mcp::cadence::StartupExclusion { frames, reason })
                }
                (None, None) => None,
                _ => bail!(
                    "--startup-exclude-frames and --startup-exclude-reason are required \
                         together"
                ),
            };
            let analysis = moonlight_mcp::cadence::analyze(
                &capture,
                &moonlight_mcp::cadence::CadenceOptions {
                    gap_threshold_nominal: gap_threshold,
                    nominal_fps_override: nominal_fps,
                    declared_startup_exclusion,
                },
            )?;
            if let Some(path) = report {
                let bytes = serde_json::to_vec_pretty(&analysis)?;
                std::fs::OpenOptions::new()
                    .write(true)
                    .create_new(true)
                    .open(&path)
                    .with_context(|| format!("writing cadence report to {}", path.display()))?
                    .write_all(&bytes)?;
            }
            print_json(&analysis)
        }
        Command::CaptureServer {
            listen,
            output_root,
            device,
        } => {
            reference::serve(
                listen,
                output_root.unwrap_or_else(reference::server_output_root),
                device.device(),
            )
            .await
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn observer_attachment_requires_readiness_and_propagates() {
        assert!(Cli::try_parse_from([
            "moonlight-mcp",
            "mcp",
            "--observer-attachment",
            "attached.json"
        ])
        .is_err());
        let cli = Cli::try_parse_from([
            "moonlight-mcp",
            "mcp",
            "--observer-ready",
            "ready.json",
            "--observer-attachment",
            "attached.json",
        ])
        .unwrap();
        let Command::Mcp { bindings, .. } = cli.command else {
            panic!("wrong command")
        };
        let mut options = runner::RunOptions::new(PathBuf::new(), PathBuf::new());
        bind_options(&mut options, bindings).unwrap();
        assert_eq!(
            options.with_report("next.json".into()).observer_attachment,
            Some("attached.json".into())
        );
    }
    #[test]
    fn finish_signal_requires_readiness_and_propagates() {
        assert!(Cli::try_parse_from(["moonlight-mcp", "mcp", "--observer-finish-signal"]).is_err());
        let cli = Cli::try_parse_from([
            "moonlight-mcp",
            "mcp",
            "--observer-ready",
            "ready.json",
            "--observer-finish-signal",
        ])
        .unwrap();
        let Command::Mcp { bindings, .. } = cli.command else {
            panic!("wrong command")
        };
        let mut options = runner::RunOptions::new(PathBuf::new(), PathBuf::new());
        bind_options(&mut options, bindings).unwrap();
        assert!(options.observer_finish_signal);
        assert!(
            options
                .with_report("next.json".into())
                .observer_finish_signal
        );
    }
    #[test]
    fn observer_readiness_is_an_optional_trusted_startup_binding() {
        for args in [
            vec![
                "moonlight-mcp",
                "run",
                "--scenario",
                "scenario.json",
                "--report",
                "report.json",
                "--observer-ready",
                "ready.json",
            ],
            vec!["moonlight-mcp", "mcp", "--observer-ready", "ready.json"],
        ] {
            let cli = Cli::try_parse_from(args).unwrap();
            let bindings = match cli.command {
                Command::Run { bindings, .. } | Command::Mcp { bindings, .. } => bindings,
                _ => panic!("unexpected command"),
            };
            let mut options = runner::RunOptions::new(PathBuf::new(), PathBuf::new());
            bind_options(&mut options, bindings).unwrap();
            assert_eq!(options.observer_ready, Some(PathBuf::from("ready.json")));
        }
    }
    #[test]
    fn automated_sunshine_delivery_requires_valid_trusted_certificate() {
        let error = pin_delivery(
            "sunshine:https://127.0.0.1:47990",
            Some("user".into()),
            Some("secret".into()),
            None,
        )
        .unwrap_err();
        assert!(error.to_string().contains("--sunshine-certificate"));
        let path = std::env::temp_dir().join(format!("sunshine-ca-{}", uuid::Uuid::new_v4()));
        for bytes in [
            b"invalid PEM".as_slice(),
            b"-----BEGIN CERTIFICATE-----\nAA==\n-----END CERTIFICATE-----\n".as_slice(),
        ] {
            std::fs::write(&path, bytes).unwrap();
            assert!(pin_delivery(
                "sunshine:https://127.0.0.1:47990",
                Some("user".into()),
                Some("secret".into()),
                Some(path.clone())
            )
            .is_err());
        }
        std::fs::remove_file(path).unwrap();
    }
}
