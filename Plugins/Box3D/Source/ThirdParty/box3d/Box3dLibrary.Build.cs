using System;
using System.IO;
using UnrealBuildTool;

public class Box3dLibrary : ModuleRules
{
    public Box3dLibrary(ReadOnlyTargetRules Target) : base(Target)
    {
        Type = ModuleType.External;

        string BasePath = ModuleDirectory;

        // Include/box3d/*.h  ->  add "Include" so code can do #include "box3d/box3d.h"
        PublicIncludePaths.Add(Path.Combine(BasePath, "include"));

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            string LibPath = Path.Combine(BasePath, "lib", "box3d.lib");
            PublicAdditionalLibraries.Add(LibPath);
        }

        // Static lib, no DLL — no RuntimeDependencies or PublicDelayLoadDLLs needed.

        // If your build used BOX3D_DOUBLE_PRECISION, mirror that here so any
        // code including these headers agrees with how the .lib was compiled:
        // PublicDefinitions.Add("BOX3D_DOUBLE_PRECISION=1");
    }
}