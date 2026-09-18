// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#include "Bridge/N2CGraphImporter.h"

#include "Bridge/N2CTypeStringConverter.h"
#include "Utils/N2CLogger.h"

#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "EdGraphNode_Comment.h"
#include "ScopedTransaction.h"
#include "BlueprintFunctionNodeSpawner.h"

#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_Self.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_StructOperation.h"
#include "K2Node_Select.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_SwitchString.h"
#include "K2Node_SwitchName.h"
#include "K2Node_MakeArray.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_Knot.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "UObject/UnrealType.h"
#include "Dom/JsonValue.h"

// ============================================================================
// Small string/lookup helpers
// ============================================================================

int32 FN2CGraphImporter::LevenshteinDistance(const FString& A, const FString& B)
{
	const int32 M = A.Len();
	const int32 N = B.Len();
	TArray<int32> Prev, Curr;
	Prev.SetNumUninitialized(N + 1);
	Curr.SetNumUninitialized(N + 1);
	for (int32 j = 0; j <= N; ++j)
	{
		Prev[j] = j;
	}
	for (int32 i = 1; i <= M; ++i)
	{
		Curr[0] = i;
		for (int32 j = 1; j <= N; ++j)
		{
			const int32 Cost = (FChar::ToLower(A[i - 1]) == FChar::ToLower(B[j - 1])) ? 0 : 1;
			Curr[j] = FMath::Min3(Prev[j] + 1, Curr[j - 1] + 1, Prev[j - 1] + Cost);
		}
		Prev = Curr;
	}
	return Prev[N];
}

FString FN2CGraphImporter::NormalizePinName(const FString& In)
{
	FString Out;
	Out.Reserve(In.Len());
	for (const TCHAR C : In)
	{
		if (C != TEXT(' ') && C != TEXT('_'))
		{
			Out.AppendChar(FChar::ToLower(C));
		}
	}
	return Out;
}

FString FN2CGraphImporter::DescribeNodePins(UEdGraphNode* Node)
{
	TArray<FString> InPins, OutPins;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin)
		{
			continue;
		}
		FString Description = Pin->PinName.ToString();
		const FString Display = Pin->GetDisplayName().ToString();
		if (!Display.IsEmpty() && Display != Description)
		{
			Description += FString::Printf(TEXT(" (\"%s\")"), *Display);
		}
		(Pin->Direction == EGPD_Input ? InPins : OutPins).Add(Description);
	}
	return FString::Printf(TEXT("in[%s] out[%s]"), *FString::Join(InPins, TEXT(", ")), *FString::Join(OutPins, TEXT(", ")));
}

// ============================================================================
// Pin resolution (docs §7.5)
// ============================================================================

UEdGraphPin* FN2CGraphImporter::ResolveSemanticAlias(UEdGraphNode* Node, const FString& DocNodeKind, const FString& LowerAlias, EEdGraphPinDirection Direction)
{
	if (DocNodeKind == TEXT("branch"))
	{
		if (LowerAlias == TEXT("true")) return Node->FindPin(TEXT("then"), Direction);
		if (LowerAlias == TEXT("false")) return Node->FindPin(TEXT("else"), Direction);
		if (LowerAlias == TEXT("condition")) return Node->FindPin(TEXT("Condition"), Direction);
	}
	else if (DocNodeKind == TEXT("cast"))
	{
		if (LowerAlias == TEXT("object")) return Node->FindPin(TEXT("Object"), Direction);
		if (LowerAlias == TEXT("failed")) return Node->FindPin(TEXT("CastFailed"), Direction);
		if (LowerAlias == TEXT("success")) return Node->FindPin(TEXT("bSuccess"), Direction);
		if (LowerAlias == TEXT("result"))
		{
			if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
			{
				return CastNode->GetCastResultPin();
			}
		}
	}
	else if (DocNodeKind == TEXT("variable_get"))
	{
		if (LowerAlias == TEXT("value"))
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin->Direction == EGPD_Output) return Pin;
			}
		}
	}
	else if (DocNodeKind == TEXT("variable_set"))
	{
		if (LowerAlias == TEXT("out")) return Node->FindPin(TEXT("Output_Get"), Direction);
		if (LowerAlias == TEXT("value"))
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					return Pin;
				}
			}
		}
	}
	else if (DocNodeKind == TEXT("call_function") || DocNodeKind == TEXT("call_parent"))
	{
		if (LowerAlias == TEXT("target")) return Node->FindPin(TEXT("self"), Direction);
		if (LowerAlias == TEXT("return")) return Node->FindPin(TEXT("ReturnValue"), Direction);
	}
	else if (DocNodeKind == TEXT("sequence"))
	{
		if (LowerAlias.IsNumeric())
		{
			return Node->FindPin(FName(*FString::Printf(TEXT("then_%s"), *LowerAlias)), Direction);
		}
	}
	else if (DocNodeKind == TEXT("select"))
	{
		if (LowerAlias == TEXT("index")) return Node->FindPin(TEXT("Index"), Direction);
		if (LowerAlias.IsNumeric())
		{
			return Node->FindPin(FName(*FString::Printf(TEXT("Option %s"), *LowerAlias)), Direction);
		}
	}
	else if (DocNodeKind == TEXT("macro"))
	{
		if (LowerAlias == TEXT("body")) return Node->FindPin(TEXT("LoopBody"), Direction);
		if (LowerAlias == TEXT("element")) return Node->FindPin(TEXT("Array Element"), Direction);
		if (LowerAlias == TEXT("index")) return Node->FindPin(TEXT("Array Index"), Direction);
		if (LowerAlias == TEXT("completed")) return Node->FindPin(TEXT("Completed"), Direction);
	}

	return nullptr;
}

UEdGraphPin* FN2CGraphImporter::ResolvePin(UEdGraphNode* Node, const FString& DocNodeKind, const FString& RequestedPinName, EEdGraphPinDirection Direction, FString& OutError)
{
	if (!Node)
	{
		OutError = TEXT("node was not created");
		return nullptr;
	}

	TArray<UEdGraphPin*> Candidates;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == Direction)
		{
			Candidates.Add(Pin);
		}
	}

	// 1. Exact internal PinName (case-sensitive, then case-insensitive)
	for (UEdGraphPin* Pin : Candidates)
	{
		if (Pin->PinName.ToString().Equals(RequestedPinName, ESearchCase::CaseSensitive))
		{
			return Pin;
		}
	}
	for (UEdGraphPin* Pin : Candidates)
	{
		if (Pin->PinName.ToString().Equals(RequestedPinName, ESearchCase::IgnoreCase))
		{
			return Pin;
		}
	}

	// 2. Semantic aliases (§7.5 table)
	if (UEdGraphPin* Aliased = ResolveSemanticAlias(Node, DocNodeKind, RequestedPinName.ToLower(), Direction))
	{
		return Aliased;
	}

	// 3. Normalized match: strip spaces/underscores, lowercase; compare vs internal + display names
	const FString NormalizedRequest = NormalizePinName(RequestedPinName);
	for (UEdGraphPin* Pin : Candidates)
	{
		if (NormalizePinName(Pin->PinName.ToString()) == NormalizedRequest ||
			NormalizePinName(Pin->GetDisplayName().ToString()) == NormalizedRequest)
		{
			return Pin;
		}
	}

	// 4. exec/then shorthand, only when exactly one exec pin exists in this direction
	if (RequestedPinName == TEXT("exec") || RequestedPinName == TEXT("then"))
	{
		UEdGraphPin* OnlyExecPin = nullptr;
		int32 ExecCount = 0;
		for (UEdGraphPin* Pin : Candidates)
		{
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				++ExecCount;
				OnlyExecPin = Pin;
			}
		}
		if (ExecCount == 1)
		{
			return OnlyExecPin;
		}
	}

	// 5. Split struct sub-pins: "Location_X" -> split "Location" if it isn't already split
	int32 UnderscoreIndex = INDEX_NONE;
	if (RequestedPinName.FindLastChar(TEXT('_'), UnderscoreIndex))
	{
		const FString ParentName = RequestedPinName.Left(UnderscoreIndex);
		for (UEdGraphPin* Pin : Candidates)
		{
			if (Pin->SubPins.Num() == 0 && Pin->PinName.ToString().Equals(ParentName, ESearchCase::IgnoreCase))
			{
				if (const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(Node->GetGraph()->GetSchema()))
				{
					K2Schema->SplitPin(Pin);
				}
				break;
			}
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Direction && Pin->PinName.ToString().Equals(RequestedPinName, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
	}

	OutError = FString::Printf(TEXT("pin '%s' not found on '%s'. Pins: %s"),
		*RequestedPinName, *Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), *DescribeNodePins(Node));
	return nullptr;
}

// ============================================================================
// Function / macro resolution (docs §8.3 step 5)
// ============================================================================

