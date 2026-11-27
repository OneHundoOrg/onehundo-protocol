use crate::json::{parse_json, JsonValue};
use crate::zip_archive::{read_and_validate_zip, MAX_COMPRESSION_RATIO, MAX_TOTAL_UNCOMPRESSED_BYTES};
use std::collections::{BTreeMap, HashMap};
use std::fs;
use std::io::Read;
use std::path::Path;
use std::process::{Command, Stdio};

const PROGRESSION_MODES: &[&str] = &["STANDARD", "ENCORE", "SPECTATOR"];
const TRUST_TIERS: &[&str] = &["LOCAL", "VERIFIED_INTEGRATION", "COMPETITIVE"];
const EVIDENCE_CLASSES: &[&str] = &[
    "DIRECT_INTEGRATION_EVENT",
    "RUNTIME_OBSERVED",
    "PLATFORM_CONFIRMED",
    "LOCALLY_OBSERVED",
    "INFERRED",
];
const PLANE_C_EVIDENCE_CLASSES: &[&str] = &[
    "PLATFORM_CONFIRMED",
    "LOCALLY_OBSERVED",
    "INFERRED",
];
const VERIFICATION_STATES: &[&str] = &["PENDING", "VERIFIED", "UNVERIFIED", "REJECTED"];
const LIFECYCLE_SIGNALS: &[&str] = &[
    "NORMAL_FRAME",
    "SAVE_STATE_LOAD",
    "SOFT_RESET",
    "HARD_RESET",
    "FRAME_GAP",
];
const SLOT_STATES: &[&str] = &["FREE", "WRITING", "READY", "READING"];
const SEMANTIC_EVENT_TYPES: &[&str] = &["STAT_SET", "STAT_INC", "STATE_OBSERVE", "UNLOCK_REQUEST"];
const MATCHER_TYPES: &[&str] = &[
    "rom_hash",
    "recomp_game_id",
    "build_id",
    "platform_id",
    "sidecar_uuid",
    "asset_archive_hash",
];

// Contract bounds mirrored from schemas/pack/manifest.schema.json.
const MAX_AUTHORS: usize = 32;
const MAX_MATCHERS: usize = 64;
const MAX_ACHIEVEMENTS: usize = 5000;
const MAX_REGIONS: usize = 64;
const MAX_PRODUCER_IDS: usize = 32;
const MAX_DECLARED_STATS: usize = 256;
const MAX_OBSERVATION_SOURCES: usize = 32;
const MAX_PATH_LEN: usize = 256;

fn check_max_items(arr: &[JsonValue], max: usize, ctx: &str) -> Result<(), String> {
    if arr.len() > max {
        return Err(format!("{}: {} items exceeds maxItems {}", ctx, arr.len(), max));
    }
    Ok(())
}

fn check_max_len(s: &str, max: usize, ctx: &str) -> Result<(), String> {
    if s.chars().count() > max {
        return Err(format!("{}: length exceeds maxLength {}", ctx, max));
    }
    Ok(())
}

fn optional_array<'a>(obj: &'a BTreeMap<String, JsonValue>, key: &str, ctx: &str) -> Result<Option<&'a [JsonValue]>, String> {
    match obj.get(key) {
        None => Ok(None),
        Some(v) => v
            .as_array()
            .map(Some)
            .ok_or_else(|| format!("{}: '{}' must be an array", ctx, key)),
    }
}

/// One archive path segment as the schema allows it: starts with a letter, digit, '_' or '-'
/// (so never '.', '..' or empty), then letters, digits, '.', '_' or '-'.
fn is_safe_segment(seg: &str) -> bool {
    let mut bytes = seg.bytes();
    match bytes.next() {
        Some(b) if b.is_ascii_alphanumeric() || b == b'_' || b == b'-' => {}
        _ => return false,
    }
    bytes.all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'-'))
}

