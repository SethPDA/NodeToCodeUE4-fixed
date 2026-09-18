// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

/**
 * @file N2CGraphDocument.h
 * @brief Plain C++ structs for the N2C Graph v2 document (docs/BLUEPRINT_CPP_BRIDGE.md §7) and its JSON parser
 *
 * Plain structs rather than USTRUCTs, per docs §8.1, to avoid UHT's "nested containers"
 * restriction (a document node's kind-specific fields are read straight from its raw JSON object,
 * which needs a TMap<FString, TSharedPtr<FJsonValue>> - see FN2CFlows for the same constraint hit
 * by the v1 model).
 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** One node entry in a graph's "nodes" array (§7.4) */
struct FN2CGraphDocNode
{
	FString Id;
	FString Kind;
	TMap<FString, FString> Defaults;
	bool bHasPos = false;
	FVector2D Pos = FVector2D::ZeroVector;
	FString Comment;
	bool bHasPure = false;
	bool bPure = false;

	/** The full original JSON object, so kind-specific fields (function, class, variable, scope, ...)
	 * can be read on demand without a dedicated struct field per node kind (§7.4's table is large,
	 * and most fields only apply to one or two kinds). */
	TSharedPtr<FJsonObject> Raw;

	bool HasField(const FString& Field) const;
	FString GetString(const FString& Field, const FString& DefaultValue = FString()) const;
	int32 GetInt(const FString& Field, int32 DefaultValue) const;
	bool GetBool(const FString& Field, bool DefaultValue) const;
	TArray<FString> GetStringArray(const FString& Field) const;
	TArray<int32> GetIntArray(const FString& Field) const;
};

/** One entry in a graph's "comments" array (§7.3) */
struct FN2CGraphDocComment
{
	FString Text;
	TArray<FString> NodeIds;
	bool bHasColor = false;
	FLinearColor Color = FLinearColor::White;
};

/** One entry in a graph's "links"/"external_links" array (§7.3) - both ends are "id.PinName" refs */
struct FN2CGraphDocLink
{
	FString FromRef;
	FString ToRef;
};

/** One graph in the document's "graphs" array (§7.3) */
struct FN2CGraphDocGraph
{
	FString Name;
	/** "event_graph" | "function" | "macro" */
	FString Kind;
	TArray<FN2CGraphDocNode> Nodes;
	TArray<FN2CGraphDocLink> Links;
	TArray<FN2CGraphDocComment> Comments;
};

/** A {name,type} pair used throughout "declarations" (§7.6) */
struct FN2CParamDecl
{
	FString Name;
	FString Type;
};

struct FN2CVariableDecl
{
	FString Name;
	FString Type;
	FString Default;
	FString Category;
	bool bHasEditable = false;
	bool bEditable = false;
};

struct FN2CFunctionDecl
{
	FString Name;
	bool bPure = false;
	bool bConst = false;
	TArray<FN2CParamDecl> Inputs;
	TArray<FN2CParamDecl> Outputs;
};

struct FN2CCustomEventDecl
{
	FString Name;
	TArray<FN2CParamDecl> Inputs;
};

struct FN2CDispatcherDecl
{
	FString Name;
	TArray<FN2CParamDecl> Inputs;
};

/** The document's "declarations" object (§7.6) - members created only if missing, never modified */
struct FN2CDeclarations
{
	TArray<FN2CVariableDecl> Variables;
	TArray<FN2CFunctionDecl> Functions;
	TArray<FN2CCustomEventDecl> CustomEvents;
	TArray<FN2CDispatcherDecl> Dispatchers;
};

/** The whole N2C Graph v2 document (§7.2) */
struct FN2CGraphDocument
{
	FString Format;
	int32 Version = 0;
	FString Engine;
	/** Target Blueprint asset path or name (§7.2); the window importer uses the focused editor instead */
	FString BlueprintPath;
	FString Summary;
	FN2CDeclarations Declarations;
	TArray<FN2CGraphDocGraph> Graphs;
};

/**
 * @class FN2CGraphDocumentParser
 * @brief Parses N2C Graph v2 JSON text into FN2CGraphDocument
 *
 * Tolerant by design (docs §7.1): unknown/extra fields are ignored, and a single malformed node,
 * link or comment is recorded in OutStructuralIssues and skipped rather than failing the whole
 * parse. Parse() only returns false for a total failure - invalid JSON, or no "graphs" array.
 */
class FN2CGraphDocumentParser
{
public:
	static bool Parse(const FString& JsonText, FN2CGraphDocument& OutDocument, TArray<FString>& OutStructuralIssues);

private:
	static void ParseGraph(const TSharedPtr<FJsonObject>& GraphJson, int32 GraphIndex, FN2CGraphDocGraph& OutGraph, TArray<FString>& OutIssues);
	static bool ParseNode(const TSharedPtr<FJsonObject>& NodeJson, int32 GraphIndex, int32 NodeIndex, FN2CGraphDocNode& OutNode, TArray<FString>& OutIssues);
	static bool ParseLink(const TSharedPtr<FJsonValue>& LinkValue, int32 GraphIndex, int32 LinkIndex, FN2CGraphDocLink& OutLink, TArray<FString>& OutIssues);
	static void ParseComment(const TSharedPtr<FJsonObject>& CommentJson, FN2CGraphDocComment& OutComment);
	static void ParseDeclarations(const TSharedPtr<FJsonObject>& DeclarationsJson, FN2CDeclarations& OutDeclarations);
	static void ParseParamList(const TSharedPtr<FJsonObject>& Parent, const FString& Field, TArray<FN2CParamDecl>& OutParams);
};
