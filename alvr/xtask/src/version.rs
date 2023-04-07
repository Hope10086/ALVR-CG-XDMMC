use crate::command::date_utc_yyyymmdd;
use alvr_filesystem as afs;
use std::{
    fs,
    path::{Path, PathBuf},
};

fn packages_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .into()
}

pub fn split_string(source: &str, start_pattern: &str, end: char) -> (String, String, String) {
    let start_idx = source.find(start_pattern).unwrap() + start_pattern.len();
    let end_idx = start_idx + source[start_idx..].find(end).unwrap();

    (
        source[..start_idx].to_owned(),
        source[start_idx..end_idx].to_owned(),
        source[end_idx..].to_owned(),
    )
}

pub fn version_from_dir<P: AsRef<Path>>(dir: P) -> String {
    let manifest_path = packages_dir().join(dir).join("Cargo.toml");
    println!("cargo:rerun-if-changed={}", manifest_path.to_string_lossy());

    let manifest = fs::read_to_string(manifest_path).unwrap();
    let (_, version, _) = split_string(&manifest, "version = \"", '\"');

    version
}

pub fn version() -> String {
    version_from_dir("common")
}

pub fn alxr_version() -> String {
    version_from_dir("openxr-client/alxr-common")
}

fn bump_client_gradle_version(new_version: &str, is_nightly: bool) {
    if new_version.is_empty() {
        return;
    }
    let gradle_file_path = afs::workspace_dir()
        .join("alvr/client/android/app")
        .join("build.gradle");
    let file_content = fs::read_to_string(&gradle_file_path).unwrap();

    // Replace versionName
    let (file_start, _, file_end) = split_string(&file_content, "versionName \"", '\"');
    let file_content = format!("{file_start}{new_version}{file_end}");

    let file_content = if !is_nightly {
        // Replace versionCode
        let (file_start, old_version_code_string, file_end) =
            split_string(&file_content, "versionCode ", '\n');
        format!(
            "{file_start}{}{file_end}",
            old_version_code_string.parse::<usize>().unwrap() + 1,
        )
    } else {
        file_content
    };

    fs::write(gradle_file_path, file_content).unwrap();
}

fn bump_cargo_version(crate_dir_name: &str, new_version: &str) {
    let manifest_path = packages_dir().join(crate_dir_name).join("Cargo.toml");

    let manifest = fs::read_to_string(&manifest_path).unwrap();

    let (file_start, _, file_end) = split_string(&manifest, "version = \"", '\"');
    let manifest = format!("{file_start}{new_version}{file_end}");

    fs::write(manifest_path, manifest).unwrap();
}

fn bump_rpm_spec_version(new_version: &str, is_nightly: bool) {
    let spec_path = afs::workspace_dir().join("packaging/rpm/alvr.spec");
    let spec = fs::read_to_string(&spec_path).unwrap();

    // If there's a '-', split the version around it
    let (version_start, version_end) = {
        if new_version.contains('-') {
            let (_, tmp_start, mut tmp_end) = split_string(new_version, "", '-');
            tmp_end.remove(0);
            (tmp_start, format!("0.0.1{tmp_end}"))
        } else {
            (new_version.to_string(), "1.0.0".to_string())
        }
    };

    // Replace Version
    let (file_start, _, file_end) = split_string(&spec, "Version: ", '\n');
    let spec = format!("{file_start}{version_start}{file_end}");

    // Reset Release to 1.0.0
    let (file_start, _, file_end) = split_string(&spec, "Release: ", '\n');
    let spec = format!("{file_start}{version_end}{file_end}");

    // Replace Source in github URL
    let spec = {
        if is_nightly {
            spec
        } else {
            // Grab version (ex: https://github.com/alvr-org/ALVR/archive/refs/tags/v16.0.0-rc1.tar.gz)
            let (file_start, _, file_end) = split_string(&spec, "refs/tags/v", 't');
            format!("{file_start}{new_version}.{file_end}")
        }
    };

    fs::write(spec_path, spec).unwrap();
}

fn bump_deb_control_version(new_version: &str) {
    let control_path = afs::workspace_dir().join("packaging/deb/control");
    let control = fs::read_to_string(&control_path).unwrap();

    // Replace Version
    let (file_start, _, file_end) = split_string(&control, "\nVersion: ", '\n');
    let control = format!("{file_start}{new_version}{file_end}");

    fs::write(control_path, control).unwrap();
}

pub fn bump_version(maybe_version: Option<String>, is_nightly: bool) {
    let mut version = maybe_version.unwrap_or_else(version);

    if is_nightly {
        version = format!("{version}+nightly.{}", date_utc_yyyymmdd());
    }

    for dir_name in [
        "audio",
        "client",
        "commands",
        "common",
        "filesystem",
        "launcher",
        "server",
        "session",
        "sockets",
        "vrcompositor-wrapper",
        "vulkan-layer",
        "xtask",
    ] {
        bump_cargo_version(dir_name, &version);
    }
    bump_client_gradle_version(&version, is_nightly);
    bump_rpm_spec_version(&version, is_nightly);
    bump_deb_control_version(&version);

    println!("Git tag:\nv{version}");
}

pub fn bump_alxr_version(maybe_version: Option<String>, is_nightly: bool) {
    let mut version = maybe_version.unwrap_or_else(alxr_version);
    let uwp_cargo_version = version.clone();
    if is_nightly {
        version = format!("{version}+nightly.{}", date_utc_yyyymmdd());
    }

    let base_dir = PathBuf::from("openxr-client");
    for dir_name in [
        "alxr-engine-sys",
        "alxr-common",
        "alxr-client",
        "alxr-android-client",
        "alxr-android-client/pico-neo",
        "alxr-android-client/quest",
    ]
    .into_iter()
    .map(|d| base_dir.join(&d).to_str().unwrap().to_owned())
    {
        bump_cargo_version(&dir_name, &version);
    }

    // +1 major version for alxr-client-uwp (maybe starting from 1 isn't neccessary for store apps?)
    if let Some((major, minor_rev)) = uwp_cargo_version.split_once('.') {
        let next_major = (major.parse::<u32>().unwrap() + 1).to_string();
        let uwp_version = format!("{next_major}.{minor_rev}");
        let alxr_uwp_dir = base_dir.join("alxr-client/uwp");
        bump_cargo_version(alxr_uwp_dir.to_str().unwrap(), &uwp_version);
    }

    println!("Git tag:\nv{version}");
}
