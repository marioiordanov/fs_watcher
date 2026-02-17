use std::io::{self, Read, Result};
use std::process::{ChildStdout, Command, Stdio};

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
            Ok(..) => (type_buffer[0], type_buffer[1]),
            // stream was closed
            Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => {
                break;
            }
            err => {
                return err;
            }
        };

        /*
                typedef enum {
            OBJECT_FILE   = 1,
            OBJECT_FOLDER = 2,
        } ObjectType;

        typedef enum {
            OP_ADDED    = 3,
            OP_MODIFIED = 4,
            OP_CREATED  = 5,
            OP_RENAMED  = 6,
            OP_REMOVED  = 7,
        } OpCode;
                 */

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

        let action = match (operation_type, object_type) {
            (6, _) => Event::Renamed {
                from: first_path,
                to: read_str_fn(&mut stdout, &mut path_length_buffer, &mut path_buffer)?,
            },
            (7, 1) => Event::FileRemoved(first_path),
            (7, 2) => Event::FolderRemoved(first_path),
            (3, 1) => Event::FileAdded(first_path),
            (3, 2) => Event::FolderAdded(first_path),
            (4, 1) => Event::FileModified(first_path),
            (5, 1) => Event::FileCreated(first_path),
            (5, 2) => Event::FolderCreated(first_path),
            (_, _) => panic!("Should not come into this case"),
        };

        println!("{action}");
    }

    Ok(())
}
