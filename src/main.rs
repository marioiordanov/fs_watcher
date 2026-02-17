use std::io::{self, Read, Result};
use std::process::{ChildStdout, Command, Stdio};

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
            _ => Err(())
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
        match value{
            3 => Ok(OperationType::Added),
            4 => Ok(OperationType::Modified),
            5 => Ok(OperationType::Created),
            6 => Ok(OperationType::Renamed),
            7 => Ok(OperationType::Removed),
            _ => Err(()),
        }
    }
}

enum Event {
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

fn main() -> Result<()> {
    let build_c_program = Command::new("./build.sh").output()?;
    assert!(build_c_program.status.success());

    let mut fs_watch_process = Command::new("./fs_watch")
        .arg("./тестова")
        .stdout(Stdio::piped())
        .stdin(Stdio::null())
        .spawn()?;

    let mut stdout = fs_watch_process
        .stdout
        .take()
        .ok_or(io::Error::from(io::ErrorKind::BrokenPipe))?;

    let mut get_char_buf = [0u8; 1];
    let mut type_buffer = [0u8; 2];
    let mut path_length_buffer = [0u8; 2];
    let mut path_buffer = vec![0u8; 500];

    loop {
        get_char_buf.fill(0);
        type_buffer.fill(0);
        path_length_buffer.fill(0);
        path_buffer.fill(0);

        if let Some(exit_status) = fs_watch_process.try_wait()? {
            println!("ended with {exit_status:?}");
            break;
        }

        let (operation_type, object_type) = match stdout.read_exact(&mut type_buffer) {
            Ok(..) => (OperationType::try_from(type_buffer[0]).unwrap(), ObjectType::try_from(type_buffer[1]).unwrap()),
            // stream was closed
            Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => {
                break;
            }
            err => {
                return err;
            }
        };

        let read_str_fn = |stream: &mut ChildStdout,
                           len_buffer: &mut [u8; 2],
                           path_buffer: &mut Vec<u8>|
         -> Result<String> {
            stream.read_exact(len_buffer.as_mut())?;
            let path_len_in_bytes = u16::from_be_bytes(*len_buffer) as usize;
            if path_buffer.len() < path_len_in_bytes {
                path_buffer.resize(path_len_in_bytes, 0);
            }

            stream.read_exact(&mut path_buffer[..path_len_in_bytes])?;
            let path_str = String::from_utf8_lossy(&path_buffer[..path_len_in_bytes]).to_string();

            Ok(path_str)
        };

        let first_path = read_str_fn(&mut stdout, &mut path_length_buffer, &mut path_buffer)?;

        let event = match (operation_type, object_type) {
            (OperationType::Renamed, _) => Event::Renamed {
                from: first_path,
                to: read_str_fn(&mut stdout, &mut path_length_buffer, &mut path_buffer)?,
            },
            (OperationType::Removed, ObjectType::File) => Event::FileRemoved(first_path),
            (OperationType::Removed, ObjectType::Folder) => Event::FolderRemoved(first_path),
            (OperationType::Added, ObjectType::File) => Event::FileAdded(first_path),
            (OperationType::Added, ObjectType::Folder) => Event::FolderAdded(first_path),
            (OperationType::Modified, ObjectType::File) => Event::FileModified(first_path),
            (OperationType::Created, ObjectType::File) => Event::FileCreated(first_path),
            (OperationType::Created, ObjectType::Folder) => Event::FolderCreated(first_path),
            _ => panic!("Impossible case")
        };

        println!("{event}");
    }

    Ok(())
}