void FN2CGraphImporter::CollectFunctionSuggestions(UClass* Class, const FString& TypoName, TArray<FString>& OutSuggestions)
{
	if (!Class)
	{
		return;
	}
	for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
	{
		UFunction* Func = *FuncIt;
		if (!Func || !Func->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
		{
			continue;
		}
		const FString FuncName = Func->GetName();
		if (LevenshteinDistance(TypoName.ToLower(), FuncName.ToLower()) <= 3)
		{
			OutSuggestions.Add(FString::Printf(TEXT("%s (%s)"), *FuncName, *Class->GetName()));
		}
	}
}

void FN2CGraphImporter::CollectFunctionSuggestionsAcrossLibraries(const FString& TypoName, TArray<FString>& OutSuggestions)
{
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!Class || !Class->IsChildOf(UBlueprintFunctionLibrary::StaticClass()))
		{
			continue;
		}
		CollectFunctionSuggestions(Class, TypoName, OutSuggestions);
		if (OutSuggestions.Num() >= 5)
		{
			break;
		}
	}
}

FN2CGraphImporter::FFunctionResolution FN2CGraphImporter::ResolveFunction(UBlueprint* Blueprint, const FString& FunctionName, const FString& ClassHint)
{
	FFunctionResolution Result;
	const FName FunctionFName(*FunctionName);

	auto IsUsable = [](UFunction* Func) -> bool
	{
		return Func && Func->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure | FUNC_BlueprintEvent);
	};

	// 1. Explicit class hint
	if (!ClassHint.IsEmpty())
	{
		UClass* HintClass = FN2CTypeStringConverter::ResolveClassByNameOrPath(ClassHint);
		if (HintClass)
		{
			if (UFunction* Func = HintClass->FindFunctionByName(FunctionFName))
			{
				if (IsUsable(Func))
				{
					Result.Function = Func;
					Result.OwnerClass = Func->GetOwnerClass();
					return Result;
				}
			}
			CollectFunctionSuggestions(HintClass, FunctionName, Result.Suggestions);
			return Result;
		}
		// Hinted class not found - fall through and search anyway (the hint might just be stale)
	}

	// 2. Skeleton generated class (so newly declared functions resolve)
	if (Blueprint->SkeletonGeneratedClass)
	{
		if (UFunction* Func = Blueprint->SkeletonGeneratedClass->FindFunctionByName(FunctionFName))
		{
			if (IsUsable(Func))
			{
				Result.Function = Func;
				Result.OwnerClass = Func->GetOwnerClass();
				return Result;
			}
		}
	}

	// 3. Parent class
	if (Blueprint->ParentClass)
	{
		if (UFunction* Func = Blueprint->ParentClass->FindFunctionByName(FunctionFName))
		{
			if (IsUsable(Func))
			{
				Result.Function = Func;
				Result.OwnerClass = Func->GetOwnerClass();
				return Result;
			}
		}
	}

	// 4. Every UBlueprintFunctionLibrary, then (if none) every other class - only if exactly one matches
	TArray<UClass*> Candidates;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!Class || !Class->IsChildOf(UBlueprintFunctionLibrary::StaticClass()))
		{
			continue;
		}
		if (UFunction* Func = Class->FindFunctionByName(FunctionFName, EIncludeSuperFlag::ExcludeSuper))
		{
			if (IsUsable(Func))
			{
				Candidates.Add(Class);
			}
		}
	}
	if (Candidates.Num() == 0)
	{
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!Class || Class->IsChildOf(UBlueprintFunctionLibrary::StaticClass()))
			{
				continue; // already checked above
			}
			if (UFunction* Func = Class->FindFunctionByName(FunctionFName, EIncludeSuperFlag::ExcludeSuper))
			{
				if (IsUsable(Func))
				{
					Candidates.Add(Class);
				}
			}
		}
	}

	if (Candidates.Num() == 1)
	{
		Result.Function = Candidates[0]->FindFunctionByName(FunctionFName);
		Result.OwnerClass = Candidates[0];
		return Result;
	}
	if (Candidates.Num() > 1)
	{
		Result.bAmbiguous = true;
		for (UClass* Candidate : Candidates)
		{
			Result.AmbiguousCandidates.Add(Candidate->GetName());
		}
		return Result;
	}

	// Nothing found anywhere - offer "did you mean" across every function library
	CollectFunctionSuggestionsAcrossLibraries(FunctionName, Result.Suggestions);
	return Result;
}

UEdGraph* FN2CGraphImporter::ResolveMacroGraph(UBlueprint* Blueprint, const FString& MacroName, const FString& LibraryPath)
{
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (Graph && Graph->GetName() == MacroName)
		{
			return Graph;
		}
	}

	UBlueprint* LibraryBlueprint = LoadObject<UBlueprint>(nullptr, *LibraryPath);
	if (!LibraryBlueprint)
	{
		return nullptr;
	}
	for (UEdGraph* Graph : LibraryBlueprint->MacroGraphs)
	{
		if (Graph && Graph->GetName() == MacroName)
		{
			return Graph;
		}
	}
	return nullptr;
}

// ============================================================================
// Node creation, one function per §7.4 kind
// ============================================================================

UEdGraphNode* FN2CGraphImporter::CreateEventNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode)
{
	const FString EventName = DocNode.GetString(TEXT("event"));
	if (EventName.IsEmpty())
	{
		Ctx.Report->AddError(TEXT("node"), DocNode.Id, TEXT("event node missing 'event' field"));
		return nullptr;
	}

	UClass* OwnerClass = Ctx.Blueprint->SkeletonGeneratedClass ? Ctx.Blueprint->SkeletonGeneratedClass.Get() : Ctx.Blueprint->ParentClass;
	const FString ClassHint = DocNode.GetString(TEXT("class"));
	if (!ClassHint.IsEmpty())
	{
		if (UClass* Hinted = FN2CTypeStringConverter::ResolveClassByNameOrPath(ClassHint))
		{
			OwnerClass = Hinted;
		}
	}

	// Reuse the existing override node if there is one (§8.3 step 6)
	if (UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(Ctx.Blueprint, OwnerClass, FName(*EventName)))
	{
		Ctx.Report->AddOk(TEXT("node"), DocNode.Id, FString::Printf(TEXT("Event %s (existing node reused)"), *EventName));
		return Existing;
	}

	UFunction* Function = OwnerClass ? OwnerClass->FindFunctionByName(FName(*EventName)) : nullptr;
	if (!Function || !Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
	{
		Ctx.Report->AddError(TEXT("node"), DocNode.Id,
			FString::Printf(TEXT("'%s' is not an overridable event on %s. If this overrides a non-event virtual function, use Blueprint > Override in the editor instead (§8.6)."),
				*EventName, OwnerClass ? *OwnerClass->GetName() : TEXT("<unknown class>")));
		return nullptr;
	}

	FGraphNodeCreator<UK2Node_Event> Creator(*Ctx.Graph);
	UK2Node_Event* EventNode = Creator.CreateNode();
	EventNode->EventReference.SetExternalMember(FName(*EventName), OwnerClass);
	EventNode->bOverrideFunction = true;
	Creator.Finalize();

	Ctx.Report->AddOk(TEXT("node"), DocNode.Id, FString::Printf(TEXT("Event %s"), *EventName));
	return EventNode;
}

UEdGraphNode* FN2CGraphImporter::CreateCustomEventNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode)
{
	const FString EventName = DocNode.GetString(TEXT("name"));
	if (EventName.IsEmpty())
	{
		Ctx.Report->AddError(TEXT("node"), DocNode.Id, TEXT("custom_event node missing 'name' field"));
		return nullptr;
	}

	for (UEdGraphNode* Node : Ctx.Graph->Nodes)
	{
		if (UK2Node_CustomEvent* Existing = Cast<UK2Node_CustomEvent>(Node))
		{
			if (Existing->CustomFunctionName == FName(*EventName))
			{
				Ctx.Report->AddOk(TEXT("node"), DocNode.Id, FString::Printf(TEXT("Custom Event %s (existing node reused)"), *EventName));
				return Existing;
			}
		}
	}

	FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Ctx.Graph);
	UK2Node_CustomEvent* EventNode = Creator.CreateNode();
	EventNode->CustomFunctionName = FName(*EventName);
	Creator.Finalize();

	// Optional inline parameter list (§7.4)
	const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
	if (DocNode.Raw.IsValid() && DocNode.Raw->TryGetArrayField(TEXT("inputs"), InputsArray) && InputsArray)
	{
		for (const TSharedPtr<FJsonValue>& InputValue : *InputsArray)
		{
			const TSharedPtr<FJsonObject> InputObj = InputValue.IsValid() ? InputValue->AsObject() : nullptr;
			if (!InputObj.IsValid())
			{
				continue;
			}
			FString ParamName, ParamType;
			InputObj->TryGetStringField(TEXT("name"), ParamName);
			InputObj->TryGetStringField(TEXT("type"), ParamType);
			if (ParamName.IsEmpty())
			{
				continue;
			}
			FEdGraphPinType PinType;
			FString TypeError;
			if (FN2CTypeStringConverter::StringToPinType(ParamType, PinType, TypeError))
			{
				EventNode->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Output);
			}
			else
			{
				Ctx.Report->AddWarning(TEXT("node"), DocNode.Id, FString::Printf(TEXT("could not add parameter '%s': %s"), *ParamName, *TypeError));
			}
		}
	}

	Ctx.Report->AddOk(TEXT("node"), DocNode.Id, FString::Printf(TEXT("Custom Event %s"), *EventName));
	return EventNode;
}

