//! `omnuv` — the marketplace from a terminal.
//!
//! One binary, one config file, and the subset of the API a person actually
//! reaches for: what do I have, make me one, let me in, what happened to it.
//!
//! It holds no marketplace logic and never will. It asks Core what exists and
//! prints it; every decision about placement, price and provider stays on the
//! other end of the wire. That boundary is the same one the Provider Agent and
//! the desktop client observe, and it is what lets this be published.
//!
//! Signing in is the device authorization flow: this prints a short code, a
//! person approves it in a browser where they are already signed in, and this
//! collects a token. No password is ever typed into a program somebody
//! downloaded.

use std::io::Write;

use anyhow::{Context, Result, bail};
use clap::{Parser, Subcommand};
use serde::Deserialize;

#[derive(Parser)]
#[command(name = "omnuv", version, about = "The Omnuv marketplace, from a terminal")]
struct Cli {
    /// Where Core lives: production unless named. Remembered after the
    /// first sign-in.
    #[arg(long, global = true)]
    core: Option<String>,
    /// Which project to act on, by id or name; OMNUV_PROJECT if unset.
    /// Without either, Core's default project, as before.
    #[arg(long, global = true)]
    project: Option<String>,
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Approve this terminal in a browser, once.
    Login,
    /// Forget the token on this machine.
    Logout,
    /// Your machines.
    #[command(alias = "ls")]
    Machines,
    /// Launch a machine.
    Launch(Launch),
    /// Print the command that opens a shell on a machine.
    Ssh { name: String },
    /// The address of a machine's browser console.
    Console { name: String },
    /// What happened to a machine, in order.
    Events { name: Option<String> },
    /// Requests waiting for capacity.
    Waiting,
    /// Delete a machine.
    Rm { name: String },
}

#[derive(clap::Args)]
struct Launch {
    name: String,
    #[arg(long, default_value_t = 2)]
    vcpus: i32,
    #[arg(long = "memory", default_value_t = 4, help = "GiB")]
    memory_gb: i32,
    #[arg(long = "disk", default_value_t = 40, help = "GiB")]
    disk_gb: i32,
    #[arg(long, help = "A GPU model, for example \"RTX 3090\"")]
    gpu: Option<String>,
    #[arg(long, default_value = "eu-west")]
    region: String,
    /// Wait for capacity rather than failing when there is none.
    #[arg(long)]
    wait: bool,
}

// ---------- what we keep on this machine ----------

/// Two values in a file readable only by its owner. The token is a credential;
/// it is not going in an environment variable a `ps` can read.
struct Session {
    core: String,
    token: String,
    /// Sent as `?project=` on every request that is about a project's
    /// resources (24 September 2026). Without it the CLI always acted on
    /// Core's default project, while the desktop client scopes by the one
    /// chosen, so the two could show one account different machines.
    project: Option<String>,
}

/// A Core URL for `path`, carrying the project when one was chosen. Built with
/// the URL parser, so a name with a space or an `&` stays one value.
fn url(s: &Session, path: &str) -> Result<reqwest::Url> {
    let mut url = reqwest::Url::parse(&format!("{}{path}", s.core))
        .with_context(|| format!("{}{path} is not a URL", s.core))?;
    if let Some(project) = s.project.as_deref().map(str::trim).filter(|p| !p.is_empty()) {
        url.query_pairs_mut().append_pair("project", project);
    }
    Ok(url)
}

/// Where the session is kept: %APPDATA% on Windows, XDG_CONFIG_HOME or
/// ~/.config elsewhere. **Never the current directory.** It fell back to "."
/// when HOME was unset, which is the normal case in cmd and PowerShell, so the
/// Windows build wrote its bearer token into whatever directory it was run
/// from and could not find it from any other.
fn config_path_from(var: impl Fn(&str) -> Option<String>, windows: bool) -> Option<std::path::PathBuf> {
    let set = |k: &str| var(k).filter(|v| !v.trim().is_empty());
    let base = if windows {
        set("APPDATA").map(std::path::PathBuf::from)
    } else {
        set("XDG_CONFIG_HOME")
            .map(std::path::PathBuf::from)
            .or_else(|| set("HOME").map(|h| std::path::Path::new(&h).join(".config")))
    }?;
    Some(base.join("omnuv").join("cli.json"))
}

