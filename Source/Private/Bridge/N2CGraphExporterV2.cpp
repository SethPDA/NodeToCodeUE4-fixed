// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#include "Bridge/N2CGraphExporterV2.h"

#include "Bridge/N2CTypeStringConverter.h"
#include "Utils/N2CLogger.h"

#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphNode_Comment.h"

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
#include "K2Node_ClassDynamicCast.h"
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

#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"

// ============================================================================
// Id generation
// ============================================================================

FString FN2CGraphExporterV2::ToSnakeCase(const FString& In)
{
	FString Result;
	Result.Reserve(In.Len() * 2);

	for (int32 i = 0; i < In.Len(); ++i)
	{
		const TCHAR C = In[i];
		if (FChar::IsAlnum(C))
		{
			const bool bPrevWasLowerOrDigit = i > 0 && (FChar::IsLower(In[i - 1]) || FChar::IsDigit(In[i - 1]));
			if (FChar::IsUpper(C) && Result.Len() > 0 && !Result.EndsWith(TEXT("_")) && bPrevWasLowerOrDigit)
			{
				Result.AppendChar(TEXT('_'));
			}
			Result.AppendChar(FChar::ToLower(C));
		}
		else if (Result.Len() > 0 && !Result.EndsWith(TEXT("_")))
		{
			Result.AppendChar(TEXT('_'));
		}
	}

	while (Result.EndsWith(TEXT("_")))
	{
		Result.LeftChopInline(1);
	}

	return Result.IsEmpty() ? TEXT("node") : Result;
}

FString FN2CGraphExporterV2::MakeIdBase(UK2Node* Node)
{
	if (UK2Node_CallFunction* FuncNode = Cast<UK2Node_CallFunction>(Node))
	{
		if (UFunction* Function = FuncNode->GetTargetFunction())
		{
			return ToSnakeCase(Function->GetName());
		}
		if (!FuncNode->FunctionReference.GetMemberName().IsNone())
		{
			return ToSnakeCase(FuncNode->FunctionReference.GetMemberName().ToString());
		}
	}
	else if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node)) // also covers UK2Node_CustomEvent
	{
		return ToSnakeCase(EventNode->GetFunctionName().ToString());
	}
	else if (UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node))
	{
		const FString Prefix = Node->IsA<UK2Node_VariableSet>() ? TEXT("set_") : TEXT("get_");
		return Prefix + ToSnakeCase(VarNode->GetVarNameString());
	}
	else if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
	{
		if (UEdGraph* MacroGraph = MacroNode->GetMacroGraph())
		{
			return ToSnakeCase(MacroGraph->GetName());
		}
	}
	else if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
	{
		if (CastNode->TargetType)
		{
			return TEXT("cast_") + ToSnakeCase(FN2CTypeStringConverter::GetClassShortName(CastNode->TargetType));
		}
	}
	else if (UK2Node_StructOperation* StructOpNode = Cast<UK2Node_StructOperation>(Node))
	{
		if (StructOpNode->StructType)
		{
			const FString Prefix = Node->IsA<UK2Node_BreakStruct>() ? TEXT("break_") : TEXT("make_");
			return Prefix + ToSnakeCase(StructOpNode->StructType->GetName());
		}
	}

	// Fall back to the node's class name with the "K2Node_" prefix stripped
	FString ClassName = Node->GetClass()->GetName();
	static const FString K2Prefix = TEXT("K2Node_");
	if (ClassName.StartsWith(K2Prefix))
	{
		ClassName.RightChopInline(K2Prefix.Len());
	}
	return ToSnakeCase(ClassName);
}

void FN2CGraphExporterV2::AssignNodeIds(FExportContext& Ctx)
{
	TMap<FString, int32> Counters;
	for (UK2Node* Node : Ctx.Nodes)
	{
		const FString Base = MakeIdBase(Node);
		int32& Counter = Counters.FindOrAdd(Base);
		++Counter;
		Ctx.NodeIds.Add(Node, FString::Printf(TEXT("%s_%d"), *Base, Counter));
	}
}

