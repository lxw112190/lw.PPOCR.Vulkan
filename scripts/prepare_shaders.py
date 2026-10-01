"""Derive portable FP32 shaders; preserve separately gated mixed-precision experiments."""
from pathlib import Path
import argparse

# Same FP32 polynomial and operation order as the unfused Div/Erf/Add/Mul/Mul.
GELU = """
float lwvk_gelu(float v) {
    precise float a=v/1.4142135381698608;
    float s=a<0.0?-1.0:1.0;
    a=abs(a);
    float t=1.0/(1.0+0.3275911*a);
    float q=t*(0.254829592+t*(-0.284496736+t*(1.421413741+t*(-1.453152027+t*1.061405429))));
    precise float e=s*(1.0-q*exp(-a*a));
    precise float m=(e+1.0)*v;
    return m*0.5;
}
vec4 lwvk_gelu(vec4 v) { return vec4(lwvk_gelu(v.x),lwvk_gelu(v.y),lwvk_gelu(v.z),lwvk_gelu(v.w)); }
"""


def prepare(source: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    paths = list(source.glob("*.comp")) + list((Path(__file__).resolve().parents[1]/"src/shaders").glob("conv_*.comp"))
    for path in sorted(paths):
        text = path.read_text(encoding="utf-8")
        if path.stem == "conv_coop_gemm":
            # Explicitly mixed precision, never run through the FP32 conversion.
            # Activation and global buffers stay FP32, including a fused GELU.
            text=text.replace("void main()",GELU+"\nvoid main()",1)
            (destination / path.name).write_text(text, encoding="utf-8", newline="\n")
            continue
        text = "\n".join(line for line in text.splitlines()
                         if not line.startswith("#extension GL_EXT_shader_16bit_storage")
                         and not line.startswith("#extension GL_EXT_shader_explicit_arithmetic_types_float16")) + "\n"
        text = text.replace("float16_t", "float").replace("f16vec4", "vec4")
        text = text.replace("fp16", "FP32 (modified)")
        if path.stem == "elem":
            text = text.replace("uint bMode; uint aux;", "uint bMode; uint aux; uint beta;")
            text = text.replace("vec2 ab = unpackHalf2x16(p.aux);", "vec2 ab = vec2(uintBitsToFloat(p.aux), uintBitsToFloat(p.beta));")
            text = text.replace("p.op == 14u", "p.op == 14u || p.op == 15u")
            text = text.replace("case 4u: r = 1.0 / (1.0 + exp(-va)); break;",
                                "case 4u: r = 1.0 / (1.0 + exp(-va)); break;\n        case 15u: r = va * (1.0 / (1.0 + exp(-va))); break; // fused SiLU")
        if path.stem == "maxpool4":
            text = text.replace("-1.0e4", "-3.402823466e38")
        # Replace upstream's low-order Erf approximation for the FP32 baseline.
        begin = text.find("float erf1(float v)")
        if begin >= 0:
            end = text.index("\n}", begin) + 2
            text = text[:begin] + """float erf1(float v)
{
    float s = v < 0.0 ? -1.0 : 1.0;
    v = abs(v);
    float t = 1.0 / (1.0 + 0.3275911 * v);
    float q = t * (0.254829592 + t * (-0.284496736 +
              t * (1.421413741 + t * (-1.453152027 + t * 1.061405429))));
    return s * (1.0 - q * exp(-v * v));
}""" + text[end:]
        attribution = """#version 450
// Derived from sdcb/SimdPaddleOCR, commit 6298596e28404f0e93af585a79e84086363eeb72.
// Apache-2.0. Modified by lw.PPOCR.Vulkan in 2026: portable FP32 baseline,
// FP32 activation parameters, improved Erf precision, FP32 MaxPool bound.
// Original unmodified source and attribution are shipped with the project."""
        if path.stem in ('conv_gemm', 'conv_gemm_tiled', 'conv_pointwise_tiled', 'conv_pointwise_tiled64', 'conv_pointwise_vector', 'conv_pointwise_smallm', 'conv_dw4', 'gelu', 'softmax_parallel', 'softmax_argmax'):
            attribution = "#version 450\n// Project-written portable FP32 kernel. Apache-2.0."
        text = text.replace("#version 450", attribution, 1)
        if path.stem in ('gelu','conv_dense','conv_gemm','conv_gemm_tiled','conv_pointwise','conv_pointwise_tiled','conv_pointwise_tiled64','conv_pointwise_vector','conv_pointwise_smallm'):
            text=text.replace("void main()",GELU+"\nvoid main()",1)
            text=text.replace("if((p.flags&16u)!=0)v=max(v,vec4(0));",
                              "if((p.flags&16u)!=0)v=max(v,vec4(0));if((p.flags&32u)!=0)v=lwvk_gelu(v);")
            text=text.replace("if((p.flags&16u)!=0)acc=max(acc,0.0);",
                              "if((p.flags&16u)!=0)acc=max(acc,0.0);if((p.flags&32u)!=0)acc=lwvk_gelu(acc);")
            if path.stem=='conv_dense':
                text=text.replace("o[gid] = actf(acc, (p.flags >> 4) & 7u, p.flags);",
                                  "o[gid] = (p.flags&32u)!=0 ? lwvk_gelu(acc) : actf(acc, (p.flags >> 4) & 7u, p.flags);")
        if path.stem in ('conv_dense','conv_gemm','conv_gemm_tiled','conv_pointwise','conv_pointwise_tiled','conv_pointwise_tiled64','conv_pointwise_vector','conv_pointwise_smallm'):
            text=text.replace('void main()', '''vec4 lwvk_silu(vec4 v) { return v * (vec4(1.0) / (vec4(1.0) + exp(-v))); }
float lwvk_silu(float v) { return v * (1.0 / (1.0 + exp(-v))); }
void main()''', 1)
            if path.stem=='conv_dense':
                text=text.replace('o[gid] = (p.flags&32u)!=0 ?', 'o[gid] = (p.flags&128u)!=0 ? lwvk_silu(acc) : (p.flags&32u)!=0 ?')
            elif path.stem=='conv_gemm':
                text=text.replace('o[m*p.Cout+n]=acc;', 'if((p.flags&128u)!=0)acc=lwvk_silu(acc);o[m*p.Cout+n]=acc;')
            elif path.stem in ('conv_pointwise_tiled64','conv_pointwise_vector'):
                marker=' if(m<p.M)o[m*N4+n4]=a;' if path.stem=='conv_pointwise_tiled64' else '    if (m < p.M) o[m * N4 + n4] = a;'
                text=text.replace(marker, 'if((p.flags&128u)!=0){a=lwvk_silu(a);c=lwvk_silu(c);d=lwvk_silu(d);e=lwvk_silu(e);}\n'+marker)
            else:
                text=text.replace('if((p.flags&32u)!=0)v=lwvk_gelu(v);', 'if((p.flags&32u)!=0)v=lwvk_gelu(v);if((p.flags&128u)!=0)v=lwvk_silu(v);')
        (destination / path.name).write_text(text, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    prepare(args.source, args.destination)
