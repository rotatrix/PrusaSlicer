"""Choose a bounded CI worker count; leave local toolchain defaults untouched."""
import ctypes
import os
import platform
import subprocess
from pathlib import Path


def memory_bytes():
    if platform.system() == "Windows":
        class MemoryStatus(ctypes.Structure):
            _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong)] + [
                (name, ctypes.c_ulonglong) for name in
                ("total", "available", "page_total", "page_available",
                 "virtual_total", "virtual_available", "extended")
            ]
        status = MemoryStatus()
        status.length = ctypes.sizeof(status)
        if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            raise OSError("Cannot determine runner memory")
        return status.total
    if platform.system() == "Darwin":
        return int(subprocess.check_output(["sysctl", "-n", "hw.memsize"]))
    return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")


if __name__ == "__main__":
    cpus = os.cpu_count() or 1
    gib = memory_bytes() / 1024**3
    # Leave 2 GiB for the OS/linker, budget 2.5 GiB per heavy C++ compilation.
    jobs = max(1, min(cpus, int((gib - 2) / 2.5), 8))
    print(f"Build budget: {cpus} CPUs, {gib:.1f} GiB RAM, {jobs} workers")
    if "GITHUB_ENV" in os.environ:
        with Path(os.environ["GITHUB_ENV"]).open("a", encoding="utf-8") as output:
            output.write(f"BUILD_JOBS={jobs}\n")
            # Runner-provided process variables are not automatically env context.
            image = os.environ.get("ImageVersion", "unknown")
            output.write(f"RUNNER_IMAGE_VERSION={image}\n")
