#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct IReplayController;
struct ResourceId;
struct TextureDescription;

namespace UE::RenderTrail::Private
{
	TSharedRef<FJsonObject> BuildResourceProvenance(
		IReplayController& Controller,
		const ResourceId& InputResource,
		uint32 ConsumerEventId,
		const FString& ResourceName,
		const FString& ShaderBinding,
		int32 ResourceIndex,
		const TextureDescription* InputTexture,
		const TextureDescription* OutputTexture,
		const TMap<uint32, FString>& ActionNames,
		const TMap<uint32, FString>& ActionKinds,
		const TMap<uint32, FString>& ActionPaths);

	TSharedRef<FJsonObject> BuildShaderCodeEvidence(
		const FString& Disassembly,
		const TArray<FIntPoint>& ObservedInstructionLines,
		int32 MaxLines = 32);
}