UEdGraphNode* FN2CGraphImporter::CreateCallFunctionNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode, bool bIsParentCall)
{
	const FString FunctionName = DocNode.GetString(TEXT("function"));
	if (FunctionName.IsEmpty())
	{
		Ctx.Report->AddError(TEXT("node"), DocNode.Id, FString::Printf(TEXT("%s node missing 'function' field"), bIsParentCall ? TEXT("call_parent") : TEXT("call_function")));
		return nullptr;
	}

	if (bIsParentCall)
	{
		if (!Ctx.Blueprint->ParentClass)
		{
			Ctx.Report->AddError(TEXT("node"), DocNode.Id, TEXT("Blueprint has no parent class"));
			return nullptr;
		}
		UFunction* Function = Ctx.Blueprint->ParentClass->FindFunctionByName(FName(*FunctionName));
		if (!Function)
		{
			TArray<FString> Suggestions;
			CollectFunctionSuggestions(Ctx.Blueprint->ParentClass, FunctionName, Suggestions);
			Ctx.Report->AddError(TEXT("node"), DocNode.Id,
				FString::Printf(TEXT("function '%s' not found on parent class %s"), *FunctionName, *Ctx.Blueprint->ParentClass->GetName()),
				Suggestions);
			return nullptr;
		}
		FGraphNodeCreator<UK2Node_CallParentFunction> Creator(*Ctx.Graph);
		UK2Node_CallParentFunction* CallNode = Creator.CreateNode();
		CallNode->FunctionReference.SetSelfMember(FName(*FunctionName));
		Creator.Finalize();
		Ctx.Report->AddOk(TEXT("node"), DocNode.Id, FString::Printf(TEXT("Parent::%s"), *FunctionName));
		return CallNode;
	}

	const FString ClassHint = DocNode.GetString(TEXT("class"));
	const FFunctionResolution Resolution = ResolveFunction(Ctx.Blueprint, FunctionName, ClassHint);
	if (Resolution.bAmbiguous)
	{
		Ctx.Report->AddError(TEXT("node"), DocNode.Id,
			FString::Printf(TEXT("function '%s' is ambiguous - found on: %s. Add a 'class' hint."), *FunctionName, *FString::Join(Resolution.AmbiguousCandidates, TEXT(", "))));
		return nullptr;
	}
	if (!Resolution.Function)
	{
		FString Message = FString::Printf(TEXT("function '%s' not found"), *FunctionName);
		if (!ClassHint.IsEmpty())
		{
			Message += FString::Printf(TEXT(" on class '%s'"), *ClassHint);
		}
		if (Resolution.Suggestions.Num() > 0)
		{
			Message += FString::Printf(TEXT(". Did you mean: %s?"), *Resolution.Suggestions[0]);
		}
		Ctx.Report->AddError(TEXT("node"), DocNode.Id, Message, Resolution.Suggestions);
		return nullptr;
	}

	UBlueprintFunctionNodeSpawner* Spawner = UBlueprintFunctionNodeSpawner::Create(Resolution.Function);
	if (!Spawner)
	{
		Ctx.Report->AddError(TEXT("node"), DocNode.Id, FString::Printf(TEXT("could not create a spawner for '%s'"), *FunctionName));
		return nullptr;
	}
	const FVector2D SpawnLocation = DocNode.bHasPos ? DocNode.Pos : FVector2D::ZeroVector;
	UEdGraphNode* NewNode = Spawner->Invoke(Ctx.Graph, IBlueprintNodeBinder::FBindingSet(), SpawnLocation);
	if (!NewNode)
	{
		Ctx.Report->AddError(TEXT("node"), DocNode.Id, FString::Printf(TEXT("failed to spawn node for '%s'"), *FunctionName));
		return nullptr;
	}

	Ctx.Report->AddOk(TEXT("node"), DocNode.Id, FString::Printf(TEXT("%s::%s"), Resolution.OwnerClass ? *Resolution.OwnerClass->GetName() : TEXT("?"), *FunctionName));
	return NewNode;
}

UEdGraphNode* FN2CGraphImporter::CreateVariableNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode, bool bIsSet)
{
	const FString VarName = DocNode.GetString(TEXT("variable"));
	if (VarName.IsEmpty())
	{
		Ctx.Report->AddError(TEXT("node"), DocNode.Id, FString::Printf(TEXT("%s node missing 'variable' field"), bIsSet ? TEXT("variable_set") : TEXT("variable_get")));
		return nullptr;
	}
	const FString Scope = DocNode.GetString(TEXT("scope"), TEXT("self"));
	const FName VarFName(*VarName);
	UK2Node_Variable* VarNode = nullptr;

	if (Scope == TEXT("local") || Scope == TEXT("param"))
	{
		// Function parameters are stored the same way as local variables once compiled (§8.3 step 5)
		FBPVariableDescription* LocalVar = FBlueprintEditorUtils::FindLocalVariable(Ctx.Blueprint, Ctx.Graph, VarFName);
		if (!LocalVar)
		{
			Ctx.Report->AddError(TEXT("node"), DocNode.Id, FString::Printf(TEXT("%s '%s' not found in this function"), *Scope, *VarName));
			return nullptr;
		}
		if (bIsSet)
		{
			FGraphNodeCreator<UK2Node_VariableSet> Creator(*Ctx.Graph);
			UK2Node_VariableSet* Node = Creator.CreateNode();
			Node->VariableReference.SetLocalMember(VarFName, Ctx.Graph->GetName(), LocalVar->VarGuid);
			Creator.Finalize();
			VarNode = Node;
		}
		else
		{
			FGraphNodeCreator<UK2Node_VariableGet> Creator(*Ctx.Graph);
			UK2Node_VariableGet* Node = Creator.CreateNode();
			Node->VariableReference.SetLocalMember(VarFName, Ctx.Graph->GetName(), LocalVar->VarGuid);
			Creator.Finalize();
			VarNode = Node;
		}
	}
	else if (Scope == TEXT("external"))
	{
		const FString ClassHint = DocNode.GetString(TEXT("class"));
		UClass* OwnerClass = FN2CTypeStringConverter::ResolveClassByNameOrPath(ClassHint);
		if (!OwnerClass)
		{
			Ctx.Report->AddError(TEXT("node"), DocNode.Id, FString::Printf(TEXT("external variable '%s' needs a resolvable 'class' (got '%s')"), *VarName, *ClassHint));
			return nullptr;
		}
		if (!FindFProperty<FProperty>(OwnerClass, VarFName))
		{
			Ctx.Report->AddError(TEXT("node"), DocNode.Id, FString::Printf(TEXT("variable '%s' not found on %s"), *VarName, *OwnerClass->GetName()));
			return nullptr;
		}
		if (bIsSet)
		{
			FGraphNodeCreator<UK2Node_VariableSet> Creator(*Ctx.Graph);
			UK2Node_VariableSet* Node = Creator.CreateNode();
			Node->VariableReference.SetExternalMember(VarFName, OwnerClass);
			Creator.Finalize();
			VarNode = Node;
		}
		else
		{
			FGraphNodeCreator<UK2Node_VariableGet> Creator(*Ctx.Graph);
			UK2Node_VariableGet* Node = Creator.CreateNode();
			Node->VariableReference.SetExternalMember(VarFName, OwnerClass);
			Creator.Finalize();
			VarNode = Node;
		}
	}
	else // "self" (default)
	{
		UClass* SelfClass = Ctx.Blueprint->SkeletonGeneratedClass ? Ctx.Blueprint->SkeletonGeneratedClass.Get() : Ctx.Blueprint->ParentClass;
		if (!SelfClass || !FindFProperty<FProperty>(SelfClass, VarFName))
		{
			Ctx.Report->AddError(TEXT("node"), DocNode.Id, FString::Printf(TEXT("variable '%s' not found on this Blueprint"), *VarName));
			return nullptr;
		}
		if (bIsSet)
		{
			FGraphNodeCreator<UK2Node_VariableSet> Creator(*Ctx.Graph);
			UK2Node_VariableSet* Node = Creator.CreateNode();
			Node->VariableReference.SetSelfMember(VarFName);
			Creator.Finalize();
			VarNode = Node;
		}
		else
		{
			FGraphNodeCreator<UK2Node_VariableGet> Creator(*Ctx.Graph);
			UK2Node_VariableGet* Node = Creator.CreateNode();
			Node->VariableReference.SetSelfMember(VarFName);
			Creator.Finalize();
			VarNode = Node;
		}
	}

	Ctx.Report->AddOk(TEXT("node"), DocNode.Id, FString::Printf(TEXT("Variable %s %s (%s)"), bIsSet ? TEXT("Set") : TEXT("Get"), *VarName, *Scope));
	return VarNode;
}

UEdGraphNode* FN2CGraphImporter::CreateSelfNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode)
{
	FGraphNodeCreator<UK2Node_Self> Creator(*Ctx.Graph);
	UK2Node_Self* Node = Creator.CreateNode();
	Creator.Finalize();
	Ctx.Report->AddOk(TEXT("node"), DocNode.Id, TEXT("Self"));
	return Node;
}

