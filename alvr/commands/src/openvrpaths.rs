use alvr_common::prelude::*;
use encoding_rs_io::DecodeReaderBytes;
use serde_json as json;
use std::{
    fs::{self, File},
    io::Read,
    path::PathBuf,
};

pub fn openvr_source_file_path() -> StrResult<PathBuf> {
    let path = trace_none!(if cfg!(windows) {
        dirs::cache_dir()
    } else {
        dirs::config_dir()
    })?
    .join("openvr/openvrpaths.vrpath");

    if path.exists() {
        Ok(path)
    } else {
        fmt_e!("{} does not exist", path.to_string_lossy())
    }
}

pub(crate) fn load_openvr_paths_json() -> StrResult<json::Value> {
    let file = trace_err!(File::open(openvr_source_file_path()?))?;

    let mut file_content_decoded = String::new();
    trace_err!(DecodeReaderBytes::new(&file).read_to_string(&mut file_content_decoded))?;

    trace_err!(json::from_str(&file_content_decoded))
}

pub(crate) fn save_openvr_paths_json(openvr_paths: &json::Value) -> StrResult {
    let file_content = trace_err!(json::to_string_pretty(openvr_paths))?;

    trace_err!(fs::write(openvr_source_file_path()?, file_content))
}

pub(crate) fn from_openvr_paths(paths: &json::Value) -> Vec<std::path::PathBuf> {
    let paths_vec = match paths.as_array() {
        Some(vec) => vec,
        None => return vec![],
    };

    paths_vec
        .iter()
        .filter_map(json::Value::as_str)
        .map(|s| PathBuf::from(s.replace(r"\\", r"\")))
        .collect()
}

pub(crate) fn to_openvr_paths(paths: &[PathBuf]) -> json::Value {
    let paths_vec = paths
        .iter()
        .map(|p| p.to_string_lossy().into())
        .map(json::Value::String) // backslashes gets duplicated here
        .collect::<Vec<_>>();

    json::Value::Array(paths_vec)
}

fn get_single_openvr_path(path_type: &str) -> StrResult<PathBuf> {
    let openvr_paths_json = load_openvr_paths_json()?;
    let paths_json = trace_none!(openvr_paths_json.get(path_type))?;
    trace_none!(from_openvr_paths(paths_json).get(0).cloned())
}

pub fn steamvr_root_dir() -> StrResult<PathBuf> {
    get_single_openvr_path("runtime")
}

pub fn steam_config_dir() -> StrResult<PathBuf> {
    get_single_openvr_path("config")
}