// ============================================================================
// Pin helpers
// ============================================================================

FString FN2CGraphExporterV2::GetPinRef(FExportContext& Ctx, const UEdGraphPin* Pin)
{
	if (!Pin || !Pin->GetOwningNode())
	{
		return FString();
	}
	UK2Node* OwningNode = Cast<UK2Node>(Pin->GetOwningNode());
	if (!OwningNode)
	{
		return FString();
	}
	const FString* Id = Ctx.NodeIds.Find(OwningNode);
	if (!Id)
	{
		return FString();
	}
	return FString::Printf(TEXT("%s.%s"), **Id, *Pin->PinName.ToString());
}

FString FN2CGraphExporterV2::DescribeExternalPin(const UEdGraphPin* Pin)
{
	if (!Pin || !Pin->GetOwningNode())
	{
		return TEXT("external:Unknown");
	}
	return FString::Printf(TEXT("external:%s.%s"),
		*Pin->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString(),
		*Pin->PinName.ToString());
}

FString FN2CGraphExporterV2::GetPinDefaultValueString(const UEdGraphPin* Pin)
{
	if (Pin->DefaultObject)
	{
		return Pin->DefaultObject->GetPathName();
	}
	if (!Pin->DefaultTextValue.IsEmpty())
	{
		return Pin->DefaultTextValue.ToString();
	}
	return Pin->DefaultValue;
}

void FN2CGraphExporterV2::FillDefaults(UK2Node* Node, const TSharedPtr<FJsonObject>& NodeJson)
{
	TSharedPtr<FJsonObject> Defaults = MakeShared<FJsonObject>();
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Input || Pin->bHidden)
		{
			continue;
		}
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			continue;
		}
		if (Pin->LinkedTo.Num() > 0)
		{
			continue;
		}
		if (Pin->DoesDefaultValueMatchAutogenerated())
		{
			continue;
		}
		const FString Value = GetPinDefaultValueString(Pin);
		if (!Value.IsEmpty())
		{
			Defaults->SetStringField(Pin->PinName.ToString(), Value);
		}
	}
	if (Defaults->Values.Num() > 0)
	{
		NodeJson->SetObjectField(TEXT("defaults"), Defaults);
	}
}

// ============================================================================
// Symbol table (docs §12.2.3)
// ============================================================================

void FN2CGraphExporterV2::RecordFunctionSymbol(FExportContext& Ctx, UK2Node_CallFunction* FuncNode)
{
	UFunction* Function = FuncNode->GetTargetFunction();
	if (!Function)
	{
		return;
	}
	UClass* OwnerClass = Function->GetOwnerClass();
	if (!OwnerClass)
	{
		return;
	}

	const FString Key = FString::Printf(TEXT("%s:%s"), *OwnerClass->GetPathName(), *Function->GetName());
	if (Ctx.FunctionSymbols->HasField(Key))
	{
		return;
	}

	TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("cpp_owner"), OwnerClass->GetPrefixCPP() + OwnerClass->GetName());
	const FString IncludePath = OwnerClass->GetMetaData(TEXT("IncludePath"));
	if (!IncludePath.IsEmpty())
	{
		Entry->SetStringField(TEXT("include"), IncludePath);
	}
	Entry->SetBoolField(TEXT("static"), Function->HasAnyFunctionFlags(FUNC_Static));
	Entry->SetBoolField(TEXT("pure"), Function->HasAnyFunctionFlags(FUNC_BlueprintPure));

	TArray<TSharedPtr<FJsonValue>> Params;
	FString ReturnType;
	for (TFieldIterator<FProperty> PropIt(Function); PropIt && PropIt->HasAnyPropertyFlags(CPF_Parm); ++PropIt)
	{
		FProperty* Param = *PropIt;
		const FString CppType = Param->GetCPPType();

		if (Param->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			ReturnType = CppType;
			continue;
		}

		// A non-const out/reference param is a genuine output; a const reference is still
		// logically an input (Blueprint treats const-ref params as normal inputs)
		const FString Dir = (Param->HasAnyPropertyFlags(CPF_OutParm) && !Param->HasAnyPropertyFlags(CPF_ConstParm))
			? TEXT("out") : TEXT("in");

		TSharedPtr<FJsonObject> ParamEntry = MakeShared<FJsonObject>();
		ParamEntry->SetStringField(TEXT("name"), Param->GetName());
		ParamEntry->SetStringField(TEXT("cpp_type"), CppType);
		ParamEntry->SetStringField(TEXT("dir"), Dir);
		Params.Add(MakeShared<FJsonValueObject>(ParamEntry));
	}
	Entry->SetArrayField(TEXT("params"), Params);
	if (!ReturnType.IsEmpty())
	{
		Entry->SetStringField(TEXT("return"), ReturnType);
	}
	const FString WorldContext = Function->GetMetaData(TEXT("WorldContext"));
	if (!WorldContext.IsEmpty())
	{
		Entry->SetStringField(TEXT("world_context"), WorldContext);
	}

	Ctx.FunctionSymbols->SetObjectField(Key, Entry);
}

