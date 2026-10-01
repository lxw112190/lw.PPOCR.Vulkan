"""Generate a C++ header from compiled SPIR-V; runtime needs no shader compiler."""
from pathlib import Path
import argparse
import struct

p = argparse.ArgumentParser()
p.add_argument("output", type=Path)
p.add_argument("inputs", nargs="+", type=Path)
a = p.parse_args()
lines = ["// Generated. Do not edit.", "#pragma once", "#include <cstdint>",
         "#include <string>", "#include <stdexcept>", "namespace lwvk::spirv {",
         "struct Code { const uint32_t* data; size_t bytes; };"]
for f in a.inputs:
    raw = f.read_bytes()
    words = struct.unpack(f"<{len(raw)//4}I", raw)
    name = f.stem
    lines.append(f"inline constexpr uint32_t {name}[] = {{")
    for i in range(0, len(words), 12):
        lines.append(",".join(f"0x{x:08x}u" for x in words[i:i+12]) + ",")
    lines.append("};")
lines.append("inline Code get(const std::string& name) {")
for f in a.inputs:
    lines.append(f'if(name == "{f.stem}") return {{{f.stem},sizeof({f.stem})}};')
lines.extend(['throw std::runtime_error("unknown embedded shader: " + name);', "}", "}"])
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text("\n".join(lines) + "\n", encoding="utf-8")
