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
    /// Where Core lives. Remembered after the first sign-in.
    #[arg(long, global = true)]
    core: Option<String>,
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
}

fn config_path() -> std::path::PathBuf {
    let home = std::env::var("HOME").unwrap_or_else(|_| ".".into());
    std::path::Path::new(&home).join(".config/omnuv/cli.json")
}

fn load_session(core_override: Option<String>) -> Result<Session> {
    let path = config_path();
    let raw = std::fs::read_to_string(&path)
        .with_context(|| format!("not signed in: run `omnuv login` (looked in {})", path.display()))?;
    let v: serde_json::Value = serde_json::from_str(&raw)?;
    Ok(Session {
        core: core_override
            .or_else(|| v["core"].as_str().map(str::to_string))
            .context("no core address recorded")?,
        token: v["token"].as_str().context("no token recorded")?.to_string(),
    })
}

fn save_session(core: &str, token: &str) -> Result<()> {
    let path = config_path();
    std::fs::create_dir_all(path.parent().unwrap())?;
    let body = serde_json::json!({ "core": core, "token": token });
    std::fs::write(&path, serde_json::to_vec_pretty(&body)?)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600))?;
    }
    Ok(())
}

// ---------- talking to Core ----------

async fn get<T: for<'de> Deserialize<'de>>(s: &Session, path: &str) -> Result<T> {
    let res = reqwest::Client::new()
        .get(format!("{}{path}", s.core))
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

    match cli.command {
        Command::Login => {
            let core = cli
                .core
                .or_else(|| load_session(None).ok().map(|s| s.core))
                .context("say where Core is: omnuv --core https://… login")?;
            login(&core).await
        }
        Command::Logout => {
            let p = config_path();
            if p.exists() {
                std::fs::remove_file(&p)?;
            }
            println!("  Forgotten. The token is still valid until you revoke it in the console.");
            Ok(())
        }
        Command::Machines => {
            let s = load_session(cli.core)?;
            print_machines(&machines(&s).await?);
            Ok(())
        }
        Command::Ssh { name } => {
            let s = load_session(cli.core)?;
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
            let s = load_session(cli.core)?;
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
            let s = load_session(cli.core)?;
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
            let s = load_session(cli.core)?;
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
            let s = load_session(cli.core)?;
            let mut body = serde_json::json!({
                "name": l.name, "region": l.region, "vcpus": l.vcpus,
                "memory_gb": l.memory_gb, "disk_gb": l.disk_gb, "wait": l.wait,
            });
            if let Some(model) = &l.gpu {
                body["gpu"] = serde_json::json!({ "model": model, "count": 1 });
            }
            let res = reqwest::Client::new()
                .post(format!("{}/v1/instances", s.core))
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
            let s = load_session(cli.core)?;
            let m = machine_named(&s, &name).await?;
            let res = reqwest::Client::new()
                .delete(format!("{}/v1/instances/{}", s.core, m.id))
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
