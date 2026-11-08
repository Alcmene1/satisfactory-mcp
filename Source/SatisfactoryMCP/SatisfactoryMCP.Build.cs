using UnrealBuildTool;

public class SatisfactoryMCP : ModuleRules
{
	public SatisfactoryMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		CppStandard = CppStandardVersion.Cpp20;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bLegacyPublicIncludePaths = false;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

		// SML transitively exports FactoryGame headers
		PrivateDependencyModuleNames.AddRange(new string[] { "SML", "FactoryGame", "Json", "Sockets", "Foliage", "GeometryCore" });
	}
}
