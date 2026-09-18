// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

/**
 * @file N2CImportReport.h
 * @brief The importer's validation report (docs/BLUEPRINT_CPP_BRIDGE.md §8.4) - what the user copies back to the AI
 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Severity of a single report entry */
enum class EN2CReportLevel : uint8
{
	OK,
	Warning,
	Error
};

/**
 * @struct FN2CImportReportEntry
 * @brief One line of the import report: a node, a link, a default, a declaration, or the compile result
 */
struct FN2CImportReportEntry
{
	EN2CReportLevel Level = EN2CReportLevel::OK;
	/** "node" | "link" | "default" | "declaration" | "compile" */
	FString Scope;
	/** e.g. a node id, or "loop.ArrayElem -> print2.InString" */
	FString Ref;
	FString Message;
	/** "Did you mean" suggestions, or the valid pin list for an unresolved pin */
	TArray<FString> Choices;
};

/**
 * @class FN2CImportReport
 * @brief Accumulates report entries during an import and renders them as text (§8.4) or JSON
 */
class FN2CImportReport
{
public:
	TArray<FN2CImportReportEntry> Entries;
	/** e.g. "BP_EnemyController / EventGraph" */
	FString TargetDescription;
	/** "Validate" | "Insert" */
	FString ModeDescription;

	void AddOk(const FString& Scope, const FString& Ref, const FString& Message);
	void AddWarning(const FString& Scope, const FString& Ref, const FString& Message, const TArray<FString>& Choices = TArray<FString>());
	void AddError(const FString& Scope, const FString& Ref, const FString& Message, const TArray<FString>& Choices = TArray<FString>());

	bool HasErrors() const;
	int32 CountErrors() const;
	int32 CountWarnings() const;

	/** Human-readable report, exactly the format the user copies back to the AI (§8.4) */
	FString ToText() const;

	/** Machine-readable report, for the future folder bridge / MCP tool (§8.4) */
	TSharedPtr<FJsonObject> ToJsonObject() const;
};
