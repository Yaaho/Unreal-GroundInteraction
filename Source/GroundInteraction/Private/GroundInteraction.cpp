// Copyright Epic Games, Inc. All Rights Reserved.

#include "GroundInteraction.h"
#include "Interfaces/IPluginManager.h"

#include "ShaderCore.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "FGroundInteractionModule"

void FGroundInteractionModule::StartupModule()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GroundInteraction"));
	check(Plugin.IsValid());

	const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/GroundInteraction"), ShaderDir);
}

void FGroundInteractionModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FGroundInteractionModule, GroundInteraction)