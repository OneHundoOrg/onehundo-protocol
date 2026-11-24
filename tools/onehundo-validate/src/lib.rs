pub mod deflate;
pub mod json;
pub mod validators;
pub mod zip_archive;

#[cfg(test)]
mod tests {
    use super::deflate::crc32;
    use super::validators::{
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
}
