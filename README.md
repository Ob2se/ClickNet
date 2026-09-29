# ClickNet

This repository contains the Unreal project (`ClickNet.uproject`), the C++ server (`ServerStage/clicknetserver.sln`), and the load generator (`ServerStage/LoadTest`). All three use the one wire definition at `Plugins/Box3D/Source/ThirdParty/ClickNetShared/ClickNetWire.h`. Keep the project and server in this layout so their relative build paths resolve after a clone.

Clone with the vcpkg submodule:

```powershell
git clone --recurse-submodules <your-GitHub-repo-url>
cd ClickNet
.\vcpkg\bootstrap-vcpkg.bat
.\vcpkg\vcpkg.exe install gamenetworkingsockets:x64-windows
```

Use Visual Studio 2022 with the C++ desktop workload. Open `ClickNet.uproject` in Unreal, or build `ServerStage/clicknetserver.sln` in x64 Debug or Release. The server project also needs the checked-in Box3D library under `Plugins/Box3D/Source/ThirdParty/box3d/lib`. Export the level to `Saved/Box3DTest.box3d` before running the server; `Saved` is local build data and is not committed. See [server instructions](ServerStage/README.md) and [load-test instructions](ServerStage/LoadTest/README.md).

This folder is the Git repository root. To publish the local `main` branch to a new empty GitHub repository:

```powershell
git remote add origin <your-GitHub-repo-url>
git push -u origin main
```

`git ls-files --stage vcpkg` shows mode `160000`. This records the vcpkg revision without copying its repository into ClickNet. When changing `ClickNetWire.h`, build the Unreal client, server, and load generator together because they must use the same packet format.
