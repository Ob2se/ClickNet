using System;
using System.IO;
using UnrealBuildTool;

public class GameNetworkingSockets : ModuleRules
{
    public GameNetworkingSockets(ReadOnlyTargetRules Target) : base(Target)
    {
        Type = ModuleType.External;

        string BasePath = ModuleDirectory;

        PublicIncludePaths.Add(Path.Combine(BasePath, "Include"));

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            string LibPath = Path.Combine(BasePath, "Lib", "Win64");
            string BinPath = Path.Combine(BasePath, "Bin", "Win64");

            PublicAdditionalLibraries.Add(Path.Combine(LibPath, "GameNetworkingSockets.lib"));

            // Delay-loaded DLLs must be beside the editor/game executable. The
            // networking DLL also imports protobuf, Abseil and OpenSSL DLLs.
            PublicDelayLoadDLLs.Add("GameNetworkingSockets.dll");
            foreach (string DllName in new[]
            {
                "GameNetworkingSockets.dll",
                "libprotobuf.dll",
                "abseil_dll.dll",
                "libcrypto-3-x64.dll",
                "libssl-3-x64.dll"
            })
            {
                RuntimeDependencies.Add(
                    Path.Combine("$(TargetOutputDir)", DllName),
                    Path.Combine(BinPath, DllName));
            }
        }
    }
}
