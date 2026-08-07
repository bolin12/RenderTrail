#pragma once

#include "CoreMinimal.h"

namespace UE::RenderTrail
{
	inline constexpr int32 CaptureMetadataSchemaVersion = 2;
	inline constexpr int32 ReplayWorkerProtocolVersion = 3;
	inline constexpr const TCHAR* MetadataExtension = TEXT("rendertrail.json");

	struct RENDERTRAILCORE_API FCaptureMetadata
	{
		FString CapturePath;
		FString ProjectName;
		FString ProjectDirectory;
		FString MapName;
		FString EngineVersion;
		FString UtcTimestamp;
		FString PreviewPath;
		uint64 FrameCounter = 0;
		int32 PreviewWidth = 0;
		int32 PreviewHeight = 0;
		bool bIsPIE = false;
		bool bPreviewPixelExact = false;

		FString ToJson() const;
		static bool FromJson(const FString& Json, FCaptureMetadata& OutMetadata, FString& OutError);
		bool SaveAdjacent(FString& OutMetadataPath, FString& OutError) const;
	};

	RENDERTRAILCORE_API FString GetMetadataPathForCapture(const FString& CapturePath);
	RENDERTRAILCORE_API FString GetPreviewPathForCapture(const FString& CapturePath);
}
