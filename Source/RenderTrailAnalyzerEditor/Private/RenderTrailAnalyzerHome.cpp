#include "RenderTrailAnalyzerHome.h"
#include "RenderTrailAnalyzerHomeInternal.h"

DEFINE_LOG_CATEGORY(LogRenderTrailAnalyzer);

namespace UE::RenderTrail::Private
{
	SRenderTrailAnalyzerHome::~SRenderTrailAnalyzerHome() = default;

	void SRenderTrailAnalyzerHome::Construct(const FArguments& Args)
	{
		SAssignNew(Implementation, SAnalyzerHome)
			.InitialCapture(Args._InitialCapture);
		ChildSlot
		[
			Implementation.ToSharedRef()
		];
	}

	void SRenderTrailAnalyzerHome::OpenCapture(const FString& CapturePath)
	{
		if (Implementation.IsValid())
		{
			Implementation->OpenCapture(CapturePath);
		}
	}
}
