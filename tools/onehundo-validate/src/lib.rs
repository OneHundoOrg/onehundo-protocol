pub mod deflate;
pub mod json;
pub mod validators;
pub mod zip_archive;

#[cfg(test)]
mod tests {
    use super::deflate::crc32;
    use super::json::parse_json;
    use super::validators::{
        is_safe_archive_path, is_safe_glob, is_safe_relative_path, validate_manifest,
        validate_observation_json_str, validate_pack_bytes, validate_semantic_jsonl_str,
        validate_sparse_jsonl_str,
    };

    /// Minimal stored-entry ZIP builder for deterministic security unit tests.
    fn build_test_zip(entries: &[(&str, &[u8], u32, u16)]) -> Vec<u8> {
        let mut out = Vec::new();
        let mut cd = Vec::new();

        for (name, data, ext_attr, method) in entries {
            let local_off = out.len() as u32;
            let crc = crc32(data);
            let name_bytes = name.as_bytes();
            let comp_len = data.len() as u32;
            let uncomp_len = data.len() as u32;

            // Local file header
            out.extend_from_slice(&0x0403_4b50u32.to_le_bytes());
            out.extend_from_slice(&20u16.to_le_bytes()); // version
            out.extend_from_slice(&0u16.to_le_bytes()); // flags
            out.extend_from_slice(&method.to_le_bytes());
            out.extend_from_slice(&0u16.to_le_bytes()); // mod time
            out.extend_from_slice(&0u16.to_le_bytes()); // mod date
            out.extend_from_slice(&crc.to_le_bytes());
            out.extend_from_slice(&comp_len.to_le_bytes());
            out.extend_from_slice(&uncomp_len.to_le_bytes());
            out.extend_from_slice(&(name_bytes.len() as u16).to_le_bytes());
            out.extend_from_slice(&0u16.to_le_bytes()); // extra len
            out.extend_from_slice(name_bytes);
            out.extend_from_slice(data);

            // Central directory header
            cd.extend_from_slice(&0x0201_4b50u32.to_le_bytes());
            cd.extend_from_slice(&((3u16 << 8) | 20u16).to_le_bytes()); // Unix
            cd.extend_from_slice(&20u16.to_le_bytes());
            cd.extend_from_slice(&0u16.to_le_bytes());
            cd.extend_from_slice(&method.to_le_bytes());
            cd.extend_from_slice(&0u16.to_le_bytes());
            cd.extend_from_slice(&0u16.to_le_bytes());
            cd.extend_from_slice(&crc.to_le_bytes());
            cd.extend_from_slice(&comp_len.to_le_bytes());
            cd.extend_from_slice(&uncomp_len.to_le_bytes());
            cd.extend_from_slice(&(name_bytes.len() as u16).to_le_bytes());
            cd.extend_from_slice(&0u16.to_le_bytes());
            cd.extend_from_slice(&0u16.to_le_bytes());
            cd.extend_from_slice(&0u16.to_le_bytes());
            cd.extend_from_slice(&0u16.to_le_bytes());
            cd.extend_from_slice(&ext_attr.to_le_bytes());
            cd.extend_from_slice(&local_off.to_le_bytes());
            cd.extend_from_slice(name_bytes);
        }

        let cd_off = out.len() as u32;
        let cd_sz = cd.len() as u32;
        out.extend_from_slice(&cd);

        // End of central directory
        out.extend_from_slice(&0x0605_4b50u32.to_le_bytes());
        out.extend_from_slice(&0u16.to_le_bytes());
        out.extend_from_slice(&0u16.to_le_bytes());
        out.extend_from_slice(&(entries.len() as u16).to_le_bytes());
        out.extend_from_slice(&(entries.len() as u16).to_le_bytes());
        out.extend_from_slice(&cd_sz.to_le_bytes());
        out.extend_from_slice(&cd_off.to_le_bytes());
        out.extend_from_slice(&0u16.to_le_bytes());
        out
    }

