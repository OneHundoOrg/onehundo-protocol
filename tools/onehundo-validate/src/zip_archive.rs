use crate::deflate::{crc32, inflate};
use std::collections::BTreeMap;

pub const MAX_SINGLE_ENTRY_BYTES: usize = 64 * 1024 * 1024;
pub const MAX_TOTAL_UNCOMPRESSED_BYTES: usize = 256 * 1024 * 1024;
pub const MAX_COMPRESSION_RATIO: usize = 100;

#[derive(Debug, Clone)]
pub struct ZipEntry {
    pub name: String,
    pub data: Vec<u8>,
}

fn read_u16_le(buf: &[u8], off: usize) -> Result<u16, String> {
    if off + 2 > buf.len() {
        return Err("truncated ZIP u16 field".into());
    }
    Ok(u16::from_le_bytes([buf[off], buf[off + 1]]))
}

fn read_u32_le(buf: &[u8], off: usize) -> Result<u32, String> {
    if off + 4 > buf.len() {
        return Err("truncated ZIP u32 field".into());
    }
    Ok(u32::from_le_bytes([
        buf[off],
        buf[off + 1],
        buf[off + 2],
        buf[off + 3],
    ]))
}

pub fn validate_entry_path(name: &str) -> Result<bool, String> {
    if name.is_empty() {
        return Err("empty archive entry path".into());
    }
    if name.contains('\0') {
        return Err(format!("archive entry contains NUL byte: {:?}", name));
    }
    if name.starts_with('/') || name.starts_with('\\') {
        return Err(format!("zip-slip / absolute path rejected: {}", name));
    }
    if name.contains('\\') {
        return Err(format!("backslash in archive entry path rejected: {}", name));
    }
    if name.len() >= 2 && name.as_bytes()[1] == b':' {
        return Err(format!("Windows drive letter path rejected: {}", name));
    }
    for seg in name.split('/') {
        if seg == ".." || seg == "." {
            return Err(format!("path traversal segment '{}' rejected in: {}", seg, name));
        }
    }

    let is_dir = name.ends_with('/');
    if !is_dir {
        let lower = name.to_ascii_lowercase();
        let allowed = [".json", ".png", ".jpg", ".jpeg", ".webp", ".wasm", ".sig", ".md"];
        if !allowed.iter().any(|ext| lower.ends_with(ext)) {
            return Err(format!("disallowed file extension in pack archive: {}", name));
        }
    }
    Ok(is_dir)
}