/// `icon_path` / `module_path`: `<prefix>/<seg>/.../<seg>.<ext>` with every segment safe.
pub fn is_safe_archive_path(path: &str, prefix: &str, exts: &[&str]) -> bool {
    if path.len() > MAX_PATH_LEN {
        return false;
    }
    let rest = match path.strip_prefix(prefix).and_then(|r| r.strip_prefix('/')) {
        Some(r) => r,
        None => return false,
    };
    let lower = rest.to_ascii_lowercase();
    if !exts.iter().any(|e| lower.ends_with(&format!(".{}", e))) {
        return false;
    }
    rest.split('/').all(is_safe_segment)
}

/// `rules_file`: a relative archive path with no absolute prefix and no '.' / '..' / empty segments.
pub fn is_safe_relative_path(path: &str) -> bool {
    !path.is_empty() && path.len() <= MAX_PATH_LEN && path.split('/').all(is_safe_segment)
}

/// `file_glob`: relative to the watched directory: no absolute path, drive letter, backslash or '..'.
pub fn is_safe_glob(glob: &str) -> bool {
    if glob.is_empty() || glob.len() > MAX_PATH_LEN || glob.starts_with('/') || glob.contains('\\') {
        return false;
    }
    let b = glob.as_bytes();
    if b.len() >= 2 && b[1] == b':' && b[0].is_ascii_alphabetic() {
        return false;
    }
    !glob.split('/').any(|seg| seg == "..")
}

fn req_str<'a>(obj: &'a BTreeMap<String, JsonValue>, key: &str, ctx: &str) -> Result<&'a str, String> {
    obj.get(key)
        .and_then(|v| v.as_str())
        .filter(|s| !s.is_empty())
        .ok_or_else(|| format!("{}: missing or invalid non-empty string '{}'", ctx, key))
}

fn req_int(obj: &BTreeMap<String, JsonValue>, key: &str, ctx: &str) -> Result<i64, String> {
    obj.get(key)
        .and_then(|v| v.as_i64())
        .ok_or_else(|| format!("{}: missing or invalid integer '{}'", ctx, key))
}

fn req_enum<'a>(
    obj: &'a BTreeMap<String, JsonValue>,
    key: &str,
    allowed: &[&str],
    ctx: &str,
) -> Result<&'a str, String> {
    let val = req_str(obj, key, ctx)?;
    if !allowed.contains(&val) {
        return Err(format!(
            "{}: invalid '{}' value '{}' (allowed: {:?})",
            ctx, key, val, allowed
        ));
    }
    Ok(val)
}

fn validate_orthogonal_dimensions(
    obj: &BTreeMap<String, JsonValue>,
    plane_c_only: bool,
    ctx: &str,
) -> Result<(), String> {
    req_enum(obj, "progression_mode", PROGRESSION_MODES, ctx)?;
    req_enum(obj, "trust_tier", TRUST_TIERS, ctx)?;
    if plane_c_only {
        req_enum(obj, "evidence_class", PLANE_C_EVIDENCE_CLASSES, ctx)?;
    } else {
        req_enum(obj, "evidence_class", EVIDENCE_CLASSES, ctx)?;
    }
    req_enum(obj, "verification_state", VERIFICATION_STATES, ctx)?;
    Ok(())
}

fn is_valid_canonical_id(s: &str) -> bool {
    if s.len() < 2 || s.len() > 128 {
        return false;
    }
    let mut chars = s.bytes();
    let first = match chars.next() {
        Some(c) => c,
        None => return false,
    };
    if !matches!(first, b'a'..=b'z' | b'0'..=b'9') {
        return false;
    }
    s.bytes()
        .all(|b| matches!(b, b'a'..=b'z' | b'0'..=b'9' | b'.' | b'_' | b'-'))
}