void FN2CGraphExporterV2::RecordVariableSymbol(FExportContext& Ctx, UK2Node_Variable* VarNode)
{
	FProperty* Property = VarNode->GetPropertyForVariable();
	if (!Property)
	{
		return;
	}

	const FMemberReference& VarRef = VarNode->VariableReference;
	FString Scope = TEXT("self");
	if (Property->HasAnyPropertyFlags(CPF_Parm))
	{
		Scope = TEXT("param");
	}
	else if (VarRef.IsLocalScope())
	{
		Scope = TEXT("local");
	}
	else if (!VarRef.IsSelfContext())
	{
		if (UClass* OwnerClass = VarRef.GetMemberParentClass())
		{
			Scope = OwnerClass->GetPathName();
		}
	}

	const FString Key = FString::Printf(TEXT("%s:%s"), *Scope, *VarNode->GetVarNameString());
	if (Ctx.VariableSymbols->HasField(Key))
	{
		return;
	}

	TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
	Ctx.VariableSymbols->SetObjectField(Key, Entry);
}

void FN2CGraphExporterV2::RecordStructSymbol(FExportContext& Ctx, const UScriptStruct* Struct)
{
	if (!Struct)
	{
		return;
	}
	const FString Key = Struct->GetPathName();
	if (Ctx.StructSymbols->HasField(Key))
	{
		return;
	}

	TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("cpp_type"), Struct->GetStructCPPName());
	const FString IncludePath = Struct->GetMetaData(TEXT("IncludePath"));
	if (!IncludePath.IsEmpty())
	{
		Entry->SetStringField(TEXT("include"), IncludePath);
	}
	Ctx.StructSymbols->SetObjectField(Key, Entry);
}

// ============================================================================
// Generic fallback for nodes without a friendly v2 kind (§7.4 "k2node")
// ============================================================================

void FN2CGraphExporterV2::FillK2NodeFallback(UK2Node* Node, const TSharedPtr<FJsonObject>& NodeJson)
{
	NodeJson->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> PropIt(Node->GetClass(), EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;
		if (!Property || Property->HasAnyPropertyFlags(CPF_Transient) || Property->HasMetaData(TEXT("DeprecatedProperty")))
		{
			continue;
		}

		FString ValueStr;
		// Delta = nullptr so the value always exports rather than diffing against itself
		if (Property->ExportText_InContainer(0, ValueStr, Node, nullptr, Node, PPF_None) && !ValueStr.IsEmpty())
		{
			Properties->SetStringField(Property->GetName(), ValueStr);
		}
	}
	if (Properties->Values.Num() > 0)
	{
		NodeJson->SetObjectField(TEXT("properties"), Properties);
	}
}

// ============================================================================
// Per-node kind dispatch (§7.4)
// ============================================================================

