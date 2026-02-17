# fs_watcher

> ⚠️ **Highly Experimental**
>
> This project is under active development. APIs, behavior, wire protocol, and process lifecycle details may change without notice.

## Platform Support

- ✅ **macOS only** (currently)
- ❌ Linux / Windows are not supported yet

This crate relies on a macOS-specific native helper (`fs_watch`) built with Apple file event APIs.

## What this is

`fs_watcher` is a Rust library that streams file-system events asynchronously.

- Rust side: Tokio + async stream API
- Native side: embedded C helper process
- Transport: anonymous pipe (helper `stdout` → Rust decoder)

## Current Status

- Experimental protocol and implementation
- Intended for testing and iteration
- Not yet recommended for production workloads

## Build Requirements (macOS)

- Rust toolchain (Cargo)
- `clang`
- Apple frameworks available on macOS:
  - `CoreServices`
  - `CoreFoundation`

The helper binary is compiled in `build.rs`, embedded into the Rust artifact, and extracted at runtime.

## Quick Start

```rust
use std::path::Path;
use fs_watcher::async_watcher::AsyncWatcher;
use tokio_stream::StreamExt;

#[tokio::main]
async fn main() -> std::io::Result<()> {
	let (mut watcher, mut events) = AsyncWatcher::spawn(Path::new(".")).await?;

	while let Some(item) = events.next().await {
		match item {
			Ok(event) => println!("{}", event),
			Err(err) => {
				eprintln!("stream error: {err}");
				break;
			}
		}
	}

	watcher.stop().await?;
	Ok(())
}
```

## Notes

- Dropping `AsyncWatcher` will attempt to stop the helper process.
- `stop()` is best-effort and safe to call even if the helper already exited.
- If the watched path is invalid, spawn returns an error when the helper exits early.

## Roadmap (high-level)

- Stabilize event protocol
- Improve startup/ready handshake
- Add richer diagnostics and error reporting
- Explore cross-platform support
- Move from starting a child process, to using FFI with callbacks
