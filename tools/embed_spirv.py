#!/usr/bin/env python3
"""Embed two offline-compiled SPIR-V shaders in a deterministic C++ header."""

import argparse
from pathlib import Path


def format_array(name: str, data: bytes) -> str:
    rows = []
    for offset in range(0, len(data), 12):
        values = ", ".join(f"0x{value:02x}" for value in data[offset:offset + 12])
        rows.append(f"    {values},")
    body = "\n".join(rows)
    return f"inline constexpr unsigned char {name}[] = {{\n{body}\n}};\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--vertex", type=Path, required=True)
    parser.add_argument("--fragment", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()

    output = "#pragma once\n\nnamespace RHI_SDL_GPU_Proof_Shaders {\n\n"
    output += format_array("Vertex", arguments.vertex.read_bytes())
    output += "\n"
    output += format_array("Fragment", arguments.fragment.read_bytes())
    output += "\n} // namespace RHI_SDL_GPU_Proof_Shaders\n"

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(output, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