TSharedPtr<FJsonObject> FN2CGraphExporterV2::BuildNodeJson(FExportContext& Ctx, UK2Node* Node)
{
	TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
	NodeJson->SetStringField(TEXT("id"), Ctx.NodeIds.FindChecked(Node));

	if (Node->NodePosX != 0 || Node->NodePosY != 0)
	{
		TArray<TSharedPtr<FJsonValue>> Pos;
		Pos.Add(MakeShared<FJsonValueNumber>(Node->NodePosX));
		Pos.Add(MakeShared<FJsonValueNumber>(Node->NodePosY));
		NodeJson->SetArrayField(TEXT("pos"), Pos);
	}
	if (!Node->NodeComment.IsEmpty())
	{
		NodeJson->SetStringField(TEXT("comment"), Node->NodeComment);
	}

	if (Node->IsA<UK2Node_Knot>())
	{
		NodeJson->SetStringField(TEXT("kind"), TEXT("reroute"));
	}
	else if (Node->IsA<UK2Node_FunctionEntry>())
	{
		NodeJson->SetStringField(TEXT("kind"), TEXT("function_entry"));
	}
	else if (Node->IsA<UK2Node_FunctionResult>())
	{
		NodeJson->SetStringField(TEXT("kind"), TEXT("function_result"));
	}
	else if (UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
	{
		NodeJson->SetStringField(TEXT("kind"), TEXT("custom_event"));
		NodeJson->SetStringField(TEXT("name"), CustomEventNode->GetFunctionName().ToString());
	}
	else if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
	{
		NodeJson->SetStringField(TEXT("kind"), TEXT("event"));
		NodeJson->SetStringField(TEXT("event"), EventNode->GetFunctionName().ToString());
		if (UClass* OwnerClass = EventNode->EventReference.GetMemberParentClass())
		{
			NodeJson->SetStringField(TEXT("class"), FN2CTypeStringConverter::GetClassShortName(OwnerClass));
		}
	}
	else if (UK2Node_CallParentFunction* ParentCallNode = Cast<UK2Node_CallParentFunction>(Node))
	{
		NodeJson->SetStringField(TEXT("kind"), TEXT("call_parent"));
		if (UFunction* Function = ParentCallNode->GetTargetFunction())
		{
			NodeJson->SetStringField(TEXT("function"), Function->GetName());
		}
	}
	else if (UK2Node_CallFunction* FuncNode = Cast<UK2Node_CallFunction>(Node))
	{
		NodeJson->SetStringField(TEXT("kind"), TEXT("call_function"));
		if (UFunction* Function = FuncNode->GetTargetFunction())
		{
			NodeJson->SetStringField(TEXT("function"), Function->GetName());
			if (UClass* OwnerClass = Function->GetOwnerClass())
			{
				NodeJson->SetStringField(TEXT("class"), OwnerClass->GetPathName());
			}
			NodeJson->SetBoolField(TEXT("pure"), FuncNode->IsNodePure());
			RecordFunctionSymbol(Ctx, FuncNode);
		}
	}
	else if (UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node))
	{
		const bool bIsSet = Node->IsA<UK2Node_VariableSet>();
		NodeJson->SetStringField(TEXT("kind"), bIsSet ? TEXT("variable_set") : TEXT("variable_get"));
		NodeJson->SetStringField(TEXT("variable"), VarNode->GetVarNameString());

		const FMemberReference& VarRef = VarNode->VariableReference;
		FProperty* VarProperty = VarNode->GetPropertyForVariable();
		if (VarProperty && VarProperty->HasAnyPropertyFlags(CPF_Parm))
		{
			NodeJson->SetStringField(TEXT("scope"), TEXT("param"));
		}
		else if (VarRef.IsLocalScope())
		{
			NodeJson->SetStringField(TEXT("scope"), TEXT("local"));
		}
		else if (!VarRef.IsSelfContext())
		{
			NodeJson->SetStringField(TEXT("scope"), TEXT("external"));
			if (UClass* OwnerClass = VarRef.GetMemberParentClass())
			{
				NodeJson->SetStringField(TEXT("class"), OwnerClass->GetPathName());
			}
		}
		// "self" is the default scope and is omitted (§7.4)
		RecordVariableSymbol(Ctx, VarNode);
	}
	else if (Node->IsA<UK2Node_Self>())
	{
		NodeJson->SetStringField(TEXT("kind"), TEXT("self"));
	}
	else if (Node->IsA<UK2Node_IfThenElse>())
	{
		NodeJson->SetStringField(TEXT("kind"), TEXT("branch"));
	}
	else if (Node->IsA<UK2Node_ExecutionSequence>())
	{
		NodeJson->SetStringField(TEXT("kind"), TEXT("sequence"));
		int32 OutputCount = 0;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				++OutputCount;
			}
		}
		NodeJson->SetNumberField(TEXT("outputs"), OutputCount);
	}
	else if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
	{
		NodeJson->SetStringField(TEXT("kind"), TEXT("macro"));
		if (UEdGraph* MacroGraph = MacroNode->GetMacroGraph())
		{
			NodeJson->SetStringField(TEXT("macro"), MacroGraph->GetName());
			if (UBlueprint* MacroBlueprint = FBlueprintEditorUtils::FindBlueprintForGraph(MacroGraph))
			{
				NodeJson->SetStringField(TEXT("library"), MacroBlueprint->GetPathName());
			}
		}
	}
	else if (UK2Node_ClassDynamicCast* ClassCastNode = Cast<UK2Node_ClassDynamicCast>(Node))
	{
		NodeJson->SetStringField(TEXT("kind"), TEXT("cast"));
		if (ClassCastNode->TargetType)
		{
			NodeJson->SetStringField(TEXT("class"), FN2CTypeStringConverter::GetClassShortName(ClassCastNode->TargetType));
		}
		NodeJson->SetBoolField(TEXT("pure"), ClassCastNode->IsNodePure());
	}
	else if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
	{
		NodeJson->SetStringField(TEXT("kind"), TEXT("cast"));
		if (CastNode->TargetType)
		{
			NodeJson->SetStringField(TEXT("class"), FN2CTypeStringConverter::GetClassShortName(CastNode->TargetType));
		}
		NodeJson->SetBoolField(TEXT("pure"), CastNode->IsNodePure());
	}
	else if (Node->IsA<UK2Node_BreakStruct>() || Node->IsA<UK2Node_MakeStruct>())
	{
		UK2Node_StructOperation* StructOpNode = CastChecked<UK2Node_StructOperation>(Node);
		NodeJson->SetStringField(TEXT("kind"), Node->IsA<UK2Node_BreakStruct>() ? TEXT("break_struct") : TEXT("make_struct"));
		if (StructOpNode->StructType)
		{
			NodeJson->SetStringField(TEXT("struct"), StructOpNode->StructType->GetName());
			RecordStructSymbol(Ctx, StructOpNode->StructType);
		}
	}
	else if (Node->IsA<UK2Node_Select>())
	{
		NodeJson->SetStringField(TEXT("kind"), TEXT("select"));
		int32 OptionCount = 0;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == EGPD_Input && Pin->PinName.ToString().StartsWith(TEXT("Option ")))
			{
				++OptionCount;
			}
		}
		NodeJson->SetNumberField(TEXT("options"), OptionCount);
	}
	else if (UK2Node_SwitchEnum* SwitchEnumNode = Cast<UK2Node_SwitchEnum>(Node))
	{
		NodeJson->SetStringField(TEXT("kind"), TEXT("switch_enum"));
		if (UEnum* Enum = SwitchEnumNode->GetEnum())
		{
			NodeJson->SetStringField(TEXT("enum"), Enum->GetName());
		}
	}
	else if (UK2Node_SwitchInteger* SwitchIntNode = Cast<UK2Node_SwitchInteger>(Node))
	{
		NodeJson->SetStringField(TEXT("kind"), TEXT("switch_int"));
		TArray<TSharedPtr<FJsonValue>> Cases;
		bool bHasDefault = false;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}
			const FString PinNameStr = Pin->PinName.ToString();
			if (PinNameStr == TEXT("Default"))
			{
				bHasDefault = true;
				continue;
			}
			Cases.Add(MakeShared<FJsonValueNumber>(FCString::Atoi(*PinNameStr)));
		}
		NodeJson->SetArrayField(TEXT("cases"), Cases);
		NodeJson->SetBoolField(TEXT("default_pin"), bHasDefault);
	}
	else if (Node->IsA<UK2Node_SwitchString>() || Node->IsA<UK2Node_SwitchName>())
	{
		NodeJson->SetStringField(TEXT("kind"), Node->IsA<UK2Node_SwitchName>() ? TEXT("switch_name") : TEXT("switch_string"));
		TArray<TSharedPtr<FJsonValue>> Cases;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin->PinName != TEXT("Default"))
			{
				Cases.Add(MakeShared<FJsonValueString>(Pin->PinName.ToString()));
			}
		}
		NodeJson->SetArrayField(TEXT("cases"), Cases);
	}
	else if (Node->IsA<UK2Node_MakeArray>())
	{
		NodeJson->SetStringField(TEXT("kind"), TEXT("make_array"));
		int32 InputCount = 0;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				++InputCount;
			}
		}
		NodeJson->SetNumberField(TEXT("inputs"), InputCount);
	}
	else if (Node->IsA<UK2Node_SpawnActorFromClass>())
	{
		NodeJson->SetStringField(TEXT("kind"), TEXT("spawn_actor"));
	}
	else
	{
		NodeJson->SetStringField(TEXT("kind"), TEXT("k2node"));
		FillK2NodeFallback(Node, NodeJson);
	}

	FillDefaults(Node, NodeJson);

	return NodeJson;
}

