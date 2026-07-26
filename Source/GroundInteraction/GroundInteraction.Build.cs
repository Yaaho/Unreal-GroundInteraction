// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class GroundInteraction : ModuleRules
{
	public GroundInteraction(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// 'TranslucentPassResource.h': No such file or directory 문제 임시 해결법
				System.IO.Path.Combine(GetModuleDirectory("Renderer"), "Internal"),
			}
		);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core"
				// ... add other public dependencies that you statically link with here ...
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore", 
				"RenderCore", // SVE 연동을 위해 렌더러 내부 접근 필요
				"Renderer", // 렌더링 관련 필수 모듈 (SVE, RDG)
				"RHI",
				"Water",
				"Niagara",
				"PhysicsCore", // Physics Asset body shape 을 사용하기 위해 필요
				"DeveloperSettings", // 플러그인 설정을 프로젝트 세팅 창에 노출하기 위해 필요
				"Projects" // 쉐이더 경로 매핑을 위해 필요 (IPluginManager)
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
