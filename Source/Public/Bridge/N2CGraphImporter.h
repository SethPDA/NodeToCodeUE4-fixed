// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

/**
 * @file N2CGraphImporter.h
 * @brief Turns an N2C Graph v2 document back into real Blueprint nodes (docs/BLUEPRINT_CPP_BRIDGE.md §8)
 */

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "K2Node.h"
#include "Bridge/N2CGraphDocument.h"
#include "Bridge/N2CImportReport.h"

class UBlueprint;
class UEdGraphSchema_K2;
class UK2Node_CallFunction;
class UK2Node_MacroInstance;

/** docs §8.2 */
enum class EN2CImportMode : uint8
{
	ValidateOnly,
	InsertIntoGraph,
	/** Builds into a scratch graph and exports the result as clipboard text; never touches the
	 * real target graph and never creates declarations (docs §8.5) */
	CopyAsNodes
};

/** docs §8.2 */
struct FN2CImportOptions
{
	EN2CImportMode Mode = EN2CImportMode::ValidateOnly;
	FVector2D Origin = FVector2D::ZeroVector;
	bool bCreateDeclarations = true;
	bool bCompileAfter = true;
	bool bSelectImportedNodes = true;
};

/** docs §8.2 */
struct FN2CImportResult
{
	/** false if the report has any Error entry */
	bool bSuccess = false;
	FN2CImportReport Report;
	/** Populated for CopyAsNodes: native clipboard text (T3D), Ctrl+V-able into any graph (§8.5) */
	FString ClipboardText;
	/** Only meaningful for InsertIntoGraph */
	TArray<UEdGraphNode*> CreatedNodes;
};

/**
 * @class FN2CGraphImporter
 * @brief Resolves, creates, wires and validates the nodes described by an N2C Graph v2 document
 *
 * A self-contained new code path (docs §8.1) - it never touches the v1 translator. ValidateOnly and
 * CopyAsNodes both build into a scratch graph outered to the Blueprint (never registered in
 * FunctionGraphs/UbergraphPages, never saved) and cancel the whole transaction afterward, so neither
 * changes the Blueprint or dirties its package. InsertIntoGraph builds directly into TargetGraph
 * inside the same kind of transaction and lets it commit, so a single Ctrl+Z removes the whole
 * import (docs §8.3 step 11, §17).
 */
class FN2CGraphImporter
{
public:
	/**
	 * @param JsonText The N2C Graph v2 document
	 * @param Blueprint The Blueprint declarations are added to and function/variable resolution is scoped to
	 * @param TargetGraph The graph to import into (InsertIntoGraph/ValidateOnly), or the graph to
	 *        resolve local variables/reused nodes against while building in a scratch graph
	 *        (CopyAsNodes). Used directly when the document has exactly one graph; for a multi-graph
	 *        document each graph is matched to an existing Blueprint graph by name instead (docs §8.3
	 *        step 2)
	 */
	static FN2CImportResult Import(const FString& JsonText, UBlueprint* Blueprint, UEdGraph* TargetGraph, const FN2CImportOptions& Options);

	/**
	 * Extract an N2C Graph v2 document from raw pasted/typed text (docs §10.2): strips a ```json ...
	 * ``` (or bare ``` ... ```) fence if present, then finds the first top-level balanced {...}
	 * block that contains "format" and "n2c.graph". Falls back to returning the input unchanged if
	 * no such block is found (e.g. the text is already clean JSON).
	 */
	static FString ExtractJsonDocument(const FString& RawText);

private:
	/** Everything the node/link/default helpers below need, threaded through by reference */
	struct FImportContext
	{
		UBlueprint* Blueprint = nullptr;
		/** Where nodes are actually created - the real target graph, or a scratch graph (CopyAsNodes) */
		UEdGraph* Graph = nullptr;
		/** Where local variables / reused function_entry etc. are looked up - always the real graph,
		 * even when Graph is a scratch graph (CopyAsNodes) */
		UEdGraph* ScopeGraph = nullptr;
		const UEdGraphSchema_K2* Schema = nullptr;
		FN2CImportReport* Report = nullptr;
		const FN2CImportOptions* Options = nullptr;
		TMap<FString, UEdGraphNode*> NodesById;
		TMap<FString, const FN2CGraphDocNode*> DocNodesById;
	};