UEdGraphNode* FN2CGraphImporter::CreateBranchNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode)
{
	FGraphNodeCreator<UK2Node_IfThenElse> Creator(*Ctx.Graph);
	UK2Node_IfThenElse* Node = Creator.CreateNode();
	Creator.Finalize();
	Ctx.Report->AddOk(TEXT("node"), DocNode.Id, TEXT("Branch"));
	return Node;
}

UEdGraphNode* FN2CGraphImporter::CreateSequenceNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode)
{
	const int32 OutputCount = FMath::Max(DocNode.GetInt(TEXT("outputs"), 2), 1);

	FGraphNodeCreator<UK2Node_ExecutionSequence> Creator(*Ctx.Graph);
	UK2Node_ExecutionSequence* Node = Creator.CreateNode();
	Creator.Finalize();

	auto CountOutputs = [&Node]()
	{
		int32 Count = 0;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == EGPD_Output)
			{
				++Count;
			}
		}
		return Count;
	};
	while (CountOutputs() < OutputCount)
	{
		Node->AddInputPin();
	}

	Ctx.Report->AddOk(TEXT("node"), DocNode.Id, FString::Printf(TEXT("Sequence (%d outputs)"), OutputCount));
	return Node;
}

UEdGraphNode* FN2CGraphImporter::CreateMacroNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode)
{
	const FString MacroName = DocNode.GetString(TEXT("macro"));
	if (MacroName.IsEmpty())
	{
		Ctx.Report->AddError(TEXT("node"), DocNode.Id, TEXT("macro node missing 'macro' field"));
		return nullptr;
	}
	const FString LibraryPath = DocNode.GetString(TEXT("library"), TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));

	UEdGraph* MacroGraph = ResolveMacroGraph(Ctx.Blueprint, MacroName, LibraryPath);
	if (!MacroGraph)
	{
		Ctx.Report->AddError(TEXT("node"), DocNode.Id, FString::Printf(TEXT("macro '%s' not found in '%s'"), *MacroName, *LibraryPath));
		return nullptr;
	}

	FGraphNodeCreator<UK2Node_MacroInstance> Creator(*Ctx.Graph);
	UK2Node_MacroInstance* Node = Creator.CreateNode();
	Node->SetMacroGraph(MacroGraph);
	Creator.Finalize();

	Ctx.Report->AddOk(TEXT("node"), DocNode.Id, FString::Printf(TEXT("Macro %s"), *MacroName));
	return Node;
}

UEdGraphNode* FN2CGraphImporter::CreateCastNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode)
{
	const FString ClassName = DocNode.GetString(TEXT("class"));
	if (ClassName.IsEmpty())
	{
		Ctx.Report->AddError(TEXT("node"), DocNode.Id, TEXT("cast node missing 'class' field"));
		return nullptr;
	}
	UClass* TargetClass = FN2CTypeStringConverter::ResolveClassByNameOrPath(ClassName);
	if (!TargetClass)
	{
		Ctx.Report->AddError(TEXT("node"), DocNode.Id, FString::Printf(TEXT("cast target class '%s' not found"), *ClassName));
		return nullptr;
	}

	FGraphNodeCreator<UK2Node_DynamicCast> Creator(*Ctx.Graph);
	UK2Node_DynamicCast* Node = Creator.CreateNode();
	Node->TargetType = TargetClass;
	Creator.Finalize();
	if (DocNode.bHasPure)
	{
		Node->SetPurity(DocNode.bPure);
	}

	Ctx.Report->AddOk(TEXT("node"), DocNode.Id, FString::Printf(TEXT("Cast to %s"), *TargetClass->GetName()));
	return Node;
}

UEdGraphNode* FN2CGraphImporter::CreateStructOpNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode, bool bIsBreak)
{
	const FString StructName = DocNode.GetString(TEXT("struct"));
	if (StructName.IsEmpty())
	{
		Ctx.Report->AddError(TEXT("node"), DocNode.Id, FString::Printf(TEXT("%s node missing 'struct' field"), bIsBreak ? TEXT("break_struct") : TEXT("make_struct")));
		return nullptr;
	}
	UScriptStruct* Struct = FN2CTypeStringConverter::ResolveStructByNameOrPath(StructName);
	if (!Struct)
	{
		Ctx.Report->AddError(TEXT("node"), DocNode.Id, FString::Printf(TEXT("struct '%s' not found"), *StructName));
		return nullptr;
	}

	UEdGraphNode* NewNode = nullptr;
	if (bIsBreak)
	{
		FGraphNodeCreator<UK2Node_BreakStruct> Creator(*Ctx.Graph);
		UK2Node_BreakStruct* Node = Creator.CreateNode();
		Node->StructType = Struct;
		Creator.Finalize();
		NewNode = Node;
	}
	else
	{
		FGraphNodeCreator<UK2Node_MakeStruct> Creator(*Ctx.Graph);
		UK2Node_MakeStruct* Node = Creator.CreateNode();
		Node->StructType = Struct;
		Creator.Finalize();
		NewNode = Node;
	}

	Ctx.Report->AddOk(TEXT("node"), DocNode.Id, FString::Printf(TEXT("%s %s"), bIsBreak ? TEXT("Break") : TEXT("Make"), *Struct->GetName()));
	return NewNode;
}

UEdGraphNode* FN2CGraphImporter::CreateSelectNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode)
{
	const int32 OptionCount = FMath::Max(DocNode.GetInt(TEXT("options"), 2), 2);

	FGraphNodeCreator<UK2Node_Select> Creator(*Ctx.Graph);
	UK2Node_Select* Node = Creator.CreateNode();
	Creator.Finalize();

	auto CountOptions = [&Node]()
	{
		int32 Count = 0;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == EGPD_Input && Pin->PinName.ToString().StartsWith(TEXT("Option ")))
			{
				++Count;
			}
		}
		return Count;
	};
	while (CountOptions() < OptionCount)
	{
		Node->AddInputPin();
	}

	Ctx.Report->AddOk(TEXT("node"), DocNode.Id, FString::Printf(TEXT("Select (%d options)"), OptionCount));
	return Node;
}

UEdGraphNode* FN2CGraphImporter::CreateSwitchIntNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode)
{
	TArray<int32> Cases = DocNode.GetIntArray(TEXT("cases"));
	if (Cases.Num() == 0)
	{
		Ctx.Report->AddError(TEXT("node"), DocNode.Id, TEXT("switch_int node has no 'cases'"));
		return nullptr;
	}
	Cases.Sort();
	const int32 StartValue = Cases[0];
	bool bContiguous = true;
	for (int32 i = 0; i < Cases.Num(); ++i)
	{
		if (Cases[i] != StartValue + i)
		{
			bContiguous = false;
			break;
		}
	}

	FGraphNodeCreator<UK2Node_SwitchInteger> Creator(*Ctx.Graph);
	UK2Node_SwitchInteger* Node = Creator.CreateNode();
	Node->StartIndex = StartValue;
	Node->bHasDefaultPin = DocNode.GetBool(TEXT("default_pin"), true);
	Creator.Finalize();

	auto CountCases = [&Node]()
	{
		int32 Count = 0;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin->PinName != TEXT("Default"))
			{
				++Count;
			}
		}
		return Count;
	};
	while (CountCases() < Cases.Num())
	{
		Node->AddPinToSwitchNode();
	}

	if (!bContiguous)
	{
		Ctx.Report->AddWarning(TEXT("node"), DocNode.Id,
			FString::Printf(TEXT("switch_int only supports a contiguous ascending range (engine limitation); created %d..%d instead of the exact case list"), StartValue, StartValue + Cases.Num() - 1));
	}
	else
	{
		Ctx.Report->AddOk(TEXT("node"), DocNode.Id, FString::Printf(TEXT("Switch on Int (%d cases)"), Cases.Num()));
	}
	return Node;
}

UEdGraphNode* FN2CGraphImporter::CreateSwitchEnumNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode)
{
	const FString EnumName = DocNode.GetString(TEXT("enum"));
	if (EnumName.IsEmpty())
	{
		Ctx.Report->AddError(TEXT("node"), DocNode.Id, TEXT("switch_enum node missing 'enum' field"));
		return nullptr;
	}
	UEnum* Enum = FN2CTypeStringConverter::ResolveEnumByNameOrPath(EnumName);
	if (!Enum)
	{
		Ctx.Report->AddError(TEXT("node"), DocNode.Id, FString::Printf(TEXT("enum '%s' not found"), *EnumName));
		return nullptr;
	}

	FGraphNodeCreator<UK2Node_SwitchEnum> Creator(*Ctx.Graph);
	UK2Node_SwitchEnum* Node = Creator.CreateNode();
	Node->Enum = Enum;
	Creator.Finalize();

	Ctx.Report->AddOk(TEXT("node"), DocNode.Id, FString::Printf(TEXT("Switch on %s"), *Enum->GetName()));
	return Node;
}