pub fn validate_manifest(val: &JsonValue, archive_entries: Option<&BTreeMap<String, Vec<u8>>>) -> Result<(), String> {
    let obj = val
        .as_object()
        .ok_or_else(|| "manifest.json must be a JSON object".to_string())?;

    let ver = req_str(obj, "manifest_version", "manifest")?;
    if ver != "0.1.0" {
        return Err(format!("manifest: unsupported manifest_version '{}'", ver));
    }

    let pack_id = req_str(obj, "pack_id", "manifest")?;
    if !is_valid_canonical_id(pack_id) {
        return Err(format!("manifest: invalid pack_id '{}'", pack_id));
    }

    let game_id = req_str(obj, "canonical_game_id", "manifest")?;
    if !is_valid_canonical_id(game_id) {
        return Err(format!("manifest: invalid canonical_game_id '{}'", game_id));
    }

    req_str(obj, "title", "manifest")?;

    if let Some(authors) = optional_array(obj, "authors", "manifest")? {
        check_max_items(authors, MAX_AUTHORS, "manifest.authors")?;
    }

    let matchers = obj
        .get("matchers")
        .and_then(|v| v.as_array())
        .filter(|a| !a.is_empty())
        .ok_or_else(|| "manifest: 'matchers' must be a non-empty array".to_string())?;
    check_max_items(matchers, MAX_MATCHERS, "manifest.matchers")?;

    for (idx, m) in matchers.iter().enumerate() {
        let m_obj = m
            .as_object()
            .ok_or_else(|| format!("manifest.matchers[{}] must be an object", idx))?;
        let ctx = format!("manifest.matchers[{}]", idx);
        req_enum(m_obj, "matcher_type", MATCHER_TYPES, &ctx)?;
        req_str(m_obj, "value", &ctx)?;
    }

    let profiles = obj
        .get("profiles")
        .and_then(|v| v.as_object())
        .filter(|p| !p.is_empty())
        .ok_or_else(|| "manifest: 'profiles' must be a non-empty object".to_string())?;

    for (pname, pval) in profiles {
        if !matches!(pname.as_str(), "memory" | "semantic" | "observation") {
            return Err(format!("manifest.profiles: unknown profile '{}'", pname));
        }
        let p_obj = pval
            .as_object()
            .ok_or_else(|| format!("manifest.profiles.{} must be an object", pname))?;
        if p_obj.get("enabled").and_then(|v| v.as_bool()).is_none() {
            return Err(format!("manifest.profiles.{}: missing boolean 'enabled'", pname));
        }
        let pctx = format!("manifest.profiles.{}", pname);
        if let Some(rules) = p_obj.get("rules_file") {
            let rules = rules
                .as_str()
                .ok_or_else(|| format!("{}: 'rules_file' must be a string", pctx))?;
            if !is_safe_relative_path(rules) {
                return Err(format!("{}: unsafe rules_file path '{}'", pctx, rules));
            }
        }
        if let Some(regions) = optional_array(p_obj, "regions", &pctx)? {
            check_max_items(regions, MAX_REGIONS, &format!("{}.regions", pctx))?;
            for (i, r) in regions.iter().enumerate() {
                if let Some(name) = r.as_object().and_then(|o| o.get("region_name")).and_then(|v| v.as_str()) {
                    check_max_len(name, 64, &format!("{}.regions[{}].region_name", pctx, i))?;
                }
            }
        }
        if let Some(ids) = optional_array(p_obj, "producer_ids", &pctx)? {
            check_max_items(ids, MAX_PRODUCER_IDS, &format!("{}.producer_ids", pctx))?;
            for (i, id) in ids.iter().enumerate() {
                let id = id
                    .as_str()
                    .ok_or_else(|| format!("{}.producer_ids[{}] must be a string", pctx, i))?;
                check_max_len(id, 128, &format!("{}.producer_ids[{}]", pctx, i))?;
            }
        }
        if let Some(stats) = optional_array(p_obj, "declared_stats", &pctx)? {
            check_max_items(stats, MAX_DECLARED_STATS, &format!("{}.declared_stats", pctx))?;
            for (i, st) in stats.iter().enumerate() {
                if let Some(key) = st.as_object().and_then(|o| o.get("key")).and_then(|v| v.as_str()) {
                    check_max_len(key, 128, &format!("{}.declared_stats[{}].key", pctx, i))?;
                }
            }
        }
        if let Some(sources) = optional_array(p_obj, "sources", &pctx)? {
            check_max_items(sources, MAX_OBSERVATION_SOURCES, &format!("{}.sources", pctx))?;
            for (i, src) in sources.iter().enumerate() {
                if let Some(glob) = src.as_object().and_then(|o| o.get("file_glob")) {
                    let glob = glob
                        .as_str()
                        .ok_or_else(|| format!("{}.sources[{}].file_glob must be a string", pctx, i))?;
                    if !is_safe_glob(glob) {
                        return Err(format!("{}.sources[{}]: unsafe file_glob '{}'", pctx, i, glob));
                    }
                }
            }
        }
    }

    let achievements = obj
        .get("achievements")
        .and_then(|v| v.as_array())
        .filter(|a| !a.is_empty())
        .ok_or_else(|| "manifest: 'achievements' must be a non-empty array".to_string())?;
    check_max_items(achievements, MAX_ACHIEVEMENTS, "manifest.achievements")?;

    for (idx, ach) in achievements.iter().enumerate() {
        let ctx = format!("manifest.achievements[{}]", idx);
        let a_obj = ach
            .as_object()
            .ok_or_else(|| format!("{} must be an object", ctx))?;
        let ach_id = req_str(a_obj, "canonical_achievement_id", &ctx)?;
        if !is_valid_canonical_id(ach_id) {
            return Err(format!("{}: invalid canonical_achievement_id '{}'", ctx, ach_id));
        }
        req_str(a_obj, "title", &ctx)?;
        req_str(a_obj, "description", &ctx)?;
        let pts = req_int(a_obj, "points", &ctx)?;
        if !(0..=1000).contains(&pts) {
            return Err(format!("{}: points {} out of range 0..=1000", ctx, pts));
        }
        let modes = a_obj
            .get("supported_modes")
            .and_then(|v| v.as_array())
            .filter(|a| !a.is_empty())
            .ok_or_else(|| format!("{}: 'supported_modes' must be non-empty array", ctx))?;
        let mut seen_modes: Vec<&str> = Vec::new();
        for mode in modes {
            let ms = mode
                .as_str()
                .ok_or_else(|| format!("{}: supported_modes item must be string", ctx))?;
            if !PROGRESSION_MODES.contains(&ms) {
                return Err(format!("{}: invalid progression_mode '{}'", ctx, ms));
            }
            if seen_modes.contains(&ms) {
                return Err(format!("{}: duplicate supported_modes item '{}'", ctx, ms));
            }
            seen_modes.push(ms);
        }
        req_enum(a_obj, "minimum_trust_tier", TRUST_TIERS, &ctx)?;
        if let Some(icon_path) = a_obj.get("icon_path").and_then(|v| v.as_str()) {
            if !is_safe_archive_path(icon_path, "assets", &["png", "jpg", "webp"]) {
                return Err(format!("{}: unsafe or invalid icon_path '{}'", ctx, icon_path));
            }
            if let Some(entries) = archive_entries {
                if !entries.contains_key(icon_path) {
                    return Err(format!("{}: referenced icon_path '{}' missing from archive", ctx, icon_path));
                }
            }
        }
    }

    if let Some(wasm) = obj.get("wasm_sandbox") {
        let w_obj = wasm
            .as_object()
            .ok_or_else(|| "manifest.wasm_sandbox must be an object".to_string())?;
        let mod_path = req_str(w_obj, "module_path", "manifest.wasm_sandbox")?;
        if !is_safe_archive_path(mod_path, "rules", &["wasm"]) {
            return Err(format!("manifest.wasm_sandbox: invalid module_path '{}'", mod_path));
        }
        let mem = req_int(w_obj, "max_memory_bytes", "manifest.wasm_sandbox")?;
        if !(65536..=67_108_864).contains(&mem) {
            return Err("manifest.wasm_sandbox: max_memory_bytes out of sandbox bounds".into());
        }
        if w_obj.get("allow_network").and_then(|v| v.as_bool()) == Some(true) {
            return Err("manifest.wasm_sandbox: allow_network must be false".into());
        }
        if w_obj.get("allow_filesystem").and_then(|v| v.as_bool()) == Some(true) {
            return Err("manifest.wasm_sandbox: allow_filesystem must be false".into());
        }
    }

    if let Some(sig) = obj.get("signature") {
        let s_obj = sig
            .as_object()
            .ok_or_else(|| "manifest.signature must be an object".to_string())?;
        req_enum(s_obj, "algorithm", &["ed25519"], "manifest.signature")?;
        req_str(s_obj, "key_id", "manifest.signature")?;
        req_str(s_obj, "signature_base64", "manifest.signature")?;
        req_str(s_obj, "signed_at", "manifest.signature")?;
    }

    Ok(())
}

