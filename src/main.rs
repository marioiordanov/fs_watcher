use std::process::Command;
use std::{env, time::Duration};
use std::io::{self, Read, Result, Write};
use std::path::PathBuf;
use std::sync::mpsc::channel;


fn watch_file(filename: &str) -> Result<()> {
    let mut watcher = kqueue::Watcher::new()?;

    watcher.add_filename(
        filename,
        kqueue::EventFilter::EVFILT_VNODE,
        kqueue::FilterFlag::NOTE_DELETE
            | kqueue::FilterFlag::NOTE_WRITE
            | kqueue::FilterFlag::NOTE_RENAME,
    )?;

    watcher.watch()?;

    println!("Watching for events, press Ctrl+C to stop...");
    for ev in watcher.iter() {
        println!("{ev:?}");
    }

    Ok(())
}

fn main1() {
    let mut pid = Command::new("./fs_watch").spawn().unwrap();

    io::stdout().flush().ok();
    let mut line = String::new();
    std::thread::sleep(dur);

    pid.kill().unwrap();
}
