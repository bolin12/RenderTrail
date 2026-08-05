#include "RenderTrailProtocol.h"
#include "IRenderTrailAnalyzerEditorModule.h"
#include "RenderTrailModelBrokerSettings.h"

#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "Containers/Ticker.h"
#include "DesktopPlatformModule.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "IDesktopPlatform.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/DateTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Rendering/DrawElements.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogRenderTrailAnalyzer, Log, All);

namespace UE::RenderTrail::Private
{
	DECLARE_DELEGATE_TwoParams(FOnPixelPicked, int32, int32);

	struct FPixelMarker
	{
		FIntPoint Pixel = FIntPoint::ZeroValue;
	};

	class SRenderTrailImageView final : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SRenderTrailImageView) {}
			SLATE_EVENT(FOnPixelPicked, OnPixelPicked)
		SLATE_END_ARGS()

		void Construct(const FArguments& Args)
		{
			OnPixelPicked = Args._OnPixelPicked;
			SetCanTick(false);
		}

		void SetImage(const TSharedPtr<FSlateDynamicImageBrush>& InBrush, FIntPoint InSize, TArray<uint8> InPixelBytes = {})
		{
			Brush = InBrush;
			ImageSize = InSize;
			PixelBytes = MoveTemp(InPixelBytes);
			Zoom = 1.0f;
			Pan = FVector2f::ZeroVector;
			Markers.Empty();
			HoveredPixel = FIntPoint::ZeroValue;
			bHasHoveredPixel = false;
			Invalidate(EInvalidateWidgetReason::Paint | EInvalidateWidgetReason::Layout);
		}

		void SetMarkers(const TArray<FPixelMarker>& InMarkers)
		{
			Markers = InMarkers;
			Invalidate(EInvalidateWidgetReason::Paint);
		}

		virtual FVector2D ComputeDesiredSize(float) const override
		{
			return FVector2D(800.0f, 520.0f);
		}

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
			FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
		{
			OutDrawElements.PushClip(FSlateClippingZone(AllottedGeometry));
			const FVector2f LocalSize = AllottedGeometry.GetLocalSize();
			const FSlateBrush* Background = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
			FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
				AllottedGeometry.ToPaintGeometry(LocalSize, FSlateLayoutTransform()), Background,
				ESlateDrawEffect::None, FLinearColor(0.015f, 0.018f, 0.022f, 1.0f));

			FVector2f Origin;
			FVector2f DrawSize;
			float Scale = 0.0f;
			if (Brush.IsValid() && ComputeImageRect(LocalSize, Origin, DrawSize, Scale))
			{
				if (Scale >= PixelExactMinScale && HasPixelBytes())
				{
					DrawPixelExact(OutDrawElements, AllottedGeometry, LayerId + 1, LocalSize, Origin, Scale);
				}
				else
				{
					FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1,
						AllottedGeometry.ToPaintGeometry(DrawSize, FSlateLayoutTransform(Origin)), Brush.Get(),
						ESlateDrawEffect::None, InWidgetStyle.GetColorAndOpacityTint());
				}

				if (Scale >= PixelGridMinScale)
				{
					DrawPixelGrid(OutDrawElements, AllottedGeometry, LayerId + 2, LocalSize, Origin, Scale);
					if (bHasHoveredPixel)
					{
						DrawPixelOutline(OutDrawElements, AllottedGeometry, LayerId + 3, Origin, Scale,
							HoveredPixel, FLinearColor(1.0f, 0.85f, 0.1f, 1.0f), 1.5f);
					}
				}

				for (const FPixelMarker& Marker : Markers)
				{
					const FLinearColor Color = GetMarkerColor();
					if (Scale >= PixelGridMinScale)
					{
						DrawPixelOutline(OutDrawElements, AllottedGeometry, LayerId + 4, Origin, Scale,
							Marker.Pixel, Color, 2.5f);
						continue;
					}

					const FVector2f PixelCenter = Origin + FVector2f((Marker.Pixel.X + 0.5f) * Scale, (Marker.Pixel.Y + 0.5f) * Scale);
					const float Arm = 10.0f;
					TArray<FVector2f> Horizontal = {PixelCenter - FVector2f(Arm, 0), PixelCenter + FVector2f(Arm, 0)};
					TArray<FVector2f> Vertical = {PixelCenter - FVector2f(0, Arm), PixelCenter + FVector2f(0, Arm)};
					FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 4, AllottedGeometry.ToPaintGeometry(), Horizontal,
						ESlateDrawEffect::None, Color, true, 2.0f);
					FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 4, AllottedGeometry.ToPaintGeometry(), Vertical,
						ESlateDrawEffect::None, Color, true, 2.0f);
				}
			}
			OutDrawElements.PopClip();
			return LayerId + 4;
		}

		virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override
		{
			if (Event.GetEffectingButton() == EKeys::LeftMouseButton)
			{
				const FVector2f Local = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
				FIntPoint Pixel;
				if (TryGetPixelAtLocal(Local, Geometry.GetLocalSize(), Pixel))
				{
					OnPixelPicked.ExecuteIfBound(Pixel.X, Pixel.Y);
				}
				return FReply::Handled();
			}
			if (Event.GetEffectingButton() == EKeys::MiddleMouseButton || Event.GetEffectingButton() == EKeys::RightMouseButton)
			{
				bPanning = true;
				LastPanPoint = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
				return FReply::Handled().CaptureMouse(SharedThis(this));
			}
			return FReply::Unhandled();
		}

		virtual FReply OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event) override
		{
			const FVector2f Current = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
			if (bPanning && HasMouseCapture())
			{
				Pan += Current - LastPanPoint;
				ClampPan(Geometry.GetLocalSize());
				LastPanPoint = Current;
				UpdateHoveredPixel(Current, Geometry.GetLocalSize());
				Invalidate(EInvalidateWidgetReason::Paint);
				return FReply::Handled();
			}
			if (UpdateHoveredPixel(Current, Geometry.GetLocalSize()))
			{
				Invalidate(EInvalidateWidgetReason::Paint);
			}
			return FReply::Unhandled();
		}

		virtual FReply OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event) override
		{
			if (bPanning && (Event.GetEffectingButton() == EKeys::MiddleMouseButton || Event.GetEffectingButton() == EKeys::RightMouseButton))
			{
				bPanning = false;
				UpdateHoveredPixel(Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition()), Geometry.GetLocalSize());
				Invalidate(EInvalidateWidgetReason::Paint);
				return FReply::Handled().ReleaseMouseCapture();
			}
			return FReply::Unhandled();
		}

		virtual FReply OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event) override
		{
			const FVector2f LocalSize = Geometry.GetLocalSize();
			const FVector2f Cursor = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
			FVector2f OldOrigin;
			FVector2f OldDrawSize;
			float OldScale = 0.0f;
			const bool bHadImageRect = ComputeImageRect(LocalSize, OldOrigin, OldDrawSize, OldScale);
			const FVector2f ImagePoint = bHadImageRect ? (Cursor - OldOrigin) / OldScale : FVector2f::ZeroVector;

			Zoom = FMath::Clamp(Zoom * FMath::Pow(1.2f, Event.GetWheelDelta()), 0.1f, 128.0f);
			if (bHadImageRect)
			{
				const float FitScale = FMath::Min(LocalSize.X / ImageSize.X, LocalSize.Y / ImageSize.Y);
				const float NewScale = FitScale * Zoom;
				const FVector2f NewDrawSize = FVector2f(ImageSize) * NewScale;
				Pan = Cursor - ImagePoint * NewScale - (LocalSize - NewDrawSize) * 0.5f;
			}
			ClampPan(LocalSize);
			UpdateHoveredPixel(Cursor, LocalSize);
			Invalidate(EInvalidateWidgetReason::Paint);
			return FReply::Handled();
		}

		virtual void OnMouseLeave(const FPointerEvent& Event) override
		{
			SLeafWidget::OnMouseLeave(Event);
			if (!bPanning && bHasHoveredPixel)
			{
				bHasHoveredPixel = false;
				Invalidate(EInvalidateWidgetReason::Paint);
			}
		}

	private:
		static constexpr float PixelExactMinScale = 4.0f;
		static constexpr float PixelGridMinScale = 8.0f;

		static FLinearColor GetMarkerColor()
		{
			return FLinearColor(0.0f, 0.0f, 0.0f, 1.0f);
		}

		bool HasPixelBytes() const
		{
			return ImageSize.X > 0 && ImageSize.Y > 0
				&& PixelBytes.Num() == static_cast<int64>(ImageSize.X) * ImageSize.Y * 4;
		}

		bool ComputeVisiblePixelRange(FVector2f LocalSize, FVector2f Origin, float Scale,
			int32& OutFirstX, int32& OutFirstY, int32& OutEndX, int32& OutEndY) const
		{
			if (Scale <= 0.0f)
			{
				return false;
			}
			OutFirstX = FMath::Clamp(FMath::FloorToInt(-Origin.X / Scale), 0, ImageSize.X);
			OutFirstY = FMath::Clamp(FMath::FloorToInt(-Origin.Y / Scale), 0, ImageSize.Y);
			OutEndX = FMath::Clamp(FMath::CeilToInt((LocalSize.X - Origin.X) / Scale), 0, ImageSize.X);
			OutEndY = FMath::Clamp(FMath::CeilToInt((LocalSize.Y - Origin.Y) / Scale), 0, ImageSize.Y);
			return OutFirstX < OutEndX && OutFirstY < OutEndY;
		}

		void DrawPixelExact(FSlateWindowElementList& OutDrawElements, const FGeometry& Geometry, int32 Layer,
			FVector2f LocalSize, FVector2f Origin, float Scale) const
		{
			int32 FirstX = 0;
			int32 FirstY = 0;
			int32 EndX = 0;
			int32 EndY = 0;
			if (!ComputeVisiblePixelRange(LocalSize, Origin, Scale, FirstX, FirstY, EndX, EndY))
			{
				return;
			}

			const FSlateBrush* WhiteBrush = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
			const FSlateResourceHandle ResourceHandle = WhiteBrush->GetRenderingResource();
			const FSlateShaderResourceProxy* ResourceProxy = ResourceHandle.GetResourceProxy();
			const FVector2f WhiteUv = ResourceProxy
				? ResourceProxy->StartUV + ResourceProxy->SizeUV * 0.5f
				: FVector2f(0.5f, 0.5f);
			const FSlateRenderTransform& RenderTransform = Geometry.GetAccumulatedRenderTransform();

			TArray<FSlateVertex> Vertices;
			TArray<SlateIndex> Indices;
			constexpr int32 MaxPixelsPerBatch = 12000;
			Vertices.Reserve(MaxPixelsPerBatch * 4);
			Indices.Reserve(MaxPixelsPerBatch * 6);

			auto FlushBatch = [&]()
			{
				if (!Vertices.IsEmpty())
				{
					FSlateDrawElement::MakeCustomVerts(OutDrawElements, Layer, ResourceHandle, Vertices, Indices,
						nullptr, 0, 0, ESlateDrawEffect::None);
					Vertices.Reset();
					Indices.Reset();
				}
			};

			int32 PixelsInBatch = 0;
			for (int32 Y = FirstY; Y < EndY; ++Y)
			{
				for (int32 X = FirstX; X < EndX; ++X)
				{
					if (PixelsInBatch == MaxPixelsPerBatch)
					{
						FlushBatch();
						PixelsInBatch = 0;
					}

					const int64 ByteOffset = (static_cast<int64>(Y) * ImageSize.X + X) * 4;
					const FColor PixelColor(
						PixelBytes[ByteOffset + 2], PixelBytes[ByteOffset + 1], PixelBytes[ByteOffset], PixelBytes[ByteOffset + 3]);
					const FVector2f TopLeft = Origin + FVector2f(X * Scale, Y * Scale);
					const FVector2f BottomRight = TopLeft + FVector2f(Scale, Scale);
					const SlateIndex BaseIndex = static_cast<SlateIndex>(Vertices.Num());
					Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(RenderTransform,
						TopLeft, WhiteUv, PixelColor));
					Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(RenderTransform,
						FVector2f(BottomRight.X, TopLeft.Y), WhiteUv, PixelColor));
					Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(RenderTransform,
						BottomRight, WhiteUv, PixelColor));
					Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(RenderTransform,
						FVector2f(TopLeft.X, BottomRight.Y), WhiteUv, PixelColor));
					Indices.Add(BaseIndex);
					Indices.Add(BaseIndex + 1);
					Indices.Add(BaseIndex + 2);
					Indices.Add(BaseIndex);
					Indices.Add(BaseIndex + 2);
					Indices.Add(BaseIndex + 3);
					++PixelsInBatch;
				}
			}
			FlushBatch();
		}

		void DrawPixelGrid(FSlateWindowElementList& OutDrawElements, const FGeometry& Geometry, int32 Layer,
			FVector2f LocalSize, FVector2f Origin, float Scale) const
		{
			int32 FirstX = 0;
			int32 FirstY = 0;
			int32 EndX = 0;
			int32 EndY = 0;
			if (!ComputeVisiblePixelRange(LocalSize, Origin, Scale, FirstX, FirstY, EndX, EndY))
			{
				return;
			}

			const FLinearColor GridColor(0.0f, 0.0f, 0.0f, 0.55f);
			const float MinY = Origin.Y + FirstY * Scale;
			const float MaxY = Origin.Y + EndY * Scale;
			for (int32 X = FirstX; X <= EndX; ++X)
			{
				const float LineX = Origin.X + X * Scale;
				const TArray<FVector2f> Line = {FVector2f(LineX, MinY), FVector2f(LineX, MaxY)};
				FSlateDrawElement::MakeLines(OutDrawElements, Layer, Geometry.ToPaintGeometry(), Line,
					ESlateDrawEffect::None, GridColor, false, 1.0f);
			}
			const float MinX = Origin.X + FirstX * Scale;
			const float MaxX = Origin.X + EndX * Scale;
			for (int32 Y = FirstY; Y <= EndY; ++Y)
			{
				const float LineY = Origin.Y + Y * Scale;
				const TArray<FVector2f> Line = {FVector2f(MinX, LineY), FVector2f(MaxX, LineY)};
				FSlateDrawElement::MakeLines(OutDrawElements, Layer, Geometry.ToPaintGeometry(), Line,
					ESlateDrawEffect::None, GridColor, false, 1.0f);
			}
		}

		static void DrawPixelOutline(FSlateWindowElementList& OutDrawElements, const FGeometry& Geometry, int32 Layer,
			FVector2f Origin, float Scale, FIntPoint Pixel, const FLinearColor& Color, float Thickness)
		{
			const float Inset = FMath::Min(Thickness * 0.5f, Scale * 0.2f);
			const FVector2f TopLeft = Origin + FVector2f(Pixel) * Scale + FVector2f(Inset);
			const FVector2f BottomRight = Origin + FVector2f(Pixel + FIntPoint(1, 1)) * Scale - FVector2f(Inset);
			const TArray<FVector2f> Outline = {
				TopLeft,
				FVector2f(BottomRight.X, TopLeft.Y),
				BottomRight,
				FVector2f(TopLeft.X, BottomRight.Y),
				TopLeft};
			FSlateDrawElement::MakeLines(OutDrawElements, Layer, Geometry.ToPaintGeometry(), Outline,
				ESlateDrawEffect::None, Color, false, Thickness);
		}

		bool TryGetPixelAtLocal(FVector2f Local, FVector2f LocalSize, FIntPoint& OutPixel) const
		{
			FVector2f Origin;
			FVector2f DrawSize;
			float Scale = 0.0f;
			if (!ComputeImageRect(LocalSize, Origin, DrawSize, Scale))
			{
				return false;
			}
			const FVector2f ImagePoint = (Local - Origin) / Scale;
			OutPixel = FIntPoint(FMath::FloorToInt(ImagePoint.X), FMath::FloorToInt(ImagePoint.Y));
			return OutPixel.X >= 0 && OutPixel.Y >= 0 && OutPixel.X < ImageSize.X && OutPixel.Y < ImageSize.Y;
		}

		bool UpdateHoveredPixel(FVector2f Local, FVector2f LocalSize)
		{
			FIntPoint NewPixel;
			const bool bNewHasHoveredPixel = TryGetPixelAtLocal(Local, LocalSize, NewPixel);
			const bool bChanged = bNewHasHoveredPixel != bHasHoveredPixel
				|| (bNewHasHoveredPixel && NewPixel != HoveredPixel);
			bHasHoveredPixel = bNewHasHoveredPixel;
			if (bNewHasHoveredPixel)
			{
				HoveredPixel = NewPixel;
			}
			return bChanged;
		}

		static FVector2f ConstrainPan(FVector2f LocalSize, FVector2f DrawSize, FVector2f RequestedPan)
		{
			FVector2f Result = RequestedPan;
			if (DrawSize.X <= LocalSize.X)
			{
				Result.X = 0.0f;
			}
			else
			{
				const float HalfOverflow = (DrawSize.X - LocalSize.X) * 0.5f;
				Result.X = FMath::Clamp(Result.X, -HalfOverflow, HalfOverflow);
			}
			if (DrawSize.Y <= LocalSize.Y)
			{
				Result.Y = 0.0f;
			}
			else
			{
				const float HalfOverflow = (DrawSize.Y - LocalSize.Y) * 0.5f;
				Result.Y = FMath::Clamp(Result.Y, -HalfOverflow, HalfOverflow);
			}
			return Result;
		}

		void ClampPan(FVector2f LocalSize)
		{
			if (ImageSize.X <= 0 || ImageSize.Y <= 0 || LocalSize.X <= 0.0f || LocalSize.Y <= 0.0f)
			{
				Pan = FVector2f::ZeroVector;
				return;
			}
			const float FitScale = FMath::Min(LocalSize.X / ImageSize.X, LocalSize.Y / ImageSize.Y);
			const FVector2f DrawSize = FVector2f(ImageSize) * (FitScale * Zoom);
			Pan = ConstrainPan(LocalSize, DrawSize, Pan);
		}

		bool ComputeImageRect(FVector2f LocalSize, FVector2f& OutOrigin, FVector2f& OutDrawSize, float& OutScale) const
		{
			if (!Brush.IsValid() || ImageSize.X <= 0 || ImageSize.Y <= 0 || LocalSize.X <= 0 || LocalSize.Y <= 0)
			{
				return false;
			}
			const float FitScale = FMath::Min(LocalSize.X / ImageSize.X, LocalSize.Y / ImageSize.Y);
			OutScale = FitScale * Zoom;
			OutDrawSize = FVector2f(ImageSize) * OutScale;
			OutOrigin = (LocalSize - OutDrawSize) * 0.5f + ConstrainPan(LocalSize, OutDrawSize, Pan);
			return true;
		}

		FOnPixelPicked OnPixelPicked;
		TSharedPtr<FSlateDynamicImageBrush> Brush;
		FIntPoint ImageSize = FIntPoint::ZeroValue;
		TArray<uint8> PixelBytes;
		TArray<FPixelMarker> Markers;
		FIntPoint HoveredPixel = FIntPoint::ZeroValue;
		FVector2f Pan = FVector2f::ZeroVector;
		FVector2f LastPanPoint = FVector2f::ZeroVector;
		float Zoom = 1.0f;
		bool bPanning = false;
		bool bHasHoveredPixel = false;
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
	};

	struct FCausalCandidate
	{
		FEventEvidence Event;
		int32 SampleCoverage = 0;
	};

	struct FBoundResourceEvidence
	{
		FString Name;
		FString Stage;
		FString Access;
		bool bTexture = false;
		int32 Width = 0;
		int32 Height = 0;
		int32 Samples = 1;
	};

	struct FEventContextEvidence
	{
		uint32 EventId = 0;
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
		TArray<FBoundResourceEvidence> Inputs;
		TArray<FBoundResourceEvidence> Outputs;
		TArray<TSharedPtr<FJsonValue>> ResourceProvenance;
		TSharedPtr<FJsonObject> PipelineState;
		TSharedPtr<FJsonObject> ShaderDebugTrace;
	};

	struct FRenderTrailDiagnosticsOptions
	{
		bool bEnabled = true;
		bool bWorkerProtocol = true;
		bool bAgentTraffic = true;
		bool bFullEvidencePayload = true;
	};

	class SAnalyzerHome final : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SAnalyzerHome) {}
			SLATE_ARGUMENT(FString, InitialCapture)
		SLATE_END_ARGS()

		~SAnalyzerHome() override
		{
			CancelAgentRun();
			StopWorker();
			ReleasePreview();
		}

		void Construct(const FArguments& Args)
		{
			DiagnosticsOptions = LoadDiagnosticsOptions();
			AgentBrokerUrl = GetDefault<URenderTrailOwnedModelSettings>()->GetChatCompletionsUrl();

			ChildSlot
			[
				SNew(SBorder)
				.Padding(12.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
						[
							SNew(STextBlock).Text(FText::FromString(TEXT("RenderTrail Analyzer")))
							.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 18))
						]
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
						[
								SAssignNew(CapturePathBox, SEditableTextBox)
								.Text(FText::FromString(Args._InitialCapture))
								.HintText(FText::FromString(TEXT("选择一个 .rdc 截帧")))
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
						[
								SNew(SButton).Text(FText::FromString(TEXT("选择..."))).OnClicked(this, &SAnalyzerHome::BrowseCapture)
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
						[
								SNew(SButton).Text(FText::FromString(TEXT("载入 / 重载"))).OnClicked(this, &SAnalyzerHome::LoadCapture)
						]
					]
					+ SVerticalBox::Slot().FillHeight(1.0f)
					[
						SNew(SSplitter)
						+ SSplitter::Slot().Value(0.68f)
						[
							SNew(SBorder).Padding(1.0f)
							[
								SAssignNew(ImageView, SRenderTrailImageView)
								.OnPixelPicked(FOnPixelPicked::CreateSP(this, &SAnalyzerHome::QueryPixel))
							]
						]
						+ SSplitter::Slot().Value(0.32f)
						[
							SNew(SBorder)
							.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
							.BorderBackgroundColor(FLinearColor(0.025f, 0.03f, 0.04f, 1.0f))
							.Padding(12.0f)
							[
								SNew(SVerticalBox)
								+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
								[
									SNew(STextBlock).Text(FText::FromString(TEXT("像素因果分析")))
									.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 17))
									.ColorAndOpacity(FLinearColor(0.88f, 0.93f, 1.0f))
								]
								+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
								[
									SNew(STextBlock)
									.Text(FText::FromString(TEXT("直接在画面选择一个关注像素；点击其他位置会替换当前点，每次分析只处理一个像素。模型供应商、Base URL、模型、API Key、Max Tokens 和 Thinking 在 Project Settings > Plugins > RenderTrail Model Broker 中统一管理；RenderTrail 只发送当前像素的有限证据。")))
									.AutoWrapText(true)
									.ColorAndOpacity(FLinearColor(0.55f, 0.62f, 0.72f))
								]
								+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
								[
									SNew(SHorizontalBox)
									+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
									[
										SAssignNew(SelectionText, STextBlock)
										.Text(FText::FromString(TEXT("尚未选择关注像素；点击画面选择一个点")))
										.AutoWrapText(true)
										.ColorAndOpacity(FLinearColor(0.70f, 0.76f, 0.84f))
									]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("清空选点")))
						.ToolTipText(FText::FromString(TEXT("清除当前选中的像素及其当前分析信息。")))
						.OnClicked(this, &SAnalyzerHome::ClearSamples)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("读取 Pixel History")))
						.ToolTipText(FText::FromString(TEXT("确认当前选点并读取 Pixel History；未变化的已分析点会复用缓存。")))
						.OnClicked(this, &SAnalyzerHome::ConfirmPixelSelection)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
					[
						SNew(SButton)
						.ToolTipText(FText::FromString(TEXT("将本次分析诉求与已经读取的像素证据一起发送，生成针对性语义回答。")))
						.OnClicked(this, &SAnalyzerHome::StartAgentAnalysis)
						[
							SAssignNew(AgentRunButtonText, STextBlock)
							.Text(FText::FromString(TEXT("运行 Agent")))
						]
					]
									]
								+ SVerticalBox::Slot().FillHeight(1.0f)
								[
									SNew(SScrollBox)
									+ SScrollBox::Slot().Padding(0, 0, 0, 8)
									[
										SNew(SBorder)
										.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
										.BorderBackgroundColor(FLinearColor(0.055f, 0.11f, 0.17f, 1.0f))
										.Padding(12.0f)
										[
											SNew(SVerticalBox)
											+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
											[
												SNew(STextBlock).Text(FText::FromString(TEXT("结论")))
												.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 12))
												.ColorAndOpacity(FLinearColor(0.31f, 0.72f, 1.0f))
											]
											+ SVerticalBox::Slot().AutoHeight()
											[
														SAssignNew(SummaryText, SMultiLineEditableText)
														.Text(FText::FromString(TEXT("等待载入截帧。")))
														.AutoWrapText(true)
														.IsReadOnly(true)
														.AllowContextMenu(true)
														.ClearTextSelectionOnFocusLoss(false)
											]
										]
									]
									+ SScrollBox::Slot().Padding(0, 0, 0, 8)
									[
										SNew(SBorder)
										.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
										.BorderBackgroundColor(FLinearColor(0.065f, 0.07f, 0.09f, 1.0f))
										.Padding(12.0f)
										[
											SNew(SVerticalBox)
											+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
											[
												SNew(STextBlock).Text(FText::FromString(TEXT("因果路径")))
												.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 12))
												.ColorAndOpacity(FLinearColor(0.75f, 0.81f, 0.90f))
											]
											+ SVerticalBox::Slot().AutoHeight()
											[
														SAssignNew(CausalPathText, SMultiLineEditableText)
														.Text(FText::FromString(TEXT("选择关注像素后生成。")))
														.AutoWrapText(true)
														.IsReadOnly(true)
														.AllowContextMenu(true)
														.ClearTextSelectionOnFocusLoss(false)
											]
										]
									]
					+ SScrollBox::Slot().Padding(0, 0, 0, 8)
									[
										SNew(SBorder)
										.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
										.BorderBackgroundColor(FLinearColor(0.105f, 0.055f, 0.16f, 1.0f))
										.Padding(12.0f)
										[
											SNew(SVerticalBox)
											+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 7)
											[
												SNew(SHorizontalBox)
												+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
												[
													SNew(STextBlock).Text(FText::FromString(TEXT("Agent 语义整理")))
													.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 12))
															.ColorAndOpacity(FLinearColor(0.78f, 0.52f, 1.0f))
														]
														+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
														[
															SNew(SButton)
															.Text(FText::FromString(TEXT("清空当前信息")))
															.ToolTipText(FText::FromString(TEXT("清空右侧报告和 Agent 输出，但保留当前选点及已分析状态。")))
															.OnClicked(this, &SAnalyzerHome::ClearCurrentInfo)
														]
													]
																				+ SVerticalBox::Slot().AutoHeight()
																				[
																						SNew(SVerticalBox)
																						+ SVerticalBox::Slot().AutoHeight()
																						[
																							SNew(STextBlock)
																							.Text(FText::FromString(TEXT("本次分析诉求（可选）")))
																							.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 11))
																							.ColorAndOpacity(FLinearColor(0.82f, 0.88f, 0.96f))
																						]
																						+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 4)
																						[
																							SNew(SBox)
																							.HeightOverride(72.0f)
																							[
																								SAssignNew(AgentIntentTextBox, SMultiLineEditableTextBox)
																								.HintText(FText::FromString(TEXT("例如：为什么这里是阴影？这个绿色最终来自哪个 Pass？请重点解释颜色变化的原因。")))
																								.AutoWrapText(true)
																								.AllowMultiLine(true)
																								.ClearTextSelectionOnFocusLoss(false)
																								.AllowContextMenu(true)
																							]
																						]
																						+ SVerticalBox::Slot().AutoHeight()
																						[
																							SNew(STextBlock)
																							.Text(FText::FromString(TEXT("运行 Agent 时会将这段诉求与下方固定的像素证据一起发送；没有填写时仍按默认溯源问题整理。")))
																									.AutoWrapText(true)
																									.ColorAndOpacity(FLinearColor(0.60f, 0.66f, 0.76f))
																					]
																					]
																				+ SVerticalBox::Slot().AutoHeight()
																				[
																																																				SAssignNew(AgentReportBox, SVerticalBox)
																+ SVerticalBox::Slot().AutoHeight()
																[
																	SAssignNew(AgentOutputText, SMultiLineEditableText)
																										.Text(FText::FromString(TEXT("填写本次分析诉求后，Agent 会优先回答你的问题，再提供相关的 Pass、Pipeline 和 Shader 证据。")))
														.AutoWrapText(true)
														.IsReadOnly(true)
														.AllowContextMenu(true)
																	.ClearTextSelectionOnFocusLoss(false)
																]
																]
													+ SVerticalBox::Slot().AutoHeight().Padding(0, 7, 0, 0)
											[
								SAssignNew(AgentStatusText, SMultiLineEditableText)
																.Text(FText::FromString(TEXT("未运行 · 只向 RenderTrail Model Broker 发送像素摘要；.rdc/图像不上传")))
														.AutoWrapText(true)
														.IsReadOnly(true)
														.AllowContextMenu(true)
														.ClearTextSelectionOnFocusLoss(false)
											]
						]
									]
									+ SScrollBox::Slot()
									[
										SNew(SBorder)
										.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
										.BorderBackgroundColor(FLinearColor(0.045f, 0.05f, 0.06f, 1.0f))
										.Padding(10.0f)
										[
											SNew(SVerticalBox)
											+ SVerticalBox::Slot().AutoHeight()
											[
												SNew(SButton)
												.ButtonColorAndOpacity(FLinearColor(0.12f, 0.13f, 0.15f, 1.0f))
												.OnClicked(this, &SAnalyzerHome::ToggleTechnicalEvidence)
												[
													SAssignNew(TechnicalToggleText, STextBlock)
															.Text(FText::FromString(TEXT("▶  详细技术证据（Pass / Pipeline / Shader）")))
												]
											]
											+ SVerticalBox::Slot().AutoHeight().Padding(4, 8, 4, 2)
											[
												SAssignNew(TechnicalEvidenceBox, SBox)
												.Visibility(EVisibility::Collapsed)
												[
																SAssignNew(EvidenceText, SMultiLineEditableText)
																.Text(FText::FromString(TEXT("尚无技术证据。")))
																.AutoWrapText(true)
																.IsReadOnly(true)
																.AllowContextMenu(true)
																.ClearTextSelectionOnFocusLoss(false)
												]
											]
										]
									]
								]
							]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
					[
					SAssignNew(StatusText, STextBlock)
					.Text(FText::FromString(TEXT("就绪。")))
						.AutoWrapText(true)
					]
				]
			];

			if (!Args._InitialCapture.IsEmpty() && FPaths::FileExists(Args._InitialCapture))
			{
				StartWorker();
			}
		}

		void OpenCapture(const FString& CapturePath)
		{
			if (!CapturePathBox.IsValid())
			{
				return;
			}
			CapturePathBox->SetText(FText::FromString(FPaths::ConvertRelativePathToFull(CapturePath)));
			StartWorker();
		}

		virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override
		{
			SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
			PollWorkerPipes();
			if (bCaptureLoading)
			{
				const double Now = FPlatformTime::Seconds();
				if (Now - LastCaptureLoadStatusSeconds >= 0.5)
				{
					SetStatus(FString::Printf(TEXT("正在载入截帧… %.1fs · %s"),
						Now - CaptureLoadStartSeconds, *CaptureLoadPhase));
					LastCaptureLoadStatusSeconds = Now;
				}
			}
		}

	private:
		FString GetCapturePath() const
		{
			return CapturePathBox.IsValid() ? CapturePathBox->GetText().ToString() : FString();
		}

		void SetStatus(const FString& Value)
		{
			if (StatusText.IsValid())
			{
				StatusText->SetText(FText::FromString(Value));
			}
		}

		void SetCaptureLoadPhase(const FString& Phase)
		{
			CaptureLoadPhase = Phase;
			if (bCaptureLoading)
			{
				const double Elapsed = FPlatformTime::Seconds() - CaptureLoadStartSeconds;
				SetStatus(FString::Printf(TEXT("正在载入截帧… %.1fs · %s"), Elapsed, *CaptureLoadPhase));
				UE_LOG(LogRenderTrailAnalyzer, Display, TEXT("Capture load phase: %s (elapsed=%.3fs)"), *Phase, Elapsed);
			}
		}

		void FinishCaptureLoad(const FString& Result)
		{
			if (!bCaptureLoading)
			{
				return;
			}
			const double Elapsed = FPlatformTime::Seconds() - CaptureLoadStartSeconds;
			bCaptureLoading = false;
			UE_LOG(LogRenderTrailAnalyzer, Display, TEXT("Capture load finished: elapsed=%.3fs result=%s capture='%s'"),
				Elapsed, *Result, *GetCapturePath());
		}

		void SetEvidence(const FString& Value)
		{
			SetReportCards(Value, TEXT("选择关注像素后生成。"), TEXT("尚无候选原因。"), Value);
		}

		void SetReportCards(const FString& Summary, const FString& CausalPath, const FString& Suspects,
			const FString& TechnicalEvidence)
		{
			LastReportSummary = Summary;
			LastReportCausalPath = CausalPath;
			if (SummaryText.IsValid())
				SummaryText->SetText(FText::FromString(Summary));
			if (CausalPathText.IsValid())
				CausalPathText->SetText(FText::FromString(CausalPath));
			if (SuspectsText.IsValid())
				SuspectsText->SetText(FText::FromString(Suspects));
			if (EvidenceText.IsValid())
				EvidenceText->SetText(FText::FromString(TechnicalEvidence));
		}

		void SetAgentOutputText(const FString& Value)
		{
			if (AgentOutputText.IsValid())
			{
				AgentOutputText->SetText(FText::FromString(Value));
			}
			TArray<TPair<FString, FString>> Sections;
			Sections.Add(TPair<FString, FString>(TEXT("Agent 输出"), Value));
			SetAgentReportSections(Sections, 0);
		}

		void SetAgentReportSections(const TArray<TPair<FString, FString>>& Sections, int32 InitiallyExpandedIndex)
		{
			if (!AgentReportBox.IsValid())
			{
				return;
			}

			AgentReportBox->ClearChildren();
			for (int32 Index = 0; Index < Sections.Num(); ++Index)
			{
				const FString& Title = Sections[Index].Key;
				const FString& Body = Sections[Index].Value;
				AgentReportBox->AddSlot()
				.AutoHeight()
				.Padding(0, 0, 0, 6)
				[
					SNew(SExpandableArea)
					.InitiallyCollapsed(Index != InitiallyExpandedIndex)
					.AllowAnimatedTransition(false)
					.BorderBackgroundColor(FLinearColor(0.10f, 0.12f, 0.16f, 1.0f))
					.BodyBorderBackgroundColor(FLinearColor(0.035f, 0.045f, 0.06f, 1.0f))
					.HeaderContent()
					[
						SNew(STextBlock)
						.Text(FText::FromString(Title))
						.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 11))
						.ColorAndOpacity(FLinearColor(0.82f, 0.88f, 0.96f))
					]
					.BodyContent()
					[
						SNew(SMultiLineEditableText)
						.Text(FText::FromString(Body.IsEmpty() ? TEXT("暂无证据") : Body))
						.AutoWrapText(true)
						.IsReadOnly(true)
						.AllowContextMenu(true)
						.ClearTextSelectionOnFocusLoss(false)
					]
				];
			}
		}

		FReply ToggleTechnicalEvidence()
		{
			bTechnicalEvidenceExpanded = !bTechnicalEvidenceExpanded;
			if (TechnicalEvidenceBox.IsValid())
			{
				TechnicalEvidenceBox->SetVisibility(bTechnicalEvidenceExpanded ? EVisibility::Visible : EVisibility::Collapsed);
			}
			if (TechnicalToggleText.IsValid())
			{
				TechnicalToggleText->SetText(FText::FromString(bTechnicalEvidenceExpanded
					? TEXT("▼  详细技术证据（Pass / Pipeline / Shader）")
					: TEXT("▶  详细技术证据（Pass / Pipeline / Shader）")));
			}
			return FReply::Handled();
		}

		void SetAgentStatus(const FString& Status)
		{
			if (AgentStatusText.IsValid())
			{
				AgentStatusText->SetText(FText::FromString(Status));
			}
		}

		static FString SerializeJson(const TSharedRef<FJsonObject>& Object)
		{
			FString Result;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
			FJsonSerializer::Serialize(Object, Writer);
			return Result;
		}

		static FString GetAgentLogPath()
		{
			return FPaths::Combine(FPaths::ProjectLogDir(), TEXT("RenderTrailAgent.log"));
		}

		static FString GetPluginDiagnosticsConfigPath()
		{
			if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("RenderTrail")))
			{
				return FPaths::ConvertRelativePathToFull(FPaths::Combine(
					Plugin->GetBaseDir(), TEXT("Config"), TEXT("RenderTrailDiagnostics.ini")));
			}
			return FString();
		}

		static bool ParseDiagnosticsBool(const FString& Value, bool DefaultValue)
		{
			if (Value.Equals(TEXT("true"), ESearchCase::IgnoreCase)
				|| Value.Equals(TEXT("1"), ESearchCase::CaseSensitive)
				|| Value.Equals(TEXT("yes"), ESearchCase::IgnoreCase)
				|| Value.Equals(TEXT("on"), ESearchCase::IgnoreCase))
			{
				return true;
			}
			if (Value.Equals(TEXT("false"), ESearchCase::IgnoreCase)
				|| Value.Equals(TEXT("0"), ESearchCase::CaseSensitive)
				|| Value.Equals(TEXT("no"), ESearchCase::IgnoreCase)
				|| Value.Equals(TEXT("off"), ESearchCase::IgnoreCase))
			{
				return false;
			}
			return DefaultValue;
		}

		static void ApplyDiagnosticsConfigFile(const FString& Path, FRenderTrailDiagnosticsOptions& Options)
		{
			FString Contents;
			if (!IFileManager::Get().FileExists(*Path) || !FFileHelper::LoadFileToString(Contents, *Path))
			{
				return;
			}

			TArray<FString> Lines;
			Contents.ParseIntoArrayLines(Lines, false);
			bool bInSection = false;
			for (FString Line : Lines)
			{
				Line.TrimStartAndEndInline();
				if (Line.IsEmpty() || Line.StartsWith(TEXT(";")) || Line.StartsWith(TEXT("#")))
				{
					continue;
				}
				if (Line.StartsWith(TEXT("[")) && Line.EndsWith(TEXT("]")))
				{
					bInSection = Line.Mid(1, Line.Len() - 2).Equals(TEXT("RenderTrailDiagnostics"), ESearchCase::IgnoreCase);
					continue;
				}
				if (!bInSection)
				{
					continue;
				}

				FString Key;
				FString Value;
				if (!Line.Split(TEXT("="), &Key, &Value))
				{
					continue;
				}
				Key.TrimStartAndEndInline();
				Value.TrimStartAndEndInline();
				if (Key.Equals(TEXT("bEnabled"), ESearchCase::IgnoreCase))
				{
					Options.bEnabled = ParseDiagnosticsBool(Value, Options.bEnabled);
				}
				else if (Key.Equals(TEXT("bWorkerProtocol"), ESearchCase::IgnoreCase))
				{
					Options.bWorkerProtocol = ParseDiagnosticsBool(Value, Options.bWorkerProtocol);
				}
				else if (Key.Equals(TEXT("bAgentTraffic"), ESearchCase::IgnoreCase))
				{
					Options.bAgentTraffic = ParseDiagnosticsBool(Value, Options.bAgentTraffic);
				}
				else if (Key.Equals(TEXT("bFullEvidencePayload"), ESearchCase::IgnoreCase))
				{
					Options.bFullEvidencePayload = ParseDiagnosticsBool(Value, Options.bFullEvidencePayload);
				}
			}
		}

		static FRenderTrailDiagnosticsOptions LoadDiagnosticsOptions()
		{
			FRenderTrailDiagnosticsOptions Options;
			// Load the plugin default first, then allow the project to override it.
			ApplyDiagnosticsConfigFile(GetPluginDiagnosticsConfigPath(), Options);
			ApplyDiagnosticsConfigFile(FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectConfigDir(), TEXT("RenderTrailDiagnostics.ini"))), Options);
			return Options;
		}

		static FString BoundAgentLogText(FString Text, int32 MaxChars = 16000)
		{
			if (Text.Len() > MaxChars)
			{
				const int32 Omitted = Text.Len() - MaxChars;
				Text.LeftInline(MaxChars);
				Text += FString::Printf(TEXT("\n... [%d chars omitted by RenderTrail log bound]"), Omitted);
			}
			return Text;
		}

		void WriteDiagnosticsRecord(const FString& Stage, const FString& Detail, bool bReset = false)
		{
			if (!DiagnosticsOptions.bEnabled || DiagnosticsFilePath.IsEmpty())
			{
				return;
			}
			const FString Line = FString::Printf(TEXT("[%s] [%s]\n%s\n\n"),
				*FDateTime::Now().ToIso8601(), *Stage, *Detail);
			FFileHelper::SaveStringToFile(Line, *DiagnosticsFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
				&IFileManager::Get(), bReset ? 0 : (FILEWRITE_Append | FILEWRITE_AllowRead));
		}

		void BeginDiagnosticsSession(const FString& CapturePath, int64 CaptureSize)
		{
			DiagnosticsFilePath.Empty();
			if (!DiagnosticsOptions.bEnabled)
			{
				return;
			}

			const FString Directory = FPaths::Combine(FPaths::ProjectLogDir(), TEXT("RenderTrailDiagnostics"));
			IFileManager::Get().MakeDirectory(*Directory, true);
			const FString CaptureBase = FPaths::GetBaseFilename(CapturePath);
			const int64 Timestamp = FDateTime::Now().ToUnixTimestamp();
			DiagnosticsFilePath = FPaths::Combine(Directory,
				FString::Printf(TEXT("RenderTrailDiagnostics_%s_%lld.log"), *CaptureBase, Timestamp));
			WriteDiagnosticsRecord(TEXT("session_start"), FString::Printf(
				TEXT("capture=%s\nbytes=%lld\nworkerProtocol=%s\nagentTraffic=%s\nfullEvidencePayload=%s"),
				*CapturePath, CaptureSize,
				DiagnosticsOptions.bWorkerProtocol ? TEXT("true") : TEXT("false"),
				DiagnosticsOptions.bAgentTraffic ? TEXT("true") : TEXT("false"),
				DiagnosticsOptions.bFullEvidencePayload ? TEXT("true") : TEXT("false")), true);
			UE_LOG(LogRenderTrailAnalyzer, Display, TEXT("RenderTrail full diagnostics: %s"), *DiagnosticsFilePath);
		}

		void WriteAgentLog(const FString& Stage, const FString& Detail, bool bReset = false)
		{
			const FString LogDirectory = FPaths::ProjectLogDir();
			IFileManager::Get().MakeDirectory(*LogDirectory, true);
			const FString Line = FString::Printf(TEXT("[%s] [%s]\n%s\n\n"),
				*FDateTime::Now().ToIso8601(), *Stage, *BoundAgentLogText(Detail));
			FFileHelper::SaveStringToFile(Line, *GetAgentLogPath(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
				&IFileManager::Get(), bReset ? 0 : (FILEWRITE_Append | FILEWRITE_AllowRead));
			if (DiagnosticsOptions.bAgentTraffic)
			{
				WriteDiagnosticsRecord(FString::Printf(TEXT("agent_%s"), *Stage), Detail);
			}
			UE_LOG(LogRenderTrailAnalyzer, Display, TEXT("Agent[%s]: %s"), *Stage, *BoundAgentLogText(Detail, 1200));
		}

		void AddAgentMessage(const FString& Role, const FString& Content)
		{
			AddAgentMessage(Role, Content, FString());
		}

		void AddAgentMessage(const FString& Role, const FString& Content, const FString& ReasoningContent)
		{
			TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
			Message->SetStringField(TEXT("role"), Role);
			Message->SetStringField(TEXT("content"), Content);
			if (!ReasoningContent.IsEmpty())
			{
				Message->SetStringField(TEXT("reasoning_content"), ReasoningContent);
			}
			AgentMessages.Add(MakeShared<FJsonValueObject>(Message));
		}

		static bool LoadAgentPromptIni(const FString& Path, FString& OutPrompt, int32& OutLineCount)
		{
			OutPrompt.Empty();
			OutLineCount = 0;
			FString Contents;
			if (!FFileHelper::LoadFileToString(Contents, *Path))
			{
				return false;
			}

			TArray<FString> Lines;
			Contents.ParseIntoArrayLines(Lines, false);
			bool bInPromptSection = false;
			for (FString Line : Lines)
			{
				Line.TrimStartAndEndInline();
				if (Line.IsEmpty() || Line.StartsWith(TEXT(";")) || Line.StartsWith(TEXT("#")))
				{
					continue;
				}
				if (Line.StartsWith(TEXT("[")) && Line.EndsWith(TEXT("]")))
				{
					bInPromptSection = Line.Mid(1, Line.Len() - 2).Equals(TEXT("RenderTrailAgentPrompt"), ESearchCase::IgnoreCase);
					continue;
				}
				if (!bInPromptSection)
				{
					continue;
				}

				int32 Separator = INDEX_NONE;
				if (Line.StartsWith(TEXT("+Line=")))
				{
					Separator = 6;
				}
				else if (Line.StartsWith(TEXT("Line=")))
				{
					Separator = 5;
				}
				if (Separator != INDEX_NONE)
				{
					FString Value = Line.Mid(Separator);
					Value.TrimStartAndEndInline();
					if (!Value.IsEmpty())
					{
						if (!OutPrompt.IsEmpty())
						{
							OutPrompt += TEXT("\n");
						}
						OutPrompt += Value;
						++OutLineCount;
					}
					continue;
				}

				if (Line.StartsWith(TEXT("Prompt=")))
				{
					OutPrompt = Line.Mid(7).TrimStartAndEnd();
				}
			}
			return !OutPrompt.IsEmpty();
		}

		FString BuildAgentSystemPrompt() const
		{
			TArray<FString> IniPromptPaths;
			const FString ProjectOverridePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectConfigDir(), TEXT("RenderTrailAgentPrompt.ini")));
			IniPromptPaths.Add(ProjectOverridePath);

			FString PluginOverridePath;
			if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("RenderTrail")))
			{
				PluginOverridePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
					Plugin->GetBaseDir(), TEXT("Config"), TEXT("RenderTrailAgentPrompt.ini")));
				IniPromptPaths.Add(PluginOverridePath);
			}

			for (const FString& PromptPath : IniPromptPaths)
			{
				if (!IFileManager::Get().FileExists(*PromptPath))
				{
					continue;
				}

				FString Prompt;
				int32 PromptLineCount = 0;
				if (LoadAgentPromptIni(PromptPath, Prompt, PromptLineCount))
				{
					UE_LOG(LogRenderTrailAnalyzer, Display,
						TEXT("Loaded Agent system prompt from INI: path='%s' lines=%d chars=%d"),
						*PromptPath, PromptLineCount, Prompt.Len());
					return Prompt;
				}
			}

			// Keep the previous text-file format as a migration fallback for existing projects.
			TArray<FString> LegacyPromptPaths;
			LegacyPromptPaths.Add(FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("RenderTrailAgentPrompt.txt")));
			if (!PluginOverridePath.IsEmpty())
			{
				LegacyPromptPaths.Add(FPaths::Combine(FPaths::GetPath(PluginOverridePath), TEXT("RenderTrailAgentPrompt.txt")));
			}
			for (const FString& PromptPath : LegacyPromptPaths)
			{
				FString Prompt;
				if (IFileManager::Get().FileExists(*PromptPath) && FFileHelper::LoadFileToString(Prompt, *PromptPath))
				{
					Prompt.TrimStartAndEndInline();
					if (!Prompt.IsEmpty())
					{
						UE_LOG(LogRenderTrailAnalyzer, Display,
							TEXT("Loaded legacy text Agent system prompt: path='%s' chars=%d"), *PromptPath, Prompt.Len());
						return Prompt;
					}
				}
			}

			UE_LOG(LogRenderTrailAnalyzer, Warning,
				TEXT("Agent system prompt INI was not found or empty; using safe fallback. Expected project='%s' plugin='%s'."),
				*ProjectOverridePath, *PluginOverridePath);
			return TEXT("You are RenderTrail's read-only selected-pixel forensics agent. Respond in Chinese with exactly one finish JSON object. Use only supplied RenderDoc evidence, answer HUMAN_REQUEST directly, distinguish observed GPU formation from unknown upstream causes, never invent shader algorithms or shadow state, and list missing evidence in unknowns.");
		}

		FString BuildAgentPrefilterEvidence() const
		{
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("evidenceType"), TEXT("prefiltered_pixel_forensics"));
			Root->SetStringField(TEXT("capture"), FPaths::GetCleanFilename(GetCapturePath()));
			Root->SetStringField(TEXT("ruleSummary"), LastReportSummary);
			Root->SetStringField(TEXT("boundedCausalPath"), LastReportCausalPath);

			FString MetadataJson;
			UE::RenderTrail::FCaptureMetadata Metadata;
			FString MetadataError;
			if (FFileHelper::LoadFileToString(MetadataJson, *UE::RenderTrail::GetMetadataPathForCapture(GetCapturePath()))
				&& UE::RenderTrail::FCaptureMetadata::FromJson(MetadataJson, Metadata, MetadataError))
			{
				TSharedRef<FJsonObject> Context = MakeShared<FJsonObject>();
				Context->SetStringField(TEXT("project"), Metadata.ProjectName);
				Context->SetStringField(TEXT("map"), Metadata.MapName);
				Context->SetStringField(TEXT("engine"), Metadata.EngineVersion);
				Context->SetBoolField(TEXT("pie"), Metadata.bIsPIE);
				Root->SetObjectField(TEXT("ueContext"), Context);
			}

			TArray<TSharedPtr<FJsonValue>> SampleValues;
			for (int32 SampleIndex = 0; SampleIndex < Samples.Num(); ++SampleIndex)
			{
				const FPixelSample& Sample = Samples[SampleIndex];
				TSharedRef<FJsonObject> SampleJson = MakeShared<FJsonObject>();
				const FString Label = FString::Printf(TEXT("P%d"), SampleIndex + 1);
				SampleJson->SetStringField(TEXT("label"), Label);
				SampleJson->SetStringField(TEXT("role"), TEXT("point_of_interest"));
				SampleJson->SetNumberField(TEXT("x"), Sample.Pixel.X);
				SampleJson->SetNumberField(TEXT("y"), Sample.Pixel.Y);
				SampleJson->SetNumberField(TEXT("modificationCount"), Sample.TotalModifications);
				const TArray<FEventEvidence> Events = AggregateEvents(Sample);
				const FString FinalObservedValue = !Sample.Modifications.IsEmpty()
					? Sample.Modifications.Last().After
					: (!Events.IsEmpty() ? Events.Last().After : TEXT("unavailable"));
				SampleJson->SetStringField(TEXT("finalObservedValue"), FinalObservedValue);
				SampleJson->SetBoolField(TEXT("detailTailTruncated"), Sample.bTruncated);

				TArray<TSharedPtr<FJsonValue>> EventValues;
				const int32 First = FMath::Max(0, Events.Num() - MaxAgentPrefilterEventsPerSample);
				for (int32 Index = Events.Num() - 1; Index >= First; --Index)
				{
					const FEventEvidence& Event = Events[Index];
					TSharedRef<FJsonObject> EventJson = MakeShared<FJsonObject>();
					EventJson->SetNumberField(TEXT("eventId"), Event.EventId);
					EventJson->SetStringField(TEXT("kind"), Event.ActionKind);
					EventJson->SetStringField(TEXT("action"), Event.Action);
					EventJson->SetStringField(TEXT("marker"), CompactMarkerPath(Event.MarkerPath));
					EventJson->SetNumberField(TEXT("actionFlags"), Event.ActionFlags);
					EventJson->SetStringField(TEXT("result"), DescribeEventResult(Event));
					EventJson->SetNumberField(TEXT("passedFragments"), Event.PassedFragments);
					EventJson->SetNumberField(TEXT("rejectedFragments"), Event.RejectedFragments);
					EventValues.Add(MakeShared<FJsonValueObject>(EventJson));
				}
				SampleJson->SetArrayField(TEXT("latestRelevantEvents"), EventValues);
				TArray<TSharedPtr<FJsonValue>> CompleteEventChain;
				const int32 AgentChainFirst = FMath::Max(0, Events.Num() - MaxAgentEventChainPerSample);
				CompleteEventChain.Reserve(Events.Num() - AgentChainFirst);
				for (int32 EventIndex = AgentChainFirst; EventIndex < Events.Num(); ++EventIndex)
				{
					const FEventEvidence& Event = Events[EventIndex];
					TSharedRef<FJsonObject> EventJson = MakeShared<FJsonObject>();
					EventJson->SetNumberField(TEXT("eventId"), Event.EventId);
					EventJson->SetStringField(TEXT("kind"), Event.ActionKind);
					EventJson->SetStringField(TEXT("action"), Event.Action);
					EventJson->SetStringField(TEXT("marker"), CompactMarkerPath(Event.MarkerPath));
					EventJson->SetNumberField(TEXT("actionFlags"), Event.ActionFlags);
					EventJson->SetStringField(TEXT("semantics"), ClassifySemantics(Event));
					EventJson->SetStringField(TEXT("result"), DescribeEventResult(Event));
					EventJson->SetStringField(TEXT("before"), Event.Before);
					EventJson->SetStringField(TEXT("shaderOutput"), Event.ShaderOutput);
					EventJson->SetStringField(TEXT("after"), Event.After);
					EventJson->SetNumberField(TEXT("passedFragments"), Event.PassedFragments);
					EventJson->SetNumberField(TEXT("rejectedFragments"), Event.RejectedFragments);
					EventJson->SetBoolField(TEXT("directShaderWrite"), Event.bDirectShaderWrite);
					EventJson->SetBoolField(TEXT("changedTextureValue"), Event.bChangedTextureValue);
					EventJson->SetBoolField(TEXT("hasPrimitiveEvidence"), Event.bHasPrimitiveEvidence);
					EventJson->SetNumberField(TEXT("primitiveId"), Event.PrimitiveId);
					TArray<TSharedPtr<FJsonValue>> FailureValues;
					for (const FString& Failure : Event.FailureReasons)
					{
						FailureValues.Add(MakeShared<FJsonValueString>(Failure));
					}
					EventJson->SetArrayField(TEXT("failureReasons"), MoveTemp(FailureValues));
					CompleteEventChain.Add(MakeShared<FJsonValueObject>(EventJson));
				}
				SampleJson->SetArrayField(TEXT("completeEventChain"), MoveTemp(CompleteEventChain));
				SampleJson->SetBoolField(TEXT("eventChainComplete"), Sample.bEventSummaryComplete && AgentChainFirst == 0);
				SampleJson->SetNumberField(TEXT("eventChainEventCount"), Events.Num());
				SampleJson->SetNumberField(TEXT("eventChainStartIndex"), AgentChainFirst);
				SampleValues.Add(MakeShared<FJsonValueObject>(SampleJson));
			}
			Root->SetArrayField(TEXT("samples"), SampleValues);

			if (LastCandidate.IsSet())
			{
				const FCausalCandidate& Candidate = LastCandidate.GetValue();
				TSharedRef<FJsonObject> CandidateJson = MakeShared<FJsonObject>();
				CandidateJson->SetNumberField(TEXT("eventId"), Candidate.Event.EventId);
				CandidateJson->SetStringField(TEXT("action"), Candidate.Event.Action);
				CandidateJson->SetStringField(TEXT("kind"), Candidate.Event.ActionKind);
				CandidateJson->SetStringField(TEXT("semantics"), ClassifySemantics(Candidate.Event));
				CandidateJson->SetStringField(TEXT("marker"), CompactMarkerPath(Candidate.Event.MarkerPath));
				CandidateJson->SetStringField(TEXT("result"), DescribeEventResult(Candidate.Event));
				CandidateJson->SetBoolField(TEXT("pointDivergence"), bLastCandidateHasDivergence);
				CandidateJson->SetNumberField(TEXT("sampleCoverage"), Candidate.SampleCoverage);
				Root->SetObjectField(TEXT("candidate"), CandidateJson);
			}

			TArray<TSharedPtr<FJsonValue>> DeterministicContexts;
			for (const TPair<uint32, FEventContextEvidence>& Pair : EventContexts)
			{
				const FEventContextEvidence& Context = Pair.Value;
				TSharedRef<FJsonObject> ContextJson = MakeShared<FJsonObject>();
				ContextJson->SetNumberField(TEXT("eventId"), Context.EventId);
				ContextJson->SetStringField(TEXT("shaderStage"), Context.ShaderStage);
				ContextJson->SetStringField(TEXT("shaderEntry"), Context.ShaderEntry);
				ContextJson->SetStringField(TEXT("shaderDebugStatus"), Context.ShaderDebugStatus);
				ContextJson->SetBoolField(TEXT("shaderDebuggable"), Context.bShaderDebuggable);
				ContextJson->SetBoolField(TEXT("sourceSymbols"), Context.bSourceDebugInfo);
				ContextJson->SetStringField(TEXT("shaderEncoding"), Context.ShaderEncoding);
				ContextJson->SetNumberField(TEXT("inputSignatureCount"), Context.ShaderInputSignatureCount);
				ContextJson->SetNumberField(TEXT("outputSignatureCount"), Context.ShaderOutputSignatureCount);
				ContextJson->SetNumberField(TEXT("constantBlockCount"), Context.ShaderConstantBlockCount);
				ContextJson->SetNumberField(TEXT("samplerCount"), Context.ShaderSamplerCount);
				ContextJson->SetNumberField(TEXT("readOnlyResourceCount"), Context.ShaderReadOnlyResourceCount);
				ContextJson->SetNumberField(TEXT("readWriteResourceCount"), Context.ShaderReadWriteResourceCount);
				if (Context.PipelineState.IsValid())
				{
					ContextJson->SetObjectField(TEXT("fixedFunctionState"), Context.PipelineState);
				}
				if (Context.ShaderDebugTrace.IsValid())
				{
					ContextJson->SetObjectField(TEXT("shaderDebugTrace"), Context.ShaderDebugTrace);
				}
				TArray<TSharedPtr<FJsonValue>> Inputs;
				for (const FBoundResourceEvidence& Input : Context.Inputs)
				{
					TSharedRef<FJsonObject> Resource = MakeShared<FJsonObject>();
					Resource->SetStringField(TEXT("name"), Input.Name);
					Resource->SetStringField(TEXT("stage"), Input.Stage);
					Resource->SetStringField(TEXT("access"), Input.Access);
					Resource->SetNumberField(TEXT("width"), Input.Width);
					Resource->SetNumberField(TEXT("height"), Input.Height);
					Inputs.Add(MakeShared<FJsonValueObject>(Resource));
				}
				ContextJson->SetArrayField(TEXT("inputs"), MoveTemp(Inputs));
				TArray<TSharedPtr<FJsonValue>> Outputs;
				for (const FBoundResourceEvidence& Output : Context.Outputs)
				{
					TSharedRef<FJsonObject> Resource = MakeShared<FJsonObject>();
					Resource->SetStringField(TEXT("name"), Output.Name);
					Resource->SetStringField(TEXT("stage"), Output.Stage);
					Resource->SetStringField(TEXT("access"), Output.Access);
					Resource->SetNumberField(TEXT("width"), Output.Width);
					Resource->SetNumberField(TEXT("height"), Output.Height);
					Outputs.Add(MakeShared<FJsonValueObject>(Resource));
				}
				ContextJson->SetArrayField(TEXT("outputs"), MoveTemp(Outputs));
				ContextJson->SetArrayField(TEXT("resourceProvenance"), Context.ResourceProvenance);
				DeterministicContexts.Add(MakeShared<FJsonValueObject>(ContextJson));
			}
			Root->SetArrayField(TEXT("deterministicEventContexts"), MoveTemp(DeterministicContexts));
			Root->SetBoolField(TEXT("deterministicContextCollectionComplete"), PendingEventContextIds.IsEmpty());
			Root->SetNumberField(TEXT("deterministicContextFailureCount"), FailedEventContextIds.Num());
			Root->SetNumberField(TEXT("shaderDebugFailureCount"), FailedShaderDebugIds.Num());
			Root->SetNumberField(TEXT("deterministicContextLimit"), MaxDeterministicContextEvents);
			Root->SetNumberField(TEXT("agentEventChainLimitPerSample"), MaxAgentEventChainPerSample);
			return SerializeJson(Root);
		}

		bool AgentEvidenceContainsEvent(uint32 EventId) const
		{
			for (const FPixelSample& Sample : Samples)
			{
				if (Sample.Modifications.ContainsByPredicate([EventId](const FPixelModificationEvidence& Item) { return Item.EventId == EventId; }))
				{
					return true;
				}
			}
			return false;
		}

		FString BuildAgentEventObservation(uint32 EventId) const
		{
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("tool"), TEXT("inspect_event"));
			Root->SetNumberField(TEXT("eventId"), EventId);
			TArray<TSharedPtr<FJsonValue>> Outcomes;
			for (int32 SampleIndex = 0; SampleIndex < Samples.Num(); ++SampleIndex)
			{
				const FPixelSample& Sample = Samples[SampleIndex];
				const FString Label = FString::Printf(TEXT("P%d"), SampleIndex + 1);
				const TArray<FEventEvidence> Events = AggregateEvents(Sample);
				if (const FEventEvidence* Event = FindEvent(Events, EventId))
				{
					TSharedRef<FJsonObject> Outcome = MakeShared<FJsonObject>();
					Outcome->SetStringField(TEXT("sample"), Label);
					Outcome->SetStringField(TEXT("action"), Event->Action);
					Outcome->SetStringField(TEXT("kind"), Event->ActionKind);
					Outcome->SetStringField(TEXT("marker"), CompactMarkerPath(Event->MarkerPath));
					Outcome->SetStringField(TEXT("result"), DescribeEventResult(*Event));
					Outcome->SetStringField(TEXT("before"), Event->Before);
					Outcome->SetStringField(TEXT("shaderOutput"), Event->ShaderOutput);
					Outcome->SetStringField(TEXT("after"), Event->After);
					Outcomes.Add(MakeShared<FJsonValueObject>(Outcome));
				}
			}
			Root->SetArrayField(TEXT("sampleOutcomes"), Outcomes);

			if (const FEventContextEvidence* Context = EventContexts.Find(EventId))
			{
				TSharedRef<FJsonObject> Pipeline = MakeShared<FJsonObject>();
				Pipeline->SetStringField(TEXT("shaderStage"), Context->ShaderStage);
				Pipeline->SetStringField(TEXT("shaderEntry"), Context->ShaderEntry);
				Pipeline->SetBoolField(TEXT("shaderDebuggable"), Context->bShaderDebuggable);
				Pipeline->SetBoolField(TEXT("sourceSymbols"), Context->bSourceDebugInfo);
				TSharedRef<FJsonObject> Reflection = MakeShared<FJsonObject>();
				Reflection->SetStringField(TEXT("encoding"), Context->ShaderEncoding);
				Reflection->SetNumberField(TEXT("inputSignatureCount"), Context->ShaderInputSignatureCount);
				Reflection->SetNumberField(TEXT("outputSignatureCount"), Context->ShaderOutputSignatureCount);
				Reflection->SetNumberField(TEXT("constantBlockCount"), Context->ShaderConstantBlockCount);
				Reflection->SetNumberField(TEXT("samplerCount"), Context->ShaderSamplerCount);
				Reflection->SetNumberField(TEXT("readOnlyResourceCount"), Context->ShaderReadOnlyResourceCount);
				Reflection->SetNumberField(TEXT("readWriteResourceCount"), Context->ShaderReadWriteResourceCount);
				Pipeline->SetObjectField(TEXT("shaderReflection"), Reflection);
				Pipeline->SetStringField(TEXT("algorithmEvidence"),
					(Context->bShaderDebuggable && Context->bSourceDebugInfo)
						? TEXT("source/debug information exists; instruction trace not executed")
						: TEXT("not proven from reflection and bindings"));
				TArray<TSharedPtr<FJsonValue>> Inputs;
				for (int32 Index = 0; Index < Context->Inputs.Num() && Index < MaxDisplayedFrontierResources; ++Index)
				{
					const FBoundResourceEvidence& Input = Context->Inputs[Index];
					TSharedRef<FJsonObject> Resource = MakeShared<FJsonObject>();
					Resource->SetStringField(TEXT("name"), Input.Name);
					Resource->SetStringField(TEXT("stage"), Input.Stage);
					Resource->SetStringField(TEXT("access"), Input.Access);
					Resource->SetNumberField(TEXT("width"), Input.Width);
					Resource->SetNumberField(TEXT("height"), Input.Height);
					Inputs.Add(MakeShared<FJsonValueObject>(Resource));
				}
				Pipeline->SetArrayField(TEXT("usedInputs"), Inputs);
				TArray<TSharedPtr<FJsonValue>> Outputs;
				for (int32 Index = 0; Index < Context->Outputs.Num() && Index < MaxDisplayedFrontierResources; ++Index)
				{
					const FBoundResourceEvidence& Output = Context->Outputs[Index];
					TSharedRef<FJsonObject> Resource = MakeShared<FJsonObject>();
					Resource->SetStringField(TEXT("name"), Output.Name);
					Resource->SetStringField(TEXT("stage"), Output.Stage);
					Resource->SetStringField(TEXT("access"), Output.Access);
					Resource->SetNumberField(TEXT("width"), Output.Width);
					Resource->SetNumberField(TEXT("height"), Output.Height);
					Outputs.Add(MakeShared<FJsonValueObject>(Resource));
				}
				Pipeline->SetArrayField(TEXT("usedOutputs"), Outputs);
				Pipeline->SetArrayField(TEXT("resourceProvenance"), Context->ResourceProvenance);
				if (Context->PipelineState.IsValid())
				{
					Pipeline->SetObjectField(TEXT("fixedFunctionState"), Context->PipelineState);
				}
				if (Context->ShaderDebugTrace.IsValid())
				{
					Pipeline->SetObjectField(TEXT("shaderDebugTrace"), Context->ShaderDebugTrace);
				}
				Root->SetObjectField(TEXT("pipeline"), Pipeline);
			}
			else
			{
				Root->SetStringField(TEXT("pipeline"), FailedEventContextIds.Contains(EventId) ? TEXT("query_failed") : TEXT("not_loaded"));
			}
			return SerializeJson(Root);
		}

		const FPixelSample* FindAgentSample(const FString& Label) const
		{
			if (!Label.StartsWith(TEXT("P"), ESearchCase::IgnoreCase))
			{
				return nullptr;
			}
			int32 OneBasedIndex = 0;
			if (!LexTryParseString(OneBasedIndex, *Label.Mid(1)) || !Samples.IsValidIndex(OneBasedIndex - 1))
			{
				return nullptr;
			}
			return &Samples[OneBasedIndex - 1];
		}

		FString BuildAgentSampleObservation(const FString& Label) const
		{
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("tool"), TEXT("inspect_sample"));
			Root->SetStringField(TEXT("sample"), Label);
			const FPixelSample* Sample = FindAgentSample(Label);
			if (!Sample)
			{
				Root->SetStringField(TEXT("error"), TEXT("unknown sample label"));
				return SerializeJson(Root);
			}

			Root->SetNumberField(TEXT("x"), Sample->Pixel.X);
			Root->SetNumberField(TEXT("y"), Sample->Pixel.Y);
			Root->SetStringField(TEXT("finalObservedValue"), Sample->Modifications.IsEmpty() ? TEXT("unavailable") : Sample->Modifications.Last().After);
			const TArray<FEventEvidence> Events = AggregateEvents(*Sample);
			TArray<TSharedPtr<FJsonValue>> History;
			const int32 First = FMath::Max(0, Events.Num() - MaxDisplayedTraceHops);
			for (int32 Index = Events.Num() - 1; Index >= First; --Index)
			{
				const FEventEvidence& Event = Events[Index];
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("eventId"), Event.EventId);
				Item->SetStringField(TEXT("kind"), Event.ActionKind);
				Item->SetStringField(TEXT("action"), Event.Action);
				Item->SetStringField(TEXT("marker"), CompactMarkerPath(Event.MarkerPath));
				Item->SetStringField(TEXT("result"), DescribeEventResult(Event));
				History.Add(MakeShared<FJsonValueObject>(Item));
			}
			Root->SetArrayField(TEXT("latestFirstHistory"), History);
			return SerializeJson(Root);
		}

		FReply StartAgentAnalysis()
		{
			if (bAgentRunning)
			{
				SetAgentStatus(TEXT("Agent 正在运行，请等待当前有界循环完成。"));
				return FReply::Handled();
			}
			if (bReplaySynchronizationPending)
			{
				SetAgentStatus(TEXT("分析正在等待 Replay 同步；完成 ReplayController、目标 RT 和 Pixel History 后才能运行 Agent。"));
				return FReply::Handled();
			}
			if (!bSelectionConfirmed)
			{
				SetAgentStatus(TEXT("选点已变化，请先点击“读取 Pixel History”。"));
				return FReply::Handled();
			}
			if (Samples.ContainsByPredicate([](const FPixelSample& Sample)
			{
				return Sample.bPending;
			}))
			{
				SetAgentStatus(TEXT("Pixel History 尚未完成，请等待查询结束。"));
				return FReply::Handled();
			}
			const bool bHasReadyPoint = Samples.ContainsByPredicate([](const FPixelSample& Sample)
			{
				return !Sample.bPending && !Sample.bFailed;
			});
			if (!bHasReadyPoint)
			{
				SetAgentStatus(TEXT("先选择至少一个完成查询的关注像素，再运行 Agent。"));
				return FReply::Handled();
			}
			if (Samples.ContainsByPredicate([](const FPixelSample& Sample)
			{
				return !Sample.bPending && !Sample.bFailed && !Sample.bEventSummaryComplete;
			}))
			{
				SetAgentStatus(TEXT("当前 Replay Worker 未返回完整 eventSummaries；已暂停 Agent，请先重新编译并重载 Worker。"));
				return FReply::Handled();
			}
			EnsureRelevantEventContexts();
			EnsureCandidateShaderDebug();
			if (PendingEventContextIds.Num() > 0 || PendingShaderDebugByRequest.Num() > 0)
			{
				bAgentWaitingForDeterministicContexts = true;
				SetAgentStatus(FString::Printf(TEXT("正在收集确定性溯源上下文（%d 个事件、%d 个 Shader 调试任务待查询）；完成后再进行语义提炼。"),
					PendingEventContextIds.Num(), PendingShaderDebugByRequest.Num()));
				return FReply::Handled();
			}

			AgentMessages.Empty();
			AgentStep = 0;
			AgentPendingEventId.Reset();
			bAgentRunning = true;
			if (AgentRunButtonText.IsValid())
				AgentRunButtonText->SetText(FText::FromString(TEXT("Agent 运行中…")));
			SetAgentOutputText(TEXT("确定性溯源已完成，Agent 只负责整理已收集的证据。"));
			const FString PrefilterEvidence = BuildAgentPrefilterEvidence();
			WriteAgentLog(TEXT("RunStart"), FString::Printf(
				TEXT("Bounded agent run started. samples=%d, evidenceChars=%d, maxTurns=%d\nPREFILTERED_EVIDENCE\n%s"),
				Samples.Num(), PrefilterEvidence.Len(), MaxAgentSteps, *PrefilterEvidence), true);
			FString HumanRequest = AgentIntentTextBox.IsValid()
				? AgentIntentTextBox->GetText().ToString().TrimStartAndEnd()
				: FString();
			if (HumanRequest.IsEmpty())
			{
				HumanRequest = TEXT("请基于当前确定性证据，整理选中像素的最终形成原因、关键 Pass、Pipeline 和 Shader 证据。");
			}
			const FString UserMessage = FString(TEXT("HUMAN_REQUEST\n")) + HumanRequest
				+ TEXT("\n\nPREFILTERED_EVIDENCE\n") + PrefilterEvidence;
			WriteAgentLog(TEXT("HumanRequest"), HumanRequest);
			AddAgentMessage(TEXT("system"), BuildAgentSystemPrompt());
			AddAgentMessage(TEXT("user"), UserMessage);
			SendAgentTurn();
			return FReply::Handled();
		}

		FReply RunPrimaryAnalysis()
		{
			if (!bSelectionConfirmed)
			{
				return ConfirmPixelSelection();
			}
			if (bAgentRunning)
			{
				return StartAgentAnalysis();
			}
			if (Samples.ContainsByPredicate([](const FPixelSample& Sample)
			{
				return Sample.bPending;
			}))
			{
				SetAgentStatus(TEXT("规则分析尚未完成，请等待 Pixel History 查询结束。"));
				return FReply::Handled();
			}
			return StartAgentAnalysis();
		}

		void SendAgentTurn()
		{
			if (!bAgentRunning)
				return;
			if (AgentStep >= MaxAgentSteps)
			{
				FinishAgentWithError(TEXT("Agent 达到语义整理轮次上限但没有给出 finish；已停止，避免无界查询。"));
				return;
			}
			++AgentStep;
			if (AgentStep == MaxAgentSteps)
			{
				AddAgentMessage(TEXT("user"), TEXT("FINAL TURN: tools are now disabled. Return action=finish using only accumulated evidence and list every unresolved fact in unknowns."));
			}
			SendAgentBrokerCompletion();
		}

		void DeferAgentContinuation(TFunction<void(SAnalyzerHome&)>&& Continuation)
		{
			const TWeakPtr<SAnalyzerHome> WeakThis = SharedThis(this);
			const TSharedPtr<TFunction<void(SAnalyzerHome&)>> Deferred =
				MakeShared<TFunction<void(SAnalyzerHome&)>>(MoveTemp(Continuation));
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([WeakThis, Deferred](float)
				{
					if (const TSharedPtr<SAnalyzerHome> Pinned = WeakThis.Pin())
					{
						(*Deferred)(*Pinned);
					}
					return false;
				}));
		}

		bool QueueAgentProcessRequest(const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& Request, const FString& FailureMessage)
		{
			DeferAgentContinuation([Request, FailureMessage](SAnalyzerHome& Analyzer)
			{
				// A user cancellation or a completed turn can replace AgentRequest before
				// the next ticker callback runs. Do not resurrect that stale request.
				if (!Analyzer.bAgentRunning || Analyzer.AgentRequest != Request)
				{
					return;
				}
				if (!Request->ProcessRequest() && !FailureMessage.IsEmpty())
				{
					Analyzer.FinishAgentWithError(FailureMessage);
				}
			});
			return true;
		}

		void QueueAgentCancelRequest(const FHttpRequestPtr& Request)
		{
			if (!Request.IsValid())
			{
				return;
			}
			DeferAgentContinuation([Request](SAnalyzerHome&)
			{
				Request->CancelRequest();
			});
		}

	#if 0
		void SendAgentBrokerInitialized()
		{
			if (!bAgentRunning)
			{
				AgentRequest.Reset();
				return;
			}
			AgentRequest.Reset();
			TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
			Body->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
			Body->SetStringField(TEXT("method"), TEXT("notifications/initialized"));
			Body->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
			AgentRequest = FHttpModule::Get().CreateRequest();
			ConfigureMcpRequest(AgentRequest.ToSharedRef());
			AgentRequest->SetVerb(TEXT("POST"));
			AgentRequest->SetContentAsString(SerializeJson(Body));
			AgentRequest->OnProcessRequestComplete().BindSP(this, &SAnalyzerHome::HandleAgentBrokerInitialized);
			if (!QueueAgentProcessRequest(AgentRequest.ToSharedRef(), TEXT("Unreal MCP request could not be queued.")))
			{
				FinishAgentWithError(TEXT("无法完成 Unreal MCP 会话初始化。"));
			}
		}

		void BeginAgentBrokerSession()
		{
			TSharedRef<FJsonObject> ClientInfo = MakeShared<FJsonObject>();
			ClientInfo->SetStringField(TEXT("name"), TEXT("RenderTrailAnalyzer"));
			ClientInfo->SetStringField(TEXT("version"), TEXT("0.2.0"));
			TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("protocolVersion"), TEXT("2025-11-25"));
			Params->SetObjectField(TEXT("capabilities"), MakeShared<FJsonObject>());
			Params->SetObjectField(TEXT("clientInfo"), ClientInfo);
			TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
			Body->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
			Body->SetNumberField(TEXT("id"), 1);
			Body->SetStringField(TEXT("method"), TEXT("initialize"));
			Body->SetObjectField(TEXT("params"), Params);

			WriteAgentLog(TEXT("McpInitializeRequest"), FString::Printf(TEXT("url=%s"), *AgentBrokerUrl));
			AgentRequest = FHttpModule::Get().CreateRequest();
			ConfigureMcpRequest(AgentRequest.ToSharedRef());
			AgentRequest->SetVerb(TEXT("POST"));
			AgentRequest->SetContentAsString(SerializeJson(Body));
			AgentRequest->OnProcessRequestComplete().BindSP(this, &SAnalyzerHome::HandleAgentBrokerInitialize);
			SetAgentStatus(FString::Printf(TEXT("Agent 第 %d/%d 轮 · 正在连接编辑器内 Unreal MCP…"), AgentStep, MaxAgentSteps));
			if (!QueueAgentProcessRequest(AgentRequest.ToSharedRef(), TEXT("Unreal MCP request could not be queued.")))
			{
				FinishAgentWithError(TEXT("无法连接 Unreal MCP。请确认编辑器仍在运行。"));
			}
		}

		void HandleAgentBrokerInitialize(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded)
		{
			if (!bAgentRunning || Request != AgentRequest)
				return;
			if (!bSucceeded || !Response.IsValid() || Response->GetResponseCode() < 200 || Response->GetResponseCode() >= 300)
			{
				FinishAgentWithError(TEXT("Unreal MCP 不在线。保持 Unreal Editor 打开后重试。"));
				return;
			}

			WriteAgentLog(TEXT("McpInitializeResponse"), FString::Printf(TEXT("http=%d body=%s"),
				Response->GetResponseCode(), *Response->GetContentAsString()));
			AgentMcpSessionId = Response->GetHeader(TEXT("Mcp-Session-Id"));
			TSharedPtr<FJsonObject> Root;
			const TSharedPtr<FJsonObject>* Result = nullptr;
			if (AgentMcpSessionId.IsEmpty() || !ParseMcpResponse(Response->GetContentAsString(), Root)
				|| !Root->TryGetObjectField(TEXT("result"), Result) || !Result || !Result->IsValid()
				|| !(*Result)->TryGetStringField(TEXT("protocolVersion"), AgentMcpProtocolVersion))
			{
				FinishAgentWithError(TEXT("Unreal MCP initialize 响应无效。"));
				return;
			}

			SendAgentBrokerInitialized();
		}

		void HandleAgentBrokerInitialized(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded)
		{
			if (!bAgentRunning || Request != AgentRequest)
				return;
			AgentRequest.Reset();
			if (!bSucceeded || !Response.IsValid() || Response->GetResponseCode() < 200 || Response->GetResponseCode() >= 300)
			{
				FinishAgentWithError(TEXT("Unreal MCP 拒绝了 initialized 通知。"));
				return;
			}
			SendAgentBrokerCompletion();
		}

		#endif

		void SendAgentBrokerCompletion()
		{
			const URenderTrailOwnedModelSettings* Settings = GetDefault<URenderTrailOwnedModelSettings>();
			const FString EndpointOverride = FPlatformMisc::GetEnvironmentVariable(TEXT("RENDERTRAIL_MODEL_ENDPOINT")).TrimStartAndEnd();
			const FString ModelOverride = FPlatformMisc::GetEnvironmentVariable(TEXT("RENDERTRAIL_MODEL_NAME")).TrimStartAndEnd();
			const FString ApiKeyOverride = FPlatformMisc::GetEnvironmentVariable(TEXT("RENDERTRAIL_MODEL_API_KEY")).TrimStartAndEnd();
			AgentBrokerUrl = EndpointOverride.IsEmpty()
				? Settings->GetChatCompletionsUrl()
				: URenderTrailOwnedModelSettings::MakeChatCompletionsUrl(EndpointOverride);
			const FString Model = ModelOverride.IsEmpty() ? Settings->Model.TrimStartAndEnd() : ModelOverride;
			const FString ApiKey = ApiKeyOverride.IsEmpty() ? Settings->ApiKey.TrimStartAndEnd() : ApiKeyOverride;
			if (AgentBrokerUrl.IsEmpty() || Model.IsEmpty())
			{
				FinishAgentWithError(TEXT("RenderTrail Model Broker is not configured. Set Base URL and Model in Project Settings > Plugins > RenderTrail Model Broker."));
				return;
			}

			TSharedRef<FJsonObject> DirectBody = MakeShared<FJsonObject>();
			DirectBody->SetStringField(TEXT("model"), Model);
			DirectBody->SetArrayField(TEXT("messages"), AgentMessages);
			const int32 MaxTokens = FMath::Clamp(Settings->MaxOutputTokens, 128, 8192);
			DirectBody->SetNumberField(TEXT("max_tokens"), MaxTokens);
			const bool bDeepSeekV4 = Model.StartsWith(TEXT("deepseek-v4-"), ESearchCase::IgnoreCase);
			if (bDeepSeekV4)
			{
				TSharedRef<FJsonObject> Thinking = MakeShared<FJsonObject>();
				Thinking->SetStringField(TEXT("type"), Settings->bEnableThinking ? TEXT("enabled") : TEXT("disabled"));
				DirectBody->SetObjectField(TEXT("thinking"), Thinking);
			}
			TSharedRef<FJsonObject> DirectLog = MakeShared<FJsonObject>();
			DirectLog->SetNumberField(TEXT("turn"), AgentStep);
			DirectLog->SetNumberField(TEXT("messageCount"), AgentMessages.Num());
			DirectLog->SetStringField(TEXT("url"), AgentBrokerUrl);
			DirectLog->SetStringField(TEXT("model"), Model);
			DirectLog->SetNumberField(TEXT("maxOutputTokens"), MaxTokens);
			DirectLog->SetStringField(TEXT("thinking"), bDeepSeekV4 ? (Settings->bEnableThinking ? TEXT("enabled") : TEXT("disabled")) : TEXT("not-applicable"));
			DirectLog->SetArrayField(TEXT("messages"), AgentMessages);
			WriteAgentLog(TEXT("ModelTurnRequest"), SerializeJson(DirectLog));

			AgentRequest = FHttpModule::Get().CreateRequest();
			AgentRequest->SetURL(AgentBrokerUrl);
			AgentRequest->SetVerb(TEXT("POST"));
			AgentRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			AgentRequest->SetHeader(TEXT("Accept"), TEXT("application/json"));
			if (!ApiKey.IsEmpty())
			{
				AgentRequest->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + ApiKey);
			}
			AgentRequest->SetContentAsString(SerializeJson(DirectBody));
			AgentRequest->SetTimeout(120.0f);
			AgentRequest->SetActivityTimeout(120.0f);
			AgentRequest->OnProcessRequestComplete().BindSP(this, &SAnalyzerHome::HandleAgentBrokerResponse);
			SetAgentStatus(FString::Printf(TEXT("Agent turn %d/%d - RenderTrail Model Broker completing..."), AgentStep, MaxAgentSteps));
			QueueAgentProcessRequest(AgentRequest.ToSharedRef(), TEXT("RenderTrail Model Broker request could not be queued."));
			return;

		#if 0
			TSharedRef<FJsonObject> Arguments = MakeShared<FJsonObject>();
			Arguments->SetArrayField(TEXT("messages"), AgentMessages);
			Arguments->SetNumberField(TEXT("max_output_tokens"), 8192);
			TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("name"), TEXT("unreal_model_complete"));
			Params->SetObjectField(TEXT("arguments"), Arguments);
			TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
			Body->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
			Body->SetNumberField(TEXT("id"), 100 + AgentStep);
			Body->SetStringField(TEXT("method"), TEXT("tools/call"));
			Body->SetObjectField(TEXT("params"), Params);

			TSharedRef<FJsonObject> LogPayload = MakeShared<FJsonObject>();
			LogPayload->SetNumberField(TEXT("turn"), AgentStep);
			LogPayload->SetNumberField(TEXT("messageCount"), AgentMessages.Num());
			LogPayload->SetArrayField(TEXT("messages"), AgentMessages);
			WriteAgentLog(TEXT("ModelTurnRequest"), SerializeJson(LogPayload));
			AgentRequest = FHttpModule::Get().CreateRequest();
			ConfigureMcpRequest(AgentRequest.ToSharedRef());
			AgentRequest->SetVerb(TEXT("POST"));
			AgentRequest->SetContentAsString(SerializeJson(Body));
			AgentRequest->OnRequestProgress64().BindSP(this, &SAnalyzerHome::HandleAgentBrokerProgress);
			AgentRequest->OnProcessRequestComplete().BindSP(this, &SAnalyzerHome::HandleAgentBrokerResponse);
			SetAgentStatus(FString::Printf(TEXT("Agent 第 %d/%d 轮 · Unreal MCP 正在执行单轮补全…"), AgentStep, MaxAgentSteps));
			if (!QueueAgentProcessRequest(AgentRequest.ToSharedRef(), TEXT("Unreal MCP request could not be queued.")))
			{
				FinishAgentWithError(TEXT("无法向 Unreal MCP 发送模型请求。"));
			}
		#endif
		}

	#if 0
		static FString ExtractMcpToolError(const TSharedPtr<FJsonObject>& Result)
		{
			const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
			if (Result.IsValid() && Result->TryGetArrayField(TEXT("content"), Content) && Content)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Content)
				{
					const TSharedPtr<FJsonObject> Item = Value->AsObject();
					FString Text;
					if (Item.IsValid() && Item->TryGetStringField(TEXT("text"), Text))
						return Text;
				}
			}
			return TEXT("Unreal MCP 模型工具返回了未知错误。");
		}

		#endif

		static bool ExtractAssistantContentDirect(const TSharedPtr<FJsonObject>& Message, FString& OutContent)
		{
			OutContent.Empty();
			if (!Message.IsValid())
			{
				return false;
			}
			if (Message->TryGetStringField(TEXT("content"), OutContent))
			{
				return true;
			}
			const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
			if (!Message->TryGetArrayField(TEXT("content"), Parts) || !Parts)
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& PartValue : *Parts)
			{
				if (!PartValue.IsValid())
				{
					continue;
				}
				if (PartValue->Type == EJson::String)
				{
					OutContent += PartValue->AsString();
					continue;
				}
				const TSharedPtr<FJsonObject> Part = PartValue->AsObject();
				FString Text;
				if (Part.IsValid() && (Part->TryGetStringField(TEXT("text"), Text)
					|| Part->TryGetStringField(TEXT("content"), Text)))
				{
					OutContent += Text;
				}
			}
			return true;
		}

	#if 0
		void HandleAgentBrokerProgress(FHttpRequestPtr Request, uint64, uint64 BytesReceived)
		{
			if (!bAgentRunning || Request != AgentRequest || BytesReceived == 0)
				return;
			const FHttpResponsePtr Response = Request->GetResponse();
			TSharedPtr<FJsonObject> Root;
			if (!Response.IsValid() || !ParseMcpResponse(Response->GetContentAsString(), Root)
				|| (!Root->HasField(TEXT("result")) && !Root->HasField(TEXT("error"))))
			{
				return;
			}

			WriteAgentLog(TEXT("ModelTurnSseResponse"), FString::Printf(TEXT("bytes=%llu body=%s"),
				static_cast<unsigned long long>(BytesReceived), *Response->GetContentAsString()));
			AgentRequest.Reset();
			Request->OnRequestProgress64().Unbind();
			Request->OnProcessRequestComplete().Unbind();
			QueueAgentCancelRequest(Request);
			HandleAgentBrokerJson(Root.ToSharedRef());
		}

		#endif

		void HandleAgentBrokerResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded)
		{
			if (!bAgentRunning || Request != AgentRequest)
				return;
			AgentRequest.Reset();
			if (!bSucceeded || !Response.IsValid())
			{
				FinishAgentWithError(TEXT("RenderTrail model request failed or timed out."));
				return;
			}
			WriteAgentLog(TEXT("ModelTurnHttpResponse"), FString::Printf(TEXT("http=%d body=%s"),
				Response->GetResponseCode(), *Response->GetContentAsString()));
			if (Response->GetResponseCode() < 200 || Response->GetResponseCode() >= 300)
			{
				FString ErrorBody = Response->GetContentAsString();
				ErrorBody.LeftInline(600);
				FinishAgentWithError(FString::Printf(TEXT("RenderTrail model endpoint returned HTTP %d: %s"),
					Response->GetResponseCode(), *ErrorBody));
				return;
			}
			TSharedPtr<FJsonObject> DirectRoot;
			const TSharedRef<TJsonReader<>> DirectReader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
			if (!FJsonSerializer::Deserialize(DirectReader, DirectRoot) || !DirectRoot.IsValid())
			{
				FinishAgentWithError(TEXT("RenderTrail model endpoint returned invalid JSON."));
				return;
			}
			const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
			if (!DirectRoot->TryGetArrayField(TEXT("choices"), Choices) || !Choices || Choices->IsEmpty())
			{
				FinishAgentWithError(TEXT("RenderTrail model endpoint returned no choices."));
				return;
			}
			const TSharedPtr<FJsonObject> Choice = (*Choices)[0].IsValid() ? (*Choices)[0]->AsObject() : nullptr;
			const TSharedPtr<FJsonObject>* Message = nullptr;
			if (!Choice.IsValid() || !Choice->TryGetObjectField(TEXT("message"), Message) || !Message || !Message->IsValid())
			{
				FinishAgentWithError(TEXT("RenderTrail model response is missing choices[0].message."));
				return;
			}
			FString Content;
			FString ReasoningContent;
			ExtractAssistantContentDirect(*Message, Content);
			(*Message)->TryGetStringField(TEXT("reasoning_content"), ReasoningContent);
			FString FinishReason = TEXT("unknown");
			Choice->TryGetStringField(TEXT("finish_reason"), FinishReason);
			WriteAgentLog(TEXT("ModelTurnParsedJson"), FString::Printf(
				TEXT("finishReason=%s contentChars=%d reasoningChars=%d content=%s"),
				*FinishReason, Content.Len(), ReasoningContent.Len(), *Content));
			if (Content.TrimStartAndEnd().IsEmpty())
			{
				FinishAgentWithError(FString::Printf(
					TEXT("RenderTrail model returned empty assistant content (finish_reason=%s, reasoning_chars=%d)."),
					*FinishReason, ReasoningContent.Len()));
				return;
			}
			HandleAgentAction(Content, ReasoningContent);
			return;
		}

		#if 0
			if (!bSucceeded || !Response.IsValid())
			{
				FinishAgentWithError(TEXT("Unreal MCP 模型请求失败或超时。"));
				return;
			}
			WriteAgentLog(TEXT("ModelTurnHttpResponse"), FString::Printf(TEXT("http=%d body=%s"),
				Response->GetResponseCode(), *Response->GetContentAsString()));
			if (Response->GetResponseCode() < 200 || Response->GetResponseCode() >= 300)
			{
				FString ErrorBody = Response->GetContentAsString();
				ErrorBody.LeftInline(600);
				FinishAgentWithError(FString::Printf(TEXT("Unreal MCP 返回 HTTP %d：%s"), Response->GetResponseCode(), *ErrorBody));
				return;
			}

			TSharedPtr<FJsonObject> Root;
			if (!ParseMcpResponse(Response->GetContentAsString(), Root))
			{
				FinishAgentWithError(TEXT("Unreal MCP 返回的不是有效 JSON-RPC/SSE。"));
				return;
			}
			HandleAgentBrokerJson(Root.ToSharedRef());
		}

		#endif

	#if 0
		void HandleAgentBrokerJson(const TSharedRef<FJsonObject>& Root)
		{
			WriteAgentLog(TEXT("ModelTurnParsedJson"), SerializeJson(Root));
			const TSharedPtr<FJsonObject>* RpcError = nullptr;
			if (Root->TryGetObjectField(TEXT("error"), RpcError) && RpcError && RpcError->IsValid())
			{
				FString Message = TEXT("未知 JSON-RPC 错误");
				(*RpcError)->TryGetStringField(TEXT("message"), Message);
				FinishAgentWithError(TEXT("Unreal MCP：") + Message);
				return;
			}

			const TSharedPtr<FJsonObject>* Result = nullptr;
			if (!Root->TryGetObjectField(TEXT("result"), Result) || !Result || !Result->IsValid())
			{
				FinishAgentWithError(TEXT("Unreal MCP tools/call 响应缺少 result。"));
				return;
			}
			bool bIsError = false;
			(*Result)->TryGetBoolField(TEXT("isError"), bIsError);
			if (bIsError)
			{
				FinishAgentWithError(ExtractMcpToolError(*Result));
				return;
			}

			FString Content;
			FString ReasoningContent;
			const TSharedPtr<FJsonObject>* Structured = nullptr;
			if ((*Result)->TryGetObjectField(TEXT("structuredContent"), Structured) && Structured && Structured->IsValid())
			{
				(*Structured)->TryGetStringField(TEXT("content"), Content);
				(*Structured)->TryGetStringField(TEXT("reasoningContent"), ReasoningContent);
			}
			if (Content.IsEmpty())
			{
				const FString Text = ExtractMcpToolError(*Result);
				TSharedPtr<FJsonObject> TextObject;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
				if (FJsonSerializer::Deserialize(Reader, TextObject) && TextObject.IsValid())
				{
					TextObject->TryGetStringField(TEXT("content"), Content);
				}
			}
			if (Content.IsEmpty())
			{
				FinishAgentWithError(TEXT("Unreal MCP 模型工具响应内容为空或缺少 content；详见 Saved/Logs/RenderTrailAgent.log。"));
				return;
			}
			HandleAgentAction(Content, ReasoningContent);
		}

		#endif

		static FString ExtractJsonObject(const FString& Content)
		{
			const int32 FirstBrace = Content.Find(TEXT("{"));
			const int32 LastBrace = Content.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			return FirstBrace != INDEX_NONE && LastBrace >= FirstBrace ? Content.Mid(FirstBrace, LastBrace - FirstBrace + 1) : FString();
		}

		static FString RepairAgentJsonBrackets(const FString& Json, bool& bOutRepaired)
		{
			bOutRepaired = false;
			FString Repaired;
			Repaired.Reserve(Json.Len() + 4);
			TArray<TCHAR> BracketStack;
			bool bInString = false;
			bool bEscaped = false;

			for (const TCHAR Character : Json)
			{
				if (bInString)
				{
					Repaired.AppendChar(Character);
					if (bEscaped)
					{
						bEscaped = false;
					}
					else if (Character == TEXT('\\'))
					{
						bEscaped = true;
					}
					else if (Character == TEXT('"'))
					{
						bInString = false;
					}
					continue;
				}

				if (Character == TEXT('"'))
				{
					bInString = true;
					Repaired.AppendChar(Character);
					continue;
				}

				if (Character == TEXT('{') || Character == TEXT('['))
				{
					BracketStack.Add(Character);
					Repaired.AppendChar(Character);
					continue;
				}

				if (Character == TEXT('}') || Character == TEXT(']'))
				{
					if (BracketStack.IsEmpty())
					{
						bOutRepaired = true;
						continue;
					}

					const TCHAR ExpectedOpen = Character == TEXT('}') ? TEXT('{') : TEXT('[');
					if (BracketStack.Last() == ExpectedOpen)
					{
						BracketStack.Pop();
						Repaired.AppendChar(Character);
					}
					else
					{
						// Some compatible endpoints occasionally emit `]` where an object
						// ends (for example: `influence: {...}], shaders: [...]`).
						// Repair only the mismatched closer; strict JSON parsing below still
						// decides whether the result is acceptable.
						const TCHAR RepairedCloser = BracketStack.Last() == TEXT('{') ? TEXT('}') : TEXT(']');
						BracketStack.Pop();
						Repaired.AppendChar(RepairedCloser);
						bOutRepaired = true;
					}
					continue;
				}

				Repaired.AppendChar(Character);
			}

			while (!BracketStack.IsEmpty())
			{
				Repaired.AppendChar(BracketStack.Pop() == TEXT('{') ? TEXT('}') : TEXT(']'));
				bOutRepaired = true;
			}
			return Repaired;
		}

		static bool TryParseAgentActionJson(const FString& Content, FString& OutJson,
			TSharedPtr<FJsonObject>& OutAction, bool& bOutRepaired)
		{
			OutJson = ExtractJsonObject(Content);
			OutAction.Reset();
			bOutRepaired = false;
			if (OutJson.IsEmpty())
			{
				return false;
			}

			const auto TryParse = [&OutAction](const FString& Candidate) -> bool
			{
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Candidate);
				return FJsonSerializer::Deserialize(Reader, OutAction) && OutAction.IsValid();
			};
			if (TryParse(OutJson))
			{
				return true;
			}

			bool bRepaired = false;
			const FString RepairedJson = RepairAgentJsonBrackets(OutJson, bRepaired);
			if (!bRepaired || !TryParse(RepairedJson))
			{
				return false;
			}
			OutJson = RepairedJson;
			bOutRepaired = true;
			return true;
		}

		bool NormalizeAnswerOnlyAgentObject(const TSharedPtr<FJsonObject>& Action)
		{
			if (!Action.IsValid())
			{
				return false;
			}
			FString Answer;
			if (!Action->TryGetStringField(TEXT("answer"), Answer) || Answer.IsEmpty())
			{
				return false;
			}

			Action->SetStringField(TEXT("action"), TEXT("finish"));
			FString HumanRequest;
			if (!Action->TryGetStringField(TEXT("humanRequest"), HumanRequest) || HumanRequest.IsEmpty())
			{
				HumanRequest = AgentIntentTextBox.IsValid()
					? AgentIntentTextBox->GetText().ToString().TrimStartAndEnd()
					: TEXT("基于当前确定性证据整理选中像素的形成原因。");
				Action->SetStringField(TEXT("humanRequest"), HumanRequest);
			}

			if (!Action->HasField(TEXT("points")))
			{
				TArray<TSharedPtr<FJsonValue>> Points;
				for (const FPixelSample& Sample : Samples)
				{
					const TSharedRef<FJsonObject> Point = MakeShared<FJsonObject>();
					Point->SetStringField(TEXT("sample"), FString::Printf(TEXT("P%d"), Points.Num() + 1));
					Point->SetStringField(TEXT("coordinate"), FString::Printf(TEXT("(%d,%d)"), Sample.Pixel.X, Sample.Pixel.Y));
					Point->SetStringField(TEXT("finalColor"), Sample.Modifications.IsEmpty()
						? TEXT("unknown") : Sample.Modifications.Last().After);
					Point->SetStringField(TEXT("formation"), Sample.Modifications.IsEmpty()
						? TEXT("Pixel History 没有可用于补全直接形成过程的修改记录。")
						: FString::Printf(TEXT("EID %u · %s；after=%s"), Sample.Modifications.Last().EventId,
							*Sample.Modifications.Last().Action, *Sample.Modifications.Last().After));
					Points.Add(MakeShared<FJsonValueObject>(Point));
				}
				Action->SetArrayField(TEXT("points"), MoveTemp(Points));
			}

			if (!Action->HasField(TEXT("influence")))
			{
				const TSharedRef<FJsonObject> Influence = MakeShared<FJsonObject>();
				if (LastCandidate.IsSet())
				{
					const FEventEvidence& Event = LastCandidate->Event;
					Influence->SetNumberField(TEXT("eventId"), Event.EventId);
					Influence->SetStringField(TEXT("type"), Event.ActionKind.IsEmpty() ? TEXT("unknown") : Event.ActionKind);
					Influence->SetStringField(TEXT("name"), Event.Action);
					Influence->SetStringField(TEXT("effect"), DescribeEventResult(Event));
					Influence->SetStringField(TEXT("evidence"), FString::Printf(TEXT("确定性 Pixel History 候选事件 EID %u；模型未返回独立 influence 字段。"), Event.EventId));
				}
				else
				{
					Influence->SetNumberField(TEXT("eventId"), 0);
					Influence->SetStringField(TEXT("type"), TEXT("unknown"));
					Influence->SetStringField(TEXT("name"), TEXT("unknown"));
					Influence->SetStringField(TEXT("effect"), TEXT("模型未返回结构化直接影响字段。"));
					Influence->SetStringField(TEXT("evidence"), TEXT("需要使用确定性 Pixel History 结果补全。"));
				}
				Action->SetObjectField(TEXT("influence"), Influence);
			}

			if (!Action->HasField(TEXT("shaders")))
			{
				TArray<TSharedPtr<FJsonValue>> Shaders;
				if (LastCandidate.IsSet())
				{
					const uint32 EventId = LastCandidate->Event.EventId;
					const FEventContextEvidence* Context = EventContexts.Find(EventId);
					const TSharedRef<FJsonObject> Shader = MakeShared<FJsonObject>();
					Shader->SetNumberField(TEXT("eventId"), EventId);
					Shader->SetStringField(TEXT("stage"), Context ? Context->ShaderStage : TEXT("unknown"));
					Shader->SetStringField(TEXT("name"), Context && !Context->ShaderEntry.IsEmpty() ? Context->ShaderEntry : TEXT("unknown"));
					Shader->SetStringField(TEXT("effect"), TEXT("仅补全模型遗漏的结构化字段；不对 Shader 算法作额外推断。"));
					Shader->SetStringField(TEXT("evidence"), Context ? FString::Printf(TEXT("pipeline.shaderEntry=%s"), *Context->ShaderEntry) : TEXT("没有对应的确定性事件上下文。"));
					Shaders.Add(MakeShared<FJsonValueObject>(Shader));
				}
				Action->SetArrayField(TEXT("shaders"), MoveTemp(Shaders));
			}

			if (!Action->HasField(TEXT("mesh")))
			{
				const TSharedRef<FJsonObject> Mesh = MakeShared<FJsonObject>();
				Mesh->SetStringField(TEXT("name"), TEXT("unknown"));
				Mesh->SetStringField(TEXT("evidence"), TEXT("模型未返回 Mesh 归属字段；当前证据不足以建立 UE 对象映射。"));
				Action->SetObjectField(TEXT("mesh"), Mesh);
			}
			if (!Action->HasField(TEXT("process")))
			{
				TArray<TSharedPtr<FJsonValue>> Process;
				Process.Add(MakeShared<FJsonValueString>(TEXT("最终画面 → RenderDoc Pixel History 确定性证据")));
				if (LastCandidate.IsSet())
				{
					Process.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("EID %u · %s"), LastCandidate->Event.EventId, *LastCandidate->Event.Action)));
				}
				Process.Add(MakeShared<FJsonValueString>(TEXT("模型回答已保留；缺失的结构化字段由本地证据补全。")));
				Action->SetArrayField(TEXT("process"), MoveTemp(Process));
			}
			if (!Action->HasField(TEXT("finding")))
			{
				Action->SetStringField(TEXT("finding"), Answer.Left(500));
			}
			if (!Action->HasField(TEXT("confidence")))
			{
				Action->SetStringField(TEXT("confidence"), TEXT("low"));
			}
			if (!Action->HasField(TEXT("unknowns")))
			{
				TArray<TSharedPtr<FJsonValue>> Unknowns;
				Unknowns.Add(MakeShared<FJsonValueString>(TEXT("模型没有返回完整的 finish 结构；已由 RenderTrail 本地确定性证据补全。")));
				Action->SetArrayField(TEXT("unknowns"), MoveTemp(Unknowns));
			}
			return true;
		}

		void HandleAgentAction(const FString& Content, const FString& ReasoningContent = FString())
		{
			WriteAgentLog(TEXT("ModelAction"), Content);
			FString Json;
			TSharedPtr<FJsonObject> Action;
			bool bRepairedJson = false;
			FString ActionName;
			if (!TryParseAgentActionJson(Content, Json, Action, bRepairedJson))
			{
				if (AgentStep >= MaxAgentSteps)
				{
					FinishAgentWithError(TEXT("模型在最终轮没有返回约定 JSON：") + Content.Left(800));
					return;
				}
				AddAgentMessage(TEXT("assistant"), Content, ReasoningContent);
				AddAgentMessage(TEXT("user"), TEXT("FORMAT_ERROR: return exactly one JSON action object. Do not use markdown."));
				SendAgentTurn();
				return;
			}
			if (!Action->TryGetStringField(TEXT("action"), ActionName))
			{
				if (!NormalizeAnswerOnlyAgentObject(Action))
				{
					FinishAgentWithError(TEXT("模型返回了 JSON，但缺少 action 且无法从 answer-only 结果恢复：") + Content.Left(800));
					return;
				}
				WriteAgentLog(TEXT("ModelActionSchemaRepair"), TEXT("Model returned answer/unknowns without action; normalized to finish using deterministic local evidence."));
				ActionName = TEXT("finish");
			}
			if (bRepairedJson)
			{
				WriteAgentLog(TEXT("ModelActionJsonRepair"), TEXT("Accepted the action after repairing a mismatched JSON bracket."));
			}

			if (ActionName == TEXT("finish"))
			{
				DisplayAgentFinal(Action.ToSharedRef());
				return;
			}
			FinishAgentWithError(TEXT("语义模型只负责整理确定性溯源证据，不允许自行追加事件查询。"));
		}

		void ResumeAgentAfterEventContext(uint32 EventId)
		{
			if (!bAgentRunning || !AgentPendingEventId.IsSet() || AgentPendingEventId.GetValue() != EventId)
				return;
			AgentPendingEventId.Reset();
			AddAgentMessage(TEXT("user"), TEXT("TOOL_RESULT\n") + BuildAgentEventObservation(EventId));
			SendAgentTurn();
		}

		void DisplayAgentFinal(const TSharedRef<FJsonObject>& Final)
		{
			FString PointsText;
			const TArray<TSharedPtr<FJsonValue>>* PointValues = nullptr;
			if (Final->TryGetArrayField(TEXT("points"), PointValues) && PointValues)
			{
				for (const TSharedPtr<FJsonValue>& PointValue : *PointValues)
				{
					const TSharedPtr<FJsonObject> Point = PointValue.IsValid() ? PointValue->AsObject() : nullptr;
					if (!Point.IsValid())
					{
						continue;
					}
					FString Sample = TEXT("P?");
					FString Coordinate = TEXT("unknown");
					FString FinalColor = TEXT("unknown");
					FString Formation = TEXT("证据不足");
					Point->TryGetStringField(TEXT("sample"), Sample);
					Point->TryGetStringField(TEXT("coordinate"), Coordinate);
					Point->TryGetStringField(TEXT("finalColor"), FinalColor);
					Point->TryGetStringField(TEXT("formation"), Formation);
					PointsText += FString::Printf(TEXT("%s · %s\n最终颜色：%s\n直接形成：%s\n\n"),
						*Sample, *Coordinate, *FinalColor, *Formation);
				}
			}

			FString TargetSample = TEXT("selected");
			FString TargetCoordinate = TEXT("unknown");
			FString PixelColor = TEXT("unknown");
			const TSharedPtr<FJsonObject>* TargetPixel = nullptr;
			if (Final->TryGetObjectField(TEXT("targetPixel"), TargetPixel) && TargetPixel && TargetPixel->IsValid())
			{
				(*TargetPixel)->TryGetStringField(TEXT("sample"), TargetSample);
				(*TargetPixel)->TryGetStringField(TEXT("coordinate"), TargetCoordinate);
				(*TargetPixel)->TryGetStringField(TEXT("finalColor"), PixelColor);
			}
			else
			{
				// Backward compatibility with results produced by the pre-0.3.1 prompt.
				Final->TryGetStringField(TEXT("pixelColor"), PixelColor);
			}
			if (PointsText.IsEmpty())
			{
				PointsText = FString::Printf(TEXT("%s · %s\n最终颜色：%s\n"),
					*TargetSample, *TargetCoordinate, *PixelColor);
			}

			FString InfluenceType = TEXT("unknown");
			FString InfluenceName = TEXT("unknown");
			FString InfluenceEffect = TEXT("证据不足");
			FString InfluenceEvidence = TEXT("没有返回直接影响证据");
			uint32 InfluenceEventId = 0;
			const TSharedPtr<FJsonObject>* Influence = nullptr;
			if (Final->TryGetObjectField(TEXT("influence"), Influence) && Influence && Influence->IsValid())
			{
				double EventNumber = 0.0;
				if ((*Influence)->TryGetNumberField(TEXT("eventId"), EventNumber) && EventNumber > 0.0)
					InfluenceEventId = static_cast<uint32>(EventNumber);
				(*Influence)->TryGetStringField(TEXT("type"), InfluenceType);
				(*Influence)->TryGetStringField(TEXT("name"), InfluenceName);
				(*Influence)->TryGetStringField(TEXT("effect"), InfluenceEffect);
				(*Influence)->TryGetStringField(TEXT("evidence"), InfluenceEvidence);
			}

			FString ShaderText;
			const TArray<TSharedPtr<FJsonValue>>* Shaders = nullptr;
			if (Final->TryGetArrayField(TEXT("shaders"), Shaders) && Shaders)
			{
				int32 ShaderIndex = 0;
				for (const TSharedPtr<FJsonValue>& ShaderValue : *Shaders)
				{
					const TSharedPtr<FJsonObject> Shader = ShaderValue.IsValid() ? ShaderValue->AsObject() : nullptr;
					if (!Shader.IsValid())
						continue;
					double EventNumber = 0.0;
					FString Stage = TEXT("unknown");
					FString Name = TEXT("unknown");
					FString Effect = TEXT("证据未说明");
					FString Evidence = TEXT("未提供");
					Shader->TryGetNumberField(TEXT("eventId"), EventNumber);
					Shader->TryGetStringField(TEXT("stage"), Stage);
					Shader->TryGetStringField(TEXT("name"), Name);
					Shader->TryGetStringField(TEXT("effect"), Effect);
					Shader->TryGetStringField(TEXT("evidence"), Evidence);
					ShaderText += FString::Printf(
						TEXT("%d. EID %u · [%s] %s\n   作用：%s\n   依据：%s\n"),
						++ShaderIndex,
						static_cast<uint32>(FMath::Max(0.0, EventNumber)), *Stage, *Name, *Effect, *Evidence);
				}
			}
			if (ShaderText.IsEmpty())
				ShaderText = TEXT("没有足够证据确认 Shader 名称\n");

			FString MeshName = TEXT("unknown");
			FString MeshEvidence = TEXT("没有可归属到 UE Mesh 的证据");
			const TSharedPtr<FJsonObject>* Mesh = nullptr;
			if (Final->TryGetObjectField(TEXT("mesh"), Mesh) && Mesh && Mesh->IsValid())
			{
				(*Mesh)->TryGetStringField(TEXT("name"), MeshName);
				(*Mesh)->TryGetStringField(TEXT("evidence"), MeshEvidence);
			}
			FString Finding = TEXT("没有生成结论");
			FString Confidence = TEXT("low");
			Final->TryGetStringField(TEXT("finding"), Finding);
			Final->TryGetStringField(TEXT("confidence"), Confidence);
			FString RequestedQuestion;
			Final->TryGetStringField(TEXT("humanRequest"), RequestedQuestion);
			if (RequestedQuestion.IsEmpty() && AgentIntentTextBox.IsValid())
			{
				RequestedQuestion = AgentIntentTextBox->GetText().ToString().TrimStartAndEnd();
			}
			if (RequestedQuestion.IsEmpty())
			{
				RequestedQuestion = TEXT("请基于当前确定性证据整理选中像素的形成原因。");
			}
			FString Answer;
			Final->TryGetStringField(TEXT("answer"), Answer);
			if (Answer.IsEmpty())
			{
				Answer = Finding;
			}

			FString ProcessText;
			const TArray<TSharedPtr<FJsonValue>>* Process = nullptr;
			if (Final->TryGetArrayField(TEXT("process"), Process) && Process)
			{
				for (int32 Index = 0; Index < Process->Num(); ++Index)
					ProcessText += FString::Printf(TEXT("%d. %s\n"), Index + 1, *(*Process)[Index]->AsString());
			}
			FString UnknownText;
			const TArray<TSharedPtr<FJsonValue>>* Unknowns = nullptr;
			if (Final->TryGetArrayField(TEXT("unknowns"), Unknowns) && Unknowns)
			{
				for (const TSharedPtr<FJsonValue>& Unknown : *Unknowns)
					UnknownText += FString::Printf(TEXT("• %s\n"), *Unknown->AsString());
			}

			const FString InfluenceHeading = InfluenceEventId > 0
				? FString::Printf(TEXT("EID %u · %s"), InfluenceEventId, *InfluenceName)
				: InfluenceName;
			FString Output;
			Output += TEXT("本次分析诉求\n");
			Output += RequestedQuestion;
			Output += TEXT("\n\n针对性回答\n");
			Output += Answer;
			Output += TEXT("\n\n");
			Output += TEXT("关注像素\n");
			Output += PointsText;
			Output += TEXT("\n结论\n");
			Output += Finding;
			Output += TEXT("\n\n直接影响\n");
			Output += InfluenceHeading;
			Output += FString::Printf(TEXT("\n类型：%s\n作用：%s\n依据：%s\n归属：%s\n"),
				*InfluenceType, *InfluenceEffect, *InfluenceEvidence, *MeshName);
			if (!MeshEvidence.IsEmpty() && MeshName.Equals(TEXT("unknown"), ESearchCase::IgnoreCase))
			{
				Output += FString::Printf(TEXT("归属依据：%s\n"), *MeshEvidence);
			}
			Output += TEXT("\nPipeline 状态\n");
			if (InfluenceEventId > 0)
			{
				if (const FEventContextEvidence* Context = EventContexts.Find(InfluenceEventId))
				{
					Output += FormatPipelineStateEvidence(Context->PipelineState);
					Output += FString::Printf(TEXT("- Shader 绑定：%s entry %s；debuggable=%s；source symbols=%s\n"),
						*Context->ShaderStage,
						Context->ShaderEntry.IsEmpty() ? TEXT("unknown") : *Context->ShaderEntry,
						Context->bShaderDebuggable ? TEXT("yes") : TEXT("no"),
						Context->bSourceDebugInfo ? TEXT("yes") : TEXT("no"));
					Output += FString::Printf(TEXT("- Shader 反射：encoding=%s；inputSig=%d；outputSig=%d；constantBlocks=%d；samplers=%d；RO=%d；RW=%d\n"),
						Context->ShaderEncoding.IsEmpty() ? TEXT("unknown") : *Context->ShaderEncoding,
						Context->ShaderInputSignatureCount, Context->ShaderOutputSignatureCount,
						Context->ShaderConstantBlockCount, Context->ShaderSamplerCount,
						Context->ShaderReadOnlyResourceCount, Context->ShaderReadWriteResourceCount);
					if (Context->ShaderDebugTrace.IsValid())
					{
						Output += FormatShaderDebugTraceEvidence(Context->ShaderDebugTrace);
					}
					else
					{
						Output += TEXT("- Shader 算法：");
						Output += (Context->bShaderDebuggable && Context->bSourceDebugInfo)
							? TEXT("有源码/调试信息，但当前结果没有执行指令级追踪。\n")
							: TEXT("当前无法从入口名和资源绑定推断具体数学算法。\n");
					}
				}
				else
				{
					Output += TEXT("- 尚未加载该事件的 Pipeline 状态；当前结论只使用 Pixel History。\n");
				}
			}
			else
			{
				Output += TEXT("- 没有确定的影响事件，未展开 Pipeline 状态。\n");
			}
			Output += TEXT("\nShader\n");
			Output += ShaderText;
			if (!ProcessText.IsEmpty())
			{
				Output += TEXT("\n像素链\n");
				Output += ProcessText;
			}
			Output += FString::Printf(TEXT("\n置信度：%s"), *Confidence);
			if (!UnknownText.IsEmpty())
			{
				Output += TEXT("\n\n未知项\n");
				Output += UnknownText;
			}
			Output += TEXT("\n分析范围\n仅覆盖所选像素及其直接 GPU 因果链；未追踪 Blueprint、C++ 或游戏逻辑上游。");

			const FEventContextEvidence* InfluenceContext = InfluenceEventId > 0
				? EventContexts.Find(InfluenceEventId)
				: nullptr;
			FString DirectWriterText = FString::Printf(
				TEXT("目标：%s · %s\n最终颜色：%s\n\n最终写入者：%s\n类型：%s\n作用：%s\n依据：%s\n归属：%s"),
				*TargetSample, *TargetCoordinate, *PixelColor, *InfluenceHeading, *InfluenceType,
				*InfluenceEffect, *InfluenceEvidence, *MeshName);
			if (MeshName.Equals(TEXT("unknown"), ESearchCase::IgnoreCase) && !MeshEvidence.IsEmpty())
			{
				DirectWriterText += FString::Printf(TEXT("\n归属依据：%s"), *MeshEvidence);
			}

			FString PipelineText;
			FString ShaderEvidenceText = ShaderText;
			if (InfluenceContext)
			{
				PipelineText += FormatPipelineStateEvidence(InfluenceContext->PipelineState);
				PipelineText += FString::Printf(
					TEXT("Shader 绑定：%s entry %s\n可调试：%s · 源码符号：%s\nShader 反射：encoding=%s · inputSig=%d · outputSig=%d · constantBlocks=%d · samplers=%d · RO=%d · RW=%d\n"),
					*InfluenceContext->ShaderStage,
					InfluenceContext->ShaderEntry.IsEmpty() ? TEXT("unknown") : *InfluenceContext->ShaderEntry,
					InfluenceContext->bShaderDebuggable ? TEXT("yes") : TEXT("no"),
					InfluenceContext->bSourceDebugInfo ? TEXT("yes") : TEXT("no"),
					InfluenceContext->ShaderEncoding.IsEmpty() ? TEXT("unknown") : *InfluenceContext->ShaderEncoding,
					InfluenceContext->ShaderInputSignatureCount, InfluenceContext->ShaderOutputSignatureCount,
					InfluenceContext->ShaderConstantBlockCount, InfluenceContext->ShaderSamplerCount,
					InfluenceContext->ShaderReadOnlyResourceCount, InfluenceContext->ShaderReadWriteResourceCount);
				if (InfluenceContext->ShaderDebugTrace.IsValid())
				{
					ShaderEvidenceText += TEXT("\n\n执行跟踪：\n");
					ShaderEvidenceText += FormatShaderDebugTraceEvidence(InfluenceContext->ShaderDebugTrace);
				}
			}
			else
			{
				PipelineText = TEXT("尚未加载该事件的 Pipeline 状态；当前结论只使用 Pixel History。\n");
			}
			if (PipelineText.IsEmpty())
			{
				PipelineText = TEXT("没有确定的影响事件，未展开 Pipeline 状态。\n");
			}
			if (ShaderEvidenceText.IsEmpty())
			{
				ShaderEvidenceText = TEXT("没有足够证据确认 Shader。\n");
			}

			FString BoundaryText = FString::Printf(TEXT("置信度：%s\n\n归属：%s\n"), *Confidence, *MeshName);
			if (MeshName.Equals(TEXT("unknown"), ESearchCase::IgnoreCase) && !MeshEvidence.IsEmpty())
			{
				BoundaryText += FString::Printf(TEXT("归属依据：%s\n"), *MeshEvidence);
			}
			if (UnknownText.IsEmpty())
			{
				BoundaryText += TEXT("没有额外未知项。\n");
			}
			else
			{
				BoundaryText += TEXT("未知项：\n");
				BoundaryText += UnknownText;
			}
			BoundaryText += TEXT("\n分析范围：仅覆盖所选像素及其直接 GPU 因果链；未追踪 Blueprint、C++ 或游戏逻辑上游。\n");

			TArray<TPair<FString, FString>> ReportSections;
			ReportSections.Add(TPair<FString, FString>(TEXT("01 · 针对本次诉求的回答"),
				FString(TEXT("用户诉求：\n")) + RequestedQuestion + TEXT("\n\n回答：\n") + Answer));
			ReportSections.Add(TPair<FString, FString>(TEXT("02 · 结论 / 最终写入者"), Finding + TEXT("\n\n") + DirectWriterText));
			ReportSections.Add(TPair<FString, FString>(TEXT("03 · 像素形成链 / Pass 影响顺序"), ProcessText.IsEmpty()
				? TEXT("模型没有返回可展开的事件顺序。")
				: ProcessText));
			ReportSections.Add(TPair<FString, FString>(TEXT("04 · Pipeline 状态 / 固定管线快照"), PipelineText));
			ReportSections.Add(TPair<FString, FString>(TEXT("05 · Shader / 反射与执行证据"), ShaderEvidenceText));
			ReportSections.Add(TPair<FString, FString>(TEXT("06 · 归属、未知项与分析边界"), BoundaryText));
			ReportSections.Add(TPair<FString, FString>(TEXT("07 · 原始完整报告（可复制）"), Output));
			SetAgentReportSections(ReportSections, 0);
			WriteAgentLog(TEXT("RunComplete"), Output);
			if (AgentOutputText.IsValid())
			{
				AgentOutputText->SetText(FText::FromString(Output));
			}
			CloseAgentBrokerSession();
			bAgentRunning = false;
			bAgentWaitingForDeterministicContexts = false;
			AgentPendingEventId.Reset();
			if (AgentRunButtonText.IsValid())
				AgentRunButtonText->SetText(FText::FromString(TEXT("重新运行 Agent")));
			SetAgentStatus(FString::Printf(TEXT("完成 · %d 个模型轮次 · 只使用所选像素的有界证据"), AgentStep));
		}

		void FinishAgentWithError(const FString& Error)
		{
			WriteAgentLog(TEXT("RunError"), Error);
			bAgentRunning = false;
			bAgentWaitingForDeterministicContexts = false;
			if (AgentRequest.IsValid())
				QueueAgentCancelRequest(AgentRequest);
			AgentRequest.Reset();
			CloseAgentBrokerSession();
			AgentPendingEventId.Reset();
			if (AgentRunButtonText.IsValid())
				AgentRunButtonText->SetText(FText::FromString(TEXT("重新运行 Agent")));
			SetAgentStatus(Error);
		}

		void CancelAgentRun()
		{
			bAgentRunning = false;
			bAgentWaitingForDeterministicContexts = false;
			if (AgentRequest.IsValid())
				QueueAgentCancelRequest(AgentRequest);
			AgentRequest.Reset();
			CloseAgentBrokerSession();
			AgentPendingEventId.Reset();
			AgentMessages.Empty();
			if (AgentRunButtonText.IsValid())
				AgentRunButtonText->SetText(FText::FromString(TEXT("运行 Agent")));
		}

		void CloseAgentBrokerSession()
		{
			// RenderTrail talks directly to the configured model endpoint; there is
			// no MCP session to initialize or close.
			return;
		#if 0
			if (AgentMcpSessionId.IsEmpty())
			{
				AgentMcpProtocolVersion.Empty();
				return;
			}

			AgentSessionCloseRequest = FHttpModule::Get().CreateRequest();
			ConfigureMcpRequest(AgentSessionCloseRequest.ToSharedRef());
			AgentSessionCloseRequest->SetVerb(TEXT("DELETE"));
			AgentSessionCloseRequest->SetTimeout(5.0f);
			AgentSessionCloseRequest->OnProcessRequestComplete().BindSP(this, &SAnalyzerHome::HandleAgentBrokerSessionClosed);
			QueueAgentProcessRequest(AgentSessionCloseRequest.ToSharedRef(), FString());
			AgentMcpSessionId.Empty();
			AgentMcpProtocolVersion.Empty();
		#endif
		}

	#if 0
		void HandleAgentBrokerSessionClosed(FHttpRequestPtr Request, FHttpResponsePtr, bool)
		{
			if (Request == AgentSessionCloseRequest)
			{
				AgentSessionCloseRequest.Reset();
			}
		}

		#endif

		FReply ClearSamples()
		{
			ResetSamples();
			const bool bPreviewReady = bWorkerReady || bPreviewReadyForSelection || bReplayStartDeferred;
			SetEvidence(bPreviewReady
				? TEXT("关注像素已清空。直接在画面选择一个需要解释的像素。")
				: TEXT("先载入 .rdc 截帧，再选择需要解释的像素。"));
			SetStatus(bPreviewReady ? TEXT("关注像素已清空，可以重新选择。") : TEXT("关注像素已清空。"));
			return FReply::Handled();
		}

		FReply ClearCurrentInfo()
		{
			CancelAgentRun();
			LastCandidate.Reset();
			bLastCandidateHasDivergence = false;
			const bool bHasSelection = !Samples.IsEmpty();
			SetAgentOutputText(bHasSelection
				? TEXT("当前报告已清空。已选像素及其分析状态仍保留，未变化的点不会重复读取。")
				: TEXT("当前没有可显示的分析信息。"));
			SetAgentStatus(bHasSelection
				? TEXT("当前信息已清空 · 选点与已分析状态保留")
				: TEXT("当前信息已清空 · 等待选择像素"));
			SetReportCards(
				bHasSelection ? TEXT("当前信息已清空；选点状态仍保留。") : TEXT("当前没有分析信息。"),
				bHasSelection ? TEXT("已选 P 点\n↓\n等待重新运行") : TEXT("选择关注像素后生成。"),
				TEXT("已清空当前显示信息；已分析点不会因重复点击而再次查询。"),
				TEXT("报告显示已清空；Pixel History 缓存仍保留。"));
			UpdateSelectionText();
			return FReply::Handled();
		}

		FReply ConfirmPixelSelection()
		{
			if (Samples.IsEmpty())
			{
				SetStatus(TEXT("请先在预览中选择至少一个像素。"));
				return FReply::Handled();
			}
			if (!bWorkerReady)
			{
				if (bReplayStartDeferred && !Samples.IsEmpty())
				{
					bQueuePixelHistoryAfterWorkerReady = true;
					SetStatus(TEXT("正在按需打开 Replay Worker；打开后自动读取选中像素的 Pixel History…"));
					MarkReplaySynchronizationPending();
					StartWorker(true);
					return FReply::Handled();
				}
				if (bPreviewReadyForSelection && bCaptureLoading)
				{
					bQueuePixelHistoryAfterWorkerReady = true;
					SetStatus(TEXT("预览已就绪，Replay Worker 仍在后台加载；Pixel History 会在就绪后自动开始。"));
					MarkReplaySynchronizationPending();
					return FReply::Handled();
				}
				SetStatus(TEXT("Replay Worker is not ready."));
				return FReply::Handled();
			}
			bReplaySynchronizationPending = false;
			const bool bSelectionChanged = !bSelectionConfirmed;
			CancelAgentRun();
			bSelectionConfirmed = true;
			if (bSelectionChanged)
			{
				EventContexts.Empty();
				PendingEventContextByRequest.Empty();
				PendingEventContextIds.Empty();
				FailedEventContextIds.Empty();
				LastCandidate.Reset();
				bLastCandidateHasDivergence = false;
			}

			int32 QueuedCount = 0;
			for (FPixelSample& Sample : Samples)
			{
				if (Sample.bPending)
				{
					++QueuedCount;
					continue;
				}
				if (Sample.bAnalyzed && !Sample.bFailed)
				{
					continue;
				}
				Sample.bPending = true;
				Sample.bFailed = false;
				Sample.bAnalyzed = false;
				Sample.bTruncated = false;
				Sample.TotalModifications = 0;
				Sample.Error.Empty();
				Sample.Modifications.Empty();

				const FString RequestId = FString::Printf(TEXT("sample-%llu-query-%llu"), Sample.Id, ++RequestSerial);
				PendingSampleByRequest.Add(RequestId, Sample.Id);
				SendWorkerRequest(TEXT("pixel_history"), RequestId,
					[&Sample](const TSharedRef<FJsonObject>& Request)
					{
						Request->SetNumberField(TEXT("x"), Sample.Pixel.X);
						Request->SetNumberField(TEXT("y"), Sample.Pixel.Y);
					});
				++QueuedCount;
			}

			UpdateSelectionText();
			RenderCausalReport();
			if (QueuedCount == 0)
			{
				SetAgentStatus(TEXT("Pixel History 已就绪，可运行 Agent 语义整理。"));
				SetStatus(TEXT("选点未变化，已复用当前像素的 Pixel History，不重复查询。"));
			}
			else
			{
				SetAgentStatus(TEXT("规则初筛进行中；Pixel History 返回后可运行 Agent 语义整理。"));
				SetStatus(TEXT("已确认当前像素，正在读取 Pixel History…"));
			}
			return FReply::Handled();
		}

		void MarkReplaySynchronizationPending()
		{
			bReplaySynchronizationPending = true;
			SetStatus(TEXT("已选点；正在等待同步：ReplayController → 目标 RT → Pixel History。"));
			SetAgentStatus(TEXT("分析已排队，等待 Replay 完整同步；同步完成后才会读取事件上下文和 Shader Debug。"));
		}

		void ResetSamples()
		{
			CancelAgentRun();
			bSelectionConfirmed = false;
			bReplaySynchronizationPending = false;
			bQueuePixelHistoryAfterWorkerReady = false;
			Samples.Empty();
			PendingSampleByRequest.Empty();
			EventContexts.Empty();
			PendingEventContextByRequest.Empty();
			PendingShaderDebugByRequest.Empty();
			PendingEventContextIds.Empty();
			FailedEventContextIds.Empty();
			FailedShaderDebugIds.Empty();
			LastCandidate.Reset();
			bLastCandidateHasDivergence = false;
			SetAgentOutputText(TEXT("选择像素后，可运行 Agent 生成 Mesh、颜色和渲染过程摘要。"));
			SetAgentStatus(TEXT("未运行 · 只发送像素摘要；.rdc/图像不上传；Key 不落盘"));
			if (ImageView.IsValid())
			{
				ImageView->SetMarkers({});
			}
			UpdateSelectionText();
		}

		void UpdateSelectionText()
		{
			if (SelectionText.IsValid())
			{
				if (Samples.IsEmpty())
				{
					SelectionText->SetText(FText::FromString(TEXT("尚未选择关注像素；点击画面选择一个点")));
					return;
				}
				const FPixelSample& Sample = Samples[0];
				const TCHAR* State = Sample.bPending
					? TEXT("查询中")
					: Sample.bFailed
						? TEXT("失败")
						: Sample.bAnalyzed
							? TEXT("已分析")
							: TEXT("待分析");
				SelectionText->SetText(FText::FromString(FString::Printf(
					TEXT("当前像素 P1 (%d,%d) · %s · %s；点击其他位置可直接替换"),
					Sample.Pixel.X, Sample.Pixel.Y, State, bSelectionConfirmed ? TEXT("已确认") : TEXT("待确认"))));
			}
		}

		void UpdateMarkers()
		{
			if (!ImageView.IsValid())
			{
				return;
			}
			TArray<FPixelMarker> Markers;
			Markers.Reserve(Samples.Num());
			for (int32 Index = 0; Index < Samples.Num(); ++Index)
			{
				Markers.Add({Samples[Index].Pixel});
			}
			ImageView->SetMarkers(Markers);
		}

		FPixelSample* FindSample(uint64 SampleId)
		{
			return Samples.FindByPredicate([SampleId](const FPixelSample& Sample) { return Sample.Id == SampleId; });
		}

		FReply BrowseCapture()
		{
			IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
			if (!DesktopPlatform)
			{
				SetStatus(TEXT("DesktopPlatform is unavailable."));
				return FReply::Handled();
			}
			TArray<FString> Files;
			if (DesktopPlatform->OpenFileDialog(
				FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
				TEXT("Open RenderDoc capture"), FPaths::ProjectSavedDir(), TEXT(""), TEXT("RenderDoc capture (*.rdc)|*.rdc"),
				EFileDialogFlags::None, Files) && !Files.IsEmpty())
			{
				CapturePathBox->SetText(FText::FromString(Files[0]));
				StartWorker();
			}
			return FReply::Handled();
		}

		FReply LoadCapture()
		{
			StartWorker();
			return FReply::Handled();
		}

		#if 0 // Legacy out-of-process replay path retained only as migration reference.
		static FString GetReplayWorkerExecutablePath()
		{
			const FString BinaryName = TEXT("RenderTrailReplayWorker.exe");
			const FString ProjectBinary = FPaths::Combine(
				FPaths::ProjectDir(), TEXT("Binaries"), FPlatformProcess::GetBinariesSubdirectory(), BinaryName);
			if (FPaths::FileExists(ProjectBinary))
			{
				return FPaths::ConvertRelativePathToFull(ProjectBinary);
			}
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::EngineDir(), TEXT("Binaries"), FPlatformProcess::GetBinariesSubdirectory(), BinaryName));
		}

		bool LaunchWorkerProcess(const FString& Worker, const FString& Capture, const FString& InPreviewPath)
		{
			if (!FPlatformProcess::CreatePipe(StdOutRead, StdOutWrite, false)
				|| !FPlatformProcess::CreatePipe(StdInRead, StdInWrite, true)
				|| !FPlatformProcess::CreatePipe(StdErrRead, StdErrWrite, false))
			{
				SetStatus(TEXT("Could not create Replay Worker pipes."));
				StopWorker();
				return false;
			}

			const FString WorkerBaseDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectDir(), TEXT("Binaries"), FPlatformProcess::GetBinariesSubdirectory()));
			const FString FullDiagnosticsArgument = DiagnosticsOptions.bEnabled && DiagnosticsOptions.bFullEvidencePayload
				? TEXT(" -RenderTrailFullDiagnostics")
				: TEXT("");
			const FString Args = FString::Printf(TEXT("-basedir=\"%s\" -Server -Capture=\"%s\" -Preview=\"%s\"%s"),
				*WorkerBaseDir, *Capture, *InPreviewPath, *FullDiagnosticsArgument);
			SetCaptureLoadPhase(TEXT("Starting isolated Replay Worker and opening .rdc"));
			SetEvidence(TEXT("Opening the capture in an isolated RenderDoc replay process and exporting its final image..."));
			UE_LOG(LogRenderTrailAnalyzer, Display,
				TEXT("Starting isolated Replay Worker. Worker='%s' BaseDir='%s' Capture='%s' Preview='%s'"),
				*Worker, *WorkerBaseDir, *Capture, *InPreviewPath);
			WorkerHandle = FPlatformProcess::CreateProc(*Worker, *Args, false, true, true, nullptr, 0, *WorkerBaseDir,
				StdOutWrite, StdInRead, StdErrWrite);
			if (!WorkerHandle.IsValid())
			{
				SetStatus(TEXT("Failed to launch isolated Replay Worker."));
				StopWorker();
				return false;
			}
			FPlatformProcess::ClosePipe(nullptr, StdOutWrite);
			StdOutWrite = nullptr;
			FPlatformProcess::ClosePipe(StdInRead, nullptr);
			StdInRead = nullptr;
			FPlatformProcess::ClosePipe(nullptr, StdErrWrite);
			StdErrWrite = nullptr;
			SetStatus(TEXT("正在载入截帧… 0.0s · 等待隔离 Replay Worker 打开 .rdc 并生成预览"));
			return true;
		}

		void StartWorker(bool bPreserveSelection = false)
		{
			if (bCaptureLoading)
			{
				FinishCaptureLoad(TEXT("restarted"));
			}
			const FString Capture = FPaths::ConvertRelativePathToFull(GetCapturePath());
			if (!FPaths::FileExists(Capture))
			{
				SetStatus(TEXT("Capture file does not exist."));
				return;
			}
			const FString Worker = GetReplayWorkerExecutablePath();
			if (!FPaths::FileExists(Worker))
			{
				SetStatus(FString::Printf(TEXT("Replay Worker is missing: %s"), *Worker));
				return;
			}

			PreviewPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::GetPath(Capture), TEXT(".."), TEXT("Previews"), FPaths::GetBaseFilename(Capture) + TEXT(".png")));
			CaptureLoadStartSeconds = FPlatformTime::Seconds();
			LastCaptureLoadStatusSeconds = CaptureLoadStartSeconds;
			bCaptureLoading = true;
			CaptureLoadPhase = TEXT("准备 Replay Worker");
			const int64 CaptureSize = IFileManager::Get().FileSize(*Capture);
			UE_LOG(LogRenderTrailAnalyzer, Display,
				TEXT("Capture load started: capture='%s' bytes=%lld worker='%s'"),
				*Capture, CaptureSize, *Worker);
			SetCaptureLoadPhase(TEXT("停止旧 Worker 并准备通信管道"));
			StopWorker();
			ReleasePreview();
			if (!bPreserveSelection)
			{
				ResetSamples();
				bQueuePixelHistoryAfterWorkerReady = false;
			}
			bReplayStartDeferred = false;
			bWorkerReady = false;
			bExitReported = false;
			OutputBuffer.Empty();
			ErrorBuffer.Empty();
			LastWorkerError.Empty();
			BeginDiagnosticsSession(Capture, CaptureSize);
			if (!bPreserveSelection && IsPreviewCacheValid(Capture, PreviewPath)
				&& LoadPreview(PreviewPath, FIntPoint::ZeroValue))
			{
				bReplayStartDeferred = true;
				FinishCaptureLoad(TEXT("cached preview; replay deferred"));
				UE_LOG(LogRenderTrailAnalyzer, Display,
					TEXT("Capture preview cache hit; deferred Replay Worker startup. capture='%s' preview='%s'"),
					*Capture, *PreviewPath);
				SetStatus(TEXT("预览已快速载入；选择像素后点击“分析”才打开完整 Replay 数据"));
				SetReportCards(
					TEXT("预览已载入。完整 Replay 数据暂不打开，先选择需要检查的像素。"),
					TEXT("确认分析后按需启动 Replay Worker，并只读取所选像素的 Pixel History。"),
					TEXT("尚无候选原因。"),
					TEXT("已复用预览缓存；.rdc 的 ReplayController 将延迟到真正分析时建立。"));
				return;
			}
			if (!FPlatformProcess::CreatePipe(StdOutRead, StdOutWrite, false)
				|| !FPlatformProcess::CreatePipe(StdInRead, StdInWrite, true)
				|| !FPlatformProcess::CreatePipe(StdErrRead, StdErrWrite, false))
			{
				FinishCaptureLoad(TEXT("pipe creation failed"));
				SetStatus(TEXT("Could not create Replay Worker pipes."));
				StopWorker();
				return;
			}

			const FString WorkerBaseDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectDir(), TEXT("Binaries"), FPlatformProcess::GetBinariesSubdirectory()));
			const FString FullDiagnosticsArgument = DiagnosticsOptions.bEnabled && DiagnosticsOptions.bFullEvidencePayload
				? TEXT(" -RenderTrailFullDiagnostics")
				: TEXT("");
			const FString Args = FString::Printf(TEXT("-basedir=\"%s\" -Server -Capture=\"%s\" -Preview=\"%s\"%s"),
				*WorkerBaseDir, *Capture, *PreviewPath, *FullDiagnosticsArgument);
			if (DiagnosticsOptions.bWorkerProtocol)
			{
				WriteDiagnosticsRecord(TEXT("worker_launch"), Args);
			}
			SetCaptureLoadPhase(TEXT("启动 Replay Worker，等待打开 .rdc 并导出预览"));
			UE_LOG(LogRenderTrailAnalyzer, Display,
				TEXT("Starting Replay Worker. Worker='%s' BaseDir='%s' Capture='%s' Preview='%s'"),
				*Worker, *WorkerBaseDir, *Capture, *PreviewPath);
			WorkerHandle = FPlatformProcess::CreateProc(*Worker, *Args, false, true, true, nullptr, 0, *WorkerBaseDir, StdOutWrite, StdInRead, StdErrWrite);
			if (!WorkerHandle.IsValid())
			{
				FinishCaptureLoad(TEXT("worker launch failed"));
				SetStatus(TEXT("Failed to launch Replay Worker."));
				StopWorker();
				return;
			}
			FPlatformProcess::ClosePipe(nullptr, StdOutWrite);
			StdOutWrite = nullptr;
			FPlatformProcess::ClosePipe(StdInRead, nullptr);
			StdInRead = nullptr;
			FPlatformProcess::ClosePipe(nullptr, StdErrWrite);
			StdErrWrite = nullptr;
			SetStatus(TEXT("正在载入截帧… 0.0s · 等待 Replay Worker 打开 .rdc 并生成预览"));
			SetEvidence(TEXT("Opening the full capture and exporting its final image..."));
		}

		void StopWorker()
		{
			if (!DiagnosticsFilePath.IsEmpty())
			{
				WriteDiagnosticsRecord(TEXT("worker_stop"), TEXT("stop requested"));
			}
			if (WorkerHandle.IsValid())
			{
				if (StdInWrite && FPlatformProcess::IsProcRunning(WorkerHandle))
				{
					if (DiagnosticsOptions.bWorkerProtocol)
					{
						WriteDiagnosticsRecord(TEXT("analyzer_to_worker"), TEXT("{\"command\":\"shutdown\"}"));
					}
					FPlatformProcess::WritePipe(StdInWrite, TEXT("{\"command\":\"shutdown\"}"));
					for (int32 Attempt = 0; Attempt < 20 && FPlatformProcess::IsProcRunning(WorkerHandle); ++Attempt)
					{
						FPlatformProcess::Sleep(0.05f);
					}
				}
				if (FPlatformProcess::IsProcRunning(WorkerHandle))
				{
					FPlatformProcess::TerminateProc(WorkerHandle, true);
				}
				FPlatformProcess::CloseProc(WorkerHandle);
				WorkerHandle.Reset();
			}
			FPlatformProcess::ClosePipe(StdOutRead, StdOutWrite);
			FPlatformProcess::ClosePipe(StdInRead, StdInWrite);
			FPlatformProcess::ClosePipe(StdErrRead, StdErrWrite);
			StdOutRead = StdOutWrite = StdInRead = StdInWrite = StdErrRead = StdErrWrite = nullptr;
			bWorkerReady = false;
		}

		#endif

		static FString GetReplayWorkerExecutablePath()
		{
			const FString BinaryName = TEXT("RenderTrailReplayWorker.exe");
			const FString ProjectBinary = FPaths::Combine(
				FPaths::ProjectDir(), TEXT("Binaries"), FPlatformProcess::GetBinariesSubdirectory(), BinaryName);
			if (FPaths::FileExists(ProjectBinary))
			{
				return FPaths::ConvertRelativePathToFull(ProjectBinary);
			}
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::EngineDir(), TEXT("Binaries"), FPlatformProcess::GetBinariesSubdirectory(), BinaryName));
		}

		bool LaunchWorkerProcess(const FString& Worker, const FString& Capture, const FString& InPreviewPath)
		{
			if (!FPlatformProcess::CreatePipe(StdOutRead, StdOutWrite, false)
				|| !FPlatformProcess::CreatePipe(StdInRead, StdInWrite, true)
				|| !FPlatformProcess::CreatePipe(StdErrRead, StdErrWrite, false))
			{
				SetStatus(TEXT("Could not create Replay Worker pipes."));
				StopWorker();
				return false;
			}

			const FString WorkerBaseDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectDir(), TEXT("Binaries"), FPlatformProcess::GetBinariesSubdirectory()));
			const FString FullDiagnosticsArgument = DiagnosticsOptions.bEnabled && DiagnosticsOptions.bFullEvidencePayload
				? TEXT(" -RenderTrailFullDiagnostics")
				: TEXT("");
			const FString Args = FString::Printf(TEXT("-basedir=\"%s\" -Server -Capture=\"%s\" -Preview=\"%s\"%s"),
				*WorkerBaseDir, *Capture, *InPreviewPath, *FullDiagnosticsArgument);
			if (DiagnosticsOptions.bWorkerProtocol)
			{
				WriteDiagnosticsRecord(TEXT("worker_launch"), Args);
			}
			SetCaptureLoadPhase(TEXT("Starting isolated Replay Worker and opening .rdc"));
			SetEvidence(TEXT("Opening the capture in an isolated RenderDoc replay process and exporting its final image..."));
			UE_LOG(LogRenderTrailAnalyzer, Display,
				TEXT("Starting isolated Replay Worker. Worker='%s' BaseDir='%s' Capture='%s' Preview='%s'"),
				*Worker, *WorkerBaseDir, *Capture, *InPreviewPath);
			WorkerHandle = FPlatformProcess::CreateProc(*Worker, *Args, false, true, true, nullptr, 0, *WorkerBaseDir,
				StdOutWrite, StdInRead, StdErrWrite);
			if (!WorkerHandle.IsValid())
			{
				SetStatus(TEXT("Failed to launch isolated Replay Worker."));
				StopWorker();
				return false;
			}
			FPlatformProcess::ClosePipe(nullptr, StdOutWrite);
			StdOutWrite = nullptr;
			FPlatformProcess::ClosePipe(StdInRead, nullptr);
			StdInRead = nullptr;
			FPlatformProcess::ClosePipe(nullptr, StdErrWrite);
			StdErrWrite = nullptr;
			SetStatus(TEXT("Loading capture 0.0s - waiting for isolated Replay Worker to open .rdc and generate preview"));
			return true;
		}

		void StartWorker(bool bPreserveSelection = false)
		{
			if (bCaptureLoading)
			{
				FinishCaptureLoad(TEXT("restarted"));
			}
			const FString Capture = FPaths::ConvertRelativePathToFull(GetCapturePath());
			if (!FPaths::FileExists(Capture))
			{
				SetStatus(TEXT("Capture file does not exist."));
				return;
			}
			const FString Worker = GetReplayWorkerExecutablePath();
			if (!FPaths::FileExists(Worker))
			{
				SetStatus(FString::Printf(TEXT("Replay Worker is missing: %s. Build the standard RenderTrailReplayWorker Program target first."), *Worker));
				UE_LOG(LogRenderTrailAnalyzer, Error, TEXT("Replay Worker is missing: %s"), *Worker);
				return;
			}

			PreviewPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::GetPath(Capture), TEXT(".."), TEXT("Previews"), FPaths::GetBaseFilename(Capture) + TEXT(".png")));
			CaptureLoadStartSeconds = FPlatformTime::Seconds();
			LastCaptureLoadStatusSeconds = CaptureLoadStartSeconds;
			bCaptureLoading = true;
			CaptureLoadPhase = TEXT("Preparing isolated Replay Worker");
			const int64 CaptureSize = IFileManager::Get().FileSize(*Capture);
			UE_LOG(LogRenderTrailAnalyzer, Display,
				TEXT("Capture load started: capture='%s' bytes=%lld"),
				*Capture, CaptureSize);
			SetCaptureLoadPhase(TEXT("Closing previous replay session"));
			StopWorker();
			BeginDiagnosticsSession(Capture, CaptureSize);
			ReleasePreview();
			if (!bPreserveSelection)
			{
				ResetSamples();
				bQueuePixelHistoryAfterWorkerReady = false;
			}
			bReplayStartDeferred = false;
			bWorkerReady = false;
			bPreviewReadyForSelection = false;
			LastWorkerError.Empty();
			if (!bPreserveSelection && IsPreviewCacheValid(Capture, PreviewPath)
				&& LoadPreview(PreviewPath, FIntPoint::ZeroValue))
			{
				bPreviewReadyForSelection = true;
				UE_LOG(LogRenderTrailAnalyzer, Display,
					TEXT("Capture preview cache hit; starting isolated Replay Worker in the background. capture='%s' preview='%s'"),
					*Capture, *PreviewPath);
				SetStatus(TEXT("预览已快速载入；完整 Replay Worker 正在后台加载。"));
				SetReportCards(
					TEXT("预览已载入，完整 Replay 数据正在后台加载。"),
					TEXT("现在可以选择像素；Pixel History 会在 Replay Worker 就绪后自动排队执行。"),
					TEXT("尚无因果证据。"),
					TEXT("当前使用预览缓存；隔离的 RenderDoc ReplayController 正在后台建立。"));
			}

			if (!LaunchWorkerProcess(Worker, Capture, PreviewPath))
			{
				bPreviewReadyForSelection = false;
				FinishCaptureLoad(TEXT("worker launch failed"));
			}
			else if (bPreviewReadyForSelection)
			{
				SetStatus(TEXT("预览已就绪；完整 Replay Worker 正在后台加载。"));
				SetReportCards(
					TEXT("预览已就绪，完整 Replay 数据正在后台加载。"),
					TEXT("现在可以选择像素；Pixel History 会在 Replay Worker 就绪后自动排队执行。"),
					TEXT("尚无因果证据。"),
					TEXT("当前使用预览缓存；隔离的 RenderDoc ReplayController 正在后台建立。"));
			}