// ============================================================================
// Links, types and comments
// ============================================================================

void FN2CGraphExporterV2::BuildLinks(FExportContext& Ctx, const TSharedPtr<FJsonObject>& GraphJson)
{
	TArray<TSharedPtr<FJsonValue>> Links;
	TArray<TSharedPtr<FJsonValue>> ExternalLinks;
	TSharedPtr<FJsonObject> Types = MakeShared<FJsonObject>();

	TSet<UK2Node*> ExportSet(Ctx.Nodes);

	for (UK2Node* Node : Ctx.Nodes)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}

			const bool bIsExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
			const FString PinRef = GetPinRef(Ctx, Pin);
			if (!bIsExec && !PinRef.IsEmpty() && !Types->HasField(PinRef))
			{
				Types->SetStringField(PinRef, FN2CTypeStringConverter::PinTypeToString(Pin->PinType));
			}

			if (Pin->Direction != EGPD_Output)
			{
				// Fan-in from a node outside the export set still needs recording as external
				if (Pin->Direction == EGPD_Input)
				{
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (!LinkedPin || !LinkedPin->GetOwningNode())
						{
							continue;
						}
						UK2Node* SourceNode = Cast<UK2Node>(LinkedPin->GetOwningNode());
						if (!SourceNode || !ExportSet.Contains(SourceNode))
						{
							TSharedPtr<FJsonObject> ExtEntry = MakeShared<FJsonObject>();
							ExtEntry->SetStringField(TEXT("from"), DescribeExternalPin(LinkedPin));
							ExtEntry->SetStringField(TEXT("to"), PinRef);
							ExternalLinks.Add(MakeShared<FJsonValueObject>(ExtEntry));
						}
					}
				}
				continue;
			}

			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin || !LinkedPin->GetOwningNode())
				{
					continue;
				}
				UK2Node* TargetNode = Cast<UK2Node>(LinkedPin->GetOwningNode());
				if (TargetNode && ExportSet.Contains(TargetNode))
				{
					TArray<TSharedPtr<FJsonValue>> Pair;
					Pair.Add(MakeShared<FJsonValueString>(PinRef));
					Pair.Add(MakeShared<FJsonValueString>(GetPinRef(Ctx, LinkedPin)));
					Links.Add(MakeShared<FJsonValueArray>(Pair));
				}
				else
				{
					TSharedPtr<FJsonObject> ExtEntry = MakeShared<FJsonObject>();
					ExtEntry->SetStringField(TEXT("from"), PinRef);
					ExtEntry->SetStringField(TEXT("to"), DescribeExternalPin(LinkedPin));
					ExternalLinks.Add(MakeShared<FJsonValueObject>(ExtEntry));
				}
			}
		}
	}

	GraphJson->SetArrayField(TEXT("links"), Links);
	if (ExternalLinks.Num() > 0)
	{
		GraphJson->SetArrayField(TEXT("external_links"), ExternalLinks);
	}
	if (Types->Values.Num() > 0)
	{
		GraphJson->SetObjectField(TEXT("types"), Types);
	}
}

