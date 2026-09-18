// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

/**
 * @file N2CBlueprintSummaryExporter.h
 * @brief Exports a Blueprint's state (variables, functions, components, ...) as "n2c.bpsummary" JSON (docs §9.2)
 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UEdGraph;
class USCS_Node;
struct FBPVariableDescription;

/**
 * @class FN2CBlueprintSummaryExporter
 * @brief Gives an AI the Blueprint's declared state without screenshots (docs/BLUEPRINT_CPP_BRIDGE.md §9.2)
 */
class FN2CBlueprintSummaryExporter
{
public:
	static FString ExportSummary(UBlueprint* Blueprint, bool bPrettyPrint = true);

private:
	static TSharedPtr<FJsonObject> BuildVariableJson(const FBPVariableDescription& Var);
	static TSharedPtr<FJsonObject> BuildFunctionJson(UEdGraph* FunctionGraph, const FString& Kind);
	static TSharedPtr<FJsonObject> BuildDispatcherJson(UEdGraph* DelegateGraph);
	static TSharedPtr<FJsonObject> BuildComponentJson(const USCS_Node* Node);
};