pub fn validate_pack_bytes(bytes: &[u8]) -> Result<usize, String> {
    let entries = read_and_validate_zip(bytes)?;
    let manifest_bytes = entries
        .get("manifest.json")
        .ok_or_else(|| "pack archive missing required root 'manifest.json'".to_string())?;
    let manifest_str = std::str::from_utf8(manifest_bytes)
        .map_err(|_| "manifest.json is not valid UTF-8".to_string())?;
    let manifest_val = parse_json(manifest_str).map_err(|e| format!("manifest.json: {}", e))?;
    validate_manifest(&manifest_val, Some(&entries))?;
    Ok(entries.len())
}

pub fn validate_pack_file(path: &Path) -> Result<usize, String> {
    let bytes = fs::read(path).map_err(|e| format!("failed to read '{}': {}", path.display(), e))?;
    validate_pack_bytes(&bytes)
}

pub fn validate_semantic_jsonl_str(content: &str) -> Result<usize, String> {
    let mut count = 0usize;
    let mut last_seq_by_stream: HashMap<(String, String), i64> = HashMap::new();

    for (line_idx, raw_line) in content.lines().enumerate() {
        let line = raw_line.trim();
        if line.is_empty() {
            continue;
        }
        let ctx = format!("line {}", line_idx + 1);
        let val = parse_json(line).map_err(|e| format!("{}: {}", ctx, e))?;
        let obj = val
            .as_object()
            .ok_or_else(|| format!("{}: expected JSON object", ctx))?;

        let ver = req_str(obj, "schema_version", &ctx)?;
        if ver != "0.1.0" {
            return Err(format!("{}: unsupported schema_version '{}'", ctx, ver));
        }
        let game_id = req_str(obj, "canonical_game_id", &ctx)?;
        if !is_valid_canonical_id(game_id) {
            return Err(format!("{}: invalid canonical_game_id '{}'", ctx, game_id));
        }
        let producer = req_str(obj, "producer_id", &ctx)?;
        let session = req_str(obj, "session_id", &ctx)?;
        let seq = req_int(obj, "sequence_number", &ctx)?;
        if seq < 1 {
            return Err(format!("{}: sequence_number must be >= 1", ctx));
        }
        let idemp = req_str(obj, "idempotency_key", &ctx)?;
        let expected_idemp = format!("{}:{}:{}", producer, session, seq);
        if idemp != expected_idemp {
            return Err(format!(
                "{}: idempotency_key '{}' does not match '{}'",
                ctx, idemp, expected_idemp
            ));
        }

        let stream_key = (producer.to_string(), session.to_string());
        if let Some(&prev) = last_seq_by_stream.get(&stream_key) {
            if seq <= prev {
                return Err(format!(
                    "{}: non-monotonic sequence_number {} after {}",
                    ctx, seq, prev
                ));
            }
        }
        last_seq_by_stream.insert(stream_key, seq);

        let _ts = req_int(obj, "timestamp_nanos", &ctx)?;
        req_enum(obj, "event_type", SEMANTIC_EVENT_TYPES, &ctx)?;
        req_str(obj, "key", &ctx)?;
        validate_orthogonal_dimensions(obj, false, &ctx)?;
        count += 1;
    }

    if count == 0 {
        return Err("semantic trace contains zero event records".into());
    }
    Ok(count)
}

