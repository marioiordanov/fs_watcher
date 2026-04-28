use std::{
    path::{Path, PathBuf},
    time::Duration,
};

use tokio::{
    io::{self, AsyncReadExt},
    process::{Child, ChildStdout, Command},
    sync::mpsc,
    time::timeout,
};
use tokio_stream::wrappers::ReceiverStream;

use crate::util;

/// Async file-system watcher backed by the embedded `fs_watch` helper process.
///
/// # How to use this library
///
/// 1. Call `AsyncWatcher::spawn()` with the directory you want to watch.
/// 2. Read events from the returned `EventStream`.
/// 3. Call `stop()` (or drop the watcher) to terminate the helper process.
///
/// ```no_run
/// use std::path::Path;
/// use tokio_stream::StreamExt;
/// use test_watcher::async_watcher::AsyncWatcher;
///
/// #[tokio::main]
/// async fn main() -> std::io::Result<()> {
///     let (mut watcher, mut events) = AsyncWatcher::spawn(Path::new("."), 2.0, &[]).await?;
///
///     while let Some(evt) = events.next().await {
///         println!("{}", evt?);
///     }
///
///     watcher.stop().await?;
///     Ok(())
/// }
/// ```

enum ObjectType {
    File = 1,
    Folder = 2,
}

impl TryFrom<u8> for ObjectType {
    type Error = ();

    fn try_from(value: u8) -> std::result::Result<Self, Self::Error> {
        match value {
            1 => Ok(ObjectType::File),
            2 => Ok(ObjectType::Folder),
            _ => Err(()),
        }
    }
}
enum OperationType {
    Added = 3,
    Modified = 4,
    Created = 5,
    Renamed = 6,
    Removed = 7,
    Replaced = 8,
}

impl TryFrom<u8> for OperationType {
    type Error = ();

    fn try_from(value: u8) -> std::result::Result<Self, Self::Error> {
        match value {
            3 => Ok(OperationType::Added),
            4 => Ok(OperationType::Modified),
            5 => Ok(OperationType::Created),
            6 => Ok(OperationType::Renamed),
            7 => Ok(OperationType::Removed),
            8 => Ok(OperationType::Replaced),
            _ => Err(()),
        }
    }
}

#[derive(Clone, Debug)]
pub enum Event {
    FileCreated(PathBuf, u64),
    FileRemoved(PathBuf, u64),
    FileAdded(PathBuf, u64),
    FileModified(PathBuf, u64, u64),
    FolderRemoved(PathBuf, u64),
    FolderAdded(PathBuf, u64),
    FolderCreated(PathBuf, u64),
    FileRenamed {
        from: PathBuf,
        to: PathBuf,
        inode: u64,
    },
    FolderRenamed {
        from: PathBuf,
        to: PathBuf,
        inode: u64,
    },
    FileReplaced {
        path: PathBuf,
        from: u64,
        to: u64,
    },
    FolderReplaced {
        path: PathBuf,
        from: u64,
        to: u64,
    },
}

impl std::fmt::Display for Event {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Event::FileRemoved(path, ..) => write!(f, "file removed: {}", path.display()),
            Event::FolderRemoved(path, ..) => write!(f, "folder removed: {}", path.display()),
            Event::FolderAdded(path, inode) => {
                write!(f, "folder added: {} {inode}", path.display())
            }
            Event::FileAdded(path, inode) => write!(f, "file added: {} {inode}", path.display()),
            Event::FileModified(path, old_inode, new_inode) => {
                if old_inode == new_inode {
                    write!(f, "file modified: {} {old_inode}", path.display())
                } else {
                    write!(
                        f,
                        "file modified: {} and inode changed {old_inode}->{new_inode}",
                        path.display()
                    )
                }
            }
            Event::FileRenamed { from, to, inode } => write!(
                f,
                "file renamed: {} -> {} {inode}",
                from.display(),
                to.display()
            ),
            Event::FolderRenamed { from, to, inode } => write!(
                f,
                "folder renamed: {} -> {} {inode}",
                from.display(),
                to.display()
            ),
            Event::FileCreated(path, inode) => {
                write!(f, "file created: {} {inode}", path.display())
            }
            Event::FolderCreated(path, inode) => {
                write!(f, "folder created: {} {inode}", path.display())
            }
            Event::FileReplaced { path, from, to } => {
                write!(f, "file replaced: {} {from} -> {to}", path.display())
            }
            Event::FolderReplaced { path, from, to } => {
                write!(f, "folder replaced: {} {from} -> {to}", path.display())
            }
        }
    }
}

