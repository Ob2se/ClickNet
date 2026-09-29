using UnrealBuildTool;

public class Box3DEditor : ModuleRules
{
    public Box3DEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Landscape",
            "Foliage",
            "UnrealEd",
            "Box3D",
            "DesktopPlatform",
            "ToolMenus",
            "Slate",
            "SlateCore"
        });
    }
}