pub fn validate_snapshot_header_obj(obj: &BTreeMap<String, JsonValue>, ctx: &str) -> Result<(), String> {
    let ver = req_str(obj, "schema_version", ctx)?;
    if ver != "0.1.0" {
        return Err(format!("{}: unsupported schema_version '{}'", ctx, ver));
    }
    let game_id = req_str(obj, "canonical_game_id", ctx)?;
    if !is_valid_canonical_id(game_id) {
        return Err(format!("{}: invalid canonical_game_id '{}'", ctx, game_id));
    }
    req_str(obj, "producer_id", ctx)?;
    req_str(obj, "session_id", ctx)?;
    let slot_idx = req_int(obj, "slot_index", ctx)?;
    if slot_idx < 0 {
        return Err(format!("{}: slot_index must be >= 0", ctx));
    }
    req_enum(obj, "slot_state", SLOT_STATES, ctx)?;
    let seq = req_int(obj, "sequence_number", ctx)?;
    if seq < 1 {
        return Err(format!("{}: sequence_number must be >= 1", ctx));
    }
    let _ts = req_int(obj, "timestamp_nanos", ctx)?;
    let sig = req_enum(obj, "lifecycle_signal", LIFECYCLE_SIGNALS, ctx)?;
    if sig == "FRAME_GAP" {
        let gap = req_int(obj, "frame_gap_count", ctx)?;
        if gap < 1 {
            return Err(format!("{}: FRAME_GAP signal requires frame_gap_count >= 1", ctx));
        }
    }
    let payload_sz = req_int(obj, "payload_size_bytes", ctx)?;
    if payload_sz < 0 {
        return Err(format!("{}: payload_size_bytes must be >= 0", ctx));
    }
    validate_orthogonal_dimensions(obj, false, ctx)?;
    Ok(())
}