pub struct AsyncWatcher {
    /// Handle to the running helper process (`fs_watch`).
    fs_watch_process: Child,
}

/// Stream of parsed watcher events.
///
/// Each item is:
/// - `Ok(Event)` when a protocol frame is decoded successfully
/// - `Err(io::Error)` when protocol/IO decoding fails
pub type EventStream = ReceiverStream<io::Result<Event>>;

impl AsyncWatcher {
    /// Stop the helper process explicitly.
    ///
    /// This is best-effort: if the process already exited, this still returns `Ok(())`.
    pub async fn stop(&mut self) -> io::Result<()> {
        let _ = self.fs_watch_process.kill().await; // best-effort
        let _ = self.fs_watch_process.wait().await;
        Ok(())
    }

    /// Read and decode a single event from helper stdout.
    ///
    /// Protocol:
    /// - remove: `[op:u8][object:u8][len:u16be][path bytes...]`
    /// - add/create/modify: `[op:u8][object:u8][inode:u64][len:u16be][path bytes...]`
    /// - rename: `[op:u8][object:u8][inode:u64be][len:u16be][from_path bytes...][len:u16be][to_path bytes...]`.
    /// - replace: `[op:u8][object:u8][inode:u64be][inode:u64be][len:u16be][path bytes...]`
    ///
    /// Returns:
    /// - `Ok(Some(Event))` for one decoded event
    /// - `Ok(None)` on clean EOF (helper stdout closed)
    /// - `Err(..)` for malformed data or read failure
    async fn next_event(
        mut stdout: &mut ChildStdout,
        type_buffer: &mut [u8; 2],
        mut path_length_buffer: &mut [u8; 2],
        mut path_buffer: &mut Vec<u8>,
        inode_buffer: &mut [u8; 8],
    ) -> io::Result<Option<Event>> {
        type_buffer.fill(0);
        path_length_buffer.fill(0);
        path_buffer.fill(0);
        inode_buffer.fill(0);

        let (operation_type, object_type) = match stdout.read_exact(type_buffer.as_mut()).await {
            Ok(..) => (
                OperationType::try_from(type_buffer[0]).unwrap(),
                ObjectType::try_from(type_buffer[1]).unwrap(),
            ),
            // stream was closed
            Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => {
                return Ok(None);
            }
            Err(err) => {
                return Err(err);
            }
        };

        let read_str_fn = async |stream: &mut ChildStdout,
                                 len_buffer: &mut [u8; 2],
                                 path_buffer: &mut Vec<u8>|
               -> io::Result<PathBuf> {
            stream.read_exact(len_buffer.as_mut()).await?;
            let path_len_in_bytes = u16::from_be_bytes(*len_buffer) as usize;
            if path_buffer.len() < path_len_in_bytes {
                path_buffer.resize(path_len_in_bytes, 0);
            }

            stream
                .read_exact(&mut path_buffer[..path_len_in_bytes])
                .await?;

            // TODO: use different conversion method
            let path_str = String::from_utf8_lossy(&path_buffer[..path_len_in_bytes]).to_string();
            Ok(PathBuf::from(path_str))
        };

        let read_inode_fn =
            async |stream: &mut ChildStdout, inode_buffer: &mut [u8; 8]| -> io::Result<u64> {
                stream.read_exact(inode_buffer).await?;
                let inode = u64::from_be_bytes(*inode_buffer);
                Ok(inode)
            };

        let event = if matches!(operation_type, OperationType::Replaced) {
            let old_inode = read_inode_fn(&mut stdout, inode_buffer).await?;
            let new_inode = read_inode_fn(&mut stdout, inode_buffer).await?;

            let path = read_str_fn(&mut stdout, &mut path_length_buffer, &mut path_buffer).await?;

            match object_type {
                ObjectType::File => Event::FileReplaced {
                    path,
                    from: old_inode,
                    to: new_inode,
                },
                ObjectType::Folder => Event::FolderReplaced {
                    path,
                    from: old_inode,
                    to: new_inode,
                },
            }
        } else if matches!(operation_type, OperationType::Modified) {
            let old_inode = read_inode_fn(&mut stdout, inode_buffer).await?;
            let new_inode = read_inode_fn(&mut stdout, inode_buffer).await?;

            let path = read_str_fn(&mut stdout, &mut path_length_buffer, &mut path_buffer).await?;
            Event::FileModified(path, old_inode, new_inode)
        } else {
            let inode = read_inode_fn(&mut stdout, inode_buffer).await?;

            let first_path =
                read_str_fn(&mut stdout, &mut path_length_buffer, &mut path_buffer).await?;

            let event = match (operation_type, object_type) {
                (OperationType::Renamed, ObjectType::File) => Event::FileRenamed {
                    from: first_path,
                    to: read_str_fn(&mut stdout, &mut path_length_buffer, &mut path_buffer).await?,
                    inode,
                },
                (OperationType::Renamed, ObjectType::Folder) => Event::FolderRenamed {
                    from: first_path,
                    to: read_str_fn(&mut stdout, &mut path_length_buffer, &mut path_buffer).await?,
                    inode,
                },
                (OperationType::Added, ObjectType::File) => Event::FileAdded(first_path, inode),
                (OperationType::Added, ObjectType::Folder) => Event::FolderAdded(first_path, inode),
                (OperationType::Created, ObjectType::File) => Event::FileCreated(first_path, inode),
                (OperationType::Created, ObjectType::Folder) => {
                    Event::FolderCreated(first_path, inode)
                }
                (OperationType::Removed, ObjectType::File) => Event::FileRemoved(first_path, inode),
                (OperationType::Removed, ObjectType::Folder) => {
                    Event::FolderRemoved(first_path, inode)
                }
                _ => panic!("Impossible case"),
            };

            event
        };

        Ok(Some(event))
    }