#if 0 // Deliberately disabled: RenderDoc Replay must never run inside UnrealEditor.
			SetCaptureLoadPhase(TEXT("Opening .rdc in the Unreal Editor and exporting preview"));
			SetEvidence(TEXT("Opening the capture inside the Unreal Editor process and exporting its final image..."));
			FString ReplayError;
			const bool bOpened = UE::RenderTrail::Replay::OpenCapture(
				Capture,
				PreviewPath,
				[this](const FString& JsonLine)
				{
					HandleWorkerMessage(JsonLine);
				},
				ReplayError);
			if (!bOpened)
			{
				LastWorkerError = ReplayError;
				FinishCaptureLoad(TEXT("replay open failed"));
				SetStatus(FString::Printf(TEXT("Replay 打开失败: %s"), *ReplayError));
				UE_LOG(LogRenderTrailAnalyzer, Error, TEXT("In-editor replay open failed. Capture='%s' Detail='%s'"),
					*Capture, *ReplayError.Left(2000));
			}
#endif
		}

		void StopWorker()
		{
			if (!DiagnosticsFilePath.IsEmpty())
			{
				WriteDiagnosticsRecord(TEXT("worker_stop"), TEXT("stop requested"));
			}
			if (WorkerHandle.IsValid())
			{
				if (StdInWrite && FPlatformProcess::IsProcRunning(WorkerHandle))
				{
					if (DiagnosticsOptions.bWorkerProtocol)
					{
						WriteDiagnosticsRecord(TEXT("analyzer_to_worker"), TEXT("{\"command\":\"shutdown\"}"));
					}
					FPlatformProcess::WritePipe(StdInWrite, TEXT("{\"command\":\"shutdown\"}"));
					for (int32 Attempt = 0; Attempt < 20 && FPlatformProcess::IsProcRunning(WorkerHandle); ++Attempt)
					{
						FPlatformProcess::Sleep(0.05f);
					}
				}
				if (FPlatformProcess::IsProcRunning(WorkerHandle))
				{
					FPlatformProcess::TerminateProc(WorkerHandle, true);
				}
				FPlatformProcess::CloseProc(WorkerHandle);
				WorkerHandle.Reset();
			}
			FPlatformProcess::ClosePipe(StdOutRead, StdOutWrite);
			FPlatformProcess::ClosePipe(StdInRead, StdInWrite);
			FPlatformProcess::ClosePipe(StdErrRead, StdErrWrite);
			StdOutRead = StdOutWrite = StdInRead = StdInWrite = StdErrRead = StdErrWrite = nullptr;
			OutputBuffer.Empty();
			ErrorBuffer.Empty();
			bWorkerReady = false;
			bPreviewReadyForSelection = false;
			bExitReported = false;
		}

		void ProcessWorkerOutput(FString& Buffer)
		{
			int32 NewlineIndex = INDEX_NONE;
			while (Buffer.FindChar(TEXT('\n'), NewlineIndex))
			{
				FString Line = Buffer.Left(NewlineIndex);
				Buffer.RightChopInline(NewlineIndex + 1, EAllowShrinking::No);
				Line.TrimStartAndEndInline();
				if (!Line.IsEmpty())
				{
					if (DiagnosticsOptions.bWorkerProtocol)
					{
						WriteDiagnosticsRecord(TEXT("worker_stdout"), Line);
					}
					HandleWorkerMessage(Line);
				}
			}
		}

		void PollWorkerPipes()
		{
			if (!WorkerHandle.IsValid())
			{
				return;
			}
			if (StdOutRead)
			{
				OutputBuffer += FPlatformProcess::ReadPipe(StdOutRead);
				ProcessWorkerOutput(OutputBuffer);
			}
			if (StdErrRead)
			{
				const FString ErrorChunk = FPlatformProcess::ReadPipe(StdErrRead);
				if (!ErrorChunk.IsEmpty() && DiagnosticsOptions.bWorkerProtocol)
				{
					WriteDiagnosticsRecord(TEXT("worker_stderr"), ErrorChunk);
				}
				ErrorBuffer += ErrorChunk;
				if (ErrorBuffer.Len() > 16384)
				{
					ErrorBuffer.RightChopInline(ErrorBuffer.Len() - 16384, EAllowShrinking::No);
				}
			}

			if (!FPlatformProcess::IsProcRunning(WorkerHandle) && !bExitReported)
			{
				bExitReported = true;
				if (!OutputBuffer.IsEmpty() && DiagnosticsOptions.bWorkerProtocol)
				{
					WriteDiagnosticsRecord(TEXT("worker_stdout_partial"), OutputBuffer);
				}
				if (StdOutRead)
				{
					OutputBuffer += FPlatformProcess::ReadPipe(StdOutRead);
					ProcessWorkerOutput(OutputBuffer);
				}
				if (!bWorkerReady && bCaptureLoading)
				{
					const FString Detail = ErrorBuffer.IsEmpty()
						? TEXT("Replay Worker exited before reporting ready.")
						: ErrorBuffer.Right(2000);
					LastWorkerError = Detail;
					FinishCaptureLoad(TEXT("worker exited"));
					SetStatus(FString::Printf(TEXT("Replay Worker exited: %s"), *Detail));
					UE_LOG(LogRenderTrailAnalyzer, Error, TEXT("Replay Worker exited before ready. Detail='%s'"), *Detail);
				}
			}
		}

		bool SendWorkerRequest(const FString& Command, const FString& RequestId,
			TFunctionRef<void(const TSharedRef<FJsonObject>&)> Populate)
		{
			if (!WorkerHandle.IsValid() || !StdInWrite || !FPlatformProcess::IsProcRunning(WorkerHandle))
			{
				SetStatus(TEXT("Replay Worker is not running."));
				return false;
			}
			const TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
			Request->SetStringField(TEXT("command"), Command);
			Request->SetStringField(TEXT("requestId"), RequestId);
			Populate(Request);
			const FString Payload = SerializeJson(Request);
			if (DiagnosticsOptions.bWorkerProtocol)
			{
				WriteDiagnosticsRecord(TEXT("analyzer_to_worker"), Payload);
			}
			return FPlatformProcess::WritePipe(StdInWrite, Payload);
		}

		void ReleasePreview()
		{
			if (ImageView.IsValid())
			{
				ImageView->SetImage(nullptr, FIntPoint::ZeroValue);
			}
			if (PreviewBrush.IsValid() && FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().GetRenderer()->ReleaseDynamicResource(*PreviewBrush);
			}
			PreviewBrush.Reset();
		}

		static bool IsPreviewCacheValid(const FString& CapturePath, const FString& InPreviewPath)
		{
			if (!IFileManager::Get().FileExists(*InPreviewPath))
			{
				return false;
			}
			const FDateTime CaptureTimestamp = IFileManager::Get().GetTimeStamp(*CapturePath);
			const FDateTime PreviewTimestamp = IFileManager::Get().GetTimeStamp(*InPreviewPath);
			return CaptureTimestamp != FDateTime::MinValue()
				&& PreviewTimestamp != FDateTime::MinValue()
				&& PreviewTimestamp >= CaptureTimestamp;
		}

		bool LoadPreview(const FString& Path, FIntPoint ExpectedSize)
		{
			TArray64<uint8> Compressed;
			if (!FFileHelper::LoadFileToArray(Compressed, *Path))
			{
				return false;
			}
			IImageWrapperModule& ImageWrapper = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
			FImage Image;
			if (!ImageWrapper.DecompressImage(Compressed.GetData(), Compressed.Num(), Image))
			{
				return false;
			}
			Image.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			TArray<uint8> Pixels(MoveTemp(Image.RawData));
			// Final UE render targets often carry alpha=0 even though their RGB is the intended
			// viewport image. Standalone Slate composites that alpha, so preview as opaque RGB.
			for (int64 Alpha = 3; Alpha < Pixels.Num(); Alpha += 4)
			{
				Pixels[Alpha] = 255;
			}
			const FString ResourceString = FString::Printf(TEXT("RenderTrailPreview_%s_%llu"), *FPaths::GetBaseFilename(Path), ++PreviewSerial);
			const FName ResourceName(*ResourceString);
			if (!FSlateApplication::Get().GetRenderer()->GenerateDynamicImageResource(ResourceName, Image.SizeX, Image.SizeY, Pixels))
			{
				return false;
			}
			PreviewBrush = MakeShared<FSlateDynamicImageBrush>(ResourceName, FVector2D(Image.SizeX, Image.SizeY));
			const FIntPoint ActualSize(Image.SizeX, Image.SizeY);
			ImageView->SetImage(PreviewBrush, ActualSize, MoveTemp(Pixels));
			return ActualSize == ExpectedSize || ExpectedSize == FIntPoint::ZeroValue;
		}

		void HandleWorkerMessage(const FString& Line)
		{
			TSharedPtr<FJsonObject> Message;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
			if (!FJsonSerializer::Deserialize(Reader, Message) || !Message.IsValid())
			{
				// The worker's stdout may also contain Unreal startup diagnostics. Only
				// JSON lines are part of the RenderTrail protocol.
				return;
			}
			FString Type;
			Message->TryGetStringField(TEXT("type"), Type);
			if (Type == TEXT("progress"))
			{
				FString Phase;
				double WorkerElapsed = 0.0;
				Message->TryGetStringField(TEXT("phase"), Phase);
				Message->TryGetNumberField(TEXT("elapsedSeconds"), WorkerElapsed);
				SetCaptureLoadPhase(FString::Printf(TEXT("Isolated Replay Worker: %s (%.1fs)"), *Phase, WorkerElapsed));
				return;
			}
			if (Type == TEXT("preview"))
			{
				FString Path;
				FString Source;
				double PreviewWidth = 0.0;
				double PreviewHeight = 0.0;
				Message->TryGetStringField(TEXT("previewPath"), Path);
				Message->TryGetStringField(TEXT("source"), Source);
				Message->TryGetNumberField(TEXT("width"), PreviewWidth);
				Message->TryGetNumberField(TEXT("height"), PreviewHeight);
				if (!Path.IsEmpty() && LoadPreview(Path, FIntPoint::ZeroValue))
				{
					bPreviewReadyForSelection = true;
					SetCaptureLoadPhase(FString::Printf(TEXT("Fast preview ready (%.0fx%.0f); full Replay Worker continues in background"),
						PreviewWidth, PreviewHeight));
					SetStatus(TEXT("快速预览已就绪；完整 Replay Worker 正在后台加载，现在可以选择像素。"));
					SetReportCards(
						TEXT("最终画面预览已就绪；完整 Replay 数据仍在加载。"),
						TEXT("现在可以选择像素；Pixel History 请求会等待隔离 Replay Worker 就绪。"),
						TEXT("尚无因果证据。"),
						FString::Printf(TEXT("预览来源：%s。Replay 就绪后会用精确最终 RT 替换当前预览。"),
							Source.IsEmpty() ? TEXT("截帧内嵌缩略图") : *Source));
				}
				else
				{
					UE_LOG(LogRenderTrailAnalyzer, Warning, TEXT("Fast capture preview could not be loaded: path='%s'"), *Path);
				}
				return;
			}
			if (Type == TEXT("ready"))
			{
				bWorkerReady = true;
				bPreviewReadyForSelection = true;
				const int32 Width = static_cast<int32>(Message->GetNumberField(TEXT("width")));
				const int32 Height = static_cast<int32>(Message->GetNumberField(TEXT("height")));
				const FString Path = Message->GetStringField(TEXT("previewPath"));
				const FString Target = Message->GetStringField(TEXT("targetName"));
				const FString Version = Message->GetStringField(TEXT("renderDocVersion"));
				const bool bPixelHistory = Message->GetBoolField(TEXT("pixelHistorySupported"));
				const bool bShaderDebug = Message->GetBoolField(TEXT("shaderDebuggingSupported"));
				bShaderDebuggingAvailable = bShaderDebug;
				bool bPreviewCached = false;
				Message->TryGetBoolField(TEXT("previewCached"), bPreviewCached);
				UE_LOG(LogRenderTrailAnalyzer, Display,
					TEXT("Isolated Replay Worker ready: elapsed=%.3fs RenderDoc=%s Size=%dx%d Target='%s' PixelHistory=%s ShaderDebug=%s Preview='%s'"),
					bCaptureLoading ? FPlatformTime::Seconds() - CaptureLoadStartSeconds : 0.0,
					*Version, Width, Height, *Target, bPixelHistory ? TEXT("yes") : TEXT("no"),
					bShaderDebug ? TEXT("yes") : TEXT("no"), *Path);
				SetCaptureLoadPhase(bPreviewCached
					? TEXT("Isolated Replay Worker ready; reusing cached preview")
					: TEXT("Isolated Replay Worker ready; decoding exported preview"));
				const double PreviewStartSeconds = FPlatformTime::Seconds();
				if (!LoadPreview(Path, FIntPoint(Width, Height)))
				{
					UE_LOG(LogRenderTrailAnalyzer, Error, TEXT("Capture preview load failed: elapsed=%.3fs path='%s'"),
						FPlatformTime::Seconds() - PreviewStartSeconds, *Path);
					FinishCaptureLoad(TEXT("preview load failed"));
					SetStatus(FString::Printf(TEXT("Replay opened, but preview could not be loaded: %s"), *Path));
					return;
				}
				const double PreviewElapsed = FPlatformTime::Seconds() - PreviewStartSeconds;
				UE_LOG(LogRenderTrailAnalyzer, Display, TEXT("Capture preview loaded: elapsed=%.3fs path='%s'"),
					PreviewElapsed, *Path);
				const double TotalLoadElapsed = bCaptureLoading ? FPlatformTime::Seconds() - CaptureLoadStartSeconds : 0.0;
				FinishCaptureLoad(TEXT("ready"));
				SetStatus(FString::Printf(TEXT("RenderDoc %s | %dx%d | %s | Pixel History: %s | Shader Debug: %s | 载入耗时 %.1fs"),
					*Version, Width, Height, *Target, bPixelHistory ? TEXT("yes") : TEXT("no"), bShaderDebug ? TEXT("yes") : TEXT("no"), TotalLoadElapsed));
				SetReportCards(
					TEXT("截帧已载入。直接选择一个需要解释的位置，不必浏览整棵事件树。"),
					TEXT("最终画面\n↓\n等待选择 P1"),
					TEXT("尚无候选原因。"),
					TEXT("截帧已由隔离 Replay Worker 完整载入；等待确认选点，尚未执行 Pixel History 查询。"));
				if (bQueuePixelHistoryAfterWorkerReady)
				{
					bQueuePixelHistoryAfterWorkerReady = false;
					ConfirmPixelSelection();
				}
				return;
			}
			if (Type == TEXT("pixel_history"))
			{
				StorePixelHistory(Message.ToSharedRef());
				return;
			}
			if (Type == TEXT("event_context"))
			{
				StoreEventContext(Message.ToSharedRef());
				return;
			}
			if (Type == TEXT("shader_debug"))
			{
				StoreShaderDebug(Message.ToSharedRef());
				return;
			}
			if (Type == TEXT("error"))
			{
				FString Stage;
				FString Error;
				FString RequestId;
				Message->TryGetStringField(TEXT("stage"), Stage);
				Message->TryGetStringField(TEXT("message"), Error);
				Message->TryGetStringField(TEXT("requestId"), RequestId);
				const bool bKnownRequest = RequestId.IsEmpty()
					|| PendingSampleByRequest.Contains(RequestId)
					|| PendingEventContextByRequest.Contains(RequestId)
					|| PendingShaderDebugByRequest.Contains(RequestId);
				if (!bKnownRequest)
				{
					UE_LOG(LogRenderTrailAnalyzer, Verbose,
						TEXT("Ignoring stale Replay Worker error after pixel replacement. Stage='%s' Request='%s' Message='%s'"),
						*Stage, *RequestId, *Error.Left(2000));
					return;
				}
				LastWorkerError = FString::Printf(TEXT("%s: %s"), *Stage, *Error);
				if (bCaptureLoading)
				{
					FinishCaptureLoad(FString::Printf(TEXT("worker error: %s"), *Stage));
					bReplaySynchronizationPending = false;
					bQueuePixelHistoryAfterWorkerReady = false;
				}
				UE_LOG(LogRenderTrailAnalyzer, Error, TEXT("Isolated Replay Worker error. Stage='%s' Request='%s' Message='%s'"),
					*Stage, *RequestId, *Error.Left(2000));
				if (const uint64* SampleId = PendingSampleByRequest.Find(RequestId))
				{
					if (FPixelSample* Sample = FindSample(*SampleId))
					{
						Sample->bPending = false;
						Sample->bFailed = true;
						Sample->bAnalyzed = false;
						Sample->Error = Error;
					}
					PendingSampleByRequest.Remove(RequestId);
					UpdateSelectionText();
					RenderCausalReport();
				}
				if (const uint32* EventId = PendingEventContextByRequest.Find(RequestId))
				{
					const uint32 FailedEventId = *EventId;
					FailedEventContextIds.Add(FailedEventId);
					PendingEventContextIds.Remove(FailedEventId);
					PendingEventContextByRequest.Remove(RequestId);
					RenderCausalReport();
					ResumeAgentAfterEventContext(FailedEventId);
					TryResumeAgentAfterDeterministicContexts();
				}
				if (const uint32* EventId = PendingShaderDebugByRequest.Find(RequestId))
				{
					FailedShaderDebugIds.Add(*EventId);
					PendingShaderDebugByRequest.Remove(RequestId);
					RenderCausalReport();
					TryResumeAgentAfterDeterministicContexts();
				}
				SetStatus(FString::Printf(TEXT("%s: %s"), *Stage, *Error));
			}
		}

		void QueryPixel(int32 X, int32 Y)
		{
			if (!bWorkerReady && !bPreviewReadyForSelection && !bReplayStartDeferred)
			{
				SetStatus(TEXT("Replay Worker is not ready."));
				return;
			}
			CancelAgentRun();
			bSelectionConfirmed = false;
			SetAgentOutputText(TEXT("选点已改变；点击“读取 Pixel History”后更新证据。"));
			SetAgentStatus(TEXT("等待确认选点；尚未启动 Pixel History。"));

			const FIntPoint Pixel(X, Y);
			if (!Samples.IsEmpty() && Samples[0].Pixel == Pixel)
			{
				ResetSamples();
				RenderCausalReport();
				SetStatus(FString::Printf(TEXT("已清除关注像素 (%d, %d)。"), X, Y));
				return;
			}

			const bool bReplacingSelection = Samples.Num() >= MaxPixelSamples;
			if (bReplacingSelection)
			{
				// A new pixel owns a completely separate evidence session. Clearing the old
				// request maps also makes late Worker responses unclaimable by the new point.
				ResetSamples();
			}

			FPixelSample Sample;
			Sample.Id = ++SampleSerial;
			Sample.Pixel = Pixel;
			Samples.Add(MoveTemp(Sample));
			SetAgentOutputText(TEXT("当前像素已改变；点击“分析”后更新证据。"));
			SetAgentStatus(TEXT("等待确认当前像素；尚未启动 Pixel History。"));
			UpdateMarkers();
			UpdateSelectionText();
			RenderCausalReport();
			if (bReplacingSelection)
			{
				SetStatus(FString::Printf(TEXT("已将关注像素替换为 P1 (%d, %d)，点击“分析”后读取该点证据。"), X, Y));
			}
			else
			{
				SetStatus(FString::Printf(TEXT("已选择关注像素 P1 (%d, %d)，点击“分析”后读取该点证据。"), X, Y));
			}
		}

		static FString FormatFloatValue(const TSharedPtr<FJsonObject>& Value)
		{
			if (!Value.IsValid() || !Value->GetBoolField(TEXT("valid")))
			{
				return TEXT("<unavailable>");
			}
			const TArray<TSharedPtr<FJsonValue>>& Components = Value->GetArrayField(TEXT("float"));
			TArray<FString> Text;
			for (const TSharedPtr<FJsonValue>& Component : Components)
			{
				Text.Add(Component->Type == EJson::Null ? TEXT("NaN") : FString::Printf(TEXT("%.6g"), Component->AsNumber()));
			}
			return FString::Printf(TEXT("RGBA (%s)  depth %.6g  stencil %d"), *FString::Join(Text, TEXT(", ")),
				Value->HasTypedField<EJson::Number>(TEXT("depth")) ? Value->GetNumberField(TEXT("depth")) : 0.0,
				static_cast<int32>(Value->GetNumberField(TEXT("stencil"))));
		}

		static FBoundResourceEvidence ParseBoundResource(const TSharedPtr<FJsonObject>& Json)
		{
			FBoundResourceEvidence Resource;
			if (!Json.IsValid())
			{
				return Resource;
			}
			Json->TryGetStringField(TEXT("name"), Resource.Name);
			Json->TryGetStringField(TEXT("stage"), Resource.Stage);
			Json->TryGetStringField(TEXT("access"), Resource.Access);
			Json->TryGetBoolField(TEXT("texture"), Resource.bTexture);
			double Number = 0.0;
			if (Json->TryGetNumberField(TEXT("width"), Number))
				Resource.Width = static_cast<int32>(Number);
			if (Json->TryGetNumberField(TEXT("height"), Number))
				Resource.Height = static_cast<int32>(Number);
			if (Json->TryGetNumberField(TEXT("samples"), Number))
				Resource.Samples = static_cast<int32>(Number);
			return Resource;
		}

		void StoreEventContext(const TSharedRef<FJsonObject>& Message)
		{
			FString RequestId;
			Message->TryGetStringField(TEXT("requestId"), RequestId);
			const uint32 EventId = static_cast<uint32>(Message->GetNumberField(TEXT("eventId")));
			const uint32* RequestedEventId = PendingEventContextByRequest.Find(RequestId);
			if (!RequestedEventId || *RequestedEventId != EventId)
			{
				// The selected pixel may have been replaced while this request was running.
				// Never attach an old point's context to the new single-pixel session.
				return;
			}
			PendingEventContextByRequest.Remove(RequestId);
			PendingEventContextIds.Remove(EventId);

			FEventContextEvidence Context;
			Context.EventId = EventId;
			Message->TryGetStringField(TEXT("shaderStage"), Context.ShaderStage);
			Message->TryGetStringField(TEXT("shaderEntry"), Context.ShaderEntry);
			Message->TryGetStringField(TEXT("shaderDebugStatus"), Context.ShaderDebugStatus);
			Message->TryGetStringField(TEXT("shaderEncoding"), Context.ShaderEncoding);
			Message->TryGetBoolField(TEXT("shaderDebuggable"), Context.bShaderDebuggable);
			Message->TryGetBoolField(TEXT("sourceDebugInfo"), Context.bSourceDebugInfo);
			double Number = 0.0;
			if (Message->TryGetNumberField(TEXT("shaderInputSignatureCount"), Number))
				Context.ShaderInputSignatureCount = static_cast<int32>(Number);
			if (Message->TryGetNumberField(TEXT("shaderOutputSignatureCount"), Number))
				Context.ShaderOutputSignatureCount = static_cast<int32>(Number);
			if (Message->TryGetNumberField(TEXT("shaderConstantBlockCount"), Number))
				Context.ShaderConstantBlockCount = static_cast<int32>(Number);
			if (Message->TryGetNumberField(TEXT("shaderSamplerCount"), Number))
				Context.ShaderSamplerCount = static_cast<int32>(Number);
			if (Message->TryGetNumberField(TEXT("shaderReadOnlyResourceCount"), Number))
				Context.ShaderReadOnlyResourceCount = static_cast<int32>(Number);
			if (Message->TryGetNumberField(TEXT("shaderReadWriteResourceCount"), Number))
				Context.ShaderReadWriteResourceCount = static_cast<int32>(Number);
			const TSharedPtr<FJsonObject>* PipelineState = nullptr;
			if (Message->TryGetObjectField(TEXT("pipelineState"), PipelineState) && PipelineState)
			{
				Context.PipelineState = *PipelineState;
			}
			for (const TSharedPtr<FJsonValue>& Input : Message->GetArrayField(TEXT("inputs")))
			{
				Context.Inputs.Add(ParseBoundResource(Input->AsObject()));
			}
			for (const TSharedPtr<FJsonValue>& Output : Message->GetArrayField(TEXT("outputs")))
			{
				Context.Outputs.Add(ParseBoundResource(Output->AsObject()));
			}
			const TArray<TSharedPtr<FJsonValue>>* Provenance = nullptr;
			if (Message->TryGetArrayField(TEXT("resourceProvenance"), Provenance) && Provenance)
			{
				Context.ResourceProvenance = *Provenance;
			}
			EventContexts.Add(EventId, MoveTemp(Context));
			RenderCausalReport();
			ResumeAgentAfterEventContext(EventId);
			SetStatus(FString::Printf(TEXT("Event %u 的 Pipeline、资源绑定和 Shader 反射已加载；详细内容可展开查看。"), EventId));
			TryResumeAgentAfterDeterministicContexts();
		}

		void StoreShaderDebug(const TSharedRef<FJsonObject>& Message)
		{
			FString RequestId;
			Message->TryGetStringField(TEXT("requestId"), RequestId);
			const uint32 EventId = static_cast<uint32>(Message->GetNumberField(TEXT("eventId")));
			const uint32* RequestedEventId = PendingShaderDebugByRequest.Find(RequestId);
			if (!RequestedEventId || *RequestedEventId != EventId)
			{
				return;
			}
			PendingShaderDebugByRequest.Remove(RequestId);
			FEventContextEvidence& Context = EventContexts.FindOrAdd(EventId);
			Context.EventId = EventId;
			Context.ShaderDebugTrace = Message;
			FailedShaderDebugIds.Remove(EventId);
			RenderCausalReport();
			SetStatus(FString::Printf(TEXT("EID %u 的 Pixel Shader 指令追踪已加载；详细证据已更新。"), EventId));
			TryResumeAgentAfterDeterministicContexts();
		}

		void EnsureEventContext(uint32 EventId)
		{
			if (!bWorkerReady || EventContexts.Contains(EventId)
				|| PendingEventContextIds.Contains(EventId) || FailedEventContextIds.Contains(EventId))
			{
				return;
			}
			const FString RequestId = FString::Printf(TEXT("context-%u-query-%llu"), EventId, ++RequestSerial);
			PendingEventContextByRequest.Add(RequestId, EventId);
			PendingEventContextIds.Add(EventId);
			SendWorkerRequest(TEXT("event_context"), RequestId,
				[EventId](const TSharedRef<FJsonObject>& Request)
				{
					Request->SetNumberField(TEXT("eventId"), EventId);
				});
		}

		void EnsureRelevantEventContexts()
		{
			if (!bWorkerReady)
			{
				return;
			}

			TArray<uint32> RelevantEventIds;
			for (const FPixelSample& Sample : Samples)
			{
				const TArray<FEventEvidence> Events = AggregateEvents(Sample);
				for (int32 Index = Events.Num() - 1; Index >= 0; --Index)
				{
					const FEventEvidence& Event = Events[Index];
					if (!IsConfirmedPixelWriter(Event) || Event.ActionKind == TEXT("present"))
					{
						continue;
					}
					RelevantEventIds.AddUnique(Event.EventId);
					if (RelevantEventIds.Num() >= MaxDeterministicContextEvents)
					{
						break;
					}
				}
				if (RelevantEventIds.Num() >= MaxDeterministicContextEvents)
				{
					break;
				}
			}
			for (const FPixelSample& Sample : Samples)
			{
				const TArray<FEventEvidence> Events = AggregateEvents(Sample);
				for (int32 Index = Events.Num() - 1; Index >= 0 && RelevantEventIds.Num() < MaxDeterministicContextEvents; --Index)
				{
					const FEventEvidence& Event = Events[Index];
					if (Event.ActionKind != TEXT("present") && !IsConfirmedPixelWriter(Event))
					{
						RelevantEventIds.AddUnique(Event.EventId);
					}
				}
			}

			for (const uint32 EventId : RelevantEventIds)
			{
				EnsureEventContext(EventId);
			}
		}

		void EnsureCandidateShaderDebug()
		{
			if (!bShaderDebuggingAvailable || !LastCandidate.IsSet() || !bWorkerReady)
			{
				return;
			}
			const FEventEvidence& Candidate = LastCandidate->Event;
			if (Candidate.ActionKind != TEXT("draw") || FailedShaderDebugIds.Contains(Candidate.EventId))
			{
				return;
			}
			if (const FEventContextEvidence* Context = EventContexts.Find(Candidate.EventId))
			{
				if (Context->ShaderDebugTrace.IsValid())
				{
					return;
				}
			}
			bool bShaderDebugPending = false;
			for (const TPair<FString, uint32>& Pair : PendingShaderDebugByRequest)
			{
				if (Pair.Value == Candidate.EventId)
				{
					bShaderDebugPending = true;
					break;
				}
			}
			if (bShaderDebugPending)
			{
				return;
			}

			const FPixelSample* SourceSample = nullptr;
			for (const FPixelSample& Sample : Samples)
			{
				const TArray<FEventEvidence> Events = AggregateEvents(Sample);
				if (FindEvent(Events, Candidate.EventId))
				{
					SourceSample = &Sample;
					break;
				}
			}
			if (!SourceSample)
			{
				return;
			}

			const FString RequestId = FString::Printf(TEXT("shader-debug-%u-query-%llu"), Candidate.EventId, ++RequestSerial);
			PendingShaderDebugByRequest.Add(RequestId, Candidate.EventId);
			SendWorkerRequest(TEXT("shader_debug"), RequestId,
				[&Candidate, SourceSample](const TSharedRef<FJsonObject>& Request)
				{
					Request->SetNumberField(TEXT("eventId"), Candidate.EventId);
					Request->SetNumberField(TEXT("x"), SourceSample->Pixel.X);
					Request->SetNumberField(TEXT("y"), SourceSample->Pixel.Y);
					Request->SetNumberField(TEXT("primitiveId"), Candidate.PrimitiveId);
					Request->SetBoolField(TEXT("hasPrimitive"), Candidate.bHasPrimitiveEvidence);
				});
		}

		void TryResumeAgentAfterDeterministicContexts()
		{
			if (bAgentWaitingForDeterministicContexts && PendingEventContextIds.IsEmpty() && PendingShaderDebugByRequest.IsEmpty())
			{
				bAgentWaitingForDeterministicContexts = false;
				StartAgentAnalysis();
			}
		}

		void StorePixelHistory(const TSharedRef<FJsonObject>& Message)
		{
			FString RequestId;
			Message->TryGetStringField(TEXT("requestId"), RequestId);
			const uint64* SampleId = PendingSampleByRequest.Find(RequestId);
			if (!SampleId)
			{
				return;
			}
			FPixelSample* Sample = FindSample(*SampleId);
			PendingSampleByRequest.Remove(RequestId);
			if (!Sample)
			{
				return;
			}

			Sample->bPending = false;
			Sample->bFailed = false;
			Sample->bAnalyzed = true;
			Sample->TotalModifications = static_cast<int32>(Message->GetNumberField(TEXT("totalModifications")));
			Sample->bTruncated = Message->GetBoolField(TEXT("truncated"));
			Sample->bEventSummaryComplete = false;
			Sample->EventSummaries.Empty();
			Sample->Modifications.Empty();
			const TArray<TSharedPtr<FJsonValue>>* EventSummaries = nullptr;
			double TotalEvents = 0.0;
			if (Message->TryGetArrayField(TEXT("eventSummaries"), EventSummaries)
				&& EventSummaries && Message->TryGetNumberField(TEXT("totalEvents"), TotalEvents))
			{
				Sample->bEventSummaryComplete = static_cast<int32>(TotalEvents) == EventSummaries->Num();
				for (const TSharedPtr<FJsonValue>& JsonValue : *EventSummaries)
				{
					const TSharedPtr<FJsonObject> Summary = JsonValue.IsValid() ? JsonValue->AsObject() : nullptr;
					if (!Summary.IsValid())
					{
						Sample->bEventSummaryComplete = false;
						continue;
					}
					FEventSummaryEvidence Evidence;
					Evidence.EventId = static_cast<uint32>(Summary->GetNumberField(TEXT("eventId")));
					Summary->TryGetStringField(TEXT("action"), Evidence.Action);
					Summary->TryGetStringField(TEXT("actionKind"), Evidence.ActionKind);
					Summary->TryGetStringField(TEXT("markerPath"), Evidence.MarkerPath);
					if (Evidence.Action.IsEmpty())
					{
						Evidence.Action = TEXT("Unnamed action");
					}
					if (Evidence.ActionKind.IsEmpty())
					{
						Evidence.ActionKind = TEXT("other");
					}
					Evidence.ActionFlags = static_cast<uint32>(Summary->GetNumberField(TEXT("actionFlags")));
					Evidence.PassedFragments = static_cast<int32>(Summary->GetNumberField(TEXT("passedFragments")));
					Evidence.RejectedFragments = static_cast<int32>(Summary->GetNumberField(TEXT("rejectedFragments")));
					Summary->TryGetBoolField(TEXT("directShaderWrite"), Evidence.bDirectShaderWrite);
					Summary->TryGetBoolField(TEXT("unboundPixelShader"), Evidence.bUnboundPixelShader);
					Summary->TryGetBoolField(TEXT("changedTextureValue"), Evidence.bChangedTextureValue);
					Summary->TryGetBoolField(TEXT("hasPrimitiveEvidence"), Evidence.bHasPrimitiveEvidence);
					Evidence.PrimitiveId = static_cast<uint32>(Summary->GetNumberField(TEXT("lastPrimitiveId")));
					for (const TSharedPtr<FJsonValue>& Failure : Summary->GetArrayField(TEXT("failureReasons")))
					{
						Evidence.FailureReasons.AddUnique(Failure->AsString());
					}
					Evidence.Before = FormatFloatValue(Summary->GetObjectField(TEXT("firstBefore")));
					Evidence.ShaderOutput = FormatFloatValue(Summary->GetObjectField(TEXT("lastShaderOutput")));
					Evidence.After = FormatFloatValue(Summary->GetObjectField(TEXT("lastAfter")));
					Sample->EventSummaries.Add(MoveTemp(Evidence));
				}
			}
			for (const TSharedPtr<FJsonValue>& JsonValue : Message->GetArrayField(TEXT("modifications")))
			{
				const TSharedPtr<FJsonObject> Modification = JsonValue->AsObject();
				if (!Modification.IsValid())
				{
					continue;
				}
				FPixelModificationEvidence Evidence;
				Evidence.EventId = static_cast<uint32>(Modification->GetNumberField(TEXT("eventId")));
				Modification->TryGetStringField(TEXT("action"), Evidence.Action);
				Modification->TryGetStringField(TEXT("actionKind"), Evidence.ActionKind);
				Modification->TryGetStringField(TEXT("markerPath"), Evidence.MarkerPath);
				Evidence.ActionFlags = static_cast<uint32>(Modification->GetNumberField(TEXT("actionFlags")));
				Modification->TryGetBoolField(TEXT("passed"), Evidence.bPassed);
				Modification->TryGetBoolField(TEXT("directShaderWrite"), Evidence.bDirectShaderWrite);
				Modification->TryGetBoolField(TEXT("unboundPixelShader"), Evidence.bUnboundPixelShader);
				Modification->TryGetBoolField(TEXT("changedTextureValue"), Evidence.bChangedTextureValue);
				Evidence.PrimitiveId = static_cast<uint32>(Modification->GetNumberField(TEXT("primitiveId")));
				Evidence.FragmentIndex = static_cast<uint32>(Modification->GetNumberField(TEXT("fragmentIndex")));
				for (const TSharedPtr<FJsonValue>& Failure : Modification->GetArrayField(TEXT("failureReasons")))
				{
					Evidence.FailureReasons.Add(Failure->AsString());
				}
				Evidence.Before = FormatFloatValue(Modification->GetObjectField(TEXT("before")));
				Evidence.ShaderOutput = FormatFloatValue(Modification->GetObjectField(TEXT("shaderOutput")));
				Evidence.After = FormatFloatValue(Modification->GetObjectField(TEXT("after")));
				if (Evidence.Action.IsEmpty())
				{
					Evidence.Action = TEXT("Unnamed action");
				}
				if (Evidence.ActionKind.IsEmpty())
				{
					Evidence.ActionKind = TEXT("other");
				}
				Sample->Modifications.Add(MoveTemp(Evidence));
			}

			UpdateSelectionText();
			RenderCausalReport();
			if (!bAgentRunning)
			{
				const bool bAnyPending = Samples.ContainsByPredicate([](const FPixelSample& Item)
				{
					return Item.bPending;
				});
				SetAgentStatus(bAnyPending
					? TEXT("部分 Pixel History 已返回 · 仍有查询进行中")
					: TEXT("确定性溯源完成 · 可运行 Agent 语义提炼"));
			}
			const uint64 CompletedSampleId = Sample->Id;
			const int32 SampleIndex = Samples.IndexOfByPredicate([CompletedSampleId](const FPixelSample& Item) { return Item.Id == CompletedSampleId; });
			SetStatus(FString::Printf(TEXT("关注点 P%d (%d, %d)：%d 个 RenderDoc modification，分析已刷新。"),
				SampleIndex + 1, Sample->Pixel.X, Sample->Pixel.Y, Sample->TotalModifications));
		}

		static TArray<FEventEvidence> AggregateEvents(const FPixelSample& Sample)
		{
			TArray<FEventEvidence> Events;
			if (Sample.bEventSummaryComplete)
			{
				Events.Reserve(Sample.EventSummaries.Num());
				for (const FEventSummaryEvidence& Summary : Sample.EventSummaries)
				{
					FEventEvidence Event;
					Event.EventId = Summary.EventId;
					Event.Action = Summary.Action;
					Event.ActionKind = Summary.ActionKind;
					Event.MarkerPath = Summary.MarkerPath;
					Event.ActionFlags = Summary.ActionFlags;
					Event.PassedFragments = Summary.PassedFragments;
					Event.RejectedFragments = Summary.RejectedFragments;
					Event.bDirectShaderWrite = Summary.bDirectShaderWrite;
					Event.bUnboundPixelShader = Summary.bUnboundPixelShader;
					Event.bChangedTextureValue = Summary.bChangedTextureValue;
					Event.bHasPrimitiveEvidence = Summary.bHasPrimitiveEvidence;
					Event.PrimitiveId = Summary.PrimitiveId;
					Event.FailureReasons = Summary.FailureReasons;
					Event.Before = Summary.Before;
					Event.ShaderOutput = Summary.ShaderOutput;
					Event.After = Summary.After;
					Events.Add(MoveTemp(Event));
				}
				return Events;
			}

			TMap<uint32, int32> EventIndex;
			for (int32 ModificationIndex = 0; ModificationIndex < Sample.Modifications.Num(); ++ModificationIndex)
			{
				const FPixelModificationEvidence& Modification = Sample.Modifications[ModificationIndex];
				int32* ExistingIndex = EventIndex.Find(Modification.EventId);
				if (!ExistingIndex)
				{
					FEventEvidence Event;
					Event.EventId = Modification.EventId;
					Event.Action = Modification.Action;
					Event.ActionKind = Modification.ActionKind;
					Event.MarkerPath = Modification.MarkerPath;
					Event.ActionFlags = Modification.ActionFlags;
					Event.Before = Modification.Before;
					EventIndex.Add(Event.EventId, Events.Add(MoveTemp(Event)));
					ExistingIndex = EventIndex.Find(Modification.EventId);
				}
				FEventEvidence& Event = Events[*ExistingIndex];
				Event.PassedFragments += Modification.bPassed ? 1 : 0;
				Event.RejectedFragments += Modification.bPassed ? 0 : 1;
				Event.LastModificationIndex = ModificationIndex;
				Event.bDirectShaderWrite |= Modification.bDirectShaderWrite;
				Event.bUnboundPixelShader |= Modification.bUnboundPixelShader;
				Event.bChangedTextureValue |= Modification.bChangedTextureValue;
				Event.bHasPrimitiveEvidence = true;
				Event.PrimitiveId = Modification.PrimitiveId;
				Event.ShaderOutput = Modification.ShaderOutput;
				Event.After = Modification.After;
				for (const FString& Failure : Modification.FailureReasons)
				{
					Event.FailureReasons.AddUnique(Failure);
				}
			}
			return Events;
		}

		static const FEventEvidence* FindEvent(const TArray<FEventEvidence>& Events, uint32 EventId)
		{
			return Events.FindByPredicate([EventId](const FEventEvidence& Event) { return Event.EventId == EventId; });
		}

		static FString DescribeEventResult(const FEventEvidence& Event)
		{
			if (Event.bDirectShaderWrite)
			{
				return Event.bChangedTextureValue ? TEXT("potential UAV/shader write changed value") : TEXT("potential UAV/shader write, no value change");
			}
			if (Event.PassedFragments > 0)
			{
				return FString::Printf(TEXT("%d fragment%s wrote; %d rejected"), Event.PassedFragments,
					Event.PassedFragments == 1 ? TEXT("") : TEXT("s"), Event.RejectedFragments);
			}
			return Event.FailureReasons.IsEmpty()
				? TEXT("did not write")
				: FString::Printf(TEXT("rejected: %s"), *FString::Join(Event.FailureReasons, TEXT(", ")));
		}

		static bool IsConfirmedPixelWriter(const FEventEvidence& Event)
		{
			return Event.PassedFragments > 0 || (Event.bDirectShaderWrite && Event.bChangedTextureValue);
		}

		static FString ClassifySemantics(const FEventEvidence& Event)
		{
			if (Event.bDirectShaderWrite || Event.ActionKind == TEXT("dispatch"))
			{
				return TEXT("compute/UAV");
			}
			if (Event.ActionKind == TEXT("copy") || Event.ActionKind == TEXT("resolve"))
			{
				return TEXT("copy/resolve");
			}
			if (Event.ActionKind == TEXT("clear"))
			{
				return TEXT("clear/load");
			}
			FString SemanticContext = Event.Action;
			TArray<FString> PathComponents;
			Event.MarkerPath.ParseIntoArray(PathComponents, TEXT(" > "), true);
			const int32 FirstRelevantPath = FMath::Max(0, PathComponents.Num() - 3);
			for (int32 Index = FirstRelevantPath; Index < PathComponents.Num(); ++Index)
			{
				SemanticContext += TEXT(" ") + PathComponents[Index];
			}
			const FString Context = SemanticContext.ToLower();
			if (Context.Contains(TEXT("temporalsuperresolution")) || Context.Contains(TEXT("upscale"))
				|| Context.Contains(TEXT("downsample")) || Context.Contains(TEXT("upsample"))
				|| Context.Contains(TEXT("resample")))
			{
				return TEXT("resample/nonlinear");
			}
			if (Context.Contains(TEXT("postprocessing")) || Context.Contains(TEXT("tonemap"))
				|| Context.Contains(TEXT("bloom")) || Context.Contains(TEXT("composite"))
				|| Context.Contains(TEXT("screenpass")))
			{
				return TEXT("post-process");
			}
			return Event.ActionKind == TEXT("draw") ? TEXT("scene-write") : TEXT("unclassified");
		}

		static FString CompactMarkerPath(const FString& MarkerPath)
		{
			TArray<FString> Components;
			MarkerPath.ParseIntoArray(Components, TEXT(" > "), true);
			if (Components.Num() <= 4)
			{
				return MarkerPath;
			}
			return FString::Printf(TEXT("%s > ... > %s > %s > %s"), *Components[0],
				*Components[Components.Num() - 3], *Components[Components.Num() - 2], *Components[Components.Num() - 1]);
		}

		static FString FormatPipelineStateEvidence(const TSharedPtr<FJsonObject>& PipelineState)
		{
			if (!PipelineState.IsValid())
			{
				return TEXT("- 固定管线状态：Worker 尚未返回。\n");
			}

			FString Api;
			PipelineState->TryGetStringField(TEXT("api"), Api);
			bool bCaptured = false;
			PipelineState->TryGetBoolField(TEXT("fixedFunctionCaptured"), bCaptured);
			FString Report = FString::Printf(TEXT("- API：%s\n- 固定管线快照：%s\n"),
				Api.IsEmpty() ? TEXT("unknown") : *Api,
				bCaptured ? TEXT("已采集") : TEXT("未采集"));
			if (!bCaptured)
			{
				FString Note;
				PipelineState->TryGetStringField(TEXT("captureNote"), Note);
				if (!Note.IsEmpty())
				{
					Report += FString::Printf(TEXT("- 说明：%s\n"), *Note);
				}
				return Report;
			}

			FString Topology;
			PipelineState->TryGetStringField(TEXT("primitiveTopology"), Topology);
			double InputLayoutCount = 0.0;
			double VertexBufferCount = 0.0;
			PipelineState->TryGetNumberField(TEXT("inputLayoutCount"), InputLayoutCount);
			PipelineState->TryGetNumberField(TEXT("vertexBufferCount"), VertexBufferCount);
			bool bIndexBuffer = false;
			PipelineState->TryGetBoolField(TEXT("indexBufferBound"), bIndexBuffer);
			Report += FString::Printf(TEXT("- 输入装配：Topology=%s；InputLayout=%d；VertexBuffer=%d；IndexBuffer=%s\n"),
				Topology.IsEmpty() ? TEXT("unknown") : *Topology,
				static_cast<int32>(InputLayoutCount), static_cast<int32>(VertexBufferCount),
				bIndexBuffer ? TEXT("bound") : TEXT("none"));

			const TSharedPtr<FJsonObject>* Rasterizer = nullptr;
			if (PipelineState->TryGetObjectField(TEXT("rasterizer"), Rasterizer) && Rasterizer && Rasterizer->IsValid())
			{
				FString FillMode;
				FString CullMode;
				(*Rasterizer)->TryGetStringField(TEXT("fillMode"), FillMode);
				(*Rasterizer)->TryGetStringField(TEXT("cullMode"), CullMode);
				bool bFrontCCW = false;
				bool bDepthClip = false;
				(*Rasterizer)->TryGetBoolField(TEXT("frontCounterClockwise"), bFrontCCW);
				(*Rasterizer)->TryGetBoolField(TEXT("depthClip"), bDepthClip);
				double ViewportCount = 0.0;
				double ScissorCount = 0.0;
				(*Rasterizer)->TryGetNumberField(TEXT("viewportCount"), ViewportCount);
				(*Rasterizer)->TryGetNumberField(TEXT("scissorCount"), ScissorCount);
				Report += FString::Printf(TEXT("- 光栅化：Fill=%s；Cull=%s；FrontCCW=%s；DepthClip=%s；Viewport=%d；Scissor=%d\n"),
					FillMode.IsEmpty() ? TEXT("unknown") : *FillMode,
					CullMode.IsEmpty() ? TEXT("unknown") : *CullMode,
					bFrontCCW ? TEXT("true") : TEXT("false"), bDepthClip ? TEXT("true") : TEXT("false"),
					static_cast<int32>(ViewportCount), static_cast<int32>(ScissorCount));
				const TArray<TSharedPtr<FJsonValue>>* Viewports = nullptr;
				if ((*Rasterizer)->TryGetArrayField(TEXT("viewports"), Viewports) && Viewports && Viewports->Num() > 0)
				{
					const TSharedPtr<FJsonObject> Viewport = (*Viewports)[0].IsValid() ? (*Viewports)[0]->AsObject() : nullptr;
					if (Viewport.IsValid())
					{
						double X = 0.0;
						double Y = 0.0;
						double Width = 0.0;
						double Height = 0.0;
						Viewport->TryGetNumberField(TEXT("x"), X);
						Viewport->TryGetNumberField(TEXT("y"), Y);
						Viewport->TryGetNumberField(TEXT("width"), Width);
						Viewport->TryGetNumberField(TEXT("height"), Height);
						Report += FString::Printf(TEXT("  首个 Viewport：x=%.2f y=%.2f w=%.2f h=%.2f\n"), X, Y, Width, Height);
					}
				}
			}

			const TSharedPtr<FJsonObject>* DepthStencil = nullptr;
			if (PipelineState->TryGetObjectField(TEXT("depthStencil"), DepthStencil) && DepthStencil && DepthStencil->IsValid())
			{
				FString DepthFunction;
				(*DepthStencil)->TryGetStringField(TEXT("depthFunction"), DepthFunction);
				bool bDepthEnable = false;
				bool bDepthWrites = false;
				bool bStencilEnable = false;
				bool bDepthReadOnly = false;
				bool bStencilReadOnly = false;
				(*DepthStencil)->TryGetBoolField(TEXT("depthEnable"), bDepthEnable);
				(*DepthStencil)->TryGetBoolField(TEXT("depthWrites"), bDepthWrites);
				(*DepthStencil)->TryGetBoolField(TEXT("stencilEnable"), bStencilEnable);
				(*DepthStencil)->TryGetBoolField(TEXT("depthReadOnly"), bDepthReadOnly);
				(*DepthStencil)->TryGetBoolField(TEXT("stencilReadOnly"), bStencilReadOnly);
				Report += FString::Printf(TEXT("- 深度/模板：Depth=%s；Write=%s；Func=%s；Stencil=%s；DepthRO=%s；StencilRO=%s\n"),
					bDepthEnable ? TEXT("on") : TEXT("off"), bDepthWrites ? TEXT("on") : TEXT("off"),
					DepthFunction.IsEmpty() ? TEXT("unknown") : *DepthFunction,
					bStencilEnable ? TEXT("on") : TEXT("off"), bDepthReadOnly ? TEXT("true") : TEXT("false"),
					bStencilReadOnly ? TEXT("true") : TEXT("false"));
			}

			const TSharedPtr<FJsonObject>* Blend = nullptr;
			if (PipelineState->TryGetObjectField(TEXT("blend"), Blend) && Blend && Blend->IsValid())
			{
				bool bAlphaToCoverage = false;
				bool bIndependentBlend = false;
				double TargetCount = 0.0;
				(*Blend)->TryGetBoolField(TEXT("alphaToCoverage"), bAlphaToCoverage);
				(*Blend)->TryGetBoolField(TEXT("independentBlend"), bIndependentBlend);
				(*Blend)->TryGetNumberField(TEXT("targetCount"), TargetCount);
				Report += FString::Printf(TEXT("- 混合：AlphaToCoverage=%s；Independent=%s；RenderTarget blend slots=%d\n"),
					bAlphaToCoverage ? TEXT("on") : TEXT("off"), bIndependentBlend ? TEXT("on") : TEXT("off"),
					static_cast<int32>(TargetCount));
				const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
				if ((*Blend)->TryGetArrayField(TEXT("targets"), Targets) && Targets && Targets->Num() > 0)
				{
					const TSharedPtr<FJsonObject> Target = (*Targets)[0].IsValid() ? (*Targets)[0]->AsObject() : nullptr;
					if (Target.IsValid())
					{
						bool bEnabled = false;
						FString ColorSource;
						FString ColorDestination;
						FString ColorOperation;
						Target->TryGetBoolField(TEXT("enabled"), bEnabled);
						Target->TryGetStringField(TEXT("colorSource"), ColorSource);
						Target->TryGetStringField(TEXT("colorDestination"), ColorDestination);
						Target->TryGetStringField(TEXT("colorOperation"), ColorOperation);
						Report += FString::Printf(TEXT("  RT0：enabled=%s；color factors=(%s, %s)；op=%s\n"),
							bEnabled ? TEXT("true") : TEXT("false"),
							ColorSource.IsEmpty() ? TEXT("unknown") : *ColorSource,
							ColorDestination.IsEmpty() ? TEXT("unknown") : *ColorDestination,
							ColorOperation.IsEmpty() ? TEXT("unknown") : *ColorOperation);
					}
				}
			}

			double RenderTargetCount = 0.0;
			PipelineState->TryGetNumberField(TEXT("renderTargetCount"), RenderTargetCount);
			bool bPredication = false;
			PipelineState->TryGetBoolField(TEXT("predicationEnabled"), bPredication);
			Report += FString::Printf(TEXT("- 输出合并器：RenderTargets=%d；Predication=%s\n"),
				static_cast<int32>(RenderTargetCount), bPredication ? TEXT("on") : TEXT("off"));
			return Report;
		}

		static FString FormatShaderVariableJson(const TSharedPtr<FJsonObject>& Variable)
		{
			if (!Variable.IsValid())
			{
				return TEXT("<unknown>");
			}
			FString Name;
			FString Type;
			Variable->TryGetStringField(TEXT("name"), Name);
			Variable->TryGetStringField(TEXT("type"), Type);
			TArray<FString> Values;
			const TArray<TSharedPtr<FJsonValue>>* ValueArray = nullptr;
			if (Variable->TryGetArrayField(TEXT("values"), ValueArray) && ValueArray)
			{
				for (const TSharedPtr<FJsonValue>& Value : *ValueArray)
				{
					if (Value.IsValid())
					{
						Values.Add(FString::Printf(TEXT("%.6g"), Value->AsNumber()));
					}
				}
			}
			const FString ValueText = Values.IsEmpty() ? TEXT("<struct/resource>") : FString::Printf(TEXT("(%s)"), *FString::Join(Values, TEXT(", ")));
			return FString::Printf(TEXT("%s[%s]=%s"), Name.IsEmpty() ? TEXT("unnamed") : *Name,
				Type.IsEmpty() ? TEXT("unknown") : *Type, *ValueText);
		}

		static FString FormatShaderDebugTraceEvidence(const TSharedPtr<FJsonObject>& Trace)
		{
			if (!Trace.IsValid())
			{
				return TEXT("- Pixel Shader 指令追踪：未执行。\n");
			}
			double StepCount = 0.0;
			double InstructionInfoCount = 0.0;
			double SourceVariableCount = 0.0;
			bool bCompleted = false;
			double TotalVariableChanges = 0.0;
			double MaxCallstackDepth = 0.0;
			Trace->TryGetNumberField(TEXT("stepCount"), StepCount);
			Trace->TryGetNumberField(TEXT("instructionInfoCount"), InstructionInfoCount);
			Trace->TryGetNumberField(TEXT("sourceVariableMappingCount"), SourceVariableCount);
			Trace->TryGetNumberField(TEXT("totalVariableChanges"), TotalVariableChanges);
			Trace->TryGetNumberField(TEXT("maxCallstackDepth"), MaxCallstackDepth);
			Trace->TryGetBoolField(TEXT("completed"), bCompleted);
			FString Stage;
			Trace->TryGetStringField(TEXT("stage"), Stage);
			FString Report = FString::Printf(TEXT("- Pixel Shader 指令追踪：stage=%s；steps=%d；instructionInfo=%d；sourceMappings=%d；variableChanges=%d；maxCallstack=%d；completed=%s\n"),
				Stage.IsEmpty() ? TEXT("unknown") : *Stage,
				static_cast<int32>(StepCount), static_cast<int32>(InstructionInfoCount),
				static_cast<int32>(SourceVariableCount), static_cast<int32>(TotalVariableChanges),
				static_cast<int32>(MaxCallstackDepth), bCompleted ? TEXT("true") : TEXT("false"));
			const TArray<TSharedPtr<FJsonValue>>* InputVariables = nullptr;
			if (Trace->TryGetArrayField(TEXT("inputVariables"), InputVariables) && InputVariables && !InputVariables->IsEmpty())
			{
				TArray<FString> Names;
				for (const TSharedPtr<FJsonValue>& Value : *InputVariables)
				{
					if (Value.IsValid())
					{
						Names.Add(Value->AsString());
					}
				}
				if (!Names.IsEmpty())
				{
					Report += FString::Printf(TEXT("  输入变量：%s\n"), *FString::Join(Names, TEXT(", ")));
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* InputVariableValues = nullptr;
			if (Trace->TryGetArrayField(TEXT("inputVariableValues"), InputVariableValues) && InputVariableValues)
			{
				TArray<FString> Values;
				for (const TSharedPtr<FJsonValue>& Value : *InputVariableValues)
				{
					const TSharedPtr<FJsonObject> Variable = Value.IsValid() ? Value->AsObject() : nullptr;
					if (Variable.IsValid())
					{
						Values.Add(FormatShaderVariableJson(Variable));
					}
					if (Values.Num() >= 8)
					{
						break;
					}
				}
				if (!Values.IsEmpty())
				{
					Report += FString::Printf(TEXT("  输入值：%s\n"), *FString::Join(Values, TEXT("；")));
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* SourceVariableNames = nullptr;
			if (Trace->TryGetArrayField(TEXT("sourceVariableNames"), SourceVariableNames) && SourceVariableNames && !SourceVariableNames->IsEmpty())
			{
				TArray<FString> Names;
				for (const TSharedPtr<FJsonValue>& Value : *SourceVariableNames)
				{
					if (Value.IsValid())
					{
						Names.Add(Value->AsString());
					}
				}
				if (!Names.IsEmpty())
				{
					Report += FString::Printf(TEXT("  源码变量映射：%s\n"), *FString::Join(Names, TEXT(", ")));
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* InstructionSources = nullptr;
			if (Trace->TryGetArrayField(TEXT("instructionSourceSamples"), InstructionSources) && InstructionSources)
			{
				int32 ShownSources = 0;
				for (const TSharedPtr<FJsonValue>& Value : *InstructionSources)
				{
					const TSharedPtr<FJsonObject> Source = Value.IsValid() ? Value->AsObject() : nullptr;
					if (!Source.IsValid())
					{
						continue;
					}
					double Instruction = 0.0;
					double Line = 0.0;
					Source->TryGetNumberField(TEXT("instruction"), Instruction);
					Source->TryGetNumberField(TEXT("lineStart"), Line);
					Report += FString::Printf(TEXT("  指令源码位置：instruction=%d；line=%d\n"),
						static_cast<int32>(Instruction), static_cast<int32>(Line));
					if (++ShownSources >= 4)
					{
						break;
					}
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* TraceStates = nullptr;
			if (Trace->TryGetArrayField(TEXT("traceStateSamples"), TraceStates) && TraceStates)
			{
				for (const TSharedPtr<FJsonValue>& Value : *TraceStates)
				{
					const TSharedPtr<FJsonObject> State = Value.IsValid() ? Value->AsObject() : nullptr;
					if (!State.IsValid())
					{
						continue;
					}
					double StepIndex = 0.0;
					double NextInstruction = 0.0;
					double ChangeCount = 0.0;
					FString Flags;
					State->TryGetNumberField(TEXT("stepIndex"), StepIndex);
					State->TryGetNumberField(TEXT("nextInstruction"), NextInstruction);
					State->TryGetNumberField(TEXT("changeCount"), ChangeCount);
					State->TryGetStringField(TEXT("flags"), Flags);
					Report += FString::Printf(TEXT("  状态样本：step=%d；nextInstruction=%d；changes=%d；flags=%s\n"),
						static_cast<int32>(StepIndex), static_cast<int32>(NextInstruction),
						static_cast<int32>(ChangeCount), Flags.IsEmpty() ? TEXT("none") : *Flags);
					const TArray<TSharedPtr<FJsonValue>>* ChangedVariables = nullptr;
					if (State->TryGetArrayField(TEXT("changedVariables"), ChangedVariables) && ChangedVariables)
					{
						TArray<FString> Changes;
						for (const TSharedPtr<FJsonValue>& ChangedValue : *ChangedVariables)
						{
							const TSharedPtr<FJsonObject> Changed = ChangedValue.IsValid() ? ChangedValue->AsObject() : nullptr;
							if (!Changed.IsValid())
							{
								continue;
							}
							const TSharedPtr<FJsonObject>* Before = nullptr;
							const TSharedPtr<FJsonObject>* After = nullptr;
							Changed->TryGetObjectField(TEXT("before"), Before);
							Changed->TryGetObjectField(TEXT("after"), After);
							const TSharedPtr<FJsonObject> BeforeObject = Before ? *Before : TSharedPtr<FJsonObject>();
							const TSharedPtr<FJsonObject> AfterObject = After ? *After : TSharedPtr<FJsonObject>();
							Changes.Add(FormatShaderVariableJson(BeforeObject) + TEXT(" -> ") + FormatShaderVariableJson(AfterObject));
							if (Changes.Num() >= 4)
							{
								break;
							}
						}
						if (!Changes.IsEmpty())
						{
							Report += FString::Printf(TEXT("    变量变化：%s\n"), *FString::Join(Changes, TEXT("；")));
						}
					}
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* StepFlags = nullptr;
			if (Trace->TryGetArrayField(TEXT("stepFlags"), StepFlags) && StepFlags)
			{
				TArray<FString> Flags;
				for (const TSharedPtr<FJsonValue>& Flag : *StepFlags)
				{
					if (Flag.IsValid())
					{
						Flags.AddUnique(Flag->AsString());
					}
				}
				if (!Flags.IsEmpty())
				{
					Report += FString::Printf(TEXT("  执行标志：%s\n"), *FString::Join(Flags, TEXT(", ")));
				}
			}
			Report += TEXT("  说明：该结果证明了本次像素调试执行经过的指令/调试状态，不等同于自动恢复出高层材质算法。\n");
			return Report;
		}

		static void AddHypothesis(TArray<FString>& Hypotheses, const FString& Hypothesis)
		{
			if (!Hypothesis.IsEmpty() && Hypotheses.Num() < 3)
			{
				Hypotheses.AddUnique(Hypothesis);
			}
		}

		static void AddFailureHypothesis(const FString& Failure, TArray<FString>& Hypotheses)
		{
			if (Failure == TEXT("depth-test"))
				AddHypothesis(Hypotheses, TEXT("深度测试拒绝：检查遮挡顺序、深度输出和当前深度比较方式。"));
			else if (Failure == TEXT("stencil-test"))
				AddHypothesis(Hypotheses, TEXT("模板测试拦截：检查模板参考值、掩码，以及更早写入模板的事件。"));
			else if (Failure == TEXT("shader-discard"))
				AddHypothesis(Hypotheses, TEXT("Shader 主动丢弃：检查透明度遮罩、clip 逻辑及其输入值。"));
			else if (Failure == TEXT("backface-cull"))
				AddHypothesis(Hypotheses, TEXT("背面剔除：检查顶点绕序、镜像变换和材质双面设置。"));
			else if (Failure == TEXT("scissor-clip") || Failure == TEXT("viewport-clip") || Failure == TEXT("depth-clip") || Failure == TEXT("depth-bounds"))
				AddHypothesis(Hypotheses, TEXT("光栅范围裁剪：检查 Viewport、Scissor、投影矩阵和深度范围。"));
			else if (Failure == TEXT("sample-mask"))
				AddHypothesis(Hypotheses, TEXT("采样覆盖不足：检查 MSAA Sample Mask 和 Alpha-to-Coverage。"));
			else if (Failure == TEXT("predication-skipped"))
				AddHypothesis(Hypotheses, TEXT("条件渲染跳过：检查 GPU Predicate 及生成该条件的上游事件。"));
		}

		void RenderCausalReport()
		{
			LastCandidate.Reset();
			bLastCandidateHasDivergence = false;
			if (Samples.IsEmpty())
			{
				SetReportCards(
					TEXT("等待选择关注像素。这里不要求你提供一个“正确”的对照点。"),
					TEXT("最终画面\n↓\n等待选择 P1"),
					TEXT("尚无候选原因。"),
					TEXT("尚未执行 Pixel History 查询。"));
				return;
			}

			if (!bSelectionConfirmed)
			{
				SetReportCards(
					TEXT("选点已就绪，等待确认后开始分析。"),
					TEXT("最终画面\n↓\n已选 P 点\n↓\n等待确认"),
					TEXT("当前不会读取 Pixel History，也不会进行因果判断。"),
					TEXT("选点阶段不会触发后台查询；点击“分析”后才开始读取证据。"));
				return;
			}

			FString Report = TEXT("BOUNDED SINGLE-PIXEL CAUSAL REPORT\n\nPixel\n");
			bool bAnyPending = false;
			bool bAnyIncompleteEventSummary = false;
			TArray<const FPixelSample*> ReadySamples;
			for (int32 Index = 0; Index < Samples.Num(); ++Index)
			{
				const FPixelSample& Sample = Samples[Index];
				bAnyPending |= Sample.bPending;
				FString State;
				if (Sample.bPending)
				{
					State = TEXT("querying");
				}
				else if (Sample.bFailed)
				{
					State = FString::Printf(TEXT("failed - %s"), *Sample.Error);
				}
				else
				{
					ReadySamples.Add(&Sample);
					bAnyIncompleteEventSummary |= !Sample.bEventSummaryComplete;
					State = FString::Printf(TEXT("%d modifications%s"), Sample.TotalModifications,
						Sample.bTruncated
							? (Sample.bEventSummaryComplete ? TEXT(", detail tail truncated; event summary complete") : TEXT(", event summary incomplete"))
							: TEXT(""));
				}
				Report += FString::Printf(TEXT("- P%d (%d, %d): %s\n"),
					Index + 1, Sample.Pixel.X, Sample.Pixel.Y, *State);
			}

			if (bAnyPending)
			{
				Report += TEXT("\nWaiting for bounded RenderDoc queries. No causal claim is made yet.");
				SetReportCards(
					TEXT("正在查询所选像素的 Pixel History；结果返回前不做因果判断。"),
					TEXT("最终画面\n↓\n正在收集 P 点的事件证据…"),
					TEXT("等待查询完成。"),
					Report);
				return;
			}

			if (ReadySamples.IsEmpty())
			{
				Report += TEXT("\nThe selected pixel query failed. The chain is broken before pixel evidence.");
				SetReportCards(
					TEXT("当前像素查询失败，目前没有 Pixel History 可以支持结论。"),
					TEXT("最终画面\n↓\nPixel History 不可用\n↓\n■ 因果链中断"),
					TEXT("可能是目标纹理、API 或当前截帧不支持 Pixel History；尚未证明任何渲染原因。"),
					Report);
				return;
			}

			if (bAnyIncompleteEventSummary)
			{
				Report += TEXT("\nEvent summary is incomplete for at least one point; the deterministic chain is stopped instead of guessing from a truncated tail.\n");
				SetReportCards(
					TEXT("Pixel History 返回了不完整的事件摘要，当前不会继续生成溯源结论。"),
					TEXT("最终画面\n↓\n事件摘要不完整\n↓\n■ 溯源暂停"),
					TEXT("需要重新编译并使用支持完整 eventSummaries 的 Replay Worker；这不是 Agent 可以补齐的缺口。"),
					Report);
				return;
			}

			TMap<uint64, TArray<FEventEvidence>> AggregatedBySample;
			for (const FPixelSample* Sample : ReadySamples)
			{
				AggregatedBySample.Add(Sample->Id, AggregateEvents(*Sample));
			}

			const FPixelSample* BaseSample = ReadySamples[0];
			const TArray<FEventEvidence>* BaseEvents = AggregatedBySample.Find(BaseSample->Id);

			FCausalCandidate Candidate;
			bool bHasCandidate = false;
			if (BaseEvents)
			{
				for (int32 EventIndex = BaseEvents->Num() - 1; EventIndex >= 0; --EventIndex)
				{
					const FEventEvidence& Event = (*BaseEvents)[EventIndex];
					if (Event.ActionKind == TEXT("present")
						|| (Event.PassedFragments <= 0 && !(Event.bDirectShaderWrite && Event.bChangedTextureValue)))
					{
						continue;
					}
					Candidate.Event = Event;
					Candidate.SampleCoverage = 1;
					bHasCandidate = true;
					break;
				}
			}

			if (bHasCandidate)
			{
				LastCandidate = Candidate;
				bLastCandidateHasDivergence = false;
			}
			EnsureRelevantEventContexts();
			EnsureCandidateShaderDebug();

			FString CausalPath = TEXT("最终画面");
			TArray<FString> Hypotheses;
			for (const FPixelSample* Sample : ReadySamples)
			{
				const int32 DisplayIndex = Samples.IndexOfByPredicate([Sample](const FPixelSample& Item)
				{
					return Item.Id == Sample->Id;
				});
				const TArray<FEventEvidence>* Events = AggregatedBySample.Find(Sample->Id);
				const FEventEvidence* LatestWriter = nullptr;
				if (Events)
				{
					for (int32 EventIndex = Events->Num() - 1; EventIndex >= 0; --EventIndex)
					{
						const FEventEvidence& Event = (*Events)[EventIndex];
						if (Event.PassedFragments > 0 || (Event.bDirectShaderWrite && Event.bChangedTextureValue))
						{
							LatestWriter = &Event;
							break;
						}
					}
				}
				if (LatestWriter)
				{
					Report += FString::Printf(TEXT("- P%d latest observed writer: EID %u [%s] %s.\n"),
						DisplayIndex + 1, LatestWriter->EventId, *LatestWriter->ActionKind, *LatestWriter->Action);
					CausalPath += FString::Printf(TEXT("\n↓\nP%d (%d,%d) ← EID %u · %s"),
						DisplayIndex + 1, Sample->Pixel.X, Sample->Pixel.Y, LatestWriter->EventId, *LatestWriter->Action);
				}
				else
				{
					Report += FString::Printf(TEXT("- P%d has no confirmed write in the selected target history.\n"), DisplayIndex + 1);
					CausalPath += FString::Printf(TEXT("\n↓\nP%d (%d,%d) ← 未确认写入"), DisplayIndex + 1, Sample->Pixel.X, Sample->Pixel.Y);
					AddHypothesis(Hypotheses, FString::Printf(
						TEXT("P%d 没有确认写入：可能只保留了 Clear/背景、没有光栅覆盖，或目标不支持完整 Pixel History；这不等于已经证明对象被剔除。"),
						DisplayIndex + 1));
				}
			}

			FString Summary;
			if (bHasCandidate)
			{
				const FString Semantics = ClassifySemantics(Candidate.Event);
				Summary = FString::Printf(
					TEXT("P1 的末端可追踪候选是 EID %u。它解释实际形成过程，但单点证据不能判断视觉结果是否符合设计意图。"),
					Candidate.Event.EventId);

				CausalPath += FString::Printf(TEXT("\n↓\n末端候选 EID %u · %s\n↓\n■ GPU 因果边界：%s"),
					Candidate.Event.EventId, *Candidate.Event.Action,
					Semantics == TEXT("resample/nonlinear")
						? TEXT("发生缩放/重采样，不能假定同坐标继续上溯")
						: TEXT("只展开该事件的已用输入与 Shader 状态"));
				Report += FString::Printf(
					TEXT("\nCandidate interval\n- EID %u [%s / %s] %s\n- result: %s\n"),
					Candidate.Event.EventId, *Candidate.Event.ActionKind, *Semantics, *Candidate.Event.Action,
					*DescribeEventResult(Candidate.Event));

				for (const FString& Failure : Candidate.Event.FailureReasons)
				{
					AddFailureHypothesis(Failure, Hypotheses);
				}
				if (Candidate.Event.bUnboundPixelShader)
				{
					AddHypothesis(Hypotheses, TEXT("该事件没有可确认的像素着色器输出；检查 PS/RT 绑定，但不要据此推断 Mesh 身份。"));
				}
				if (Candidate.Event.bDirectShaderWrite)
				{
					AddHypothesis(Hypotheses, Candidate.Event.bChangedTextureValue
						? TEXT("UAV/Storage 写入改变了像素；当前证据指向 Dispatch/直接写入路径。")
						: TEXT("UAV/Storage 事件没有改变数值，可能只是命中历史中的干扰项。"));
				}
				else if (Candidate.Event.PassedFragments > 0 && Candidate.Event.ActionKind == TEXT("draw"))
				{
					AddHypothesis(Hypotheses, Semantics == TEXT("scene-write")
						? TEXT("场景 Draw 确认写入：可检查已证明的 Shader 输出、资源绑定以及可用的 UE Marker/Mesh 映射。")
						: TEXT("后处理 Draw 写入最终目标：优先看绑定输入，不能仅凭全屏 Draw 名称判断根因。"));
				}

				Report += TEXT("\nEvent Context（Shader、资源与 Pipeline）\n");
				if (const FEventContextEvidence* Context = EventContexts.Find(Candidate.Event.EventId))
				{
					Report += FString::Printf(TEXT("- Shader %s%s | debuggable: %s | source symbols: %s\n"),
						*Context->ShaderStage,
						Context->ShaderEntry.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" entry %s"), *Context->ShaderEntry),
						Context->bShaderDebuggable ? TEXT("yes") : TEXT("no"),
						Context->bSourceDebugInfo ? TEXT("yes") : TEXT("no"));
					Report += FString::Printf(TEXT("- Shader reflection：encoding=%s；inputSig=%d；outputSig=%d；constantBlocks=%d；samplers=%d；RO=%d；RW=%d\n"),
						Context->ShaderEncoding.IsEmpty() ? TEXT("unknown") : *Context->ShaderEncoding,
						Context->ShaderInputSignatureCount, Context->ShaderOutputSignatureCount,
						Context->ShaderConstantBlockCount, Context->ShaderSamplerCount,
						Context->ShaderReadOnlyResourceCount, Context->ShaderReadWriteResourceCount);
					int32 ShownInputs = 0;
					for (const FBoundResourceEvidence& Input : Context->Inputs)
					{
						if (ShownInputs++ >= MaxDisplayedFrontierResources)
						{
							break;
						}
						Report += Input.bTexture
							? FString::Printf(TEXT("- Input: %s [%s, %s] %dx%d\n"),
								*Input.Name, *Input.Stage, *Input.Access, Input.Width, Input.Height)
							: FString::Printf(TEXT("- Input: %s [%s, %s] buffer/resource\n"),
								*Input.Name, *Input.Stage, *Input.Access);
					}
					int32 ShownOutputs = 0;
					for (const FBoundResourceEvidence& Output : Context->Outputs)
					{
						if (ShownOutputs++ >= MaxDisplayedFrontierResources)
						{
							break;
						}
						Report += Output.bTexture
							? FString::Printf(TEXT("- Output: %s [%s, %s] %dx%d\n"),
								*Output.Name, *Output.Stage, *Output.Access, Output.Width, Output.Height)
							: FString::Printf(TEXT("- Output: %s [%s, %s] buffer/resource\n"),
								*Output.Name, *Output.Stage, *Output.Access);
					}
					for (const TSharedPtr<FJsonValue>& ProvenanceValue : Context->ResourceProvenance)
					{
						const TSharedPtr<FJsonObject> Provenance = ProvenanceValue.IsValid() ? ProvenanceValue->AsObject() : nullptr;
						if (!Provenance.IsValid())
						{
							continue;
						}
						FString ResourceName;
						FString ProducerAction;
						FString ProducerKind;
						FString CoordinateMapping;
						bool bProducerFound = false;
						double ProducerEventId = 0.0;
						Provenance->TryGetStringField(TEXT("resource"), ResourceName);
						Provenance->TryGetStringField(TEXT("producerAction"), ProducerAction);
						Provenance->TryGetStringField(TEXT("producerKind"), ProducerKind);
						Provenance->TryGetStringField(TEXT("coordinateMapping"), CoordinateMapping);
						Provenance->TryGetBoolField(TEXT("producerFound"), bProducerFound);
						Provenance->TryGetNumberField(TEXT("producerEventId"), ProducerEventId);
						Report += bProducerFound
							? FString::Printf(TEXT("- Input producer: %s ← EID %u [%s] %s；%s\n"), *ResourceName,
								static_cast<uint32>(ProducerEventId), *ProducerKind, *ProducerAction, *CoordinateMapping)
							: FString::Printf(TEXT("- Input producer: %s ← 未找到此前写入事件；%s\n"), *ResourceName, *CoordinateMapping);
					}

					Report += TEXT("\nPipeline 固定功能状态\n");
					Report += FormatPipelineStateEvidence(Context->PipelineState);
					Report += TEXT("\nShader 算法证据\n");
					if (Context->ShaderDebugTrace.IsValid())
					{
						Report += FormatShaderDebugTraceEvidence(Context->ShaderDebugTrace);
					}
					else if (Context->bShaderDebuggable && Context->bSourceDebugInfo)
					{
						Report += TEXT("- Shader has debug/source information; this report currently has reflection and bindings only, not an instruction-level trace.\n");
					}
					else
					{
						Report += TEXT("- Algorithm is not proven: entry name and bound input do not identify the math performed by Main. Shader source/debug trace is unavailable for this event.\n");
					}
					if (!Context->ShaderDebugStatus.IsEmpty())
					{
						Report += FString::Printf(TEXT("- Shader debug status: %s\n"), *Context->ShaderDebugStatus);
					}
				}
				else if (FailedEventContextIds.Contains(Candidate.Event.EventId))
				{
					Report += TEXT("- Event-context query failed; the chain remains broken here.\n");
				}
				else
				{
					Report += TEXT("- Querying this event's used inputs and shader-debug capability.\n");
				}
			}
			else
			{
				Summary = TEXT("Pixel History 没有提供可用的非 Present 事件；当前只能确认最终目标处的因果链中断。");
				CausalPath += TEXT("\n↓\n未找到可追踪 GPU 写入\n↓\n■ 因果链中断");
				AddHypothesis(Hypotheses,
					TEXT("当前目标可能只有背景/Clear、没有可见 Fragment，或该 API/资源的 Pixel History 不完整；不能据此断言具体剔除原因。"));
			}

			if (Hypotheses.IsEmpty())
			{
				AddHypothesis(Hypotheses,
					TEXT("当前证据只说明像素实际经历的 GPU 过程；没有设计期参考值时，不判断颜色“应该”是什么。"));
			}
			FString Suspects;
				Report += TEXT("\n证据支持的可能性\n");
			for (int32 Index = 0; Index < Hypotheses.Num(); ++Index)
			{
				Report += FString::Printf(TEXT("%d. %s\n"), Index + 1, *Hypotheses[Index]);
				Suspects += FString::Printf(TEXT("%d  %s%s"), Index + 1, *Hypotheses[Index],
					Index + 1 < Hypotheses.Num() ? TEXT("\n\n") : TEXT(""));
			}

			for (const FPixelSample* Sample : ReadySamples)
			{
				const int32 DisplayIndex = Samples.IndexOfByPredicate([Sample](const FPixelSample& Item)
				{
					return Item.Id == Sample->Id;
				});
				if (const TArray<FEventEvidence>* Events = AggregatedBySample.Find(Sample->Id))
				{
					Report += FString::Printf(TEXT("\nPass / event chain P%d (marker-derived; showing latest %d/%d events)\n"),
						DisplayIndex + 1, FMath::Min(MaxDisplayedTraceHops, Events->Num()), Events->Num());
					Report += TEXT("Pass boundary is inferred from RenderDoc action/marker data; a generic Slate ElementBatch marker is not a guaranteed RenderGraph pass name.\n");
					const int32 First = FMath::Max(0, Events->Num() - MaxDisplayedTraceHops);
					int32 Hop = 0;
					for (int32 EventIndex = Events->Num() - 1; EventIndex >= First; --EventIndex)
					{
						const FEventEvidence& Event = (*Events)[EventIndex];
						Report += FString::Printf(TEXT("%d. EID %u [%s] %s\n   marker/pass: %s\n   semantic: %s | flags=%u | result: %s\n   pixel: before=%s | shaderOutput=%s | after=%s\n"),
							++Hop, Event.EventId, *Event.ActionKind, *Event.Action,
							*CompactMarkerPath(Event.MarkerPath), *ClassifySemantics(Event), Event.ActionFlags, *DescribeEventResult(Event),
							*Event.Before, *Event.ShaderOutput, *Event.After);
						if (const FEventContextEvidence* Context = EventContexts.Find(Event.EventId))
						{
							Report += FString::Printf(TEXT("   context: shader=%s%s; inputs=%d; outputs=%d; fixedFunction=%s; debugTrace=%s\n"),
								*Context->ShaderStage,
								Context->ShaderEntry.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("/%s"), *Context->ShaderEntry),
								Context->Inputs.Num(), Context->Outputs.Num(),
								Context->PipelineState.IsValid() ? TEXT("available") : TEXT("unavailable"),
								Context->ShaderDebugTrace.IsValid() ? TEXT("available") : TEXT("not-run"));
						}
					}
				}
			}

			Report += FString::Printf(TEXT("\nConfidence: %s. Applies only to the observed GPU chain, not design intent or engine-side cause.\n"),
				ReadySamples.Num() >= 2 ? TEXT("medium") : TEXT("low"));
			SetReportCards(Summary, CausalPath, Suspects, Report);
		}
		TSharedPtr<SEditableTextBox> CapturePathBox;
		TSharedPtr<STextBlock> StatusText;
		TSharedPtr<STextBlock> SelectionText;
		TSharedPtr<SMultiLineEditableText> SummaryText;
		TSharedPtr<SMultiLineEditableText> CausalPathText;
		TSharedPtr<SMultiLineEditableText> SuspectsText;
		TSharedPtr<SMultiLineEditableText> EvidenceText;
		TSharedPtr<STextBlock> TechnicalToggleText;
		TSharedPtr<SBox> TechnicalEvidenceBox;
		TSharedPtr<SMultiLineEditableTextBox> AgentIntentTextBox;
		TSharedPtr<SVerticalBox> AgentReportBox;
		TSharedPtr<SMultiLineEditableText> AgentOutputText;
		TSharedPtr<SMultiLineEditableText> AgentStatusText;
		TSharedPtr<STextBlock> AgentRunButtonText;
		TSharedPtr<SRenderTrailImageView> ImageView;
		TSharedPtr<FSlateDynamicImageBrush> PreviewBrush;
		TArray<FPixelSample> Samples;
		TMap<FString, uint64> PendingSampleByRequest;
		TMap<uint32, FEventContextEvidence> EventContexts;
		TMap<FString, uint32> PendingEventContextByRequest;
		TMap<FString, uint32> PendingShaderDebugByRequest;
		TSet<uint32> PendingEventContextIds;
		TSet<uint32> FailedEventContextIds;
		TSet<uint32> FailedShaderDebugIds;
		TOptional<FCausalCandidate> LastCandidate;
		FString LastReportSummary;
		FString LastReportCausalPath;
		FString AgentBrokerUrl;
		TArray<TSharedPtr<FJsonValue>> AgentMessages;
		FHttpRequestPtr AgentRequest;
		TOptional<uint32> AgentPendingEventId;
		FString LastWorkerError;
		FRenderTrailDiagnosticsOptions DiagnosticsOptions;
		FString DiagnosticsFilePath;
		FProcHandle WorkerHandle;
		void* StdOutRead = nullptr;
		void* StdOutWrite = nullptr;
		void* StdInRead = nullptr;
		void* StdInWrite = nullptr;
		void* StdErrRead = nullptr;
		void* StdErrWrite = nullptr;
		FString OutputBuffer;
		FString ErrorBuffer;
		FString PreviewPath;
		uint64 PreviewSerial = 0;
		uint64 RequestSerial = 0;
		uint64 SampleSerial = 0;
		int32 AgentStep = 0;
		bool bWorkerReady = false;
		bool bTechnicalEvidenceExpanded = false;
		bool bAgentRunning = false;
		bool bAgentWaitingForDeterministicContexts = false;
		bool bShaderDebuggingAvailable = false;
		bool bSelectionConfirmed = false;
		bool bLastCandidateHasDivergence = false;
		bool bCaptureLoading = false;
		bool bPreviewReadyForSelection = false;
		bool bReplaySynchronizationPending = false;
		bool bReplayStartDeferred = false;
		bool bQueuePixelHistoryAfterWorkerReady = false;
		bool bExitReported = false;
		double CaptureLoadStartSeconds = 0.0;
		double LastCaptureLoadStatusSeconds = 0.0;
		FString CaptureLoadPhase;
		static constexpr int32 MaxPixelSamples = 1;
		static constexpr int32 MaxDisplayedTraceHops = 24;
		static constexpr int32 MaxDisplayedFrontierResources = 6;
		static constexpr int32 MaxAgentPrefilterEventsPerSample = 6;
		static constexpr int32 MaxAgentEventChainPerSample = 48;
		static constexpr int32 MaxDeterministicContextEvents = 24;
		static constexpr int32 MaxAgentSteps = 1;
	};
}

namespace UE::RenderTrail::Private
{
	static const FName AnalyzerTabId(TEXT("RenderTrailAnalyzer"));

	class FRenderTrailAnalyzerEditorModule final : public IRenderTrailAnalyzerEditorModule
	{
	public:
		virtual void StartupModule() override
		{
			FGlobalTabmanager::Get()->RegisterNomadTabSpawner(AnalyzerTabId,
				FOnSpawnTab::CreateRaw(this, &FRenderTrailAnalyzerEditorModule::SpawnAnalyzerTab))
				.SetDisplayName(FText::FromString(TEXT("RenderTrail Analyzer")))
				.SetTooltipText(FText::FromString(TEXT("Inspect RenderDoc captures with bounded pixel-local evidence.")));
		}

		virtual void ShutdownModule() override
		{
			AnalyzerWidget.Reset();
			if (FSlateApplication::IsInitialized())
			{
				FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(AnalyzerTabId);
			}
		}

		virtual void OpenCapture(const FString& CapturePath) override
		{
			PendingCapturePath = FPaths::ConvertRelativePathToFull(CapturePath);
			const TSharedPtr<SAnalyzerHome> ExistingWidget = AnalyzerWidget.Pin();
			FGlobalTabmanager::Get()->TryInvokeTab(AnalyzerTabId);
			if (ExistingWidget.IsValid())
			{
				ExistingWidget->OpenCapture(PendingCapturePath);
			}
		}

	private:
		TSharedRef<SDockTab> SpawnAnalyzerTab(const FSpawnTabArgs&)
		{
			TSharedRef<SAnalyzerHome> Widget = SNew(SAnalyzerHome).InitialCapture(PendingCapturePath);
			AnalyzerWidget = Widget;
			return SNew(SDockTab)
				.TabRole(ETabRole::NomadTab)
				[
					Widget
				];
		}

		FString PendingCapturePath;
		TWeakPtr<SAnalyzerHome> AnalyzerWidget;
	};
}

IMPLEMENT_MODULE(UE::RenderTrail::Private::FRenderTrailAnalyzerEditorModule, RenderTrailAnalyzerEditor)
