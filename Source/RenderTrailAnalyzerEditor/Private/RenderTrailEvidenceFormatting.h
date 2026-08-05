#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace UE::RenderTrail::Private::EvidenceFormatting
{
	FString FormatPipelineCompactSummary(const TSharedPtr<FJsonObject>& PipelineState);
	FString FormatPipelineState(const TSharedPtr<FJsonObject>& PipelineState);
	FString FormatShaderDebugTrace(const TSharedPtr<FJsonObject>& Trace);
}