	static void ImportGraph(FImportContext& Ctx, const FN2CGraphDocGraph& DocGraph);
	static void CreateDeclarations(UBlueprint* Blueprint, const FN2CDeclarations& Declarations, FN2CImportReport& Report);

	// --- node creation, one function per §7.4 kind (or a closely related family of kinds) ---
	static UEdGraphNode* CreateOrResolveNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode);
	static UEdGraphNode* CreateEventNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode);
	static UEdGraphNode* CreateCustomEventNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode);
	static UEdGraphNode* CreateCallFunctionNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode, bool bIsParentCall);
	static UEdGraphNode* CreateVariableNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode, bool bIsSet);
	static UEdGraphNode* CreateSelfNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode);
	static UEdGraphNode* CreateBranchNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode);
	static UEdGraphNode* CreateSequenceNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode);
	static UEdGraphNode* CreateMacroNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode);
	static UEdGraphNode* CreateCastNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode);
	static UEdGraphNode* CreateStructOpNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode, bool bIsBreak);
	static UEdGraphNode* CreateSelectNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode);
	static UEdGraphNode* CreateSwitchIntNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode);
	static UEdGraphNode* CreateSwitchEnumNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode);
	static UEdGraphNode* CreateSwitchStringOrNameNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode, bool bIsName);
	static UEdGraphNode* CreateMakeArrayNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode);
	static UEdGraphNode* CreateSpawnActorNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode);
	static UEdGraphNode* CreateRerouteNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode);
	static UEdGraphNode* FindFunctionEntryNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode);
	static UEdGraphNode* FindOrCreateFunctionResultNode(FImportContext& Ctx, const FN2CGraphDocNode& DocNode);
	static UEdGraphNode* CreateK2NodeFallback(FImportContext& Ctx, const FN2CGraphDocNode& DocNode);

	// --- resolution helpers (docs §8.3 step 5) ---
	struct FFunctionResolution
	{
		UFunction* Function = nullptr;
		UClass* OwnerClass = nullptr;
		bool bAmbiguous = false;
		TArray<FString> AmbiguousCandidates;
		TArray<FString> Suggestions;
	};
	static FFunctionResolution ResolveFunction(UBlueprint* Blueprint, const FString& FunctionName, const FString& ClassHint);
	static void CollectFunctionSuggestions(UClass* Class, const FString& TypoName, TArray<FString>& OutSuggestions);
	static void CollectFunctionSuggestionsAcrossLibraries(const FString& TypoName, TArray<FString>& OutSuggestions);
	static UEdGraph* ResolveMacroGraph(UBlueprint* Blueprint, const FString& MacroName, const FString& LibraryPath);
	static int32 LevenshteinDistance(const FString& A, const FString& B);

	// --- pin resolution (docs §7.5) ---
	static UEdGraphPin* ResolvePin(UEdGraphNode* Node, const FString& DocNodeKind, const FString& RequestedPinName, EEdGraphPinDirection Direction, FString& OutError);
	static UEdGraphPin* ResolveSemanticAlias(UEdGraphNode* Node, const FString& DocNodeKind, const FString& LowerAlias, EEdGraphPinDirection Direction);
	static FString NormalizePinName(const FString& In);
	static FString DescribeNodePins(UEdGraphNode* Node);

	// --- linking, defaults, layout, comments (docs §8.3 steps 7-10) ---
	static void CreateLinks(FImportContext& Ctx, const FN2CGraphDocGraph& DocGraph);
	static void ApplyDefaults(FImportContext& Ctx, UEdGraphNode* Node, const FN2CGraphDocNode& DocNode);
	static void LayoutNodes(FImportContext& Ctx, const FN2CGraphDocGraph& DocGraph, const TArray<UEdGraphNode*>& NewlyCreatedNodes);
	static void CreateComments(FImportContext& Ctx, const FN2CGraphDocGraph& DocGraph);

	// --- scratch graph for ValidateOnly and CopyAsNodes, docs §8.5 ---
	static UEdGraph* CreateScratchGraph(UBlueprint* Blueprint);
	static void DestroyScratchGraph(UBlueprint* Blueprint, UEdGraph* ScratchGraph, bool bBlueprintWasDirty);
};
