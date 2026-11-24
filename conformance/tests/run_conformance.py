#!/usr/bin/env python3
"""
End-to-end conformance runner for onehundo-protocol:
1. Validates all JSON Schema Draft 2020-12 files and OpenAPI 3.1 spec.
2. Verifies all internal markdown links resolve cleanly.
3. Exercises onehundo-validate against all positive fixtures and generated
   negative security/malformed cases (zip-slip, symlinks, zip-bombs, bad schemas).
"""

import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
import yaml

ROOT = Path(__file__).resolve().parents[2]
VALIDATOR_BIN = ROOT / "tools" / "onehundo-validate" / "target" / "debug" / "onehundo-validate"


def check_json_schemas() -> None:
    schema_files = sorted((ROOT / "schemas").rglob("*.schema.json"))
    assert len(schema_files) >= 6, "Expected at least 6 schema files"
    for sf in schema_files:
        data = json.loads(sf.read_text(encoding="utf-8"))
        assert data.get("$schema") == "https://json-schema.org/draft/2020-12/schema", (
            f"{sf}: must declare Draft 2020-12 $schema"
        )
        assert "$id" in data and data["$id"].startswith("https://schemas.onehundo.org/"), (
            f"{sf}: missing canonical $id"
        )
        # Verify internal #/$defs/... refs exist
        defs = data.get("$defs", {})
        raw = json.dumps(data)
        for ref in re.findall(r'"#\/\$defs\/([^"]+)"', raw):
            assert ref in defs, f"{sf}: broken internal $ref #/$defs/{ref}"
    print(f"[OK] Verified {len(schema_files)} JSON Schema Draft 2020-12 files")


def check_openapi_spec() -> None:
    spec_path = ROOT / "schemas" / "sync" / "openapi.yaml"
    spec = yaml.safe_load(spec_path.read_text(encoding="utf-8"))
    assert spec.get("openapi") == "3.1.0", "OpenAPI version must be 3.1.0"
    paths = spec.get("paths", {})
    for required_path in [
        "/v1/devices/register",
        "/v1/packs/resolve",
        "/v1/sync/events",
        "/v1/progression/{canonical_game_id}",
    ]:
        assert required_path in paths, f"Missing OpenAPI endpoint {required_path}"

    schemas = spec.get("components", {}).get("schemas", {})
    for req_schema in [
        "ProgressionMode",
        "TrustTier",
        "EvidenceClass",
        "VerificationState",
        "ReceiptStatus",
        "AcceptanceStatus",
    ]:
        assert req_schema in schemas, f"Missing OpenAPI component schema {req_schema}"

    assert schemas["ReceiptStatus"]["enum"] == ["RECEIVED", "DUPLICATE"]
    assert schemas["AcceptanceStatus"]["enum"] == ["ACCEPTED", "PENDING_REVIEW", "REJECTED"]

    # Check component refs resolve
    raw = json.dumps(spec)
    for ref in re.findall(r'"#\/components\/schemas\/([^"]+)"', raw):
        assert ref in schemas, f"Broken OpenAPI $ref #/components/schemas/{ref}"
    print("[OK] Verified OpenAPI 3.1.0 specification and Receipt vs. Acceptance separation")


def check_markdown_links() -> None:
    md_files = [
        p for p in ROOT.rglob("*.md")
        if ".git" not in p.parts and "target" not in p.parts and "skills" not in p.parts
    ]
    link_re = re.compile(r"\[[^\]]+\]\(([^)#]+)(?:#[^)]+)?\)")
    for mf in md_files:
        text = mf.read_text(encoding="utf-8")
        # Ignore fenced code blocks
        cleaned = re.sub(r"```.*?```", "", text, flags=re.DOTALL)
        for target in link_re.findall(cleaned):
            if target.startswith(("http://", "https://", "mailto:")):
                continue
            resolved = (mf.parent / target).resolve()
            assert resolved.exists(), f"{mf}: broken markdown link -> {target}"
    print(f"[OK] Verified markdown links across {len(md_files)} files")


def run_validator(mode: str, path: Path, expect_ok: bool) -> None:
    res = subprocess.run(
        [str(VALIDATOR_BIN), mode, str(path)],
        capture_output=True,
        text=True,
        check=False,
    )
    if expect_ok and res.returncode != 0:
        raise AssertionError(
            f"Expected validator OK on {path}, got exit {res.returncode}:\n{res.stderr}"
        )
    if not expect_ok and res.returncode == 0:
        raise AssertionError(
            f"Expected validator REJECT on {path}, but it succeeded:\n{res.stdout}"
        )


