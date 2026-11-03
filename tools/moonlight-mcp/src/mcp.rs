//! Local stdio MCP 2025-06-18. Messages are newline-delimited JSON-RPC; stdout is protocol only.
//! Sources: https://modelcontextprotocol.io/specification/2025-06-18/basic/transports
//! and /basic/utilities/cancellation and /server/tools at the same pinned protocol version.

use crate::report::{Report, Status};
use crate::runner::{self, RunOptions};
use anyhow::{ensure, Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use tokio::io::{AsyncBufRead, AsyncBufReadExt, AsyncWrite, AsyncWriteExt};
use tokio::sync::watch;

pub const MAX_MESSAGE_BYTES: usize = 65536;
const MAX_RESPONSE_BYTES: usize = 262144;
const MAX_REPORT_BYTES: u64 = 8 * 1024 * 1024;

pub struct Config {
    pub options: RunOptions,
}
impl Config {
    pub fn new(root: PathBuf, config_dir: Option<PathBuf>) -> Result<Self> {
        let root = crate::capture_server::confined_root(&root)?;
        let mut options = RunOptions::new(root, PathBuf::new());
        options.config_dir = config_dir;
        Ok(Self { options })
    }
}

#[derive(Debug, Serialize)]
pub struct CriterionSummary {
    pub id: String,
    pub status: Status,
    pub reason: String,
}
#[derive(Debug, Serialize)]
pub struct ToolOutcome {
    pub overall_status: Status,
    pub reasons: Vec<String>,
    pub report: String,
    pub run_id: Option<String>,
    pub criteria: Vec<CriterionSummary>,
}
impl ToolOutcome {
    fn from_report(report: &Report, path: String) -> Self {
        Self {
            overall_status: report.overall_status,
            reasons: report
                .limitations
                .iter()
                .take(16)
                .map(|reason| reason.chars().take(64).collect())
                .collect(),
            report: path,
            run_id: Some(report.run_id.clone()),
            criteria: report
                .criteria
                .iter()
                .map(|c| CriterionSummary {
                    id: c.id.clone(),
                    status: c.status,
                    reason: c.reason.chars().take(64).collect(),
                })
                .collect(),
        }
    }
    fn from_comparison(result: &Value, path: String) -> Result<Self> {
        let status: Status = serde_json::from_value(result["overall_status"].clone())?;
        let results = result["criteria"]
            .as_array()
            .context("missing comparison criteria")?;
        ensure!(results.len() <= 128, "comparison criterion capacity");
        if !result["profile"].is_null() {
            let profile: runner::ComparisonProfile =
                serde_json::from_value(result["profile"].clone())?;
            profile.validate()?;
            ensure!(
                profile.criteria.len() == results.len()
                    && profile.criteria.iter().all(|required| results
                        .iter()
                        .filter(|reported| reported["id"] == required.id
                            && reported["threshold"]
                                == serde_json::to_value(&required.threshold).unwrap_or(Value::Null))
                        .count()
                        == 1),
                "comparison report removed or changed a declared criterion"
            );
        } else {
            ensure!(
                status == Status::Incomplete && results.is_empty(),
                "comparison without a profile cannot claim acceptance"
            );
        }
        let mut criteria = Vec::new();
        for c in results {
            let id = c["id"]
                .as_str()
                .context("missing comparison ID")?
                .to_string();
            crate::evidence::validate_id(&id)?;
            ensure!(
                !criteria
                    .iter()
                    .any(|other: &CriterionSummary| other.id == id),
                "duplicate comparison ID"
            );
            let reason = c["reason"]
                .as_str()
                .context("missing comparison reason")?
                .to_string();
            crate::evidence::validate_reason(&reason)?;
            criteria.push(CriterionSummary {
                id,
                status: serde_json::from_value(c["status"].clone())?,
                reason: reason.chars().take(64).collect(),
            });
        }
        ensure!(
            runner::exit_code(status)
                == runner::exit_code(crate::verdict::overall(criteria.iter().map(|c| c.status))),
            "comparison status contradicts criteria"
        );
        Ok(Self {
            overall_status: status,
            reasons: vec![
                "local picture subchecks do not establish complete release acceptance".into(),
            ],
            report: path,
            run_id: None,
            criteria,
        })
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RunArguments {
    scenario: String,
    report: String,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct CompareArguments {
    stream: String,
    reference: String,
    out: String,
    profile: Option<String>,
    report: String,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ReadArguments {
    report: String,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Call {
    name: String,
    arguments: Value,
}

fn local_path(root: &Path, text: &str) -> Result<PathBuf> {
    let reference = crate::evidence::ArtifactRef(text.into());
    reference.validate()?;
    let mut path = root.to_path_buf();
    for component in Path::new(text).components() {
        path.push(component);
        match std::fs::symlink_metadata(&path) {
            Ok(metadata) => ensure!(
                !metadata.file_type().is_symlink(),
                "tool paths cannot contain symlinks"
            ),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => (),
            Err(e) => return Err(e.into()),
        }
    }
    Ok(path)
}

enum Operation {
    Run(Box<crate::scenario::Scenario>, PathBuf),
    Compare(CompareArguments),
    Read(String),
}
fn check_file(path: &Path, limit: u64) -> Result<()> {
    let metadata = std::fs::symlink_metadata(path)?;
    ensure!(
        metadata.is_file() && !metadata.file_type().is_symlink() && metadata.len() <= limit,
        "tool input must be a bounded regular file"
    );
    Ok(())
}
fn operation(config: &Config, call: Call) -> Result<Operation> {
    let root = &config.options.root;
    Ok(match call.name.as_str() {
        "run_scenario" => {
            let a: RunArguments = serde_json::from_value(call.arguments)?;
            let scenario = local_path(root, &a.scenario)?;
            let report = local_path(root, &a.report)?;
            ensure!(!report.exists(), "report destination already exists");
            check_file(&scenario, crate::scenario::MAX_SCENARIO_BYTES)?;
            let scenario = crate::scenario::Scenario::from_reader(std::fs::File::open(scenario)?)?;
            // No creation or network access during admission.
            scenario.resolve_output(root)?;
            Operation::Run(Box::new(scenario), PathBuf::from(a.report))
        }
        "compare_recordings" => {
            let a: CompareArguments = serde_json::from_value(call.arguments)?;
            for text in [&a.stream, &a.reference, &a.out, &a.report] {
                local_path(root, text)?;
            }
            if let Some(text) = &a.profile {
                check_file(&local_path(root, text)?, 65536)?;
            }
            for directory in [&a.stream, &a.reference] {
                for name in ["frames.json", "recording.json"] {
                    let sidecar = root.join(directory).join(name);
                    if std::fs::symlink_metadata(&sidecar).is_ok() {
                        check_file(&sidecar, 4 * 1024 * 1024)?;
                    }
                }
            }
            Operation::Compare(a)
        }
        "read_report" => {
            let a: ReadArguments = serde_json::from_value(call.arguments)?;
            check_file(&local_path(root, &a.report)?, MAX_REPORT_BYTES)?;
            Operation::Read(a.report)
        }
        _ => anyhow::bail!("unknown tool"),
    })
}

async fn execute(
    config: Arc<Config>,
    operation: Operation,
    cancel: watch::Receiver<bool>,
) -> Result<ToolOutcome> {
    match operation {
        Operation::Run(scenario, path) => {
            let options = config.options.with_report(path.clone());
            let report = runner::run(&scenario, &options, cancel).await?;
            Ok(ToolOutcome::from_report(
                &report,
                path.display().to_string(),
            ))
        }
        Operation::Compare(a) => {
            let root = &config.options.root;
            let value = runner::compare_in_root(
                root,
                &root.join(&a.stream),
                &root.join(&a.reference),
                &root.join(&a.out),
                a.profile.as_ref().map(|p| root.join(p)).as_deref(),
                Some(Path::new(&a.report)),
                cancel,
            )
            .await?;
            ToolOutcome::from_comparison(&value, a.report)
        }
        Operation::Read(path) => {
            let file = std::fs::File::open(local_path(&config.options.root, &path)?)?;
            ensure!(
                file.metadata()?.len() <= MAX_REPORT_BYTES,
                "report read capacity"
            );
            let value: Value = serde_json::from_reader(file.take(MAX_REPORT_BYTES + 1))?;
            if value.get("scenario").is_some() {
                let report: Report = serde_json::from_value(value)?;
                report.validate()?;
                Ok(ToolOutcome::from_report(&report, path))
            } else {
                ToolOutcome::from_comparison(&value, path)
            }
        }
    }
}

fn tools() -> Value {
    let path = json!({"type":"string","minLength":1,"maxLength":1024});
    let output = json!({"type":"object","properties":{"overall_status":{"type":"string","enum":["Pass","Fail","Incomplete"]},
        "reasons":{"type":"array","maxItems":128,"items":{"type":"string"}},"report":{"type":"string"},"run_id":{"type":["string","null"]},
        "criteria":{"type":"array","maxItems":128,"items":{"type":"object","properties":{"id":{"type":"string"},"status":{"enum":["Pass","Fail","Incomplete"]},"reason":{"type":"string"}},"required":["id","status","reason"],"additionalProperties":false}}},
        "required":["overall_status","reasons","report","run_id","criteria"],"additionalProperties":false});
    let definition = |name, description, properties: Value, required: Value| {
        json!({"name":name,"description":description,
        "inputSchema":{"type":"object","properties":properties,"required":required,"additionalProperties":false},"outputSchema":output})
    };
    json!({"tools":[definition("run_scenario","Run a local immutable scenario; success requires every declared criterion to Pass. Only trusted startup configuration supplies credentials and device bindings.",json!({"scenario":path,"report":path}),json!(["scenario","report"])),
        definition("compare_recordings","Compare complete native recording populations locally; unresolved calibration and missing evidence remain Incomplete.",json!({"stream":path,"reference":path,"out":path,"profile":path,"report":path}),json!(["stream","reference","out","report"])),
        definition("read_report","Read a bounded local report summary without uploading frames, captures or reports.",json!({"report":path}),json!(["report"]))]})
}

enum Incoming {
    Message(Value),
    Malformed,
    Eof,
}
async fn read_message(
    reader: &mut (impl AsyncBufRead + Unpin),
    bytes: &mut Vec<u8>,
) -> Result<Incoming> {
    loop {
        let available = reader.fill_buf().await?;
        if available.is_empty() {
            ensure!(bytes.is_empty(), "truncated MCP message");
            return Ok(Incoming::Eof);
        }
        let count = available
            .iter()
            .position(|b| *b == b'\n')
            .map_or(available.len(), |n| n + 1);
        ensure!(
            bytes.len() + count <= MAX_MESSAGE_BYTES,
            "MCP message capacity exceeded"
        );
        let complete = available[count - 1] == b'\n';
        bytes.extend_from_slice(&available[..count]);
        reader.consume(count);
        if complete {
            let result = serde_json::from_slice(bytes);
            bytes.clear();
            return Ok(match result {
                Ok(message) => Incoming::Message(message),
                Err(_) => Incoming::Malformed,
            });
        }
    }
}
async fn send(writer: &mut (impl AsyncWrite + Unpin), message: Value) -> Result<()> {
    let mut bytes = serde_json::to_vec(&message)?;
    ensure!(
        bytes.len() < MAX_RESPONSE_BYTES,
        "MCP response capacity exceeded"
    );
    bytes.push(b'\n');
    tokio::time::timeout(std::time::Duration::from_secs(5), async {
        writer.write_all(&bytes).await?;
        writer.flush().await
    })
    .await??;
    Ok(())
}
fn error(id: Value, code: i32, message: &str) -> Value {
    json!({"jsonrpc":"2.0","id":id,"error":{"code":code,"message":message}})
}
fn result(id: Value, result: Value) -> Value {
    json!({"jsonrpc":"2.0","id":id,"result":result})
}
struct Active {
    id: Value,
    cancel: watch::Sender<bool>,
    worker: tokio::task::JoinHandle<Result<ToolOutcome>>,
    suppress: bool,
}
impl Drop for Active {
    fn drop(&mut self) {
        let _ = self.cancel.send(true);
    }
}

#[derive(Default)]
struct State {
    initialized: bool,
    ready: bool,
}
impl State {
    fn dispatch(
        &mut self,
        message: Value,
        config: &Arc<Config>,
        active: &mut Option<Active>,
    ) -> Option<Value> {
        let id = message.get("id").cloned();
        let method = message["method"].as_str().unwrap_or("");
        if message["jsonrpc"] != "2.0"
            || !message.is_object()
            || id
                .as_ref()
                .is_some_and(|id| !id.is_string() && !id.is_i64() && !id.is_u64())
        {
            return Some(error(
                id.unwrap_or(Value::Null),
                -32600,
                "invalid JSON-RPC request",
            ));
        }
        let Some(id) = id else {
            match method {
                "notifications/initialized" if self.initialized => self.ready = true,
                "notifications/cancelled" => {
                    if let Some(running) = active.as_mut() {
                        if message.pointer("/params/requestId") == Some(&running.id) {
                            running.suppress = true;
                            let _ = running.cancel.send(true);
                        }
                    }
                }
                _ => (),
            }
            return None;
        };
        Some(match method {
            "initialize" if !self.initialized => {
                self.initialized = true;
                result(
                    id,
                    json!({"protocolVersion":"2025-06-18","capabilities":{"tools":{}},
                    "serverInfo":{"name":"moonlight-mcp","version":env!("CARGO_PKG_VERSION")},
                    "instructions":"Only overall_status Pass is acceptance. Fail and Incomplete block acceptance. Outputs stay in the selected local root; full reasons remain in the local report."}),
                )
            }
            "ping" => result(id, json!({})),
            _ if !self.ready => error(
                id,
                -32000,
                "initialize and notifications/initialized are required",
            ),
            "tools/list" => result(id, tools()),
            "tools/call" if active.is_some() => error(
                id,
                -32000,
                "one local operation is already active; cancel or await it",
            ),
            "tools/call" => {
                let parsed = serde_json::from_value::<Call>(message["params"].clone())
                    .and_then(|call| operation(config, call).map_err(serde::de::Error::custom));
                match parsed {
                    Ok(operation) => {
                        let (cancel, receiver) = watch::channel(false);
                        let worker = tokio::spawn(execute(config.clone(), operation, receiver));
                        *active = Some(Active {
                            id,
                            cancel,
                            worker,
                            suppress: false,
                        });
                        return None;
                    }
                    Err(_) => error(
                        id,
                        -32602,
                        "invalid tool arguments, declaration or local path",
                    ),
                }
            }
            _ => error(id, -32601, "method not found"),
        })
    }
}

async fn serve_loop(
    reader: &mut (impl AsyncBufRead + Unpin),
    writer: &mut (impl AsyncWrite + Unpin),
    config: &Arc<Config>,
    active: &mut Option<Active>,
) -> Result<()> {
    let mut state = State::default();
    let mut bytes = Vec::new();
    loop {
        tokio::select! {
            completed=async {(&mut active.as_mut().unwrap().worker).await},if active.is_some()=>{
                let finished=active.take().unwrap();
                if !finished.suppress {
                    let response=match completed {
                        Ok(Ok(outcome))=>{
                            let failed=outcome.overall_status!=Status::Pass;
                            let data=serde_json::to_value(outcome)?;
                            result(finished.id.clone(),json!({"content":[{"type":"text","text":serde_json::to_string(&data)?}],"structuredContent":data,"isError":failed}))
                        }
                        _=>error(finished.id.clone(),-32000,"local operation could not be admitted or finalized; no passing acceptance result"),
                    };
                    send(writer,response).await?;
                }
            }
            incoming=read_message(reader,&mut bytes)=>{
                match incoming? {
                    Incoming::Eof=>return Ok(()),
                    Incoming::Malformed=>send(writer,error(Value::Null,-32700,"invalid JSON")).await?,
                    Incoming::Message(message)=>{
                        if let Some(response)=state.dispatch(message,config,active) {
                            send(writer,response).await?;
                        }
                    }
                }
            }
        }
    }
}

/// At most one expensive operation is in flight. Cancellation notifications and EOF finalize
/// through the same runner; response/write errors also await cleanup before the server exits.
/// Typed Fail and Incomplete stay non-success even when JSON-RPC transport succeeds.
pub async fn serve(
    mut reader: impl AsyncBufRead + Unpin,
    mut writer: impl AsyncWrite + Unpin,
    config: Config,
) -> Result<()> {
    let config = Arc::new(config);
    let mut active = None;
    let outcome = serve_loop(&mut reader, &mut writer, &config, &mut active).await;
    if let Some(running) = active.as_mut() {
        let _ = running.cancel.send(true);
        let _ = (&mut running.worker).await;
    }
    outcome
}

pub async fn stdio(config: Config) -> Result<()> {
    serve(
        tokio::io::BufReader::new(tokio::io::stdin()),
        tokio::io::stdout(),
        config,
    )
    .await
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn observer_readiness_cannot_be_overridden_by_tool_input() {
        let mut arguments = json!({"scenario": "scenario.json", "report": "report.json"});
        assert!(serde_json::from_value::<RunArguments>(arguments.clone()).is_ok());
        arguments["observer_ready"] = json!("untrusted.json");
        assert!(serde_json::from_value::<RunArguments>(arguments).is_err());
        assert!(serde_json::from_value::<RunArguments>(json!({"scenario":"scenario.json", "report":"report.json", "observer_attachment":"untrusted.json"})).is_err());
        assert!(serde_json::from_value::<RunArguments>(
            json!({"scenario":"scenario.json","report":"report.json","observer_finish_signal":true})
        )
        .is_err());
    }
    use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
    struct Scratch(std::path::PathBuf);
    impl Scratch {
        fn new() -> Self {
            let path = std::env::temp_dir()
                .join("t037")
                .join(uuid::Uuid::new_v4().to_string());
            std::fs::create_dir_all(&path).unwrap();
            Self(path)
        }
    }
    impl Drop for Scratch {
        fn drop(&mut self) {
            std::fs::remove_dir_all(&self.0).unwrap();
        }
    }
    async fn response(
        reader: &mut BufReader<tokio::io::ReadHalf<tokio::io::DuplexStream>>,
    ) -> serde_json::Value {
        let mut line = String::new();
        tokio::time::timeout(
            std::time::Duration::from_secs(10),
            reader.read_line(&mut line),
        )
        .await
        .unwrap()
        .unwrap();
        serde_json::from_str(&line).unwrap()
    }
    #[tokio::test]
    async fn stdio_handshake_typed_tools_and_path_rejection() {
        let scratch = Scratch::new();
        let (client, server) = tokio::io::duplex(65536);
        let (server_r, server_w) = tokio::io::split(server);
        let (client_r, mut client_w) = tokio::io::split(client);
        let config = Config::new(scratch.0.clone(), Some(scratch.0.join("identity"))).unwrap();
        let worker = tokio::spawn(serve(BufReader::new(server_r), server_w, config));
        let mut reader = BufReader::new(client_r);
        client_w.write_all(b"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\"}}\n").await.unwrap();
        assert_eq!(
            response(&mut reader).await["result"]["protocolVersion"],
            "2025-06-18"
        );
        client_w.write_all(b"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n").await.unwrap();
        let tools = response(&mut reader).await;
        assert_eq!(tools["result"]["tools"].as_array().unwrap().len(), 3);
        for tool in tools["result"]["tools"].as_array().unwrap() {
            assert_eq!(tool["inputSchema"]["additionalProperties"], false);
        }
        client_w.write_all(b"{broken\n").await.unwrap();
        assert_eq!(response(&mut reader).await["error"]["code"], -32700);
        client_w.write_all(b"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"read_report\",\"arguments\":{\"report\":\"../outside.json\"}}}\n").await.unwrap();
        let result = response(&mut reader).await;
        assert!(
            result.get("error").is_some(),
            "invalid local path cannot become an acceptance result"
        );
        client_w.shutdown().await.unwrap();
        worker.await.unwrap().unwrap();
    }
    #[tokio::test]
    async fn oversized_or_uninitialized_messages_never_start_work() {
        let scratch = Scratch::new();
        let config = Config::new(scratch.0.clone(), None).unwrap();
        let input = b"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}\n";
        let mut output = Vec::new();
        serve(BufReader::new(&input[..]), &mut output, config)
            .await
            .unwrap();
        let value: serde_json::Value = serde_json::from_slice(&output).unwrap();
        assert!(value.get("error").is_some());
        let huge = vec![b'x'; MAX_MESSAGE_BYTES + 1];
        let config = Config::new(scratch.0.clone(), None).unwrap();
        assert!(
            serve(BufReader::new(huge.as_slice()), tokio::io::sink(), config)
                .await
                .is_err()
        );
    }
    #[cfg(unix)]
    #[tokio::test]
    async fn nonregular_report_inputs_are_rejected_before_a_worker_starts() {
        let scratch = Scratch::new();
        let _socket = std::os::unix::net::UnixListener::bind(scratch.0.join("not-a-file")).unwrap();
        let config = Arc::new(Config::new(scratch.0.clone(), None).unwrap());
        let mut state = State {
            initialized: true,
            ready: true,
        };
        let mut active = None;
        let reply=state.dispatch(json!({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"read_report","arguments":{"report":"not-a-file"}}}),&config,&mut active);
        assert_eq!(reply.unwrap()["error"]["code"], -32602);
        assert!(active.is_none());
    }
    fn scenario() -> Value {
        json!({"schema_version":"1.0.0","id":"synthetic-agent","host":"127.0.0.1","pairing":{"pin_delivery":"already_paired"},
            "requested":{"width":1920,"height":1080,"fps":60,"bitrate_kbps":20000,"codec":"h264","dynamic_range":"sdr"},"duration_seconds":1,
            "controllers":[],"reference":{"required":false},"required_criteria":{"profile_id":"reference_host_compatibility","profile_version":"1.0.0",
                "ids":["REF-pairing","REF-stream","REF-controller","REF-teardown","REF-recording","REF-report"]},
            "evidence":{"host_metrics":true,"input_delivery":true,"reference_recording":false,"official_client_observations":true,"operator_observations":true},
            "output_dir":"artifacts","write_policy":{"max_bytes":1048576,"max_files":100,"on_limit":"incomplete"}})
    }
    async fn initialize(
        writer: &mut tokio::io::WriteHalf<tokio::io::DuplexStream>,
        reader: &mut BufReader<tokio::io::ReadHalf<tokio::io::DuplexStream>>,
    ) {
        writer
            .write_all(b"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}\n")
            .await
            .unwrap();
        response(reader).await;
        writer
            .write_all(b"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n")
            .await
            .unwrap();
    }
    #[tokio::test]
    async fn transport_success_keeps_incomplete_and_demonstrated_failure_typed() {
        let scratch = Scratch::new();
        let mut report = Report::new(
            scenario(),
            crate::report::Environment {
                firmware: None,
                host_version: None,
                client_version: "synthetic".into(),
                network: "fixture".into(),
                devices: vec![],
                reference_chain: None,
                clocks: vec![],
                calibration_ids: vec![],
            },
            "fixture-run".into(),
            "fixture-session".into(),
        )
        .unwrap();
        report.criteria[0].status = Status::Fail;
        report.criteria[0].reason = "synthetic observed failure".into();
        report.finalize(None).unwrap();
        report
            .write_to(std::fs::File::create(scratch.0.join("report.json")).unwrap())
            .unwrap();
        let (client, server) = tokio::io::duplex(65536);
        let (sr, sw) = tokio::io::split(server);
        let (cr, mut cw) = tokio::io::split(client);
        let mut reader = BufReader::new(cr);
        let worker = tokio::spawn(serve(
            BufReader::new(sr),
            sw,
            Config::new(scratch.0.clone(), None).unwrap(),
        ));
        initialize(&mut cw, &mut reader).await;
        cw.write_all(b"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"read_report\",\"arguments\":{\"report\":\"report.json\"}}}\n").await.unwrap();
        let result = response(&mut reader).await;
        assert_eq!(result["result"]["isError"], true);
        assert_eq!(
            result["result"]["structuredContent"]["overall_status"],
            "Incomplete"
        );
        assert_eq!(
            result["result"]["structuredContent"]["criteria"][0]["status"],
            "Fail"
        );
        cw.shutdown().await.unwrap();
        worker.await.unwrap().unwrap();
    }
    #[tokio::test]
    async fn cancellation_and_eof_finalize_active_run_while_busy_requests_are_refused() {
        for notify in [false, true] {
            let scratch = Scratch::new();
            std::fs::write(
                scratch.0.join("scenario.json"),
                serde_json::to_vec(&scenario()).unwrap(),
            )
            .unwrap();
            let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
            let mut config =
                Config::new(scratch.0.clone(), Some(scratch.0.join("identity"))).unwrap();
            config.options.http_port = listener.local_addr().unwrap().port();
            let (client, server) = tokio::io::duplex(65536);
            let (sr, sw) = tokio::io::split(server);
            let (cr, mut cw) = tokio::io::split(client);
            let mut reader = BufReader::new(cr);
            let worker = tokio::spawn(serve(BufReader::new(sr), sw, config));
            initialize(&mut cw, &mut reader).await;
            cw.write_all(b"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"run_scenario\",\"arguments\":{\"scenario\":\"scenario.json\",\"report\":\"report.json\"}}}\n").await.unwrap();
            let (_host, _) =
                tokio::time::timeout(std::time::Duration::from_secs(10), listener.accept())
                    .await
                    .unwrap()
                    .unwrap();
            cw.write_all(b"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"read_report\",\"arguments\":{\"report\":\"report.json\"}}}\n").await.unwrap();
            assert!(response(&mut reader).await.get("error").is_some());
            if notify {
                cw.write_all(b"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":3}}\n{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"ping\"}\n").await.unwrap();
                let ping = response(&mut reader).await;
                assert_eq!(ping["id"], 5);
                assert!(ping.get("result").is_some());
            }
            cw.shutdown().await.unwrap();
            tokio::time::timeout(std::time::Duration::from_secs(10), worker)
                .await
                .unwrap()
                .unwrap()
                .unwrap();
            let report: Report = serde_json::from_reader(
                std::fs::File::open(scratch.0.join("report.json")).unwrap(),
            )
            .unwrap();
            report.validate().unwrap();
            assert_eq!(report.overall_status, Status::Incomplete);
            assert_eq!(report.criteria.len(), 6);
            assert!(!report.limitations.is_empty());
        }
    }
}
