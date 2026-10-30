"""Build the Camera Mods plugin DLL (CameraMods.dll) with Zig.
Usage: python build_plugin.py /path/to/zig.exe [--output PATH]
The plugin is loaded by Extra Options (mods/<name>/plugins/CameraMods.dll); it does not replace winmm.dll.
"""
import argparse
from argparse import Namespace
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import time

import build  # reuses the compiler flags and the cached build helpers

SOURCE = build.SOURCE
PLUGIN_SOURCES = [
    "plugin/plugin_main.c",
    "features/drone_probe.c",
    "features/overlay_window.c",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("zig", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--rebuild", action="store_true")
    options = parser.parse_args()
    compiler = options.zig.resolve()
    output = (options.output or SOURCE.parent / "CameraMods.dll").resolve()
    directory = (SOURCE.parent / ".build" / "plugin").resolve()
    directory.mkdir(parents=True, exist_ok=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()

    arguments = build.compiler_arguments(compiler, Namespace(test=False, azimuth=False))
    first_include = arguments.index("-I")
    # Plugin headers shadow the host's: found before the source root.
    arguments[first_include:first_include] = ["-I", str(SOURCE / "plugin" / "shim")]

    def compile_source(filename):
        object_file = directory / (Path(filename).stem + ".obj")
        command = [*arguments, str(SOURCE / filename), "-o", str(object_file)]
        compiled = build.cached_command(command, build.project_dependencies(SOURCE / filename),
                                        object_file, compiler, options.rebuild)
        return str(object_file), compiled

    with ThreadPoolExecutor(max_workers=options.jobs) as workers:
        results = list(workers.map(compile_source, PLUGIN_SOURCES))
    objects = [path for path, _ in results]

    libraries = []
    for name in ("kernel32", "ntdll"):
        library = directory / (name + ".lib")
        build.cached_command([str(compiler), "dlltool", "-m", "i386:x86-64", "-d",
                              str(SOURCE / (name + ".def")), "-l", str(library)],
                             [SOURCE / (name + ".def")], library, compiler, options.rebuild)
        libraries.append(str(library))

    subprocess.run([str(compiler), "lld-link", "/dll", "/entry:DllMain", "/machine:x64",
                    "/nodefaultlib", "/dynamicbase", "/nxcompat", "/Brepro",
                    "/out:" + str(output), "/implib:" + str(directory / "CameraMods.lib"),
                    *objects, *libraries], check=True)
    print(f"Compiled {sum(c for _, c in results)}/{len(PLUGIN_SOURCES)} objects; "
          f"{time.monotonic() - started:.1f}s", flush=True)
    print(output)


if __name__ == "__main__":
    main()
