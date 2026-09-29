# ClickNet

ClickNet is a side project I'm building with Unreal Engine as the client and a custom C++ server using Valve's GameNetworkingSockets and Box3D for physics. This is my first project trying something like this. So far, it supports most Box3D collision shapes, the Box3D character mover, simulation on both the client and server, static meshes, skeletal meshes, and terrain.

The Box3D plugin I wrote exports the loaded Unreal world from the editor to a file. The client and server use that file to create matching physics worlds.

The server has several optimizations aimed at higher player counts, although my current tests start to break down at around 600 connections:

- An interest radius limits which players and bodies each client needs to receive.
- Client and server prediction reduce how often movement corrections need to be sent.
- For incremental replication, the server checks nearby spatial cells and skips players whose motion has not changed for that recipient. It collects the resulting messages in a temporary, per recipient outbox and submits them in one networking API call.

I plan to keep testing and raising that connection count, and to add raycasts, projectiles, and Blueprint support.

## Repository and setup

This repository contains the Unreal project (`ClickNet.uproject`), the C++ server (`ServerStage/clicknetserver.sln`), and the load generator (`ServerStage/LoadTest`). All three use the one wire definition at `Plugins/Box3D/Source/ThirdParty/ClickNetShared/ClickNetWire.h`. Keep the project and server in this layout so their relative build paths resolve after a clone.

Clone with the vcpkg submodule:

```powershell
git clone --recurse-submodules https://github.com/Ob2se/ClickNet.git
cd ClickNet
.\vcpkg\bootstrap-vcpkg.bat
.\vcpkg\vcpkg.exe install gamenetworkingsockets:x64-windows
```
