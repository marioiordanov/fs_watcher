use std::{fs, io, path::PathBuf, sync::OnceLock};

#[cfg(unix)]
use std::os::unix::fs::PermissionsExt;

static HELPER_PATH: OnceLock<PathBuf> = OnceLock::new();
// Built by build.rs, then embedded into Rust binary/library.
const HELPER_BYTES: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/fs_watch"));

fn extract_helper() -> io::Result<PathBuf> {
    let dir = std::env::temp_dir().join(env!("CARGO_PKG_NAME"));
    fs::create_dir_all(&dir)?;

    let path = dir.join("fs_watch");
    fs::write(&path, HELPER_BYTES)?;

    #[cfg(unix)]
    fs::set_permissions(&path, fs::Permissions::from_mode(0o700))?;

    Ok(path)
}

pub fn ensure_helper_on_disk() -> io::Result<&'static PathBuf> {
    if let Some(p) = HELPER_PATH.get() {
        return Ok(p);
    }

    let p = extract_helper()?;
    let _ = HELPER_PATH.set(p); // ignore if another thread already set it

    Ok(HELPER_PATH.get().expect("helper path initialized"))
}