UEdGraphNode* FN2CGraphImporter::CreateSwitchStringOrNameNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode, bool bIsName)
{
	TArray<FString> Cases = DocNode.GetStringArray(TEXT("cases"));
	if (Cases.Num() == 0)
	{
		Ctx.Report->AddError(TEXT("node"), DocNode.Id, FString::Printf(TEXT("%s node has no 'cases'"), bIsName ? TEXT("switch_name") : TEXT("switch_string")));
		return nullptr;
	}

	UEdGraphNode* NewNode = nullptr;
	if (bIsName)
	{
		FGraphNodeCreator<UK2Node_SwitchName> Creator(*Ctx.Graph);
		UK2Node_SwitchName* Node = Creator.CreateNode();
		for (const FString& CaseValue : Cases)
		{
			Node->PinNames.Add(FName(*CaseValue));
		}
		Creator.Finalize();
		NewNode = Node;
	}
	else
	{
		FGraphNodeCreator<UK2Node_SwitchString> Creator(*Ctx.Graph);
		UK2Node_SwitchString* Node = Creator.CreateNode();
		for (const FString& CaseValue : Cases)
		{
			Node->PinNames.Add(FName(*CaseValue));
		}
		Creator.Finalize();
		NewNode = Node;
	}

	Ctx.Report->AddOk(TEXT("node"), DocNode.Id, FString::Printf(TEXT("Switch on %s (%d cases)"), bIsName ? TEXT("Name") : TEXT("String"), Cases.Num()));
	return NewNode;
}

UEdGraphNode* FN2CGraphImporter::CreateMakeArrayNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode)
{
	const int32 InputCount = FMath::Max(DocNode.GetInt(TEXT("inputs"), 1), 1);

	FGraphNodeCreator<UK2Node_MakeArray> Creator(*Ctx.Graph);
	UK2Node_MakeArray* Node = Creator.CreateNode();
	Node->NumInputs = InputCount;
	Creator.Finalize();

	auto CountInputs = [&Node]()
	{
		int32 Count = 0;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == EGPD_Input)
			{
				++Count;
			}
		}
		return Count;
	};
	while (CountInputs() < InputCount)
	{
		Node->AddInputPin();
	}

	Ctx.Report->AddOk(TEXT("node"), DocNode.Id, FString::Printf(TEXT("Make Array (%d inputs)"), InputCount));
	return Node;
}

UEdGraphNode* FN2CGraphImporter::CreateSpawnActorNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode)
{
	FGraphNodeCreator<UK2Node_SpawnActorFromClass> Creator(*Ctx.Graph);
	UK2Node_SpawnActorFromClass* Node = Creator.CreateNode();
	Creator.Finalize();

	const FString ClassName = DocNode.GetString(TEXT("class"));
	if (!ClassName.IsEmpty())
	{
		if (UClass* SpawnClass = FN2CTypeStringConverter::ResolveClassByNameOrPath(ClassName))
		{
			if (UEdGraphPin* ClassPin = Node->FindPin(TEXT("Class"), EGPD_Input))
			{
				Ctx.Schema->TrySetDefaultObject(*ClassPin, SpawnClass);
			}
		}
		else
		{
			Ctx.Report->AddWarning(TEXT("node"), DocNode.Id, FString::Printf(TEXT("spawn_actor class '%s' not found; Class pin left unset"), *ClassName));
		}
	}

	Ctx.Report->AddOk(TEXT("node"), DocNode.Id, TEXT("Spawn Actor from Class"));
	return Node;
}

UEdGraphNode* FN2CGraphImporter::CreateRerouteNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode)
{
	FGraphNodeCreator<UK2Node_Knot> Creator(*Ctx.Graph);
	UK2Node_Knot* Node = Creator.CreateNode();
	Creator.Finalize();
	Ctx.Report->AddOk(TEXT("node"), DocNode.Id, TEXT("Reroute"));
	return Node;
}

UEdGraphNode* FN2CGraphImporter::FindFunctionEntryNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode)
{
	for (UEdGraphNode* Node : Ctx.Graph->Nodes)
	{
		if (Node->IsA<UK2Node_FunctionEntry>())
		{
			Ctx.Report->AddOk(TEXT("node"), DocNode.Id, TEXT("Function Entry (existing node reused)"));
			return Node;
		}
	}
	Ctx.Report->AddError(TEXT("node"), DocNode.Id, TEXT("no function_entry node exists in this graph (function_entry is only ever reused, never created)"));
	return nullptr;
}

UEdGraphNode* FN2CGraphImporter::FindOrCreateFunctionResultNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode)
{
	for (UEdGraphNode* Node : Ctx.Graph->Nodes)
	{
		if (Node->IsA<UK2Node_FunctionResult>())
		{
			Ctx.Report->AddOk(TEXT("node"), DocNode.Id, TEXT("Function Result (existing node reused)"));
			return Node;
		}
	}

	FGraphNodeCreator<UK2Node_FunctionResult> Creator(*Ctx.Graph);
	UK2Node_FunctionResult* Node = Creator.CreateNode();
	Creator.Finalize();
	Ctx.Report->AddOk(TEXT("node"), DocNode.Id, TEXT("Function Result (created)"));
	return Node;
}

UEdGraphNode* FN2CGraphImporter::CreateK2NodeFallback(FImportContext& Ctx, const FN2CGraphDocNode& DocNode)
{
	const FString ClassPath = DocNode.GetString(TEXT("class"));
	UClass* NodeClass = nullptr;
	if (!ClassPath.IsEmpty())
	{
		NodeClass = FindObject<UClass>(ANY_PACKAGE, *ClassPath);
		if (!NodeClass)
		{
			NodeClass = LoadObject<UClass>(nullptr, *ClassPath);
		}
	}
	if (!NodeClass || !NodeClass->IsChildOf(UK2Node::StaticClass()))
	{
		Ctx.Report->AddError(TEXT("node"), DocNode.Id, FString::Printf(TEXT("k2node class '%s' not found or is not a UK2Node subclass"), *ClassPath));
		return nullptr;
	}

	UK2Node* Node = NewObject<UK2Node>(Ctx.Graph, NodeClass);
	Ctx.Graph->AddNode(Node, /*bUserAction=*/true, /*bSelectNewNode=*/false);
	Node->CreateNewGuid();
	Node->PostPlacedNewNode();

	const TSharedPtr<FJsonObject>* PropertiesJson = nullptr;
	if (DocNode.Raw.IsValid() && DocNode.Raw->TryGetObjectField(TEXT("properties"), PropertiesJson) && PropertiesJson && PropertiesJson->IsValid())
	{
		for (const auto& Pair : (*PropertiesJson)->Values)
		{
			FString ValueStr;
			if (!Pair.Value->TryGetString(ValueStr))
			{
				continue;
			}
			FProperty* Property = FindFProperty<FProperty>(NodeClass, FName(*Pair.Key));
			if (!Property)
			{
				Ctx.Report->AddWarning(TEXT("node"), DocNode.Id, FString::Printf(TEXT("k2node property '%s' not found on %s, ignored"), *Pair.Key, *NodeClass->GetName()));
				continue;
			}
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Node);
			Property->ImportText(*ValueStr, ValuePtr, PPF_None, Node);
		}
	}

	Node->AllocateDefaultPins();

	Ctx.Report->AddOk(TEXT("node"), DocNode.Id, FString::Printf(TEXT("%s (generic fallback)"), *NodeClass->GetName()));
	return Node;
}

UEdGraphNode* FN2CGraphImporter::CreateOrResolveNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode)
{
	const FString& Kind = DocNode.Kind;

	if (Kind == TEXT("event")) return CreateEventNode(Ctx, DocNode);
	if (Kind == TEXT("custom_event")) return CreateCustomEventNode(Ctx, DocNode);
	if (Kind == TEXT("call_function")) return CreateCallFunctionNode(Ctx, DocNode, false);
	if (Kind == TEXT("call_parent")) return CreateCallFunctionNode(Ctx, DocNode, true);
	if (Kind == TEXT("variable_get")) return CreateVariableNode(Ctx, DocNode, false);
	if (Kind == TEXT("variable_set")) return CreateVariableNode(Ctx, DocNode, true);
	if (Kind == TEXT("self")) return CreateSelfNode(Ctx, DocNode);
	if (Kind == TEXT("branch")) return CreateBranchNode(Ctx, DocNode);
	if (Kind == TEXT("sequence")) return CreateSequenceNode(Ctx, DocNode);
	if (Kind == TEXT("macro")) return CreateMacroNode(Ctx, DocNode);
	if (Kind == TEXT("cast")) return CreateCastNode(Ctx, DocNode);
	if (Kind == TEXT("make_struct")) return CreateStructOpNode(Ctx, DocNode, false);
	if (Kind == TEXT("break_struct")) return CreateStructOpNode(Ctx, DocNode, true);
	if (Kind == TEXT("select")) return CreateSelectNode(Ctx, DocNode);
	if (Kind == TEXT("switch_int")) return CreateSwitchIntNode(Ctx, DocNode);
	if (Kind == TEXT("switch_enum")) return CreateSwitchEnumNode(Ctx, DocNode);
	if (Kind == TEXT("switch_string")) return CreateSwitchStringOrNameNode(Ctx, DocNode, false);
	if (Kind == TEXT("switch_name")) return CreateSwitchStringOrNameNode(Ctx, DocNode, true);
	if (Kind == TEXT("make_array")) return CreateMakeArrayNode(Ctx, DocNode);
	if (Kind == TEXT("spawn_actor")) return CreateSpawnActorNode(Ctx, DocNode);
	if (Kind == TEXT("reroute")) return CreateRerouteNode(Ctx, DocNode);
	if (Kind == TEXT("function_entry")) return FindFunctionEntryNode(Ctx, DocNode);
	if (Kind == TEXT("function_result")) return FindOrCreateFunctionResultNode(Ctx, DocNode);
	if (Kind == TEXT("k2node")) return CreateK2NodeFallback(Ctx, DocNode);

	Ctx.Report->AddError(TEXT("node"), DocNode.Id, FString::Printf(TEXT("unrecognized node kind '%s'"), *Kind));
	return nullptr;
}