void FN2CGraphExporterV2::BuildComments(FExportContext& Ctx, const TSharedPtr<FJsonObject>& GraphJson)
{
	if (!Ctx.Graph)
	{
		return;
	}

	TArray<TSharedPtr<FJsonValue>> Comments;
	for (UEdGraphNode* GraphNode : Ctx.Graph->Nodes)
	{
		UEdGraphNode_Comment* CommentNode = Cast<UEdGraphNode_Comment>(GraphNode);
		if (!CommentNode)
		{
			continue;
		}

		TArray<TSharedPtr<FJsonValue>> ContainedIds;
		for (UObject* Obj : CommentNode->GetNodesUnderComment())
		{
			if (UK2Node* K2 = Cast<UK2Node>(Obj))
			{
				if (const FString* Id = Ctx.NodeIds.Find(K2))
				{
					ContainedIds.Add(MakeShared<FJsonValueString>(*Id));
				}
			}
		}
		if (ContainedIds.Num() == 0)
		{
			continue; // only comment boxes that contain at least one exported node (§9.1)
		}

		TSharedPtr<FJsonObject> CommentJson = MakeShared<FJsonObject>();
		CommentJson->SetStringField(TEXT("text"), CommentNode->NodeComment);
		CommentJson->SetArrayField(TEXT("nodes"), ContainedIds);

		TArray<TSharedPtr<FJsonValue>> Color;
		Color.Add(MakeShared<FJsonValueNumber>(CommentNode->CommentColor.R));
		Color.Add(MakeShared<FJsonValueNumber>(CommentNode->CommentColor.G));
		Color.Add(MakeShared<FJsonValueNumber>(CommentNode->CommentColor.B));
		Color.Add(MakeShared<FJsonValueNumber>(CommentNode->CommentColor.A));
		CommentJson->SetArrayField(TEXT("color"), Color);

		Comments.Add(MakeShared<FJsonValueObject>(CommentJson));
	}

	if (Comments.Num() > 0)
	{
		GraphJson->SetArrayField(TEXT("comments"), Comments);
	}
}

