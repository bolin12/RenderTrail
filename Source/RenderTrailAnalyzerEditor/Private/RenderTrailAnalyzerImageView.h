#pragma once

#include "CoreMinimal.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Rendering/DrawElements.h"
#include "Widgets/SLeafWidget.h"

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


}

