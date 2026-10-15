from __future__ import annotations

import argparse
import subprocess
import sys
import venv
from pathlib import Path


def main() -> int:
	parser = argparse.ArgumentParser(description="Install pinned PulseForge quality tools into a local virtual environment.")
	parser.add_argument("--venv", type=Path, required=True, help="Build-local virtual environment directory")
	parser.add_argument("--wheelhouse", type=Path, help="Optional local wheel directory for offline installation")
	args = parser.parse_args()

	root = Path(__file__).resolve().parents[2]
	venv_path = args.venv.resolve()
	venv.EnvBuilder(with_pip=True, clear=False).create(venv_path)
	python = venv_path / "Scripts" / "python.exe"
	if not python.is_file():
		print(f"Windows virtual-environment interpreter was not created at {python}", file=sys.stderr)
		return 1

	command = [str(python), "-m", "pip", "install", "--require-hashes"]
	if args.wheelhouse:
		command.extend(["--no-index", "--find-links", str(args.wheelhouse.resolve())])
	command.extend(["-r", str(root / "Tools" / "Quality" / "requirements.txt")])
	result = subprocess.run(command)
	if result.returncode != 0:
		return result.returncode

	print(f"Quality tools installed in {venv_path}; Python {python}")
	print("Installed tool versions: Lizard 1.24.1, Pygments 2.21.0, pathspec 1.1.1")
	return 0


if __name__ == "__main__":
	sys.exit(main())
