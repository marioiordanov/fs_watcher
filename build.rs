use std::{env, path::PathBuf, process::Command};

fn main() {
    println!("cargo:rerun-if-changed=c/fs_watch.c");
    println!("cargo:rerun-if-changed=c/watcher.c");
    println!("cargo:rerun-if-changed=c/protocol.c");
    println!("cargo:rerun-if-changed=c/util.c");
    println!("cargo:rerun-if-changed=c/watcher.h");
    println!("cargo:rerun-if-changed=c/protocol.h");
    println!("cargo:rerun-if-changed=c/util.h");

    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));
    let helper = out_dir.join("fs_watch");

    let status = Command::new("clang")
        .args([
            "-Wall",
            "-Wextra",
            "-O2",
            "-Ic/include",
            "c/fs_watch.c",
            "c/watcher.c",
            "c/protocol.c",
            "c/util.c",
            "-framework",
            "CoreServices",
            "-framework",
            "CoreFoundation",
            "-o",
        ])
        .arg(&helper)
        .status()
        .expect("failed to run clang");

    assert!(status.success(), "clang failed");
}