pub fn validate_sparse_jsonl_str(content: &str) -> Result<usize, String> {
    let mut count = 0usize;
    for (line_idx, raw_line) in content.lines().enumerate() {
        let line = raw_line.trim();
        if line.is_empty() {
            continue;
        }
        let ctx = format!("sparse line {}", line_idx + 1);
        let val = parse_json(line).map_err(|e| format!("{}: {}", ctx, e))?;
        let obj = val
            .as_object()
            .ok_or_else(|| format!("{}: expected JSON object", ctx))?;
        validate_snapshot_header_obj(obj, &ctx)?;
        if let Some(writes) = obj.get("sparse_writes") {
            let arr = writes
                .as_array()
                .ok_or_else(|| format!("{}: 'sparse_writes' must be an array", ctx))?;
            for (wi, w) in arr.iter().enumerate() {
                let wobj = w
                    .as_object()
                    .ok_or_else(|| format!("{}.sparse_writes[{}] must be an object", ctx, wi))?;
                let wctx = format!("{}.sparse_writes[{}]", ctx, wi);
                let _rid = req_int(wobj, "region_id", &wctx)?;
                req_str(wobj, "offset_hex", &wctx)?;
                let hex_bytes = req_str(wobj, "bytes_hex", &wctx)?;
                if hex_bytes.len() % 2 != 0 || !hex_bytes.bytes().all(|b| b.is_ascii_hexdigit()) {
                    return Err(format!("{}: invalid hex string '{}'", wctx, hex_bytes));
                }
            }
        }
        count += 1;
    }
    if count == 0 {
        return Err("sparse trace contains zero frame records".into());
    }
    Ok(count)
}

