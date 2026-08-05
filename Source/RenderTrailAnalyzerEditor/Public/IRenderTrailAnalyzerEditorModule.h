#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

class RENDERTRAILANALYZEREDITOR_API IRenderTrailAnalyzerEditorModule : public IModuleInterface
{
public:
	static IRenderTrailAnalyzerEditorModule& Get()
	{
		return FModuleManager::LoadModuleChecked<IRenderTrailAnalyzerEditorModule>(TEXT("RenderTrailAnalyzerEditor"));
	}

	virtual void OpenCapture(const FString& CapturePath) = 0;
};
