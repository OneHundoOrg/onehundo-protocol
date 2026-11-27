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

    # Contract bounds match the platform's enforcement, and abuse responses are documented.
    env = schemas["SyncEventEnvelope"]["properties"]
    for field, limit in [("canonical_game_id", 128), ("producer_id", 128), ("key", 128), ("string_value", 512), ("idempotency_key", 320)]:
        assert env[field].get("maxLength") == limit, f"SyncEventEnvelope.{field} maxLength must be {limit}"
    result = schemas["SyncEventResult"]["properties"]
    assert result["idempotency_key"].get("maxLength") == env["idempotency_key"]["maxLength"], "response idempotency_key echoes the request's bounds"
    assert schemas["ErrorResponse"]["properties"]["message"].get("maxLength") == 512
    assert schemas["ErrorResponse"]["properties"]["code"].get("maxLength") == 64
    responses = spec.get("components", {}).get("responses", {})
    assert "Retry-After" in responses["TooManyRequests"]["headers"], "429 must document Retry-After"
    for path, codes in [("/v1/sync/events", ("413", "429")), ("/v1/devices/register", ("413", "429"))]:
        got = paths[path]["post"]["responses"]
        for code in codes:
            assert code in got, f"{path} must document {code}"
    for ref in re.findall(r'"#\/components\/responses\/([^"]+)"', raw):
        assert ref in responses, f"Broken OpenAPI $ref #/components/responses/{ref}"
    print("[OK] Verified OpenAPI size bounds and 413 / 429 (Retry-After) responses")


def check_schema_bounds() -> None:
    """Exercises the manifest and observation schemas' own patterns and bounds (no jsonschema dependency)."""
    manifest = json.loads((ROOT / "schemas" / "pack" / "manifest.schema.json").read_text(encoding="utf-8"))
    defs, props = manifest["$defs"], manifest["properties"]
    ach = defs["achievement_definition"]["properties"]
    icon = re.compile(ach["icon_path"]["pattern"])
    module = re.compile(defs["wasm_sandbox_metadata"]["properties"]["module_path"]["pattern"])
    for ok in ["assets/icon.png", "assets/a/b-c_d.v2.webp"]:
        assert icon.search(ok), f"icon_path should accept {ok}"
    for bad in ["assets/../x.png", "assets/./x.png", "assets//x.png", "assets/a/../../x.png", "assets/.x.png"]:
        assert not icon.search(bad), f"icon_path must reject {bad}"
    assert module.search("rules/eval/main.wasm") and not module.search("rules/../main.wasm")
    rules = defs["semantic_profile"]["properties"]["rules_file"]
    rules_ok, rules_not = re.compile(rules["pattern"]), re.compile(rules["not"]["pattern"])
    for ok in ["achievements/default.json", "rules/memory.json"]:
        assert rules_ok.search(ok) and not rules_not.search(ok), f"rules_file should accept {ok}"
    for bad in ["../x.json", "a/../x.json", "./x.json", "/etc/x.json"]:
        assert not rules_ok.search(bad) or rules_not.search(bad), f"rules_file must reject {bad}"
    glob_not = re.compile(defs["observation_profile"]["properties"]["sources"]["items"]["properties"]["file_glob"]["not"]["pattern"])
    assert not glob_not.search("**/progress.sav")
    for bad in ["../*.sav", "/home/*/x", "C:/Saves/*", "a\\b", "x/../y"]:
        assert glob_not.search(bad), f"file_glob must reject {bad}"
    bounds = {
        "authors": props["authors"], "matchers": props["matchers"], "achievements": props["achievements"],
        "regions": defs["memory_profile"]["properties"]["regions"],
        "producer_ids": defs["semantic_profile"]["properties"]["producer_ids"],
        "declared_stats": defs["semantic_profile"]["properties"]["declared_stats"],
        "sources": defs["observation_profile"]["properties"]["sources"],
        "supported_modes": ach["supported_modes"],
    }
    for name, node in bounds.items():
        assert isinstance(node.get("maxItems"), int), f"manifest {name} must declare maxItems"

    obs = json.loads((ROOT / "schemas" / "external-observation" / "observation-record.schema.json").read_text(encoding="utf-8"))
    assert obs["properties"]["trust_tier"]["enum"] == ["LOCAL"], "Plane C records must be LOCAL only"
    cond = obs["allOf"][0]
    restricted = set(cond["if"]["properties"]["evidence_class"]["enum"])
    for rec in json.loads((ROOT / "conformance" / "fixtures" / "observation" / "steam_local_save.oh-obs.json").read_text(encoding="utf-8")):
        assert rec["trust_tier"] in obs["properties"]["trust_tier"]["enum"], "fixture trust_tier must satisfy the schema"
        if rec["evidence_class"] in restricted:
            assert rec["verification_state"] != "VERIFIED", "fixture must satisfy the VERIFIED conditional"
    print("[OK] Verified manifest path patterns, maxItems bounds and Plane C LOCAL-only trust tier")


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
    check_schema_bounds()
    check_markdown_links()
    check_fixtures_and_security()
    print("ALL CONFORMANCE CHECKS PASSED.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