    /// Spawn the embedded helper process and return `(watcher_handle, event_stream)`.
    ///
    /// If the helper exits very quickly (for example invalid path/arguments),
    /// this returns an error instead of a watcher.
    ///
    /// `excluded` is a list of absolute paths to suppress from the event stream.
    /// Any event whose path contains an entry as a substring is dropped.
    /// - Paths must be absolute (starting with `/`); relative paths are rejected with an error.
    /// - To exclude a directory and everything inside it, the path **must** end with `/`
    ///   (e.g. `"/data/archive/"`) so the substring check does not accidentally match
    ///   files or directories that merely share a common prefix.
    pub async fn spawn(
        dir: &Path,
        latency: f32,
        excluded: &[&str],
    ) -> io::Result<(Self, EventStream)> {
        let fs_watch_code = util::ensure_helper_on_disk()?;
        let mut cmd = Command::new(fs_watch_code);
        cmd.arg(dir)
            .arg(latency.to_string())
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::null());
        for name in excluded {
            let path = Path::new(name);

            if path.is_relative() {
                return io::Result::Err(io::Error::new(
                    std::io::ErrorKind::InvalidInput,
                    "only supporting absolute paths",
                ));
            }

            // only for UNIX
            if path.is_dir() && !path.ends_with("/") {
                return io::Result::Err(io::Error::new(
                    std::io::ErrorKind::InvalidInput,
                    "path must end with '/' when targeting a directory: \"/some/path/\"",
                ));
            }
            cmd.arg(name);
        }
        let mut fs_watch_process = cmd.spawn()?;

        match timeout(Duration::from_millis(150), fs_watch_process.wait()).await {
            Ok(Ok(status)) => {
                return Err(io::Error::other(format!(
                    "fs_watch exited early with status: {status}"
                )));
            }
            Ok(Err(e)) => return Err(e),
            Err(_) => {
                // timed out => still running, continue normal setup
            }
        }

        let mut stdout = fs_watch_process.stdout.take().expect("stdout piped");
        let (tx, rx) = mpsc::channel::<io::Result<Event>>(256);

        // Background task: reads binary protocol frames and pushes decoded events.
        tokio::spawn(async move {
            let mut type_buffer = [0u8; 2];
            let mut path_length_buffer = [0u8; 2];
            let mut inode_buffer = [0u8; 8];
            let mut path_buffer = vec![0u8; 500];

            loop {
                match Self::next_event(
                    &mut stdout,
                    &mut type_buffer,
                    &mut path_length_buffer,
                    &mut path_buffer,
                    &mut inode_buffer,
                )
                .await
                {
                    Ok(Some(event)) => {
                        if tx.send(Ok(event)).await.is_err() {
                            break;
                        }
                    }
                    Ok(None) => {
                        break;
                    }
                    Err(e) => {
                        let _ = tx.send(Err(e)).await;
                        break;
                    }
                }
            }
        });

        Ok((Self { fs_watch_process }, ReceiverStream::new(rx)))
    }
}

impl Drop for AsyncWatcher {
    fn drop(&mut self) {
        // If user did not call `stop()`, still try to terminate helper on drop.
        let _ = self.fs_watch_process.start_kill();
    }
}
