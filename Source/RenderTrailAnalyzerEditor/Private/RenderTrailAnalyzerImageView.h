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

		void Construct(const FArguments& Args);

		void SetImage(const TSharedPtr<FSlateDynamicImageBrush>& InBrush, FIntPoint InSize, TArray<uint8> InPixelBytes = {});

		void SetMarkers(const TArray<FPixelMarker>& InMarkers);

		virtual FVector2D ComputeDesiredSize(float) const override;

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
			FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

		virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override;

		virtual FReply OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event) override;

		virtual FReply OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event) override;

		virtual FReply OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event) override;

		virtual void OnMouseLeave(const FPointerEvent& Event) override;

	private:
		static constexpr float PixelExactMinScale = 4.0f;
		static constexpr float PixelGridMinScale = 8.0f;

		static FLinearColor GetMarkerColor();

		bool HasPixelBytes() const;

		bool ComputeVisiblePixelRange(FVector2f LocalSize, FVector2f Origin, float Scale,
			int32& OutFirstX, int32& OutFirstY, int32& OutEndX, int32& OutEndY) const;

		void DrawPixelExact(FSlateWindowElementList& OutDrawElements, const FGeometry& Geometry, int32 Layer,
			FVector2f LocalSize, FVector2f Origin, float Scale) const;

		void DrawPixelGrid(FSlateWindowElementList& OutDrawElements, const FGeometry& Geometry, int32 Layer,
			FVector2f LocalSize, FVector2f Origin, float Scale) const;

		static void DrawPixelOutline(FSlateWindowElementList& OutDrawElements, const FGeometry& Geometry, int32 Layer,
			FVector2f Origin, float Scale, FIntPoint Pixel, const FLinearColor& Color, float Thickness);

		bool TryGetPixelAtLocal(FVector2f Local, FVector2f LocalSize, FIntPoint& OutPixel) const;

		bool UpdateHoveredPixel(FVector2f Local, FVector2f LocalSize);

		static FVector2f ConstrainPan(FVector2f LocalSize, FVector2f DrawSize, FVector2f RequestedPan);

		void ClampPan(FVector2f LocalSize);

		bool ComputeImageRect(FVector2f LocalSize, FVector2f& OutOrigin, FVector2f& OutDrawSize, float& OutScale) const;

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