    const VALID_MANIFEST: &str = r#"{
        "manifest_version": "0.1.0",
        "pack_id": "sony.bloodborne.core",
        "canonical_game_id": "sony.bloodborne",
        "title": "Bloodborne (PC Port)",
        "matchers": [{"matcher_type": "build_id", "value": "bbport-0.9.1"}],
        "profiles": {"semantic": {"enabled": true, "producer_ids": ["bbport_pc"]}},
        "achievements": [{
            "canonical_achievement_id": "ach_cleric_beast",
            "title": "Cleric Beast",
            "description": "Defeat Cleric Beast.",
            "points": 15,
            "supported_modes": ["STANDARD", "ENCORE"],
            "minimum_trust_tier": "VERIFIED_INTEGRATION"
        }]
    }"#;

    #[test]
    fn accepts_valid_pack() {
        let zip = build_test_zip(&[("manifest.json", VALID_MANIFEST.as_bytes(), 0o100644 << 16, 0)]);
        assert_eq!(validate_pack_bytes(&zip).unwrap(), 1);
    }

    #[test]
    fn rejects_zip_slip_traversal() {
        let zip = build_test_zip(&[
            ("manifest.json", VALID_MANIFEST.as_bytes(), 0o100644 << 16, 0),
            ("../evil.json", b"{}", 0o100644 << 16, 0),
        ]);
        let err = validate_pack_bytes(&zip).unwrap_err();
        assert!(err.contains("traversal"), "unexpected err: {}", err);
    }

    #[test]
    fn rejects_symlink_entry() {
        let zip = build_test_zip(&[
            ("manifest.json", VALID_MANIFEST.as_bytes(), 0o100644 << 16, 0),
            ("assets/link.json", b"/etc/passwd", 0o120777 << 16, 0),
        ]);
        let err = validate_pack_bytes(&zip).unwrap_err();
        assert!(err.contains("symbolic link"), "unexpected err: {}", err);
    }

    #[test]
    fn rejects_disallowed_executable_extension() {
        let zip = build_test_zip(&[
            ("manifest.json", VALID_MANIFEST.as_bytes(), 0o100644 << 16, 0),
            ("payload.sh", b"#!/bin/sh", 0o100755 << 16, 0),
        ]);
        let err = validate_pack_bytes(&zip).unwrap_err();
        assert!(err.contains("disallowed file extension"), "unexpected err: {}", err);
    }

    #[test]
    fn rejects_plane_c_claiming_direct_integration() {
        let bad_obs = r#"{
            "schema_version": "0.1.0",
            "observation_id": "550e8400-e29b-41d4-a716-446655440001",
            "canonical_game_id": "shared.steam.game",
            "observer_id": "steam_save_scanner",
            "source_type": "save_file",
            "observed_at": "2026-11-24T12:00:00Z",
            "record_kind": "ACHIEVEMENT_EVIDENCE",
            "key": "ach_first_clear",
            "progression_mode": "STANDARD",
            "trust_tier": "LOCAL",
            "evidence_class": "DIRECT_INTEGRATION_EVENT",
            "verification_state": "UNVERIFIED"
        }"#;
        let err = validate_observation_json_str(bad_obs).unwrap_err();
        assert!(err.contains("evidence_class"), "unexpected err: {}", err);
    }

    #[test]
    fn rejects_non_monotonic_semantic_sequence() {
        let bad_events = concat!(
            r#"{"schema_version":"0.1.0","canonical_game_id":"sony.bloodborne","producer_id":"bbport","session_id":"550e8400-e29b-41d4-a716-446655440000","sequence_number":2,"idempotency_key":"bbport:550e8400-e29b-41d4-a716-446655440000:2","timestamp_nanos":200,"event_type":"STAT_INC","key":"kills","int_value":1,"progression_mode":"STANDARD","trust_tier":"LOCAL","evidence_class":"DIRECT_INTEGRATION_EVENT","verification_state":"PENDING"}"#,
            "\n",
            r#"{"schema_version":"0.1.0","canonical_game_id":"sony.bloodborne","producer_id":"bbport","session_id":"550e8400-e29b-41d4-a716-446655440000","sequence_number":1,"idempotency_key":"bbport:550e8400-e29b-41d4-a716-446655440000:1","timestamp_nanos":100,"event_type":"STAT_INC","key":"kills","int_value":1,"progression_mode":"STANDARD","trust_tier":"LOCAL","evidence_class":"DIRECT_INTEGRATION_EVENT","verification_state":"PENDING"}"#,
            "\n"
        );
        let err = validate_semantic_jsonl_str(bad_events).unwrap_err();
        assert!(err.contains("non-monotonic"), "unexpected err: {}", err);
    }

    #[test]
    fn rejects_frame_gap_without_count() {
        let bad_sparse = r#"{"schema_version":"0.1.0","canonical_game_id":"shared.emu.game","producer_id":"emu_a","session_id":"550e8400-e29b-41d4-a716-446655440002","slot_index":0,"slot_state":"READY","sequence_number":1,"timestamp_nanos":16666667,"lifecycle_signal":"FRAME_GAP","frame_gap_count":0,"payload_size_bytes":16,"progression_mode":"STANDARD","trust_tier":"LOCAL","evidence_class":"RUNTIME_OBSERVED","verification_state":"PENDING"}"#;
        let err = validate_sparse_jsonl_str(bad_sparse).unwrap_err();
        assert!(err.contains("frame_gap_count"), "unexpected err: {}", err);
    }

    fn manifest_with(patch: &str, value: &str) -> String {
        VALID_MANIFEST.replacen(patch, value, 1)
    }

    fn manifest_err(json: &str) -> String {
        validate_manifest(&parse_json(json).expect("test manifest parses"), None).unwrap_err()
    }

    fn observation(trust: &str, evidence: &str, verification: &str) -> String {
        format!(
            r#"{{"schema_version":"0.1.0","observation_id":"550e8400-e29b-41d4-a716-446655440009","canonical_game_id":"shared.steam.game","observer_id":"scanner","source_type":"save_file","observed_at":"2026-11-27T12:00:00Z","record_kind":"ACHIEVEMENT_EVIDENCE","key":"ach_one","progression_mode":"STANDARD","trust_tier":"{}","evidence_class":"{}","verification_state":"{}"}}"#,
            trust, evidence, verification
        )
    }

    #[test]
    fn archive_paths_reject_traversal_and_empty_segments() {
        for ok in ["assets/icon.png", "assets/a/b-c_d.v2.webp", "assets/x.JPG"] {
            assert!(is_safe_archive_path(ok, "assets", &["png", "jpg", "webp"]), "{}", ok);
        }
        for bad in [
            "assets/../secret.png",
            "assets/./icon.png",
            "assets//icon.png",
            "assets/a/../../x.png",
            "assets/.hidden.png",
            "/assets/icon.png",
            "assets/icon.svg",
            "assetsx/icon.png",
        ] {
            assert!(!is_safe_archive_path(bad, "assets", &["png", "jpg", "webp"]), "{}", bad);
        }
        assert!(is_safe_archive_path("rules/eval/main.wasm", "rules", &["wasm"]));
        assert!(!is_safe_archive_path("rules/../main.wasm", "rules", &["wasm"]));
        assert!(is_safe_relative_path("achievements/default.json"));
        assert!(!is_safe_relative_path("../default.json"));
        assert!(!is_safe_relative_path("/etc/passwd"));
        assert!(is_safe_glob("**/progress.sav"));
        for bad in ["../**/*.sav", "/home/*/x", "C:/Saves/*", "saves\\*.sav", "a/../../b"] {
            assert!(!is_safe_glob(bad), "{}", bad);
        }
    }

    #[test]
    fn manifest_rejects_traversal_icon_and_module_paths() {
        let icon = manifest_with(r#""points": 15,"#, r#""points": 15, "icon_path": "assets/../../x.png","#);
        assert!(manifest_err(&icon).contains("icon_path"));
        let wasm = manifest_with(
            r#""title": "Bloodborne (PC Port)","#,
            r#""title": "Bloodborne (PC Port)", "wasm_sandbox": {"module_path": "rules/../x.wasm", "sha256": "aa", "max_memory_bytes": 65536, "max_fuel_per_frame": 1000},"#,
        );
        assert!(manifest_err(&wasm).contains("module_path"));
        let rules = manifest_with(r#""producer_ids": ["bbport_pc"]"#, r#""producer_ids": ["bbport_pc"], "rules_file": "../rules.json""#);
        assert!(manifest_err(&rules).contains("rules_file"));
    }

    #[test]
    fn manifest_enforces_max_items_and_lengths() {
        let ids: Vec<String> = (0..33).map(|i| format!("\"p{}\"", i)).collect();
        let many = manifest_with(r#""producer_ids": ["bbport_pc"]"#, &format!(r#""producer_ids": [{}]"#, ids.join(",")));
        assert!(manifest_err(&many).contains("maxItems"));
        let long = manifest_with(r#""producer_ids": ["bbport_pc"]"#, &format!(r#""producer_ids": ["{}"]"#, "p".repeat(129)));
        assert!(manifest_err(&long).contains("maxLength"));
        let dup = manifest_with(r#""supported_modes": ["STANDARD", "ENCORE"]"#, r#""supported_modes": ["STANDARD", "STANDARD"]"#);
        assert!(manifest_err(&dup).contains("duplicate"));
        let authors: Vec<String> = (0..33).map(|i| format!("\"a{}\"", i)).collect();
        let crowd = manifest_with(r#""title": "Bloodborne (PC Port)","#, &format!(r#""title": "Bloodborne (PC Port)", "authors": [{}],"#, authors.join(",")));
        assert!(manifest_err(&crowd).contains("maxItems"));
    }

    #[test]
    fn plane_c_records_cannot_claim_verified_integration_or_competitive() {
        assert_eq!(validate_observation_json_str(&observation("LOCAL", "LOCALLY_OBSERVED", "UNVERIFIED")).unwrap(), 1);
        assert_eq!(validate_observation_json_str(&observation("LOCAL", "PLATFORM_CONFIRMED", "VERIFIED")).unwrap(), 1);
        for tier in ["VERIFIED_INTEGRATION", "COMPETITIVE"] {
            let err = validate_observation_json_str(&observation(tier, "PLATFORM_CONFIRMED", "UNVERIFIED")).unwrap_err();
            assert!(err.contains("trust_tier"), "{}: {}", tier, err);
        }
        for evidence in ["LOCALLY_OBSERVED", "INFERRED"] {
            let err = validate_observation_json_str(&observation("LOCAL", evidence, "VERIFIED")).unwrap_err();
            assert!(err.contains("PLATFORM_CONFIRMED"), "{}: {}", evidence, err);
        }
    }
}