// ============================================================================
// Declarations (§7.6) - created only if missing, never modified
// ============================================================================

void FN2CGraphImporter::CreateDeclarations(UBlueprint* Blueprint, const FN2CDeclarations& Declarations, FN2CImportReport& Report)
{
	for (const FN2CVariableDecl& VarDecl : Declarations.Variables)
	{
		const FName VarFName(*VarDecl.Name);
		FEdGraphPinType PinType;
		FString TypeError;
		if (!FN2CTypeStringConverter::StringToPinType(VarDecl.Type, PinType, TypeError))
		{
			Report.AddError(TEXT("declaration"), VarDecl.Name, FString::Printf(TEXT("variable type error: %s"), *TypeError));
			continue;
		}

		const FBPVariableDescription* Existing = Blueprint->NewVariables.FindByPredicate(
			[&VarFName](const FBPVariableDescription& V) { return V.VarName == VarFName; });
		if (Existing)
		{
			if (Existing->VarType == PinType)
			{
				Report.AddOk(TEXT("declaration"), VarDecl.Name, TEXT("exists"));
			}
			else
			{
				Report.AddError(TEXT("declaration"), VarDecl.Name, TEXT("already exists with a different type; left unchanged"));
			}
			continue;
		}

		if (FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarFName, PinType, VarDecl.Default))
		{
			Report.AddOk(TEXT("declaration"), VarDecl.Name, TEXT("variable created"));
		}
		else
		{
			Report.AddError(TEXT("declaration"), VarDecl.Name, TEXT("failed to create variable"));
		}
	}

	for (const FN2CFunctionDecl& FuncDecl : Declarations.Functions)
	{
		UEdGraph* ExistingGraph = nullptr;
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetName() == FuncDecl.Name)
			{
				ExistingGraph = Graph;
				break;
			}
		}
		if (ExistingGraph)
		{
			Report.AddOk(TEXT("declaration"), FuncDecl.Name, TEXT("exists"));
			continue;
		}

		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*FuncDecl.Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewGraph, /*bIsUserCreated=*/true, nullptr);

		UK2Node_FunctionEntry* EntryNode = nullptr;
		UK2Node_FunctionResult* ResultNode = nullptr;
		for (UEdGraphNode* Node : NewGraph->Nodes)
		{
			if (!EntryNode) EntryNode = Cast<UK2Node_FunctionEntry>(Node);
			if (!ResultNode) ResultNode = Cast<UK2Node_FunctionResult>(Node);
		}

		// A brand-new function only gets an Entry node; a Result node only exists once something
		// needs it. Create one now if this function declares any outputs.
		if (!ResultNode && FuncDecl.Outputs.Num() > 0)
		{
			FGraphNodeCreator<UK2Node_FunctionResult> ResultCreator(*NewGraph);
			ResultNode = ResultCreator.CreateNode();
			ResultCreator.Finalize();
		}

		bool bAllParamsOk = true;
		if (EntryNode)
		{
			for (const FN2CParamDecl& Input : FuncDecl.Inputs)
			{
				FEdGraphPinType PinType;
				FString TypeError;
				if (FN2CTypeStringConverter::StringToPinType(Input.Type, PinType, TypeError))
				{
					EntryNode->CreateUserDefinedPin(FName(*Input.Name), PinType, EGPD_Output);
				}
				else
				{
					Report.AddWarning(TEXT("declaration"), FuncDecl.Name, FString::Printf(TEXT("input '%s': %s"), *Input.Name, *TypeError));
					bAllParamsOk = false;
				}
			}
		}
		if (ResultNode)
		{
			for (const FN2CParamDecl& Output : FuncDecl.Outputs)
			{
				FEdGraphPinType PinType;
				FString TypeError;
				if (FN2CTypeStringConverter::StringToPinType(Output.Type, PinType, TypeError))
				{
					ResultNode->CreateUserDefinedPin(FName(*Output.Name), PinType, EGPD_Input);
				}
				else
				{
					Report.AddWarning(TEXT("declaration"), FuncDecl.Name, FString::Printf(TEXT("output '%s': %s"), *Output.Name, *TypeError));
					bAllParamsOk = false;
				}
			}
		}

		Report.AddOk(TEXT("declaration"), FuncDecl.Name, bAllParamsOk ? TEXT("function created") : TEXT("function created (some parameters skipped)"));
	}

	for (const FN2CDispatcherDecl& DispatcherDecl : Declarations.Dispatchers)
	{
		bool bExists = false;
		for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
		{
			if (Graph && Graph->GetName() == DispatcherDecl.Name)
			{
				bExists = true;
				break;
			}
		}
		Report.AddWarning(TEXT("declaration"), DispatcherDecl.Name,
			bExists ? TEXT("exists") : TEXT("event dispatcher declarations are not yet supported by the importer"));
	}

	// custom_events: a custom_event NODE with a matching "name" creates or reuses the real node
	// directly (§7.4); there's nothing separate to pre-declare here.
}

// ============================================================================
// Links (§8.3 step 7)
// ============================================================================

void FN2CGraphImporter::CreateLinks(FImportContext& Ctx, const FN2CGraphDocGraph& DocGraph)
{
	struct FPendingLink
	{
		FString FromRef, ToRef;
		UEdGraphPin* FromPin = nullptr;
		UEdGraphPin* ToPin = nullptr;
		bool bResolved = false;
		bool bFailed = false;
	};

	auto SplitRef = [](const FString& Ref, FString& OutId, FString& OutPin) -> bool
	{
		int32 DotIndex = INDEX_NONE;
		if (!Ref.FindChar(TEXT('.'), DotIndex))
		{
			return false;
		}
		OutId = Ref.Left(DotIndex);
		OutPin = Ref.Mid(DotIndex + 1);
		return true;
	};

	auto ResolveEndpoint = [&](const FString& Ref, EEdGraphPinDirection ExpectedDirection, FString& OutError) -> UEdGraphPin*
	{
		FString NodeId, PinName;
		if (!SplitRef(Ref, NodeId, PinName))
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid 'id.pin' reference"), *Ref);
			return nullptr;
		}
		UEdGraphNode** NodePtr = Ctx.NodesById.Find(NodeId);
		if (!NodePtr || !*NodePtr)
		{
			OutError = FString::Printf(TEXT("node '%s' was not created (see its own error above)"), *NodeId);
			return nullptr;
		}
		const FN2CGraphDocNode* const* DocNodePtr = Ctx.DocNodesById.Find(NodeId);
		const FString DocKind = (DocNodePtr && *DocNodePtr) ? (*DocNodePtr)->Kind : FString();
		return ResolvePin(*NodePtr, DocKind, PinName, ExpectedDirection, OutError);
	};

	TArray<FPendingLink> Pending;
	for (const FN2CGraphDocLink& Link : DocGraph.Links)
	{
		FPendingLink P;
		P.FromRef = Link.FromRef;
		P.ToRef = Link.ToRef;
		Pending.Add(P);
	}

	// Resolve every endpoint up front, allowing from/to to be reversed (§7.3: "The importer swaps
	// reversed pairs and warns")
	for (FPendingLink& P : Pending)
	{
		FString FromError, ToError;
		P.FromPin = ResolveEndpoint(P.FromRef, EGPD_Output, FromError);
		P.ToPin = ResolveEndpoint(P.ToRef, EGPD_Input, ToError);

		if (!P.FromPin || !P.ToPin)
		{
			FString SwappedFromError, SwappedToError;
			UEdGraphPin* SwappedFrom = ResolveEndpoint(P.ToRef, EGPD_Output, SwappedFromError);
			UEdGraphPin* SwappedTo = ResolveEndpoint(P.FromRef, EGPD_Input, SwappedToError);
			if (SwappedFrom && SwappedTo)
			{
				P.FromPin = SwappedFrom;
				P.ToPin = SwappedTo;
				Ctx.Report->AddWarning(TEXT("link"), FString::Printf(TEXT("%s -> %s"), *P.FromRef, *P.ToRef), TEXT("endpoints were reversed; swapped automatically"));
			}
			else
			{
				Ctx.Report->AddError(TEXT("link"), FString::Printf(TEXT("%s -> %s"), *P.FromRef, *P.ToRef), !P.FromPin ? FromError : ToError);
				P.bFailed = true;
			}
		}
	}

	// Retry loop: a wildcard pin (ForEachLoop Array, Select, MakeArray, Knot) only resolves its
	// concrete type once something is actually connected, so keep retrying until a pass makes no
	// progress (§8.3 step 7).
	bool bProgress = true;
	int32 SafetyPasses = 0;
	while (bProgress && SafetyPasses < 10)
	{
		bProgress = false;
		++SafetyPasses;
		for (FPendingLink& P : Pending)
		{
			if (P.bResolved || P.bFailed || !P.FromPin || !P.ToPin)
			{
				continue;
			}
			const FPinConnectionResponse Response = Ctx.Schema->CanCreateConnection(P.FromPin, P.ToPin);
			if (Response.Response == CONNECT_RESPONSE_DISALLOW)
			{
				continue; // maybe a wildcard hasn't resolved yet; retry next pass
			}
			if (Ctx.Schema->TryCreateConnection(P.FromPin, P.ToPin))
			{
				P.bResolved = true;
				bProgress = true;
				if (Response.Response == CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE)
				{
					Ctx.Report->AddWarning(TEXT("link"), FString::Printf(TEXT("%s -> %s"), *P.FromRef, *P.ToRef), TEXT("connected via an automatic conversion node"));
				}
				else
				{
					Ctx.Report->AddOk(TEXT("link"), FString::Printf(TEXT("%s -> %s"), *P.FromRef, *P.ToRef), TEXT("connected"));
				}
			}
		}
	}

	for (FPendingLink& P : Pending)
	{
		if (!P.bResolved && !P.bFailed && P.FromPin && P.ToPin)
		{
			const FPinConnectionResponse Response = Ctx.Schema->CanCreateConnection(P.FromPin, P.ToPin);
			Ctx.Report->AddError(TEXT("link"), FString::Printf(TEXT("%s -> %s"), *P.FromRef, *P.ToRef), Response.Message.ToString());
		}
	}
}

