import os
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parent.parent
server_dir = Path(__file__).resolve().parent / "x64" / "Release"
environment = {key: value for key, value in os.environ.items() if key.lower() != "path"}
environment["PATH"] = os.environ["PATH"]
with (root / "Saved" / "dead-reckoning-server.out.log").open("w") as stdout, \
        (root / "Saved" / "dead-reckoning-server.err.log").open("w") as stderr:
    process = subprocess.Popen(
        [str(server_dir / "clicknetserver.exe"), "--map", str(root / "Saved" / "Box3DTest.box3d"),
         "--spawn", "0", "0", "2"],
        cwd=server_dir, env=environment, stdout=stdout, stderr=stderr,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
print(process.pid)