fn config_path() -> Result<std::path::PathBuf> {
    config_path_from(|k| std::env::var(k).ok(), cfg!(windows))
        .context("no place to keep the sign-in: set APPDATA on Windows, or HOME")
}

/// Where a sign-in goes when nobody named a Core and none is remembered: the
/// operator's decision of 25 September 2026, the same default as the window.
const PRODUCTION_CORE: &str = "https://api.omnuv.com";

fn load_session(core_override: Option<String>, project: Option<String>) -> Result<Session> {
    let path = config_path()?;
    let raw = std::fs::read_to_string(&path)
        .with_context(|| format!("not signed in: run `omnuv login` (looked in {})", path.display()))?;
    let v: serde_json::Value = serde_json::from_str(&raw)?;
    Ok(Session {
        core: core_override
            .or_else(|| v["core"].as_str().map(str::to_string))
            .context("no core address recorded")?,
        token: v["token"].as_str().context("no token recorded")?.to_string(),
        project,
    })
}

fn save_session(core: &str, token: &str) -> Result<()> {
    write_private(&config_path()?, &serde_json::to_vec_pretty(&serde_json::json!({ "core": core, "token": token }))?)
}

/// **0600 from the moment the file exists.** It was written with the umask
/// and narrowed afterwards, so for that moment the bearer token was readable
/// by other local users. On Windows the file lives under the user's own
/// %APPDATA%, whose ACL is that user's.
fn write_private(path: &std::path::Path, bytes: &[u8]) -> Result<()> {
    use std::io::Write as _;
    std::fs::create_dir_all(path.parent().context("a path with no directory")?)?;
    let mut options = std::fs::OpenOptions::new();
    options.write(true).create(true).truncate(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt as _;
        options.mode(0o600);
    }
    let mut file = options.open(path)?;
    // A file that existed before keeps its old mode through open(); narrow it.
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        file.set_permissions(std::fs::Permissions::from_mode(0o600))?;
    }
    file.write_all(bytes)?;
    Ok(())
}

// ---------- talking to Core ----------

async fn get<T: for<'de> Deserialize<'de>>(s: &Session, path: &str) -> Result<T> {
    let res = reqwest::Client::new()
        .get(url(s, path)?)
        .bearer_auth(&s.token)
        .send()
        .await?;
    if res.status() == reqwest::StatusCode::UNAUTHORIZED {
        bail!("that token is no longer valid; run `omnuv login` again");
    }
    if !res.status().is_success() {
        bail!("{}: {}", res.status(), res.text().await.unwrap_or_default());
    }
    Ok(res.json().await?)
}

#[derive(Deserialize)]
struct Machine {
    id: String,
    name: String,
    region: String,
    status: String,
    default_user: String,
    private_ip: Option<String>,
    price_per_hour: String,
    #[serde(default)]
    stream_app: Option<String>,
    #[serde(default)]
    gpu: Option<Gpu>,
    vcpus: i32,
    memory_mib: i64,
}

#[derive(Deserialize)]
struct Gpu {
    model: String,
    count: i64,
}

async fn machines(s: &Session) -> Result<Vec<Machine>> {
    get(s, "/v1/instances").await
}

async fn machine_named(s: &Session, name: &str) -> Result<Machine> {
    machines(s)
        .await?
        .into_iter()
        .find(|m| m.name == name)
        .with_context(|| format!("no machine called {name}"))
}

// ---------- signing in ----------

#[derive(Deserialize)]
struct Pending {
    device_code: String,
    user_code: String,
    verification_uri: String,
    interval: u64,
}

async fn login(core: &str) -> Result<()> {
    let http = reqwest::Client::new();
    let start: Pending = http
        .post(format!("{core}/v1/auth/device"))
        .json(&serde_json::json!({ "client": format!("omnuv command line {} on {}", env!("CARGO_PKG_VERSION"), std::env::consts::OS) }))
        .send()
        .await
        .with_context(|| format!("could not reach {core}"))?
        .json()
        .await?;

    println!();
    println!("  Open {} and enter this code:", start.verification_uri);
    println!();
    println!("      {}", start.user_code);
    println!();
    print!("  Waiting");
    std::io::stdout().flush().ok();

    loop {
        tokio::time::sleep(std::time::Duration::from_secs(start.interval.max(1))).await;
        let res = http
            .post(format!("{core}/v1/auth/device/token"))
            .json(&serde_json::json!({ "device_code": start.device_code }))
            .send()
            .await?;
        match res.status() {
            // Nobody has approved it yet, which is the normal case for most of
            // the exchange rather than an error.
            reqwest::StatusCode::ACCEPTED => {
                print!(".");
                std::io::stdout().flush().ok();
            }
            s if s.is_success() => {
                let v: serde_json::Value = res.json().await?;
                let token = v["token"].as_str().context("no token in the reply")?;
                save_session(core, token)?;
                println!("\n\n  Signed in. Take this access back any time in the console, under Network.");
                return Ok(());
            }
            _ => bail!("\n  refused or expired: {}", res.text().await.unwrap_or_default()),
        }
    }
}

