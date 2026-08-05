#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

namespace UE::RenderTrail::Private
{
	class SAnalyzerHome;

	class SRenderTrailAnalyzerHome final : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SRenderTrailAnalyzerHome) {}
			SLATE_ARGUMENT(FString, InitialCapture)
		SLATE_END_ARGS()

		~SRenderTrailAnalyzerHome() override;

		void Construct(const FArguments& Args);
		void OpenCapture(const FString& CapturePath);

	private:
		TSharedPtr<SAnalyzerHome> Implementation;
	};
}