pub fn validate_snapshot_zst_file(path: &Path) -> Result<usize, String> {
    let raw = fs::read(path).map_err(|e| format!("failed to read '{}': {}", path.display(), e))?;
    if raw.len() < 4 || raw[0..4] != [0x28, 0xB5, 0x2F, 0xFD] {
        return Err("snapshot file does not start with valid Zstandard magic (28 B5 2F FD)".into());
    }

    let max_allowed = raw
        .len()
        .saturating_mul(MAX_COMPRESSION_RATIO)
        .min(MAX_TOTAL_UNCOMPRESSED_BYTES);

    let mut child = Command::new("zstd")
        .arg("-dc")
        .arg(path)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("failed to invoke 'zstd -dc' for snapshot validation: {}", e))?;

    let mut stdout_buf = Vec::new();
    if let Some(mut out) = child.stdout.take() {
        let mut limited = out.by_ref().take((max_allowed + 1) as u64);
        limited
            .read_to_end(&mut stdout_buf)
            .map_err(|e| format!("error reading zstd output: {}", e))?;
    }
    let status = child
        .wait()
        .map_err(|e| format!("failed waiting for zstd process: {}", e))?;
    if !status.success() {
        return Err("zstd decompression failed (corrupt archive)".into());
    }
    if stdout_buf.len() > max_allowed {
        return Err("zstd snapshot exceeds maximum allowed decompression ratio/size".into());
    }

    let text = std::str::from_utf8(&stdout_buf)
        .map_err(|_| "decompressed snapshot payload is not valid UTF-8 JSONL".to_string())?;
    let mut count = 0usize;
    for (idx, raw_line) in text.lines().enumerate() {
        let line = raw_line.trim();
        if line.is_empty() {
            continue;
        }
        let ctx = format!("snapshot line {}", idx + 1);
        let val = parse_json(line).map_err(|e| format!("{}: {}", ctx, e))?;
        let obj = val
            .as_object()
            .ok_or_else(|| format!("{}: expected JSON object", ctx))?;
        validate_snapshot_header_obj(obj, &ctx)?;
        count += 1;
    }
    if count == 0 {
        return Err("snapshot archive contains zero frame records".into());
    }
    Ok(count)
}

pub fn validate_observation_json_str(content: &str) -> Result<usize, String> {
    let val = parse_json(content).map_err(|e| format!("observation json: {}", e))?;
    let records: Vec<&JsonValue> = match &val {
        JsonValue::Array(arr) => arr.iter().collect(),
        JsonValue::Object(_) => vec![&val],
        _ => return Err("observation file must be a JSON object or array of objects".into()),
    };
    if records.is_empty() {
        return Err("observation file contains zero records".into());
    }

    for (idx, rec) in records.iter().enumerate() {
        let ctx = format!("observation[{}]", idx);
        let obj = rec
            .as_object()
            .ok_or_else(|| format!("{}: expected JSON object", ctx))?;
        let ver = req_str(obj, "schema_version", &ctx)?;
        if ver != "0.1.0" {
            return Err(format!("{}: unsupported schema_version '{}'", ctx, ver));
        }
        req_str(obj, "observation_id", &ctx)?;
        let game_id = req_str(obj, "canonical_game_id", &ctx)?;
        if !is_valid_canonical_id(game_id) {
            return Err(format!("{}: invalid canonical_game_id '{}'", ctx, game_id));
        }
        req_str(obj, "observer_id", &ctx)?;
        req_enum(
            obj,
            "source_type",
            &["steam_api", "psn_trophy", "save_file", "emulator_sram", "log_parser"],
            &ctx,
        )?;
        req_str(obj, "observed_at", &ctx)?;
        req_enum(
            obj,
            "record_kind",
            &["ACHIEVEMENT_EVIDENCE", "STAT_SNAPSHOT", "PLAYTIME_EVIDENCE"],
            &ctx,
        )?;
        req_str(obj, "key", &ctx)?;
        // Plane C MUST NOT use live event evidence classes
        validate_orthogonal_dimensions(obj, true, &ctx)?;
        // Plane C is passive evidence: always LOCAL, and only platform-confirmed evidence may be VERIFIED.
        req_enum(obj, "trust_tier", &["LOCAL"], &ctx)?;
        let evidence = req_str(obj, "evidence_class", &ctx)?;
        let verification = req_str(obj, "verification_state", &ctx)?;
        if verification == "VERIFIED" && evidence != "PLATFORM_CONFIRMED" {
            return Err(format!(
                "{}: verification_state VERIFIED requires evidence_class PLATFORM_CONFIRMED (got {})",
                ctx, evidence
            ));
        }
    }
    Ok(records.len())
}
