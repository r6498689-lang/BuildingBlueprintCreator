using UnrealBuildTool;

public class BuildingBlueprintCreator : ModuleRules
{
    public BuildingBlueprintCreator(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
        PrivateDependencyModuleNames.AddRange(new[] {
            "UnrealEd", "LevelEditor", "Slate", "SlateCore", "ContentBrowser", "RenderCore", "RHI", "ApplicationCore",
            "AssetRegistry", "AssetTools", "Kismet", "KismetCompiler", "BlueprintGraph", "Projects", "DatasmithContent", "SceneOutliner", "ToolMenus", "StaticMeshEditor"
        });
    }
}
