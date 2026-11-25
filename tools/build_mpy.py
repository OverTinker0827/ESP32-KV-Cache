"""Precompile upstream microredis to .mpy bytecode (fair best-case deployment).

Usage: python tools/build_mpy.py
Output: build/microredis_mpy/microredis/**.mpy
"""
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / "ref" / "microredis" / "microredis"
OUT = ROOT / "build" / "microredis_mpy" / "microredis"
MPY_CROSS = pathlib.Path(sys.executable).parent / "mpy-cross.exe"


# Minimal, behaviour-preserving patches so upstream compiles under MicroPython.
# Upstream only runs its tests on CPython; these two files are rejected by the
# MicroPython compiler. None of them touch the GET/SET hot path.
PATCHES = {
    # const(ord('+')) is not foldable by the MicroPython compiler -> use literals
    "core/constants.py": [
        ("const(ord('+'))", "const(43)"),
        ("const(ord('-'))", "const(45)"),
        ("const(ord(':'))", "const(58)"),
        ("const(ord('$'))", "const(36)"),
        ("const(ord('*'))", "const(42)"),
    ],
    # NAME = const(x) in try: and NAME = x in except: -> SyntaxError on MicroPython.
    # Drop const() here (HLL/stream config values, not on the hot path).
    "commands/__init__.py": [("const(", "(")],
    # MicroPython bytearray has no slice deletion; clearing by rebinding is equivalent
    "core/response.py": [("del self._buffer[:]", "self._buffer = bytearray()")],
}
# Runtime dependency missing from stock MicroPython: install with `mpremote mip install bisect`
BUILD_SRC = ROOT / "build" / "microredis_src_patched"


def main():
    if OUT.exists():
        shutil.rmtree(OUT)
    n = 0
    total = 0
    for py in sorted(SRC.rglob("*.py")):
        rel = py.relative_to(SRC)
        if rel.parts[0] == "tests":
            continue  # host-only test runner
        key = str(rel).replace("\\", "/")
        src = py
        if key in PATCHES:
            text = py.read_text(encoding="utf-8")
            for old, new in PATCHES[key]:
                assert old in text, (key, old)
                text = text.replace(old, new)
            src = BUILD_SRC / rel
            src.parent.mkdir(parents=True, exist_ok=True)
            src.write_text(text, encoding="utf-8")
            print(f"patched {key}")
        dst = (OUT / rel).with_suffix(".mpy")
        dst.parent.mkdir(parents=True, exist_ok=True)
        # -s sets the source name stored in tracebacks; -O1 strips asserts/__debug__
        subprocess.run(
            [str(MPY_CROSS), "-O1", "-march=xtensawin", "-s", key,
             "-o", str(dst), str(src)],
            check=True,
        )
        n += 1
        total += dst.stat().st_size
    print(f"compiled {n} modules, {total/1024:.1f} KiB of .mpy -> {OUT}")


if __name__ == "__main__":
    main()
