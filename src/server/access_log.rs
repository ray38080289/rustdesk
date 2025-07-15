
use chrono::Local;
use std::env;
use std::fs::{OpenOptions, create_dir_all};
use std::io::Write;
use std::path::PathBuf;

pub enum AccessEvent<'a> {
    Incoming {
        ip: &'a str,
        user: &'a str,
        method: &'a str,
    },
    ConnectResult {
        ip: &'a str,
        user: &'a str,
        method: &'a str,
        ok: bool,
        msg: &'a str,
    },
    FileTransfer {
        ip: &'a str,
        user: &'a str,
        path: &'a str,
        success: bool,
    },
    Disconnect {
        ip: &'a str,
        user: &'a str,
        reason: &'a str,
    },
}

fn get_access_log_path() -> PathBuf {
    let base = env::var("LOCALAPPDATA").unwrap_or_else(|_| ".".to_string());
    let mut path = PathBuf::from(base);
    path.push("RustDesk/log/access_log");
    let _ = create_dir_all(&path);
    let filename = format!("rustdesk_{}.log", Local::now().format("%Y-%m-%d"));
    path.push(filename);
    path
}

pub fn log_access(event: AccessEvent) {
    let now = Local::now();
    let mut file = match OpenOptions::new().create(true).append(true).open(get_access_log_path()) {
        Ok(f) => f,
        Err(e) => {
            log::error!("Failed to open access log file: {}", e);
            return;
        }
    };
    let line = match event {
        AccessEvent::Incoming { ip, user, method } => {
            format!("[{}] INCOMING ip={} user={} method={}\n", now.format("%Y-%m-%d %H:%M:%S"), ip, user, method)
        }
        AccessEvent::ConnectResult { ip, user, method, ok, msg } => {
            format!("[{}] CONNECT_RESULT ip={} user={} method={} ok={} msg={}\n", now.format("%Y-%m-%d %H:%M:%S"), ip, user, method, ok, msg)
        }
        AccessEvent::FileTransfer { ip, user, path, success } => {
            format!("[{}] FILE_TRANSFER ip={} user={} path={} success={}\n", now.format("%Y-%m-%d %H:%M:%S"), ip, user, path, success)
        }
        AccessEvent::Disconnect { ip, user, reason } => {
            format!("[{}] DISCONNECT ip={} user={} reason={}\n", now.format("%Y-%m-%d %H:%M:%S"), ip, user, reason)
        }
    };
    let _ = file.write_all(line.as_bytes());
}