// ---------- printing ----------

fn width(rows: &[String]) -> usize {
    rows.iter().map(|r| r.chars().count()).max().unwrap_or(0)
}

fn print_machines(list: &[Machine]) {
    if list.is_empty() {
        println!("  No machines yet. `omnuv launch <name>` makes one.");
        return;
    }
    let names: Vec<String> = list.iter().map(|m| m.name.clone()).collect();
    let w = width(&names).max(4);
    for m in list {
        let spec = match &m.gpu {
            Some(g) => format!("{} vCPU, {} GiB, {} × {}", m.vcpus, m.memory_mib / 1024, g.count, g.model),
            None => format!("{} vCPU, {} GiB", m.vcpus, m.memory_mib / 1024),
        };
        println!(
            "  {:w$}  {:<12} {:<10} {:<34} €{}/h",
            m.name,
            m.status,
            m.region,
            spec,
            m.price_per_hour,
            w = w
        );
        if let Some(ip) = &m.private_ip {
            println!("  {:w$}  {}.internal ({ip}){}", "", m.name, if m.stream_app.is_some() { "  streamable" } else { "" }, w = w);
        }
    }
}

#[derive(Deserialize)]
struct Event {
    at: String,
    summary: String,
}

#[derive(Deserialize)]
struct Parked {
    status: String,
    waiting_on: String,
    attempts: i32,
    expires_at: String,
}

