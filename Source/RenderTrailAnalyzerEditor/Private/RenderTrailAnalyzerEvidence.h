#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace UE::RenderTrail::Private
{
	inline constexpr double SignificantColorDeltaThreshold = 1.0 / 255.0;
	inline constexpr int32 MaxCausalGraphHops = 10;
	inline constexpr int32 MaxCausalGraphBreaks = 12;
	inline constexpr int32 MaxCausalGraphResourceEdges = 32;
	inline constexpr int32 MaxRecursiveProducerContextsPerEvent = 1;
	inline constexpr int32 MaxInitialEventContexts = 2;

	struct FPixelValueEvidence
	{
		bool bValid = false;
		double R = 0.0;
		double G = 0.0;
		double B = 0.0;
		double A = 0.0;
		bool bHasDepth = false;
		double Depth = 0.0;
		int32 Stencil = 0;
		FString Text;
	};

	struct FPixelModificationEvidence
	{
		uint32 EventId = 0;
		FString Action;
		FString ActionKind;
		FString MarkerPath;
		uint32 ActionFlags = 0;
		bool bPassed = false;
		bool bDirectShaderWrite = false;
		bool bUnboundPixelShader = false;
		bool bChangedTextureValue = false;
		uint32 PrimitiveId = 0;
		uint32 FragmentIndex = 0;
		TArray<FString> FailureReasons;
		FString Before;
		FString ShaderOutput;
		FString After;
		FPixelValueEvidence BeforeValue;
		FPixelValueEvidence ShaderOutputValue;
		FPixelValueEvidence AfterValue;
	};

	struct FEventSummaryEvidence
	{
		uint32 EventId = 0;
		FString Action;
		FString ActionKind;
		FString MarkerPath;
		uint32 ActionFlags = 0;
		int32 PassedFragments = 0;
		int32 RejectedFragments = 0;
		bool bDirectShaderWrite = false;
		bool bUnboundPixelShader = false;
		bool bChangedTextureValue = false;
		bool bHasPrimitiveEvidence = false;
		uint32 PrimitiveId = 0;
		TArray<FString> FailureReasons;
		FString Before;
		FString ShaderOutput;
		FString After;
		FPixelValueEvidence BeforeValue;
		FPixelValueEvidence ShaderOutputValue;
		FPixelValueEvidence AfterValue;
	};

	struct FPixelSample
	{
		uint64 Id = 0;
		FIntPoint Pixel = FIntPoint::ZeroValue;
		bool bPending = false;
		bool bFailed = false;
		bool bAnalyzed = false;
		bool bTruncated = false;
		bool bEventSummaryComplete = false;
		int32 TotalModifications = 0;
		FString Error;
		TArray<FEventSummaryEvidence> EventSummaries;
		TArray<FPixelModificationEvidence> Modifications;
	};

	struct FEventEvidence
	{
		uint32 EventId = 0;
		FString Action;
		FString ActionKind;
		FString MarkerPath;
		uint32 ActionFlags = 0;
		int32 PassedFragments = 0;
		int32 RejectedFragments = 0;
		int32 LastModificationIndex = INDEX_NONE;
		bool bDirectShaderWrite = false;
		bool bUnboundPixelShader = false;
		bool bChangedTextureValue = false;
		bool bHasPrimitiveEvidence = false;
		uint32 PrimitiveId = 0;
		TArray<FString> FailureReasons;
		FString Before;
		FString ShaderOutput;
		FString After;
		FPixelValueEvidence BeforeValue;
		FPixelValueEvidence ShaderOutputValue;
		FPixelValueEvidence AfterValue;
		double ColorDeltaMax = -1.0;
		double ColorDeltaL1 = -1.0;
	};

	struct FCausalCandidate
	{
		FEventEvidence Event;
		int32 SampleCoverage = 0;
	};

	struct FBoundResourceEvidence
	{
		int32 ResourceIndex = INDEX_NONE;
		FString Name;
		FString Format;
		FString Stage;
		FString Access;
		FString ShaderBinding;
		bool bTexture = false;
		int32 Width = 0;
		int32 Height = 0;
		int32 Samples = 1;
		int32 BindingIndex = INDEX_NONE;
		int32 ArrayElement = 0;
		int32 FirstMip = 0;
		int32 FirstSlice = 0;
		int32 TypeCast = INDEX_NONE;
	};

	struct FEventContextEvidence
	{
		uint32 EventId = 0;
		FString Action;
		FString ActionKind;
		FString MarkerPath;
		FString ShaderStage;
		FString ShaderEntry;
		FString ShaderDebugStatus;
		FString ShaderEncoding;
		int32 ShaderInputSignatureCount = 0;
		int32 ShaderOutputSignatureCount = 0;
		int32 ShaderConstantBlockCount = 0;
		int32 ShaderSamplerCount = 0;
		int32 ShaderReadOnlyResourceCount = 0;
		int32 ShaderReadWriteResourceCount = 0;
		bool bShaderDebuggable = false;
		bool bSourceDebugInfo = false;
		FString TraceStopReason;
		TArray<FBoundResourceEvidence> Inputs;
		TArray<FBoundResourceEvidence> Outputs;
		TArray<TSharedPtr<FJsonValue>> ResourceProvenance;
		TArray<TSharedPtr<FJsonValue>> ResourcePixelHistories;
		TSharedPtr<FJsonObject> PipelineState;
		TSharedPtr<FJsonObject> ShaderDebugTrace;
	};

	struct FAgentContextCoverageEvidence
	{
		uint32 EventId = 0;
		int32 ReverseDepth = 0;
		int32 CausalDistance = MAX_int32;
		int32 PassedFragments = 0;
		int32 RejectedFragments = 0;
		int64 PriorityScore = 0;
		bool bCritical = false;
		bool bReferencedPixelWriter = false;
		bool bChangedTextureValue = false;
		bool bAssetMarker = false;
		bool bSceneRaster = false;
		bool bNanite = false;
		bool bDepthStage = false;
		bool bBranchBoundary = false;
		bool bUnresolvedProducer = false;
		TArray<uint32> ProducerEventIds;
		TArray<uint32> DownstreamConsumerEventIds;
	};

	struct FAgentContextCoverageSelection
	{
		TArray<uint32> DetailedEventIds;
		TMap<uint32, TArray<FString>> SelectionReasons;
		TMap<uint32, FAgentContextCoverageEvidence> CoverageByEventId;
	};

	struct FCausalLaneBranchEvidence
	{
		FString TracePurpose;
		uint32 ConsumerEventId = 0;
		uint32 ProducerEventId = 0;
		uint32 ResetBoundaryEventId = 0;
		int32 ResourceIndex = INDEX_NONE;
		int32 ReverseDepth = 0;
		FString ResourceName;
		FString ShaderBinding;
		FString ResourceAccess;
		FString BranchStatus;
		FString MappingConfidence;
		FString EdgeRole;
		FString EdgeConfidence;
		FString ExecutedSampleValue;
		FString ProducerBeforeValue;
		FString ProducerShaderOutputValue;
		FString ProducerWrittenValue;
		FString ProducerActionKind;
		TArray<int32> Samples;
		int32 EvidenceRecordCount = 0;
		int32 QueryRecordCount = 0;
		int32 CollapsedShaderAccessCount = 0;
		bool bExecutedShaderAccess = false;
		bool bHasExecutedSampleValue = false;
		bool bExecutedSampleValueNeutral = false;
		bool bProducerChangedValue = false;
		bool bProducerValueMatchesExecutedSample = false;
	};

	struct FCausalLaneEvidence
	{
		FString TracePurpose;
		int32 ConfirmedProducerCount = 0;
		int32 ResetBoundaryCount = 0;
		int32 UnresolvedBoundaryCount = 0;
		int32 EvidenceRecordCount = 0;
		int32 QueryRecordCount = 0;
		TArray<FCausalLaneBranchEvidence> Branches;
	};

	struct FPrimaryCausalPathEvidence
	{
		uint32 RootEventId = 0;
		bool bReachedConfirmedSceneSource = false;
		bool bReachedExplicitBoundary = false;
		FString StopReason;
		TArray<FCausalLaneBranchEvidence> Branches;
	};

	FPixelValueEvidence ParsePixelValue(const TSharedPtr<FJsonObject>& Value);
	double ComputeColorDeltaMax(const FPixelValueEvidence& Before, const FPixelValueEvidence& After);
	double ComputeColorDeltaL1(const FPixelValueEvidence& Before, const FPixelValueEvidence& After);
	FString ClassifyColorDelta(const FEventEvidence& Event);
	void AddColorDeltaJson(const TSharedRef<FJsonObject>& Json, const FEventEvidence& Event);
	FBoundResourceEvidence ParseBoundResource(const TSharedPtr<FJsonObject>& Json);
	FAgentContextCoverageSelection SelectAgentContextsForCausalCoverage(
		const TMap<uint32, FEventContextEvidence>& EventContexts,
		const TMap<uint32, int32>& EventContextDepths,
		const TSet<uint32>& CriticalEventIds,
		int32 MaxDetailedContexts);
	TArray<FCausalLaneEvidence> BuildCausalLaneEvidence(
		const TMap<uint32, FEventContextEvidence>& EventContexts,
		const TMap<uint32, int32>& EventContextDepths);
	FPrimaryCausalPathEvidence BuildPrimaryColorPathEvidence(
		const TArray<FCausalLaneEvidence>& Lanes,
		const TMap<uint32, FEventContextEvidence>& EventContexts,
		uint32 RootEventId,
		int32 MaxHops = MaxCausalGraphHops);
	TArray<FEventEvidence> AggregateEvents(const FPixelSample& Sample);
	const FEventEvidence* FindEvent(const TArray<FEventEvidence>& Events, uint32 EventId);
	FString DescribeEventResult(const FEventEvidence& Event);
	bool IsConfirmedPixelWriter(const FEventEvidence& Event);
	int32 SelectDominatingWriterSummaryIndex(const TArray<FEventSummaryEvidence>& Events,
		uint32 ConsumerEventId, const FString& TracePurpose);
	FString ClassifyResourceTracePurpose(const FString& ResourceName, const FString& ShaderBinding);
	bool IsSceneSourceEvent(const FString& ActionKind, const FString& MarkerPath);
	FString BuildReplayPixelHistoryKey(int32 ResourceIndex, const FIntPoint& Pixel,
		int32 Mip, int32 Slice, int32 Sample, int32 TypeCast);
	FString ClassifySemantics(const FEventEvidence& Event);
	FString CompactMarkerPath(const FString& MarkerPath);

	TSharedRef<FJsonObject> BuildPixelCausalGraph(
		const FPixelSample& Sample,
		const TArray<FEventEvidence>& Events,
		const TMap<uint32, FEventContextEvidence>& EventContexts,
		int32 MaxDisplayedFrontierResources);
}