// ============================================================================
// Entry point
// ============================================================================

FString FN2CGraphExporterV2::ExportGraph(UEdGraph* Graph, const TSet<UEdGraphNode*>& SelectedNodes, bool bPrettyPrint)
{
	if (!Graph)
	{
		FN2CLogger::Get().LogError(TEXT("FN2CGraphExporterV2::ExportGraph called with a null graph"));
		return FString();
	}

	FExportContext Ctx;
	Ctx.Graph = Graph;
	Ctx.Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	Ctx.FunctionSymbols = MakeShared<FJsonObject>();
	Ctx.VariableSymbols = MakeShared<FJsonObject>();
	Ctx.StructSymbols = MakeShared<FJsonObject>();

	// Scope: the selection if non-empty, else the whole graph (§9.1)
	for (UEdGraphNode* GraphNode : Graph->Nodes)
	{
		UK2Node* K2Node = Cast<UK2Node>(GraphNode);
		if (!K2Node)
		{
			continue; // comment boxes are handled separately in BuildComments
		}
		if (SelectedNodes.Num() > 0 && !SelectedNodes.Contains(GraphNode))
		{
			continue;
		}
		Ctx.Nodes.Add(K2Node);
	}

	if (Ctx.Nodes.Num() == 0)
	{
		FN2CLogger::Get().LogWarning(TEXT("FN2CGraphExporterV2::ExportGraph: nothing to export"));
		return FString();
	}

	AssignNodeIds(Ctx);

	TSharedPtr<FJsonObject> GraphJson = MakeShared<FJsonObject>();
	GraphJson->SetStringField(TEXT("name"), Graph->GetName());

	// Defaults to event_graph unless a function_entry node is present (§7.3)
	FString GraphKind = TEXT("event_graph");
	for (UK2Node* Node : Ctx.Nodes)
	{
		if (Node->IsA<UK2Node_FunctionEntry>())
		{
			GraphKind = TEXT("function");
			break;
		}
	}
	GraphJson->SetStringField(TEXT("kind"), GraphKind);

	TArray<TSharedPtr<FJsonValue>> NodesJson;
	for (UK2Node* Node : Ctx.Nodes)
	{
		NodesJson.Add(MakeShared<FJsonValueObject>(BuildNodeJson(Ctx, Node)));

		// Function-graph local variables round-trip as a synthetic pseudo-node (§7.4 "local_variables")
		if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
		{
			if (EntryNode->LocalVariables.Num() > 0)
			{
				TSharedPtr<FJsonObject> LocalVarsNode = MakeShared<FJsonObject>();
				LocalVarsNode->SetStringField(TEXT("id"), TEXT("local_variables"));
				LocalVarsNode->SetStringField(TEXT("kind"), TEXT("local_variables"));

				TArray<TSharedPtr<FJsonValue>> VarsArray;
				for (const FBPVariableDescription& LocalVar : EntryNode->LocalVariables)
				{
					TSharedPtr<FJsonObject> VarJson = MakeShared<FJsonObject>();
					VarJson->SetStringField(TEXT("name"), LocalVar.VarName.ToString());
					VarJson->SetStringField(TEXT("type"), FN2CTypeStringConverter::PinTypeToString(LocalVar.VarType));
					if (!LocalVar.DefaultValue.IsEmpty())
					{
						VarJson->SetStringField(TEXT("default"), LocalVar.DefaultValue);
					}
					VarsArray.Add(MakeShared<FJsonValueObject>(VarJson));
				}
				LocalVarsNode->SetArrayField(TEXT("variables"), VarsArray);
				NodesJson.Add(MakeShared<FJsonValueObject>(LocalVarsNode));
			}
		}
	}
	GraphJson->SetArrayField(TEXT("nodes"), NodesJson);

	BuildLinks(Ctx, GraphJson);
	BuildComments(Ctx, GraphJson);

	TSharedPtr<FJsonObject> RootJson = MakeShared<FJsonObject>();
	RootJson->SetStringField(TEXT("format"), TEXT("n2c.graph"));
	RootJson->SetNumberField(TEXT("version"), 2);
	RootJson->SetStringField(TEXT("engine"), TEXT("4.27"));
	if (Ctx.Blueprint)
	{
		RootJson->SetStringField(TEXT("blueprint"), Ctx.Blueprint->GetOutermost()->GetName());
	}

	TArray<TSharedPtr<FJsonValue>> GraphsArray;
	GraphsArray.Add(MakeShared<FJsonValueObject>(GraphJson));
	RootJson->SetArrayField(TEXT("graphs"), GraphsArray);

	TSharedPtr<FJsonObject> Symbols = MakeShared<FJsonObject>();
	if (Ctx.FunctionSymbols->Values.Num() > 0)
	{
		Symbols->SetObjectField(TEXT("functions"), Ctx.FunctionSymbols);
	}
	if (Ctx.VariableSymbols->Values.Num() > 0)
	{
		Symbols->SetObjectField(TEXT("variables"), Ctx.VariableSymbols);
	}
	if (Ctx.StructSymbols->Values.Num() > 0)
	{
		Symbols->SetObjectField(TEXT("structs"), Ctx.StructSymbols);
	}
	if (Symbols->Values.Num() > 0)
	{
		RootJson->SetObjectField(TEXT("symbols"), Symbols);
	}

	FString OutputString;
	if (bPrettyPrint)
	{
		TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutputString, 1);
		FJsonSerializer::Serialize(RootJson.ToSharedRef(), Writer);
	}
	else
	{
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
		FJsonSerializer::Serialize(RootJson.ToSharedRef(), Writer);
	}

	return OutputString;
}
