# PlatformIO extra script for the PC twin (env:twin):
#  - finds a MinGW-w64 GCC even when it isn't on PATH yet (a freshly installed compiler often
#    isn't visible to an already running VS Code / Explorer until you sign out and back in)
#  - adds "Run PC twin" buttons to the PlatformIO sidebar (Project Tasks > twin > Custom).
#    Same as: pio run -e twin -t run_twin   /   pio run -e twin -t run_twin_local
import glob
import os
import shutil

Import("env")  # noqa: F821 (provided by PlatformIO)


def find_gcc_dir():
    if shutil.which("g++", path=env["ENV"].get("PATH")):  # noqa: F821
        return None  # already on PATH
    local = os.environ.get("LOCALAPPDATA", "")
    candidates = glob.glob(os.path.join(local, r"Microsoft\WinGet\Packages\BrechtSanders.WinLibs*\mingw64\bin"))
    candidates += [r"C:\msys64\ucrt64\bin", r"C:\msys64\mingw64\bin", r"C:\mingw64\bin", r"C:\MinGW\bin"]
    for d in candidates:
        if os.path.isfile(os.path.join(d, "g++.exe")):
            return d
    print(
        "\nerror: no g++ found. Install one with\n"
        "    winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT\n"
        "or put your MinGW-w64 bin folder on PATH. See docs/pc-twin.md.\n"
    )
    env.Exit(1)  # noqa: F821


gcc_dir = find_gcc_dir()
if gcc_dir:
    print(f"Using g++ from {gcc_dir} (not on PATH)")
    env.PrependENVPath("PATH", gcc_dir)  # noqa: F821

program = "$BUILD_DIR/${PROGNAME}${PROGSUFFIX}"

env.AddCustomTarget(  # noqa: F821
    name="run_twin",
    dependencies=program,
    actions=f'"{program}"',
    title="Run PC twin",
    description="Build and start the twin; like the ESP32, VRChat finds it via OSCQuery",
)

env.AddCustomTarget(  # noqa: F821
    name="run_twin_local",
    dependencies=program,
    actions=f'"{program}" --local',
    title="Run PC twin (local only)",
    description="Loopback only, no discovery: VRChat needs --osc=9000:127.0.0.1:9101",
)