// ============================================================================
// Defaults (§8.3 step 8, §7.8)
// ============================================================================

void FN2CGraphImporter::ApplyDefaults(FImportContext& Ctx, UEdGraphNode* Node, const FN2CGraphDocNode& DocNode)
{
	for (const auto& Pair : DocNode.Defaults)
	{
		FString PinError;
		UEdGraphPin* Pin = ResolvePin(Node, DocNode.Kind, Pair.Key, EGPD_Input, PinError);
		if (!Pin)
		{
			Ctx.Report->AddError(TEXT("default"), FString::Printf(TEXT("%s.%s"), *DocNode.Id, *Pair.Key), PinError);
			continue;
		}
		if (Pin->LinkedTo.Num() > 0)
		{
			continue; // a connected pin has no meaningful default to set
		}

		const FString RequestedValue = Pair.Value;
		const bool bIsObjectLike = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object ||
			Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class ||
			Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject ||
			Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass;

		if (bIsObjectLike && RequestedValue.StartsWith(TEXT("/")))
		{
			UObject* ResolvedObject = FN2CTypeStringConverter::ResolveClassByNameOrPath(RequestedValue);
			if (!ResolvedObject)
			{
				ResolvedObject = LoadObject<UObject>(nullptr, *RequestedValue);
			}
			if (ResolvedObject)
			{
				Ctx.Schema->TrySetDefaultObject(*Pin, ResolvedObject);
			}
			else
			{
				Ctx.Report->AddWarning(TEXT("default"), FString::Printf(TEXT("%s.%s"), *DocNode.Id, *Pair.Key),
					FString::Printf(TEXT("asset '%s' not found"), *RequestedValue));
			}
			continue;
		}

		Ctx.Schema->TrySetDefaultValue(*Pin, RequestedValue);

		const FString StoredValue = Pin->DefaultValue;
		if (StoredValue.IsEmpty() && !RequestedValue.IsEmpty())
		{
			Ctx.Report->AddWarning(TEXT("default"), FString::Printf(TEXT("%s.%s"), *DocNode.Id, *Pair.Key),
				FString::Printf(TEXT("value '%s' was rejected for this pin's type"), *RequestedValue));
		}
		else if (StoredValue != RequestedValue)
		{
			Ctx.Report->AddWarning(TEXT("default"), FString::Printf(TEXT("%s.%s"), *DocNode.Id, *Pair.Key),
				FString::Printf(TEXT("'%s' -> stored as '%s'"), *RequestedValue, *StoredValue));
		}
	}
}

// ============================================================================
// Layout (§8.3 step 9) and comments (§8.3 step 10)
// ============================================================================

void FN2CGraphImporter::LayoutNodes(FImportContext& Ctx, const FN2CGraphDocGraph& DocGraph, const TArray<UEdGraphNode*>& NewlyCreatedNodes)
{
	bool bAnyHasPos = false;
	FVector2D MinPos(TNumericLimits<float>::Max(), TNumericLimits<float>::Max());
	for (const FN2CGraphDocNode& DocNode : DocGraph.Nodes)
	{
		if (DocNode.bHasPos)
		{
			bAnyHasPos = true;
			MinPos.X = FMath::Min(MinPos.X, DocNode.Pos.X);
			MinPos.Y = FMath::Min(MinPos.Y, DocNode.Pos.Y);
		}
	}

	int32 AutoLayoutIndex = 0;
	for (const FN2CGraphDocNode& DocNode : DocGraph.Nodes)
	{
		UEdGraphNode** NodePtr = Ctx.NodesById.Find(DocNode.Id);
		if (!NodePtr || !*NodePtr || !NewlyCreatedNodes.Contains(*NodePtr))
		{
			continue; // don't move a reused (pre-existing) node
		}
		UEdGraphNode* Node = *NodePtr;

		if (DocNode.bHasPos && bAnyHasPos)
		{
			Node->NodePosX = Ctx.Options->Origin.X + (DocNode.Pos.X - MinPos.X);
			Node->NodePosY = Ctx.Options->Origin.Y + (DocNode.Pos.Y - MinPos.Y);
		}
		else
		{
			// No position data: a simple grid rather than the full longest-exec-path layering
			// algorithm (§8.3 step 9 describes the fuller version). The round-trip property (§9.1)
			// only requires matching nodes/links - "ids and positions may differ" - so this is
			// sufficient for correctness; it only needs to stop new nodes from stacking exactly on
			// top of each other.
			Node->NodePosX = Ctx.Options->Origin.X + (AutoLayoutIndex % 6) * 250;
			Node->NodePosY = Ctx.Options->Origin.Y + (AutoLayoutIndex / 6) * 150;
			++AutoLayoutIndex;
		}
	}
}

void FN2CGraphImporter::CreateComments(FImportContext& Ctx, const FN2CGraphDocGraph& DocGraph)
{
	for (const FN2CGraphDocComment& DocComment : DocGraph.Comments)
	{
		TArray<UEdGraphNode*> ContainedNodes;
		for (const FString& NodeId : DocComment.NodeIds)
		{
			if (UEdGraphNode** NodePtr = Ctx.NodesById.Find(NodeId))
			{
				if (*NodePtr)
				{
					ContainedNodes.Add(*NodePtr);
				}
			}
		}
		if (ContainedNodes.Num() == 0)
		{
			continue;
		}

		FGraphNodeCreator<UEdGraphNode_Comment> Creator(*Ctx.Graph);
		UEdGraphNode_Comment* CommentNode = Creator.CreateNode();
		CommentNode->NodeComment = DocComment.Text;
		if (DocComment.bHasColor)
		{
			CommentNode->CommentColor = DocComment.Color;
		}
		Creator.Finalize();

		const FIntRect Bounds = FEdGraphUtilities::CalculateApproximateNodeBoundaries(ContainedNodes);
		constexpr int32 Padding = 60;
		CommentNode->NodePosX = Bounds.Min.X - Padding;
		CommentNode->NodePosY = Bounds.Min.Y - Padding * 2; // extra room for the title bar
		CommentNode->NodeWidth = (Bounds.Max.X - Bounds.Min.X) + Padding * 2;
		CommentNode->NodeHeight = (Bounds.Max.Y - Bounds.Min.Y) + Padding * 3;

		for (UObject* Obj : ContainedNodes)
		{
			CommentNode->AddNodeUnderComment(Obj);
		}
	}
}

// ============================================================================
// Scratch graph (§8.5) - not used by P2's ValidateOnly (which cancels the whole transaction
// instead, see Import() below), kept ready for P3's Copy as nodes.
// ============================================================================

UEdGraph* FN2CGraphImporter::CreateScratchGraph(UBlueprint* Blueprint)
{
	UEdGraph* ScratchGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint,
		MakeUniqueObjectName(Blueprint, UEdGraph::StaticClass(), TEXT("N2CScratchGraph")),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass());
	// Deliberately NOT added to FunctionGraphs/UbergraphPages - it must never become part of the
	// Blueprint's real graph list, even transiently (§8.5).
	return ScratchGraph;
}

void FN2CGraphImporter::DestroyScratchGraph(UBlueprint* Blueprint, UEdGraph* ScratchGraph, bool bBlueprintWasDirty)
{
	if (!ScratchGraph)
	{
		return;
	}
	ScratchGraph->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	ScratchGraph->MarkPendingKill();

	if (Blueprint)
	{
		if (UPackage* Package = Blueprint->GetOutermost())
		{
			Package->SetDirtyFlag(bBlueprintWasDirty);
		}
	}
}