#[tokio::main]
async fn main() -> Result<()> {
    let _ = rustls::crypto::ring::default_provider().install_default();
    let cli = Cli::parse();
    let project = cli.project.clone().or_else(|| std::env::var("OMNUV_PROJECT").ok());

    match cli.command {
        Command::Login => {
            let core = cli
                .core
                .or_else(|| load_session(None, None).ok().map(|s| s.core))
                .unwrap_or_else(|| PRODUCTION_CORE.to_string());
            login(&core).await
        }
        Command::Logout => {
            let p = config_path()?;
            if p.exists() {
                std::fs::remove_file(&p)?;
            }
            println!("  Forgotten. The token is still valid until you revoke it in the console.");
            Ok(())
        }
        Command::Machines => {
            let s = load_session(cli.core, project.clone())?;
            print_machines(&machines(&s).await?);
            Ok(())
        }
        Command::Ssh { name } => {
            let s = load_session(cli.core, project.clone())?;
            let m = machine_named(&s, &name).await?;
            if m.private_ip.is_none() {
                bail!("{name} has no private address yet; it may still be starting");
            }
            // Printed rather than executed: this way it composes, and nobody is
            // surprised by a program that suddenly opens a shell.
            println!("ssh {}@{}.internal", m.default_user, m.name);
            Ok(())
        }
        Command::Console { name } => {
            let s = load_session(cli.core, project.clone())?;
            let m = machine_named(&s, &name).await?;
            // api.<domain> -> console.<domain> in a deployment; the port swap is
            // the lab's own shape, where both run on one address.
            let console = s
                .core
                .replace("//api.", "//console.")
                .replace(":8410", ":5273");
            println!("{console}/compute/instances#{}", m.id);
            println!("  The console runs in a browser and works even when the machine's network does not.");
            Ok(())
        }
        Command::Events { name } => {
            let s = load_session(cli.core, project.clone())?;
            let path = match &name {
                Some(n) => format!("/v1/events?limit=25&resource_id={}", machine_named(&s, n).await?.id),
                None => "/v1/events?limit=25".to_string(),
            };
            for e in get::<Vec<Event>>(&s, &path).await? {
                println!("  {}  {}", &e.at[..19].replace('T', " "), e.summary);
            }
            Ok(())
        }
        Command::Waiting => {
            let s = load_session(cli.core, project.clone())?;
            let rows: Vec<Parked> = get(&s, "/v1/parked").await?;
            let waiting: Vec<_> = rows.into_iter().filter(|r| r.status == "WAITING").collect();
            if waiting.is_empty() {
                println!("  Nothing waiting.");
            }
            for r in waiting {
                println!(
                    "  waiting for {}\n    tried {} time(s), stops waiting {}",
                    r.waiting_on,
                    r.attempts,
                    &r.expires_at[..16].replace('T', " ")
                );
            }
            println!("  Nothing is reserved or charged while a request waits.");
            Ok(())
        }
        Command::Launch(l) => {
            let s = load_session(cli.core, project.clone())?;
            let mut body = serde_json::json!({
                "name": l.name, "region": l.region, "vcpus": l.vcpus,
                "memory_gb": l.memory_gb, "disk_gb": l.disk_gb, "wait": l.wait,
            });
            if let Some(model) = &l.gpu {
                body["gpu"] = serde_json::json!({ "model": model, "count": 1 });
            }
            let res = reqwest::Client::new()
                .post(url(&s, "/v1/instances")?)
                .bearer_auth(&s.token)
                .json(&body)
                .send()
                .await?;
            let status = res.status();
            let v: serde_json::Value = res.json().await.unwrap_or_default();
            if status == reqwest::StatusCode::ACCEPTED {
                println!("  No capacity yet, so it is waiting: {}", v["waiting_on"].as_str().unwrap_or(""));
                println!("  Nothing is reserved or charged while it waits. `omnuv waiting` shows it.");
                return Ok(());
            }
            if !status.is_success() {
                bail!("{status}: {}", v["error"].as_str().unwrap_or("failed"));
            }
            println!("  {} is starting.", v["name"].as_str().unwrap_or(&l.name));
            if let Some(p) = v["console_password"].as_str() {
                println!("  Console password, shown once: {p}");
            }
            Ok(())
        }
        Command::Rm { name } => {
            let s = load_session(cli.core, project.clone())?;
            let m = machine_named(&s, &name).await?;
            let res = reqwest::Client::new()
                .delete(url(&s, &format!("/v1/instances/{}", m.id))?)
                .bearer_auth(&s.token)
                .send()
                .await?;
            if !res.status().is_success() {
                bail!("{}", res.status());
            }
            println!("  {name} is being deleted. Its allocation is released when it is gone.");
            Ok(())
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn env<'a>(pairs: &'a [(&'a str, &'a str)]) -> impl Fn(&str) -> Option<String> + 'a {
        move |k| pairs.iter().find(|(n, _)| *n == k).map(|(_, v)| v.to_string())
    }

    /// Core's replies, parsed with this client's types. **Synthetic, not
    /// captured**: each is written field for field from Core's own view
    /// (`InstanceView`, `EventView`, `ParkedView`, `device_auth::Pending`, as
    /// of 24 September 2026), with every field Core sends, the optional ones
    /// it omits left out, and fields this client ignores kept in.
    #[test]
    fn cores_replies_parse_as_core_writes_them() {
        let machines: Vec<Machine> = serde_json::from_str(r#"[
            {"id":"6f1c2d4e-0000-4000-8000-000000000001","name":"gpu-1","region":"eu-west",
             "image":"ubuntu-26.04","os_family":"linux","default_user":"ubuntu","auth_mode":"key",
             "console_kind":"serial","vcpus":8,"memory_mib":32768,"disk_gib":200,
             "gpu":{"model":"RTX 3090","count":1},"status":"RUNNING","stream_app":"Desktop",
             "private_ip":"10.210.0.11","private_name":"gpu-1.internal","last_error":null,
             "price_per_hour":"0.4200","created_at":"2026-09-24T01:00:00Z"},
            {"id":"6f1c2d4e-0000-4000-8000-000000000002","name":"small","region":"eu-west",
             "image":"ubuntu-26.04","os_family":"linux","default_user":"ubuntu","auth_mode":"key",
             "console_kind":"serial","vcpus":2,"memory_mib":4096,"disk_gib":40,
             "status":"PENDING","private_ip":null,"last_error":null,
             "price_per_hour":"0.0300","created_at":"2026-09-24T01:00:00Z"}
        ]"#).expect("/v1/instances as Core writes it");
        assert_eq!(machines[0].gpu.as_ref().map(|g| (g.model.as_str(), g.count)), Some(("RTX 3090", 1)));
        assert!(machines[1].gpu.is_none() && machines[1].stream_app.is_none() && machines[1].private_ip.is_none());
        assert_eq!((machines[1].vcpus, machines[1].memory_mib), (2, 4096));

        let events: Vec<Event> = serde_json::from_str(r#"[{"id":7,"at":"2026-09-24T01:02:03.456Z",
            "kind":"instance.observed","severity":"info","class":"lifecycle","actor":"provider:x",
            "summary":"gpu-1 is now running","resource_type":"instance","resource_id":null,"detail":{}}]"#)
            .expect("/v1/events as Core writes it");
        // main prints the first 19 characters with the T replaced.
        assert_eq!(events[0].at[..19].replace('T', " "), "2026-09-24 01:02:03");

        let parked: Vec<Parked> = serde_json::from_str(r#"[{"id":"6f1c2d4e-0000-4000-8000-000000000003",
            "status":"WAITING","waiting_on":"a GPU","attempts":3,"last_reason":null,
            "created_at":"2026-09-24T01:00:00Z","expires_at":"2026-09-25T01:00:00Z",
            "instance_id":null,"billing":"nothing is charged while a request waits"}]"#)
            .expect("/v1/parked as Core writes it");
        assert_eq!((parked[0].status.as_str(), parked[0].attempts), ("WAITING", 3));

        let pending: Pending = serde_json::from_str(r#"{"device_code":"dc","user_code":"ABCD-EFGH",
            "verification_uri":"https://console.omnuv.com/device","interval":5,"expires_in":600}"#)
            .expect("the device-code start as Core writes it");
        assert_eq!((pending.user_code.as_str(), pending.interval), ("ABCD-EFGH", 5));
    }

    #[test]
    fn a_chosen_project_travels_on_every_request_and_none_changes_nothing() {
        let session = |project: Option<&str>| Session {
            core: "https://api.omnuv.com".into(),
            token: "t".into(),
            project: project.map(str::to_string),
        };
        assert_eq!(url(&session(None), "/v1/instances").unwrap().as_str(), "https://api.omnuv.com/v1/instances");
        assert_eq!(url(&session(Some("  ")), "/v1/parked").unwrap().as_str(), "https://api.omnuv.com/v1/parked");
        assert_eq!(
            url(&session(Some("default")), "/v1/events?limit=25").unwrap().as_str(),
            "https://api.omnuv.com/v1/events?limit=25&project=default"
        );
        // A name is one value, whatever it holds.
        let odd = url(&session(Some("a b&c=d")), "/v1/instances").unwrap();
        assert_eq!(odd.query_pairs().collect::<Vec<_>>(), vec![("project".into(), "a b&c=d".into())]);
    }

    #[test]
    fn the_session_lives_in_the_users_config_never_the_current_directory() {
        let p = |pairs, windows| config_path_from(env(pairs), windows).map(|p| p.to_string_lossy().replace('\\', "/"));
        assert_eq!(p(&[("APPDATA", "C:/Users/a/AppData/Roaming"), ("HOME", "")], true).as_deref(),
                   Some("C:/Users/a/AppData/Roaming/omnuv/cli.json"));
        assert_eq!(p(&[("HOME", "/home/a")], false).as_deref(), Some("/home/a/.config/omnuv/cli.json"));
        assert_eq!(p(&[("HOME", "/home/a"), ("XDG_CONFIG_HOME", "/cfg")], false).as_deref(), Some("/cfg/omnuv/cli.json"));
        assert_eq!(p(&[], true), None, "Windows with no APPDATA fell back somewhere");
        assert_eq!(p(&[("HOME", " ")], false), None, "an empty HOME became the current directory");
    }

    #[cfg(unix)]
    #[test]
    fn the_token_file_is_private_from_the_start_and_after_an_overwrite() {
        use std::os::unix::fs::PermissionsExt as _;
        let dir = std::env::temp_dir().join(format!("omnuv-cli-{}", std::process::id()));
        let path = dir.join("omnuv/cli.json");
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(&path, b"old").unwrap();
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o644)).unwrap();
        write_private(&path, b"{}").unwrap();
        assert_eq!(std::fs::metadata(&path).unwrap().permissions().mode() & 0o777, 0o600);
        std::fs::remove_file(&path).unwrap();
        write_private(&path, b"{}").unwrap();
        assert_eq!(std::fs::metadata(&path).unwrap().permissions().mode() & 0o777, 0o600);
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
