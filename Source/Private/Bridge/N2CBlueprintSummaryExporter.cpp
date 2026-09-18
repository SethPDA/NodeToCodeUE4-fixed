// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#include "Bridge/N2CBlueprintSummaryExporter.h"
#include "Bridge/N2CTypeStringConverter.h"
#include "Utils/N2CLogger.h"

#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "EdGraphSchema_K2.h"

#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"

TSharedPtr<FJsonObject> FN2CBlueprintSummaryExporter::BuildVariableJson(const FBPVariableDescription& Var)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Var.VarName.ToString());
	Json->SetStringField(TEXT("type"), FN2CTypeStringConverter::PinTypeToString(Var.VarType));
	if (!Var.DefaultValue.IsEmpty())
	{
		Json->SetStringField(TEXT("default"), Var.DefaultValue);
	}
	const FString Category = Var.Category.ToString();
	if (!Category.IsEmpty())
	{
		Json->SetStringField(TEXT("category"), Category);
	}
	const bool bEditable = (Var.PropertyFlags & CPF_Edit) != 0 && (Var.PropertyFlags & CPF_DisableEditOnInstance) == 0;
	Json->SetBoolField(TEXT("editable"), bEditable);
	Json->SetBoolField(TEXT("replicated"), (Var.PropertyFlags & CPF_Net) != 0);
	return Json;
}

TSharedPtr<FJsonObject> FN2CBlueprintSummaryExporter::BuildFunctionJson(UEdGraph* FunctionGraph, const FString& Kind)
{
	if (!FunctionGraph)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), FunctionGraph->GetName());
	Json->SetStringField(TEXT("kind"), Kind);

	UK2Node_FunctionEntry* EntryNode = nullptr;
	UK2Node_FunctionResult* ResultNode = nullptr;
	for (UEdGraphNode* Node : FunctionGraph->Nodes)
	{
		if (!EntryNode)
		{
			EntryNode = Cast<UK2Node_FunctionEntry>(Node);
		}
		if (!ResultNode)
		{
			ResultNode = Cast<UK2Node_FunctionResult>(Node);
		}
	}

	TArray<TSharedPtr<FJsonValue>> Inputs;
	if (EntryNode)
	{
		for (UEdGraphPin* Pin : EntryNode->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
				|| Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate)
			{
				continue;
			}
			TSharedPtr<FJsonObject> InputJson = MakeShared<FJsonObject>();
			InputJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
			InputJson->SetStringField(TEXT("type"), FN2CTypeStringConverter::PinTypeToString(Pin->PinType));
			Inputs.Add(MakeShared<FJsonValueObject>(InputJson));
		}
	}
	if (Inputs.Num() > 0)
	{
		Json->SetArrayField(TEXT("inputs"), Inputs);
	}

	TArray<TSharedPtr<FJsonValue>> Outputs;
	if (ResultNode)
	{
		for (UEdGraphPin* Pin : ResultNode->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}
			TSharedPtr<FJsonObject> OutputJson = MakeShared<FJsonObject>();
			OutputJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
			OutputJson->SetStringField(TEXT("type"), FN2CTypeStringConverter::PinTypeToString(Pin->PinType));
			Outputs.Add(MakeShared<FJsonValueObject>(OutputJson));
		}
	}
	if (Outputs.Num() > 0)
	{
		Json->SetArrayField(TEXT("outputs"), Outputs);
	}

	return Json;
}

TSharedPtr<FJsonObject> FN2CBlueprintSummaryExporter::BuildDispatcherJson(UEdGraph* DelegateGraph)
{
	if (!DelegateGraph)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), DelegateGraph->GetName());

	TArray<TSharedPtr<FJsonValue>> Inputs;
	for (UEdGraphNode* Node : DelegateGraph->Nodes)
	{
		UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node);
		if (!EntryNode)
		{
			continue;
		}
		for (UEdGraphPin* Pin : EntryNode->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
				|| Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate)
			{
				continue;
			}
			TSharedPtr<FJsonObject> InputJson = MakeShared<FJsonObject>();
			InputJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
			InputJson->SetStringField(TEXT("type"), FN2CTypeStringConverter::PinTypeToString(Pin->PinType));
			Inputs.Add(MakeShared<FJsonValueObject>(InputJson));
		}
		break;
	}
	if (Inputs.Num() > 0)
	{
		Json->SetArrayField(TEXT("inputs"), Inputs);
	}

	return Json;
}

