"""Build the pinned clauf revision in Linux or Ubuntu on WSL."""
from pathlib import Path
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
REVISION = "25b91226c1e8193c4498149cbedf69c2068458da"


def main():
    if os.name == "nt":
        linux_script = subprocess.check_output(
            ["wsl", "-d", "Ubuntu", "--exec", "wslpath", "-a", Path(__file__).resolve().as_posix()],
            text=True,
        ).strip()
        subprocess.run(["wsl", "-d", "Ubuntu", "--exec", "python3", linux_script], check=True)
        return

    source = ROOT / "vendor" / "clauf"
    if not source.exists():
        source.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", "https://github.com/foonathan/clauf.git", str(source)], check=True)
        subprocess.run(["git", "-C", str(source), "checkout", "--detach", REVISION], check=True)
    actual = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if actual != REVISION:
        sys.exit("Unexpected clauf revision; refusing to replace an existing checkout.")

    build = ROOT / ".build"
    subprocess.run([
        "cmake", "-S", str(source), "-B", str(build), "-G", "Ninja", "-Wno-dev",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
        # The archived source relies on a transitive cstdarg include. Its unused
        # QBE backend also triggers newer Clang format diagnostics under -Werror.
        "-DCMAKE_CXX_FLAGS=-include cstdarg -Wno-error=format",
    ], check=True)
    subprocess.run(["cmake", "--build", str(build), "--target", "clauf", "-j", "4"], check=True)
    subprocess.run(["ctest", "--test-dir", str(build), "--output-on-failure"], check=True)


if __name__ == "__main__":
    main()
