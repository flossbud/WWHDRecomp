"""Shared PyGhidra setup: install dir, heap size, and the project location."""
import importlib.util
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROJECT_DIR = ROOT / "ghidra" / "projects"
PROJECT_NAME = "wwhd"
PROGRAM = "/cking.rpx"


def start():
    """Start the JVM headless. GHIDRA_INSTALL_DIR / GHIDRA_MAXMEM as in headless.sh."""
    os.environ.setdefault("GHIDRA_INSTALL_DIR", str(Path.home() / "opt" / "ghidra_12.0.4_PUBLIC"))
    from pyghidra.launcher import HeadlessPyGhidraLauncher
    launcher = HeadlessPyGhidraLauncher()
    launcher.add_vmargs(f"-Xmx{os.environ.get('GHIDRA_MAXMEM', '6G')}")
    launcher.start()


def open_project(path=PROJECT_DIR):
    import pyghidra
    return pyghidra.open_project(str(path), PROJECT_NAME)


def load_tool(name):
    """Import tools/<name>.py by path. Never put tools/ on sys.path: tools/ghidra/
    would then shadow Ghidra's own `ghidra` Java package."""
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module