def check_fixtures_and_security() -> None:
    if not VALIDATOR_BIN.exists():
        subprocess.run(
            ["cargo", "build", "--manifest-path", str(ROOT / "tools" / "onehundo-validate" / "Cargo.toml")],
            check=True,
        )

    positive = [
        ROOT / "conformance" / "fixtures" / "packs" / "bbport_sample.onehundo",
        ROOT / "conformance" / "fixtures" / "sparse" / "shared_game_emulated.oh-sparse.jsonl",
        ROOT / "conformance" / "fixtures" / "full-snapshot" / "shared_game_recomp.oh-snap.zst",
        ROOT / "conformance" / "fixtures" / "semantic" / "bbport_sample.oh-events.jsonl",
        ROOT / "conformance" / "fixtures" / "observation" / "steam_local_save.oh-obs.json",
    ]
    for p in positive:
        run_validator("auto", p, expect_ok=True)
    print(f"[OK] Verified {len(positive)} positive conformance fixtures")

    valid_manifest = {
        "manifest_version": "0.1.0",
        "pack_id": "test.negative.pack",
        "canonical_game_id": "test.game",
        "title": "Negative Security Test Pack",
        "matchers": [{"matcher_type": "build_id", "value": "test-1"}],
        "profiles": {"semantic": {"enabled": True, "producer_ids": ["test_prod"]}},
        "achievements": [{
            "canonical_achievement_id": "ach_one",
            "title": "One",
            "description": "Test",
            "points": 10,
            "supported_modes": ["STANDARD"],
            "minimum_trust_tier": "LOCAL",
        }],
    }

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)

        # 1. Zip-slip traversal
        p_slip = tmp / "zip_slip.onehundo"
        with zipfile.ZipFile(p_slip, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("manifest.json", json.dumps(valid_manifest))
            zf.writestr("../../etc/cron.d/pwn.json", "{}")
        run_validator("pack", p_slip, expect_ok=False)

        # 2. Symlink entry
        p_sym = tmp / "symlink.onehundo"
        with zipfile.ZipFile(p_sym, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("manifest.json", json.dumps(valid_manifest))
            zi = zipfile.ZipInfo("assets/evil.json")
            zi.create_system = 3
            zi.external_attr = (0o120777 << 16)
            zf.writestr(zi, "/etc/passwd")
        run_validator("pack", p_sym, expect_ok=False)

        # 3. Compression bomb (> 100:1 ratio)
        p_bomb = tmp / "zip_bomb.onehundo"
        with zipfile.ZipFile(p_bomb, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("manifest.json", json.dumps(valid_manifest))
            # 2 MiB of zeros compresses > 1000:1
            zf.writestr("achievements/bomb.json", b" " * (2 * 1024 * 1024))
        run_validator("pack", p_bomb, expect_ok=False)

        # 4. Disallowed executable file
        p_exe = tmp / "exec.onehundo"
        with zipfile.ZipFile(p_exe, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("manifest.json", json.dumps(valid_manifest))
            zf.writestr("run.exe", b"MZ\x90\x00")
        run_validator("pack", p_exe, expect_ok=False)

        # 5. Missing manifest
        p_nomanifest = tmp / "no_manifest.onehundo"
        with zipfile.ZipFile(p_nomanifest, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("achievements/default.json", "{}")
        run_validator("pack", p_nomanifest, expect_ok=False)

        # 6. Plane C claiming DIRECT_INTEGRATION_EVENT
        p_bad_obs = tmp / "bad.oh-obs.json"
        p_bad_obs.write_text(json.dumps({
            "schema_version": "0.1.0",
            "observation_id": "22222222-2222-4222-8222-222222222229",
            "canonical_game_id": "test.game",
            "observer_id": "bad_obs",
            "source_type": "save_file",
            "observed_at": "2026-11-24T12:00:00Z",
            "record_kind": "ACHIEVEMENT_EVIDENCE",
            "key": "ach_one",
            "progression_mode": "STANDARD",
            "trust_tier": "LOCAL",
            "evidence_class": "DIRECT_INTEGRATION_EVENT",
            "verification_state": "UNVERIFIED",
        }), encoding="utf-8")
        run_validator("observation", p_bad_obs, expect_ok=False)

    print("[OK] Verified 6 negative security & malformed conformance cases rejected")


def main() -> int:
    check_json_schemas()
    check_openapi_spec()
    check_markdown_links()
    check_fixtures_and_security()
    print("ALL CONFORMANCE CHECKS PASSED.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
