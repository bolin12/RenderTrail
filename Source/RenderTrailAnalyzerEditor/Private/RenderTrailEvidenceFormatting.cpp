#include "RenderTrailEvidenceFormatting.h"

namespace UE::RenderTrail::Private::EvidenceFormatting
{
	namespace
	{
		FString FormatShaderVariable(const TSharedPtr<FJsonObject>& Variable)
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
			const FString ValueText = Values.IsEmpty()
				? TEXT("<struct/resource>")
				: FString::Printf(TEXT("(%s)"), *FString::Join(Values, TEXT(", ")));
			return FString::Printf(TEXT("%s[%s]=%s"), Name.IsEmpty() ? TEXT("unnamed") : *Name,
				Type.IsEmpty() ? TEXT("unknown") : *Type, *ValueText);
		}
	}

	FString FormatPipelineCompactSummary(const TSharedPtr<FJsonObject>& PipelineState)
	{
		if (!PipelineState.IsValid())
		{
			return TEXT("未采集");
		}
		bool bCaptured = false;
		PipelineState->TryGetBoolField(TEXT("fixedFunctionCaptured"), bCaptured);
		if (!bCaptured)
		{
			return TEXT("固定管线不可用");
		}

		FString Topology = TEXT("Topology unknown");
		PipelineState->TryGetStringField(TEXT("primitiveTopology"), Topology);
		bool bDepth = false;
		bool bStencil = false;
		const TSharedPtr<FJsonObject>* DepthStencil = nullptr;
		if (PipelineState->TryGetObjectField(TEXT("depthStencil"), DepthStencil) && DepthStencil && DepthStencil->IsValid())
		{
			(*DepthStencil)->TryGetBoolField(TEXT("depthEnable"), bDepth);
			(*DepthStencil)->TryGetBoolField(TEXT("stencilEnable"), bStencil);
		}
		bool bBlend = false;
		const TSharedPtr<FJsonObject>* Blend = nullptr;
		if (PipelineState->TryGetObjectField(TEXT("blend"), Blend) && Blend && Blend->IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
			if ((*Blend)->TryGetArrayField(TEXT("targets"), Targets) && Targets && !Targets->IsEmpty())
			{
				const TSharedPtr<FJsonObject> Target = (*Targets)[0].IsValid() ? (*Targets)[0]->AsObject() : nullptr;
				if (Target.IsValid())
				{
					Target->TryGetBoolField(TEXT("enabled"), bBlend);
				}
			}
		}
		return FString::Printf(TEXT("%s · Depth %s · Stencil %s · Blend %s"), *Topology,
			bDepth ? TEXT("on") : TEXT("off"), bStencil ? TEXT("on") : TEXT("off"), bBlend ? TEXT("on") : TEXT("off"));
	}

	FString FormatPipelineState(const TSharedPtr<FJsonObject>& PipelineState)
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
			Api.IsEmpty() ? TEXT("unknown") : *Api, bCaptured ? TEXT("已采集") : TEXT("未采集"));
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

	FString FormatShaderDebugTrace(const TSharedPtr<FJsonObject>& Trace)
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
					Values.Add(FormatShaderVariable(Variable));
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

		const TSharedPtr<FJsonObject>* ShaderCodeEvidence = nullptr;
		if (Trace->TryGetObjectField(TEXT("shaderCodeEvidence"), ShaderCodeEvidence)
			&& ShaderCodeEvidence && ShaderCodeEvidence->IsValid())
		{
			FString CodeStatus;
			(*ShaderCodeEvidence)->TryGetStringField(TEXT("status"), CodeStatus);
			Report += FString::Printf(TEXT("  Shader 代码证据：%s（DebugPixel 指令映射的反汇编窗口）\n"),
				CodeStatus.IsEmpty() ? TEXT("unknown") : *CodeStatus);
			const TArray<TSharedPtr<FJsonValue>>* CodeLines = nullptr;
			if ((*ShaderCodeEvidence)->TryGetArrayField(TEXT("lines"), CodeLines) && CodeLines)
			{
				int32 ShownCodeLines = 0;
				for (const TSharedPtr<FJsonValue>& Value : *CodeLines)
				{
					const TSharedPtr<FJsonObject> Line = Value.IsValid() ? Value->AsObject() : nullptr;
					if (!Line.IsValid())
					{
						continue;
					}
					double Instruction = 0.0;
					double DisassemblyLine = 0.0;
					FString Code;
					Line->TryGetNumberField(TEXT("instruction"), Instruction);
					Line->TryGetNumberField(TEXT("disassemblyLine"), DisassemblyLine);
					Line->TryGetStringField(TEXT("text"), Code);
					Report += FString::Printf(TEXT("    i%d / line %d: %s\n"), static_cast<int32>(Instruction),
						static_cast<int32>(DisassemblyLine), *Code);
					if (++ShownCodeLines >= 8)
					{
						break;
					}
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
						Changes.Add(FormatShaderVariable(BeforeObject) + TEXT(" -> ") + FormatShaderVariable(AfterObject));
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
}
