#!/usr/bin/env python3
"""Flatten `#include "Res/shaders/X.metal"` directives in a Metal shader.

The engine compiles .metal from source at runtime via newLibraryWithSource, and
Metal resolves a relative `#include` against the process CWD. That works when the
app runs from its output dir, but a GPU-trace *replay* tool has a different CWD
and cannot find the included file — the captured program_source keeps the
unresolved `#include`. Flattening the include into the .metal at build time makes
the copied shader self-contained, so both the app and trace replay compile it
without needing the include to resolve.

Only local, double-quoted `#include "Res/shaders/..."` directives are expanded;
`#include <...>` system headers (metal_stdlib) are left for the Metal compiler.
An include is expanded at most once per translation unit (mirrors the shaders'
own `#ifndef` guards and avoids duplicate definitions).
"""
import re
import sys

# Matches:  #include "Res/shaders/<name>"   (optionally with trailing comment)
_INCLUDE_RE = re.compile(r'^\s*#include\s+"Res/shaders/([^"]+)"')


def flatten(path, shader_root, already):
    """Return the flattened text of `path`, expanding local includes once."""
    out = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            m = _INCLUDE_RE.match(line)
            if not m:
                out.append(line)
                continue
            name = m.group(1)
            if name in already:
                out.append(f"// [flattened] #include \"Res/shaders/{name}\" (already expanded)\n")
                continue
            already.add(name)
            out.append(f"// >>> flattened from Res/shaders/{name}\n")
            out.append(flatten(f"{shader_root}/{name}", shader_root, already))
            out.append(f"// <<< end Res/shaders/{name}\n")
    return "".join(out)


def main():
    if len(sys.argv) != 4:
        sys.stderr.write(
            "usage: flatten_metal_includes.py <input.metal> <shader_src_dir> <output.metal>\n")
        return 2
    src, shader_root, dst = sys.argv[1], sys.argv[2], sys.argv[3]
    text = flatten(src, shader_root, set())
    with open(dst, "w", encoding="utf-8") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
