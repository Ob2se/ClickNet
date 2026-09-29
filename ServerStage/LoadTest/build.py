import os
import subprocess
import sys
from pathlib import Path

configuration = sys.argv[1] if len(sys.argv) > 1 else "Release"
if configuration not in {"Debug", "Release"}:
    raise SystemExit("Choose Debug or Release")
project = Path(__file__).with_name("ClickNetLoadTest.vcxproj")
msbuild = Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe")
environment = {key: value for key, value in os.environ.items() if key.lower() != "path"}
environment["PATH"] = os.environ["PATH"]
raise SystemExit(subprocess.call(
    [str(msbuild), str(project), f"/p:Configuration={configuration}", "/p:Platform=x64", "/v:minimal"],
    env=environment,
))
