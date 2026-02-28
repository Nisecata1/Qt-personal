from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ENV_NAME = "calabiyau-hil"

# ===== Launcher Config Area =====
# Choose default target here: "qt" or "pc".
TARGET_MODE = "qt"
TARGET_MAP = {
    "qt": Path("model") / "mouse-proxy-yoloai-qt-Android.py",
    "pc": Path("model") / "mouse-proxy-yoloai-pc.py",
}


def run_with_conda(target: Path, working_dir: Path) -> int | None:
    cmd = ["conda", "run", "-n", ENV_NAME, "python", target.name]
    try:
        return subprocess.run(cmd, cwd=working_dir, check=False).returncode
    except KeyboardInterrupt:
        print("\n[Launcher] Received Ctrl+C, exiting.")
        return 130
    except FileNotFoundError:
        return None


def run_with_current_python(target: Path, working_dir: Path) -> int:
    cmd = [sys.executable, target.name]
    try:
        return subprocess.run(cmd, cwd=working_dir, check=False).returncode
    except KeyboardInterrupt:
        print("\n[Launcher] Received Ctrl+C, exiting.")
        return 130


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="CalabiYau AI launcher")
    parser.add_argument(
        "--target",
        choices=sorted(TARGET_MAP.keys()),
        help="Select predefined target script.",
    )
    parser.add_argument(
        "--script",
        type=str,
        help="Run explicit script path (absolute or relative to project root).",
    )
    return parser.parse_args()


def resolve_target(project_root: Path, args: argparse.Namespace) -> Path:
    if args.script:
        script_path = Path(args.script)
        if not script_path.is_absolute():
            script_path = project_root / script_path
        return script_path.resolve()

    mode = args.target or TARGET_MODE
    if mode not in TARGET_MAP:
        raise ValueError(f"Unsupported target mode: {mode}")
    return (project_root / TARGET_MAP[mode]).resolve()


def main() -> int:
    project_root = Path(__file__).resolve().parent
    args = parse_args()

    try:
        target = resolve_target(project_root, args)
    except ValueError as exc:
        print(f"[Launcher] {exc}")
        return 2

    if not target.exists():
        print(f"[Launcher] Missing file: {target}")
        return 1

    working_dir = target.parent
    current_env = os.environ.get("CONDA_DEFAULT_ENV", "")

    print(f"[Launcher] Working directory: {working_dir}")
    print(f"[Launcher] Target script: {target.name}")

    if current_env == ENV_NAME:
        return run_with_current_python(target, working_dir)

    code = run_with_conda(target, working_dir)
    if code is not None:
        return code

    print("[Launcher] `conda` not found in PATH, fallback to current Python.")
    return run_with_current_python(target, working_dir)


if __name__ == "__main__":
    raise SystemExit(main())