// ============================================================================
// Per-graph orchestration
// ============================================================================

void FN2CGraphImporter::ImportGraph(FImportContext& Ctx, const FN2CGraphDocGraph& DocGraph)
{
	TArray<UEdGraphNode*> NewlyCreatedNodes;

	for (const FN2CGraphDocNode& DocNode : DocGraph.Nodes)
	{
		if (DocNode.Kind == TEXT("local_variables"))
		{
			UK2Node_FunctionEntry* EntryNode = nullptr;
			for (UEdGraphNode* Node : Ctx.Graph->Nodes)
			{
				EntryNode = Cast<UK2Node_FunctionEntry>(Node);
				if (EntryNode)
				{
					break;
				}
			}
			if (!EntryNode)
			{
				Ctx.Report->AddError(TEXT("declaration"), DocNode.Id, TEXT("local_variables requires an existing function_entry node"));
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* VarsArray = nullptr;
			if (DocNode.Raw.IsValid() && DocNode.Raw->TryGetArrayField(TEXT("variables"), VarsArray) && VarsArray)
			{
				for (const TSharedPtr<FJsonValue>& VarValue : *VarsArray)
				{
					const TSharedPtr<FJsonObject> VarObj = VarValue.IsValid() ? VarValue->AsObject() : nullptr;
					if (!VarObj.IsValid())
					{
						continue;
					}
					FString VarName, VarType, VarDefault;
					VarObj->TryGetStringField(TEXT("name"), VarName);
					VarObj->TryGetStringField(TEXT("type"), VarType);
					VarObj->TryGetStringField(TEXT("default"), VarDefault);
					if (VarName.IsEmpty())
					{
						continue;
					}

					const FName VarFName(*VarName);
					const bool bExists = EntryNode->LocalVariables.ContainsByPredicate(
						[&VarFName](const FBPVariableDescription& V) { return V.VarName == VarFName; });
					if (bExists)
					{
						Ctx.Report->AddOk(TEXT("declaration"), VarName, TEXT("exists"));
						continue;
					}

					FEdGraphPinType PinType;
					FString TypeError;
					if (!FN2CTypeStringConverter::StringToPinType(VarType, PinType, TypeError))
					{
						Ctx.Report->AddError(TEXT("declaration"), VarName, FString::Printf(TEXT("local variable type error: %s"), *TypeError));
						continue;
					}

					FBPVariableDescription NewVar;
					NewVar.VarName = VarFName;
					NewVar.VarGuid = FGuid::NewGuid();
					NewVar.VarType = PinType;
					NewVar.FriendlyName = VarName;
					NewVar.DefaultValue = VarDefault;
					NewVar.Category = FText::FromString(TEXT("Default"));
					EntryNode->LocalVariables.Add(NewVar);
					Ctx.Report->AddOk(TEXT("declaration"), VarName, TEXT("local variable created"));
				}
			}
			continue;
		}

		UEdGraphNode* NewNode = CreateOrResolveNode(Ctx, DocNode);
		if (NewNode)
		{
			Ctx.NodesById.Add(DocNode.Id, NewNode);
			Ctx.DocNodesById.Add(DocNode.Id, &DocNode);
			NewlyCreatedNodes.Add(NewNode);
		}
	}

	// Defaults after every node exists, so pin resolution for aliases that depend on node kind works
	for (const FN2CGraphDocNode& DocNode : DocGraph.Nodes)
	{
		if (UEdGraphNode** NodePtr = Ctx.NodesById.Find(DocNode.Id))
		{
			ApplyDefaults(Ctx, *NodePtr, DocNode);
		}
	}

	CreateLinks(Ctx, DocGraph);
	LayoutNodes(Ctx, DocGraph, NewlyCreatedNodes);
	CreateComments(Ctx, DocGraph);
}

// ============================================================================
// Entry point
// ============================================================================

FN2CImportResult FN2CGraphImporter::Import(const FString& JsonText, UBlueprint* Blueprint, UEdGraph* TargetGraph, const FN2CImportOptions& Options)
{
	FN2CImportResult Result;

	if (!Blueprint)
	{
		Result.Report.AddError(TEXT("structure"), TEXT("<document>"), TEXT("no target Blueprint given"));
		return Result;
	}
	if (Options.Mode == EN2CImportMode::CopyAsNodes)
	{
		Result.Report.AddError(TEXT("structure"), TEXT("<document>"), TEXT("Copy as nodes is not implemented until P3 (docs §8.5)"));
		return Result;
	}

	FN2CGraphDocument Document;
	TArray<FString> StructuralIssues;
	if (!FN2CGraphDocumentParser::Parse(JsonText, Document, StructuralIssues))
	{
		for (const FString& Issue : StructuralIssues)
		{
			Result.Report.AddError(TEXT("structure"), TEXT("<document>"), Issue);
		}
		if (StructuralIssues.Num() == 0)
		{
			Result.Report.AddError(TEXT("structure"), TEXT("<document>"), TEXT("failed to parse document"));
		}
		return Result;
	}
	for (const FString& Issue : StructuralIssues)
	{
		Result.Report.AddWarning(TEXT("structure"), TEXT("<document>"), Issue);
	}

	Result.Report.ModeDescription = (Options.Mode == EN2CImportMode::ValidateOnly) ? TEXT("Validate") : TEXT("Insert");
	Result.Report.TargetDescription = FString::Printf(TEXT("%s / %s"), *Blueprint->GetName(),
		(Document.Graphs.Num() == 1 && TargetGraph) ? *TargetGraph->GetName() : TEXT("(matched by name)"));

	// One transaction for the whole import, so a single Ctrl+Z removes it entirely (§8.3 step 3,
	// §17). ValidateOnly cancels it at the end instead of letting it commit, so nothing is left
	// behind - including any function graphs newly created by "declarations" (§8.3 step 11).
	FScopedTransaction Transaction(NSLOCTEXT("NodeToCode", "N2CImport", "Import N2C Graph"));
	Blueprint->Modify();

	if (Options.bCreateDeclarations)
	{
		CreateDeclarations(Blueprint, Document.Declarations, Result.Report);

		// Compile just the skeleton so newly declared members resolve when creating nodes that
		// reference them below - the cheapest option that works (§8.3 step 4). This happens inside
		// the same transaction; if ValidateOnly cancels it, a later real compile (triggered by
		// anything else touching the Blueprint) regenerates the class fresh from the reverted state.
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	}

	for (const FN2CGraphDocGraph& DocGraph : Document.Graphs)
	{
		UEdGraph* GraphForThisEntry = (Document.Graphs.Num() == 1) ? TargetGraph : nullptr;
		if (!GraphForThisEntry)
		{
			for (UEdGraph* Candidate : Blueprint->FunctionGraphs)
			{
				if (Candidate && Candidate->GetName() == DocGraph.Name)
				{
					GraphForThisEntry = Candidate;
					break;
				}
			}
			if (!GraphForThisEntry)
			{
				for (UEdGraph* Candidate : Blueprint->UbergraphPages)
				{
					if (Candidate && Candidate->GetName() == DocGraph.Name)
					{
						GraphForThisEntry = Candidate;
						break;
					}
				}
			}
		}
		if (!GraphForThisEntry)
		{
			Result.Report.AddError(TEXT("structure"), DocGraph.Name.IsEmpty() ? TEXT("<graph>") : DocGraph.Name,
				TEXT("no matching graph in the target Blueprint"));
			continue;
		}

		GraphForThisEntry->Modify();

		FImportContext Ctx;
		Ctx.Blueprint = Blueprint;
		Ctx.Graph = GraphForThisEntry;
		Ctx.Schema = CastChecked<UEdGraphSchema_K2>(GraphForThisEntry->GetSchema());
		Ctx.Report = &Result.Report;
		Ctx.Options = &Options;

		ImportGraph(Ctx, DocGraph);

		for (const auto& Pair : Ctx.NodesById)
		{
			Result.CreatedNodes.AddUnique(Pair.Value);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	if (Options.Mode == EN2CImportMode::InsertIntoGraph && Options.bCompileAfter)
	{
		FCompilerResultsLog CompilerLog;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
		if (CompilerLog.NumErrors > 0 || CompilerLog.NumWarnings > 0)
		{
			Result.Report.AddWarning(TEXT("compile"), TEXT("<compile>"),
				FString::Printf(TEXT("%d error(s), %d warning(s)"), CompilerLog.NumErrors, CompilerLog.NumWarnings));
		}
		else
		{
			Result.Report.AddOk(TEXT("compile"), TEXT("<compile>"), TEXT("0 errors, 0 warnings"));
		}
	}

	if (Options.Mode == EN2CImportMode::ValidateOnly)
	{
		Transaction.Cancel();
		Result.CreatedNodes.Empty(); // nothing actually exists once the transaction is cancelled
	}

	// A note on bSelectImportedNodes: selecting nodes in a graph editor needs a live SGraphEditor
	// widget, which this importer has no dependency on. Left to the caller (the Import panel, P3).

	Result.bSuccess = !Result.Report.HasErrors();
	return Result;
}
