#pragma once

#include "CoreMinimal.h"

namespace UE::RenderTrail
{
	inline constexpr int32 ProtocolVersion = 2;
	inline constexpr const TCHAR* MetadataExtension = TEXT("rendertrail.json");

	struct RENDERTRAILCORE_API FCaptureMetadata
	{
		FString CapturePath;
		FString ProjectName;
		FString ProjectDirectory;
		FString MapName;
		FString EngineVersion;
		FString UtcTimestamp;
		uint64 FrameCounter = 0;
		bool bIsPIE = false;

		FString ToJson() const;
		static bool FromJson(const FString& Json, FCaptureMetadata& OutMetadata, FString& OutError);
		bool SaveAdjacent(FString& OutMetadataPath, FString& OutError) const;
	};

	RENDERTRAILCORE_API FString GetMetadataPathForCapture(const FString& CapturePath);
}
