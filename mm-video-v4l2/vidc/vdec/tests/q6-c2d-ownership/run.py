#!/usr/bin/env python3
"""Compile the production allocator and conversion ownership bodies on the host."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def block(text: str, start: int) -> str:
    opening = text.index("{", start)
    depth = 1
    closing = opening + 1
    while depth:
        depth += (text[closing] == "{") - (text[closing] == "}")
        closing += 1
    return text[start:closing]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--source-root", type=Path)
    arguments = parser.parse_args()
    test_root = Path(__file__).resolve().parent
    decoder = arguments.source_root or test_root.parents[1]
    source_path = decoder / "src/omx_vdec_msm8974.cpp"
    header_path = decoder / "inc/omx_vdec.h"
    source = source_path.read_text()
    header = header_path.read_text()
    nested_class = (
        block(header, header.index("class allocate_color_convert_buf\n")) + ";"
    )
    allocator_decl = header[header.index("        int alloc_map_ion_memory(") :]
    allocator_decl = allocator_decl[: allocator_decl.index(";") + 1]
    enum_match = re.search(r"enum ion_buffer_consumer\s*\{[^}]+\};", header)
    enum_decl = enum_match.group() if enum_match else ""
    methods = [
        "int omx_vdec::alloc_map_ion_memory(",
        "void omx_vdec::free_ion_memory(",
        "OMX_ERRORTYPE omx_vdec::free_output_buffer(",
        "OMX_ERRORTYPE omx_vdec::prepare_output_buffer(",
        "bool omx_vdec::shutdown_driver()",
        "bool omx_vdec::release_output_done(",
        "omx_vdec::allocate_color_convert_buf::allocate_color_convert_buf()",
        "omx_vdec::allocate_color_convert_buf::~allocate_color_convert_buf()",
        "void omx_vdec::allocate_color_convert_buf::init_members()",
        "bool omx_vdec::allocate_color_convert_buf::update_buffer_req()",
        "OMX_BUFFERHEADERTYPE* omx_vdec::allocate_color_convert_buf::get_il_buf_hdr()",
        "OMX_BUFFERHEADERTYPE* omx_vdec::allocate_color_convert_buf::get_dr_buf_hdr",
        "OMX_ERRORTYPE omx_vdec::allocate_color_convert_buf::free_output_buffer(",
        "OMX_ERRORTYPE omx_vdec::allocate_color_convert_buf::allocate_buffers_color_convert(",
    ]
    rollback = (
        "OMX_ERRORTYPE omx_vdec::allocate_color_convert_buf::rollback_driver_buffer("
    )
    if rollback in source:
        methods.append(rollback)
        methods.append(
            "OMX_ERRORTYPE omx_vdec::allocate_color_convert_buf::finish_driver_release()"
        )
    definitions = "\n\n".join(block(source, source.index(method)) for method in methods)
    deinit_start = source.index(
        "if (!shutdown_driver())", source.index("omx_vdec::component_deinit(")
    )
    deinit_end = source.index(
        "/*Check if the input buffers have to be cleaned up*/", deinit_start
    )
    deinit = source[deinit_start:deinit_end]
    free_start = source.index(
        "nPortIndex = buffer - client_buffers.get_il_buf_hdr();",
        source.index("omx_vdec::free_buffer("),
    )
    free_branch = block(
        source, source.index("if (nPortIndex < drv_ctx.op_buf.actualcount", free_start)
    )
    fixture = (test_root / "ownership-test.cpp").read_text()
    fixture = fixture.replace(
        "/* ALLOCATOR_DECLARATION */", enum_decl + "\n" + allocator_decl
    )
    fixture = fixture.replace(
        "/* CONVERSION_CLASS */", nested_class.replace("private:", "public:")
    )
    fixture = fixture.replace("/* PRODUCTION_FUNCTIONS */", definitions)
    fixture = fixture.replace("/* DEINIT_OUTPUT_BLOCK */", deinit)
    fixture = fixture.replace("/* NORMAL_FREE_OUTPUT_BLOCK */", free_branch)
    arguments.output.mkdir(parents=True, exist_ok=True)
    generated = arguments.output / "production-ownership-test.cpp"
    generated.write_text(fixture)
    executable = arguments.output / "ownership-test"
    command = [
        arguments.cxx,
        "-std=c++11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        "-g",
        "-pthread",
        str(generated),
        "-o",
        str(executable),
    ]
    receipt = {
        "command": command,
        "compiler": subprocess.check_output(
            [arguments.cxx, "--version"], text=True
        ).splitlines()[0],
        "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
        "header_sha256": hashlib.sha256(header.encode()).hexdigest(),
        "fixture_sha256": hashlib.sha256(fixture.encode()).hexdigest(),
    }
    (arguments.output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    subprocess.run(command, check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
