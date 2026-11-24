use onehundo_validate::validators::{
    validate_observation_json_str, validate_pack_file, validate_semantic_jsonl_str,
    validate_snapshot_zst_file, validate_sparse_jsonl_str,
};
use std::env;
use std::fs;
use std::path::Path;
use std::process;

fn print_usage() {
    eprintln!(
        "onehundo-validate v0.1.0\n\
         Usage:\n\
         \tonehundo-validate pack <file.onehundo>\n\
         \tonehundo-validate semantic <file.oh-events.jsonl>\n\
         \tonehundo-validate sparse <file.oh-sparse.jsonl>\n\
         \tonehundo-validate snapshot <file.oh-snap.zst>\n\
         \tonehundo-validate observation <file.oh-obs.json>\n\
         \tonehundo-validate auto <file>"
    );
}

fn run_subcommand(mode: &str, path: &Path) -> Result<String, String> {
    match mode {
        "pack" => {
            let entries = validate_pack_file(path)?;
            Ok(format!("VALID pack archive ({} entries verified)", entries))
        }
        "semantic" => {
            let text = fs::read_to_string(path)
                .map_err(|e| format!("failed to read '{}': {}", path.display(), e))?;
            let count = validate_semantic_jsonl_str(&text)?;
            Ok(format!("VALID semantic trace ({} events verified)", count))
        }
        "sparse" => {
            let text = fs::read_to_string(path)
                .map_err(|e| format!("failed to read '{}': {}", path.display(), e))?;
            let count = validate_sparse_jsonl_str(&text)?;
            Ok(format!("VALID sparse memory trace ({} frames verified)", count))
        }
        "snapshot" => {
            let count = validate_snapshot_zst_file(path)?;
            Ok(format!("VALID full snapshot archive ({} frames verified)", count))
        }
        "observation" => {
            let text = fs::read_to_string(path)
                .map_err(|e| format!("failed to read '{}': {}", path.display(), e))?;
            let count = validate_observation_json_str(&text)?;
            Ok(format!("VALID observation file ({} records verified)", count))
        }
        "auto" => {
            let s = path.to_string_lossy().to_ascii_lowercase();
            if s.ends_with(".onehundo") || s.ends_with(".zip") {
                run_subcommand("pack", path)
            } else if s.ends_with(".oh-events.jsonl") {
                run_subcommand("semantic", path)
            } else if s.ends_with(".oh-sparse.jsonl") {
                run_subcommand("sparse", path)
            } else if s.ends_with(".oh-snap.zst") {
                run_subcommand("snapshot", path)
            } else if s.ends_with(".oh-obs.json") {
                run_subcommand("observation", path)
            } else {
                Err(format!(
                    "cannot auto-detect artifact type for '{}'",
                    path.display()
                ))
            }
        }
        _ => Err(format!("unknown subcommand '{}'", mode)),
    }
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() == 2 && (args[1] == "--help" || args[1] == "-h") {
        print_usage();
        process::exit(0);
    }
    if args.len() == 2 && (args[1] == "--version" || args[1] == "-V") {
        println!("onehundo-validate 0.1.0");
        process::exit(0);
    }
    if args.len() != 3 {
        print_usage();
        process::exit(2);
    }

    let mode = &args[1];
    let path = Path::new(&args[2]);
    match run_subcommand(mode, path) {
        Ok(msg) => {
            println!("[OK] {}: {}", path.display(), msg);
        }
        Err(err) => {
            eprintln!("[FAIL] {}: {}", path.display(), err);
            process::exit(1);
        }
    }
}
