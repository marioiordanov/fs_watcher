use std::{ path::Path, time::Duration};

use tokio::{
    io::{self, AsyncReadExt},
    process::{Child, ChildStdout, Command},
    sync::mpsc,
    time::timeout,
};
use tokio_stream::wrappers::ReceiverStream;

use crate::util;

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
            _ => Err(()),
        }
    }
}

pub enum Event {
    FileRemoved(String),
    FolderRemoved(String),
    FolderAdded(String),
    FileAdded(String),
    FileModified(String),
    Renamed { from: String, to: String },
    FileCreated(String),
    FolderCreated(String),
}

impl std::fmt::Display for Event {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Event::FileRemoved(path) => write!(f, "file removed: {path}"),
            Event::FolderRemoved(path) => write!(f, "folder removed: {path}"),
            Event::FolderAdded(path) => write!(f, "folder added: {path}"),
            Event::FileAdded(path) => write!(f, "file added: {path}"),
            Event::FileModified(path) => write!(f, "file modified: {path}"),
            Event::Renamed { from, to } => write!(f, "renamed: {from} -> {to}"),
            Event::FileCreated(path) => write!(f, "file created: {path}"),
            Event::FolderCreated(path) => write!(f, "folder created: {path}"),
        }
    }
}

pub struct AsyncWatcher {
    fs_watch_process: Child,
}

pub type EventStream = ReceiverStream<io::Result<Event>>;

impl AsyncWatcher {
    /// Stop the helper explicitly.
    pub async fn stop(&mut self) -> io::Result<()> {
        let _ = self.fs_watch_process.kill().await; // best-effort
        let _ = self.fs_watch_process.wait().await;
        Ok(())
    }

    async fn next_event(
        mut stdout: &mut ChildStdout,
        type_buffer: &mut [u8; 2],
        mut path_length_buffer: &mut [u8; 2],
        mut path_buffer: &mut Vec<u8>,
    ) -> io::Result<Option<Event>> {
        type_buffer.fill(0);
        path_length_buffer.fill(0);
        path_buffer.fill(0);

        let (operation_type, object_type) = match stdout.read_exact(type_buffer.as_mut()).await {
            Ok(..) => (
                OperationType::try_from(type_buffer[0]).unwrap(),
                ObjectType::try_from(type_buffer[1]).unwrap(),
            ),
            // stream was closed
            Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => {
                println!("s");
                return Ok(None);
            }
            Err(err) => {
                return Err(err);
            }
        };

        let read_str_fn = async |stream: &mut ChildStdout,
                                 len_buffer: &mut [u8; 2],
                                 path_buffer: &mut Vec<u8>|
               -> io::Result<String> {
            stream.read_exact(len_buffer.as_mut()).await?;
            let path_len_in_bytes = u16::from_be_bytes(*len_buffer) as usize;
            if path_buffer.len() < path_len_in_bytes {
                path_buffer.resize(path_len_in_bytes, 0);
            }

            stream
                .read_exact(&mut path_buffer[..path_len_in_bytes])
                .await?;
            let path_str = String::from_utf8_lossy(&path_buffer[..path_len_in_bytes]).to_string();

            Ok(path_str)
        };

        let first_path =
            read_str_fn(&mut stdout, &mut path_length_buffer, &mut path_buffer).await?;

        let event = match (operation_type, object_type) {
            (OperationType::Renamed, _) => Event::Renamed {
                from: first_path,
                to: read_str_fn(&mut stdout, &mut path_length_buffer, &mut path_buffer).await?,
            },
            (OperationType::Removed, ObjectType::File) => Event::FileRemoved(first_path),
            (OperationType::Removed, ObjectType::Folder) => Event::FolderRemoved(first_path),
            (OperationType::Added, ObjectType::File) => Event::FileAdded(first_path),
            (OperationType::Added, ObjectType::Folder) => Event::FolderAdded(first_path),
            (OperationType::Modified, ObjectType::File) => Event::FileModified(first_path),
            (OperationType::Created, ObjectType::File) => Event::FileCreated(first_path),
            (OperationType::Created, ObjectType::Folder) => Event::FolderCreated(first_path),
            _ => panic!("Impossible case"),
        };

        Ok(Some(event))
    }
    pub async fn spawn(dir: &Path) -> io::Result<(Self, EventStream)> {
        let fs_watch_code = util::ensure_helper_on_disk()?;
        let mut fs_watch_process = Command::new(fs_watch_code)
            .arg(dir)
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::null())
            .spawn()?;

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

        if let Ok(Some(_)) = fs_watch_process.try_wait() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "Unable to launch process",
            ));
        }

        tokio::spawn(async move {
            let mut type_buffer = [0u8; 2];
            let mut path_length_buffer = [0u8; 2];
            let mut path_buffer = vec![0u8; 500];

            loop {
                match Self::next_event(
                    &mut stdout,
                    &mut type_buffer,
                    &mut path_length_buffer,
                    &mut path_buffer,
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
        let _ = self.fs_watch_process.start_kill();
    }
}