TSharedPtr<FJsonObject> FN2CBlueprintSummaryExporter::BuildComponentJson(const USCS_Node* Node)
{
	if (!Node)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
	Json->SetStringField(TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT("Unknown"));

	if (!Node->ParentComponentOrVariableName.IsNone())
	{
		Json->SetStringField(TEXT("parent"), Node->ParentComponentOrVariableName.ToString());
	}
	else
	{
		Json->SetField(TEXT("parent"), MakeShared<FJsonValueNull>());
	}

	return Json;
}

FString FN2CBlueprintSummaryExporter::ExportSummary(UBlueprint* Blueprint, bool bPrettyPrint)
{
	if (!Blueprint)
	{
		FN2CLogger::Get().LogError(TEXT("FN2CBlueprintSummaryExporter::ExportSummary called with a null Blueprint"));
		return FString();
	}

	TSharedPtr<FJsonObject> RootJson = MakeShared<FJsonObject>();
	RootJson->SetStringField(TEXT("format"), TEXT("n2c.bpsummary"));
	RootJson->SetNumberField(TEXT("version"), 1);
	RootJson->SetStringField(TEXT("blueprint"), Blueprint->GetOutermost()->GetName());
	RootJson->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));

	// Interfaces
	TArray<TSharedPtr<FJsonValue>> Interfaces;
	for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
	{
		if (InterfaceDesc.Interface)
		{
			Interfaces.Add(MakeShared<FJsonValueString>(InterfaceDesc.Interface->GetPathName()));
		}
	}
	RootJson->SetArrayField(TEXT("interfaces"), Interfaces);

	// Variables
	TArray<TSharedPtr<FJsonValue>> Variables;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		Variables.Add(MakeShared<FJsonValueObject>(BuildVariableJson(Var)));
	}
	RootJson->SetArrayField(TEXT("variables"), Variables);

	// Functions: the Blueprint's own function graphs, plus interface event graphs
	TArray<TSharedPtr<FJsonValue>> Functions;
	for (UEdGraph* FunctionGraph : Blueprint->FunctionGraphs)
	{
		if (TSharedPtr<FJsonObject> FunctionJson = BuildFunctionJson(FunctionGraph, TEXT("function")))
		{
			Functions.Add(MakeShared<FJsonValueObject>(FunctionJson));
		}
	}
	for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
	{
		for (UEdGraph* InterfaceGraph : InterfaceDesc.Graphs)
		{
			if (TSharedPtr<FJsonObject> FunctionJson = BuildFunctionJson(InterfaceGraph, TEXT("interface_event")))
			{
				Functions.Add(MakeShared<FJsonValueObject>(FunctionJson));
			}
		}
	}
	RootJson->SetArrayField(TEXT("functions"), Functions);

	// Dispatchers (event dispatchers / multicast delegates)
	TArray<TSharedPtr<FJsonValue>> Dispatchers;
	for (UEdGraph* DelegateGraph : Blueprint->DelegateSignatureGraphs)
	{
		if (TSharedPtr<FJsonObject> DispatcherJson = BuildDispatcherJson(DelegateGraph))
		{
			Dispatchers.Add(MakeShared<FJsonValueObject>(DispatcherJson));
		}
	}
	RootJson->SetArrayField(TEXT("dispatchers"), Dispatchers);

	// Components (Simple Construction Script)
	TArray<TSharedPtr<FJsonValue>> Components;
	if (Blueprint->SimpleConstructionScript)
	{
		for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
		{
			if (TSharedPtr<FJsonObject> ComponentJson = BuildComponentJson(Node))
			{
				Components.Add(MakeShared<FJsonValueObject>(ComponentJson));
			}
		}
	}
	RootJson->SetArrayField(TEXT("components"), Components);

	// Graphs (event graphs + ubergraph pages), for informational purposes
	TArray<TSharedPtr<FJsonValue>> Graphs;
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph)
		{
			Graphs.Add(MakeShared<FJsonValueString>(Graph->GetName()));
		}
	}
	RootJson->SetArrayField(TEXT("graphs"), Graphs);

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