pub fn read_and_validate_zip(bytes: &[u8]) -> Result<BTreeMap<String, Vec<u8>>, String> {
    if bytes.len() < 22 {
        return Err("file too small to be a valid ZIP archive".into());
    }

    // Locate End of Central Directory (0x06054b50)
    let search_start = bytes.len().saturating_sub(22 + 65535);
    let mut eocd_off = None;
    let mut i = bytes.len() - 22;
    loop {
        if bytes[i..i + 4] == [0x50, 0x4b, 0x05, 0x06] {
            let comment_len = read_u16_le(bytes, i + 20)? as usize;
            if i + 22 + comment_len == bytes.len() {
                eocd_off = Some(i);
                break;
            }
        }
        if i == search_start {
            break;
        }
        i -= 1;
    }

    let eocd = eocd_off.ok_or_else(|| "missing ZIP End of Central Directory record".to_string())?;
    let total_entries = read_u16_le(bytes, eocd + 10)? as usize;
    let cd_size = read_u32_le(bytes, eocd + 12)? as usize;
    let cd_offset = read_u32_le(bytes, eocd + 16)? as usize;

    if cd_offset.saturating_add(cd_size) > eocd {
        return Err("ZIP central directory out of bounds".into());
    }

    let mut entries = BTreeMap::new();
    let mut pos = cd_offset;
    let mut total_uncompressed: usize = 0;

    for _ in 0..total_entries {
        if pos + 46 > cd_offset + cd_size {
            return Err("truncated ZIP central directory header".into());
        }
        if read_u32_le(bytes, pos)? != 0x0201_4b50 {
            return Err("invalid ZIP central directory signature".into());
        }

        let version_made_by = read_u16_le(bytes, pos + 4)?;
        let flags = read_u16_le(bytes, pos + 8)?;
        let method = read_u16_le(bytes, pos + 10)?;
        let expected_crc = read_u32_le(bytes, pos + 16)?;
        let comp_size = read_u32_le(bytes, pos + 20)? as usize;
        let uncomp_size = read_u32_le(bytes, pos + 24)? as usize;
        let name_len = read_u16_le(bytes, pos + 28)? as usize;
        let extra_len = read_u16_le(bytes, pos + 30)? as usize;
        let comment_len = read_u16_le(bytes, pos + 32)? as usize;
        let external_attr = read_u32_le(bytes, pos + 38)?;
        let local_hdr_off = read_u32_le(bytes, pos + 42)? as usize;

        if flags & 0x0001 != 0 {
            return Err("encrypted ZIP entries are not supported in .onehundo packs".into());
        }
        if pos + 46 + name_len + extra_len + comment_len > cd_offset + cd_size {
            return Err("ZIP central directory entry extends past central directory".into());
        }

        let name_bytes = &bytes[pos + 46..pos + 46 + name_len];
        let name = std::str::from_utf8(name_bytes)
            .map_err(|_| "non-UTF8 archive entry filename rejected".to_string())?
            .to_string();

        // Check for Unix symlink (mode 0120000 = 0xA000) in upper 16 bits of external_attr
        let unix_mode = (external_attr >> 16) & 0xF000;
        let host_os = version_made_by >> 8;
        if (host_os == 3 || host_os == 19 || unix_mode != 0) && unix_mode == 0xA000 {
            return Err(format!("symbolic link rejected in pack archive: {}", name));
        }

        let is_dir = validate_entry_path(&name)?;

        if uncomp_size > MAX_SINGLE_ENTRY_BYTES {
            return Err(format!(
                "entry '{}' exceeds max single file size ({} > {})",
                name, uncomp_size, MAX_SINGLE_ENTRY_BYTES
            ));
        }
        total_uncompressed = total_uncompressed.saturating_add(uncomp_size);
        if total_uncompressed > MAX_TOTAL_UNCOMPRESSED_BYTES {
            return Err("archive total uncompressed size exceeds 256 MiB limit".into());
        }

        // Compression bomb ratio guard (declared)
        if !is_dir && uncomp_size > 0 {
            if comp_size == 0 || uncomp_size / comp_size > MAX_COMPRESSION_RATIO {
                return Err(format!(
                    "compression bomb detected on '{}' (declared ratio > {}:1)",
                    name, MAX_COMPRESSION_RATIO
                ));
            }
        }

        // Read Local File Header
        if local_hdr_off + 30 > bytes.len() {
            return Err(format!("truncated local file header for '{}'", name));
        }
        if read_u32_le(bytes, local_hdr_off)? != 0x0403_4b50 {
            return Err(format!("invalid local file header signature for '{}'", name));
        }
        let lfh_name_len = read_u16_le(bytes, local_hdr_off + 26)? as usize;
        let lfh_extra_len = read_u16_le(bytes, local_hdr_off + 28)? as usize;
        let data_start = local_hdr_off + 30 + lfh_name_len + lfh_extra_len;
        let data_end = data_start
            .checked_add(comp_size)
            .ok_or_else(|| "compressed data offset overflow".to_string())?;
        if data_end > bytes.len() {
            return Err(format!("compressed data out of bounds for '{}'", name));
        }

        let comp_slice = &bytes[data_start..data_end];
        if !is_dir {
            let max_allowed = if comp_size == 0 {
                0
            } else {
                comp_size.saturating_mul(MAX_COMPRESSION_RATIO).min(MAX_SINGLE_ENTRY_BYTES)
            };
            let inflated = match method {
                0 => {
                    if comp_slice.len() != uncomp_size {
                        return Err(format!("stored entry '{}' size mismatch", name));
                    }
                    comp_slice.to_vec()
                }
                8 => inflate(comp_slice, max_allowed)
                    .map_err(|e| format!("entry '{}' decompression failed (possible bomb/corrupt): {}", name, e))?,
                _ => {
                    return Err(format!(
                        "unsupported ZIP compression method {} for '{}'",
                        method, name
                    ))
                }
            };

            if inflated.len() != uncomp_size {
                return Err(format!(
                    "entry '{}' uncompressed size mismatch (header={}, actual={})",
                    name,
                    uncomp_size,
                    inflated.len()
                ));
            }
            if crc32(&inflated) != expected_crc {
                return Err(format!("CRC-32 mismatch for entry '{}'", name));
            }
            entries.insert(name, inflated);
        }

        pos += 46 + name_len + extra_len + comment_len;
    }

    Ok(entries)
}
