// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#include "Bridge/N2CGraphDocument.h"

#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ============================================================================
// FN2CGraphDocNode accessors
// ============================================================================

bool FN2CGraphDocNode::HasField(const FString& Field) const
{
	return Raw.IsValid() && Raw->HasField(Field);
}

FString FN2CGraphDocNode::GetString(const FString& Field, const FString& DefaultValue) const
{
	FString Value;
	if (Raw.IsValid() && Raw->TryGetStringField(Field, Value))
	{
		return Value;
	}
	return DefaultValue;
}

int32 FN2CGraphDocNode::GetInt(const FString& Field, int32 DefaultValue) const
{
	int32 Value = DefaultValue;
	if (Raw.IsValid())
	{
		Raw->TryGetNumberField(Field, Value);
	}
	return Value;
}

bool FN2CGraphDocNode::GetBool(const FString& Field, bool DefaultValue) const
{
	bool Value = DefaultValue;
	if (Raw.IsValid())
	{
		Raw->TryGetBoolField(Field, Value);
	}
	return Value;
}

TArray<FString> FN2CGraphDocNode::GetStringArray(const FString& Field) const
{
	TArray<FString> Result;
	if (Raw.IsValid())
	{
		Raw->TryGetStringArrayField(Field, Result);
	}
	return Result;
}

TArray<int32> FN2CGraphDocNode::GetIntArray(const FString& Field) const
{
	TArray<int32> Result;
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (Raw.IsValid() && Raw->TryGetArrayField(Field, Array) && Array)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			int32 Number = 0;
			if (Value->TryGetNumber(Number))
			{
				Result.Add(Number);
			}
		}
	}
	return Result;
}

// ============================================================================
// Parser
// ============================================================================

void FN2CGraphDocumentParser::ParseParamList(const TSharedPtr<FJsonObject>& Parent, const FString& Field, TArray<FN2CParamDecl>& OutParams)
{
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (!Parent->TryGetArrayField(Field, Array) || !Array)
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Array)
	{
		const TSharedPtr<FJsonObject> Obj = Value->AsObject();
		if (!Obj.IsValid())
		{
			continue;
		}
		FN2CParamDecl Param;
		Obj->TryGetStringField(TEXT("name"), Param.Name);
		Obj->TryGetStringField(TEXT("type"), Param.Type);
		if (!Param.Name.IsEmpty())
		{
			OutParams.Add(MoveTemp(Param));
		}
	}
}

void FN2CGraphDocumentParser::ParseDeclarations(const TSharedPtr<FJsonObject>& DeclarationsJson, FN2CDeclarations& OutDeclarations)
{
	const TArray<TSharedPtr<FJsonValue>>* VarsArray = nullptr;
	if (DeclarationsJson->TryGetArrayField(TEXT("variables"), VarsArray) && VarsArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *VarsArray)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (!Obj.IsValid())
			{
				continue;
			}
			FN2CVariableDecl Var;
			Obj->TryGetStringField(TEXT("name"), Var.Name);
			Obj->TryGetStringField(TEXT("type"), Var.Type);
			Obj->TryGetStringField(TEXT("default"), Var.Default);
			Obj->TryGetStringField(TEXT("category"), Var.Category);
			bool bEditable = false;
			if (Obj->TryGetBoolField(TEXT("editable"), bEditable))
			{
				Var.bHasEditable = true;
				Var.bEditable = bEditable;
			}
			if (!Var.Name.IsEmpty() && !Var.Type.IsEmpty())
			{
				OutDeclarations.Variables.Add(MoveTemp(Var));
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* FuncsArray = nullptr;
	if (DeclarationsJson->TryGetArrayField(TEXT("functions"), FuncsArray) && FuncsArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *FuncsArray)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (!Obj.IsValid())
			{
				continue;
			}
			FN2CFunctionDecl Func;
			Obj->TryGetStringField(TEXT("name"), Func.Name);
			Obj->TryGetBoolField(TEXT("pure"), Func.bPure);
			Obj->TryGetBoolField(TEXT("const"), Func.bConst);
			ParseParamList(Obj, TEXT("inputs"), Func.Inputs);
			ParseParamList(Obj, TEXT("outputs"), Func.Outputs);
			if (!Func.Name.IsEmpty())
			{
				OutDeclarations.Functions.Add(MoveTemp(Func));
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* CustomEventsArray = nullptr;
	if (DeclarationsJson->TryGetArrayField(TEXT("custom_events"), CustomEventsArray) && CustomEventsArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *CustomEventsArray)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (!Obj.IsValid())
			{
				continue;
			}
			FN2CCustomEventDecl EventDecl;
			Obj->TryGetStringField(TEXT("name"), EventDecl.Name);
			ParseParamList(Obj, TEXT("inputs"), EventDecl.Inputs);
			if (!EventDecl.Name.IsEmpty())
			{
				OutDeclarations.CustomEvents.Add(MoveTemp(EventDecl));
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* DispatchersArray = nullptr;
	if (DeclarationsJson->TryGetArrayField(TEXT("dispatchers"), DispatchersArray) && DispatchersArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *DispatchersArray)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (!Obj.IsValid())
			{
				continue;
			}
			FN2CDispatcherDecl Dispatcher;
			Obj->TryGetStringField(TEXT("name"), Dispatcher.Name);
			ParseParamList(Obj, TEXT("inputs"), Dispatcher.Inputs);
			if (!Dispatcher.Name.IsEmpty())
			{
				OutDeclarations.Dispatchers.Add(MoveTemp(Dispatcher));
			}
		}
	}
}

void FN2CGraphDocumentParser::ParseComment(const TSharedPtr<FJsonObject>& CommentJson, FN2CGraphDocComment& OutComment)
{
	CommentJson->TryGetStringField(TEXT("text"), OutComment.Text);
	CommentJson->TryGetStringArrayField(TEXT("nodes"), OutComment.NodeIds);

	const TArray<TSharedPtr<FJsonValue>>* ColorArray = nullptr;
	if (CommentJson->TryGetArrayField(TEXT("color"), ColorArray) && ColorArray && ColorArray->Num() == 4)
	{
		double R = 1.0, G = 1.0, B = 1.0, A = 1.0;
		(*ColorArray)[0]->TryGetNumber(R);
		(*ColorArray)[1]->TryGetNumber(G);
		(*ColorArray)[2]->TryGetNumber(B);
		(*ColorArray)[3]->TryGetNumber(A);
		OutComment.Color = FLinearColor(R, G, B, A);
		OutComment.bHasColor = true;
	}
}

bool FN2CGraphDocumentParser::ParseLink(const TSharedPtr<FJsonValue>& LinkValue, int32 GraphIndex, int32 LinkIndex, FN2CGraphDocLink& OutLink, TArray<FString>& OutIssues)
{
	const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
	if (!LinkValue.IsValid() || !LinkValue->TryGetArray(Pair) || !Pair || Pair->Num() != 2)
	{
		OutIssues.Add(FString::Printf(TEXT("graphs[%d].links[%d] is not a 2-element array, skipped"), GraphIndex, LinkIndex));
		return false;
	}
	FString From, To;
	if (!(*Pair)[0]->TryGetString(From) || !(*Pair)[1]->TryGetString(To))
	{
		OutIssues.Add(FString::Printf(TEXT("graphs[%d].links[%d] endpoints must be strings, skipped"), GraphIndex, LinkIndex));
		return false;
	}
	OutLink.FromRef = From;
	OutLink.ToRef = To;
	return true;
}

bool FN2CGraphDocumentParser::ParseNode(const TSharedPtr<FJsonObject>& NodeJson, int32 GraphIndex, int32 NodeIndex, FN2CGraphDocNode& OutNode, TArray<FString>& OutIssues)
{
	if (!NodeJson->TryGetStringField(TEXT("id"), OutNode.Id) || OutNode.Id.IsEmpty())
	{
		OutIssues.Add(FString::Printf(TEXT("graphs[%d].nodes[%d] is missing 'id', skipped"), GraphIndex, NodeIndex));
		return false;
	}
	if (!NodeJson->TryGetStringField(TEXT("kind"), OutNode.Kind) || OutNode.Kind.IsEmpty())
	{
		OutIssues.Add(FString::Printf(TEXT("graphs[%d].nodes[%d] (id '%s') is missing 'kind', skipped"), GraphIndex, NodeIndex, *OutNode.Id));
		return false;
	}

	OutNode.Raw = NodeJson;

	const TSharedPtr<FJsonObject>* DefaultsJson = nullptr;
	if (NodeJson->TryGetObjectField(TEXT("defaults"), DefaultsJson) && DefaultsJson && DefaultsJson->IsValid())
	{
		for (const auto& Pair : (*DefaultsJson)->Values)
		{
			FString Value;
			if (Pair.Value->TryGetString(Value))
			{
				OutNode.Defaults.Add(Pair.Key, Value);
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* PosArray = nullptr;
	if (NodeJson->TryGetArrayField(TEXT("pos"), PosArray) && PosArray && PosArray->Num() == 2)
	{
		double X = 0.0, Y = 0.0;
		(*PosArray)[0]->TryGetNumber(X);
		(*PosArray)[1]->TryGetNumber(Y);
		OutNode.Pos = FVector2D(X, Y);
		OutNode.bHasPos = true;
	}

	NodeJson->TryGetStringField(TEXT("comment"), OutNode.Comment);

	bool PureValue = false;
	if (NodeJson->TryGetBoolField(TEXT("pure"), PureValue))
	{
		OutNode.bPure = PureValue;
		OutNode.bHasPure = true;
	}

	return true;
}

void FN2CGraphDocumentParser::ParseGraph(const TSharedPtr<FJsonObject>& GraphJson, int32 GraphIndex, FN2CGraphDocGraph& OutGraph, TArray<FString>& OutIssues)
{
	GraphJson->TryGetStringField(TEXT("name"), OutGraph.Name);
	GraphJson->TryGetStringField(TEXT("kind"), OutGraph.Kind);

	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (GraphJson->TryGetArrayField(TEXT("nodes"), NodesArray) && NodesArray)
	{
		int32 NodeIndex = 0;
		for (const TSharedPtr<FJsonValue>& NodeValue : *NodesArray)
		{
			const TSharedPtr<FJsonObject> NodeJson = NodeValue.IsValid() ? NodeValue->AsObject() : nullptr;
			if (!NodeJson.IsValid())
			{
				OutIssues.Add(FString::Printf(TEXT("graphs[%d].nodes[%d] is not an object, skipped"), GraphIndex, NodeIndex));
				++NodeIndex;
				continue;
			}
			FN2CGraphDocNode Node;
			if (ParseNode(NodeJson, GraphIndex, NodeIndex, Node, OutIssues))
			{
				OutGraph.Nodes.Add(MoveTemp(Node));
			}
			++NodeIndex;
		}
	}

	if (OutGraph.Kind.IsEmpty())
	{
		// Defaults to "function" if there's a function_entry node, otherwise "event_graph" (§7.3)
		const bool bHasEntry = OutGraph.Nodes.ContainsByPredicate([](const FN2CGraphDocNode& N) { return N.Kind == TEXT("function_entry"); });
		OutGraph.Kind = bHasEntry ? TEXT("function") : TEXT("event_graph");
	}

	const TArray<TSharedPtr<FJsonValue>>* LinksArray = nullptr;
	if (GraphJson->TryGetArrayField(TEXT("links"), LinksArray) && LinksArray)
	{
		int32 LinkIndex = 0;
		for (const TSharedPtr<FJsonValue>& LinkValue : *LinksArray)
		{
			FN2CGraphDocLink Link;
			if (ParseLink(LinkValue, GraphIndex, LinkIndex, Link, OutIssues))
			{
				OutGraph.Links.Add(MoveTemp(Link));
			}
			++LinkIndex;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* CommentsArray = nullptr;
	if (GraphJson->TryGetArrayField(TEXT("comments"), CommentsArray) && CommentsArray)
	{
		for (const TSharedPtr<FJsonValue>& CommentValue : *CommentsArray)
		{
			const TSharedPtr<FJsonObject> CommentJson = CommentValue.IsValid() ? CommentValue->AsObject() : nullptr;
			if (CommentJson.IsValid())
			{
				FN2CGraphDocComment Comment;
				ParseComment(CommentJson, Comment);
				OutGraph.Comments.Add(MoveTemp(Comment));
			}
		}
	}
}

bool FN2CGraphDocumentParser::Parse(const FString& JsonText, FN2CGraphDocument& OutDocument, TArray<FString>& OutStructuralIssues)
{
	TSharedPtr<FJsonObject> RootJson;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootJson) || !RootJson.IsValid())
	{
		OutStructuralIssues.Add(TEXT("Document is not valid JSON"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* GraphsArray = nullptr;
	if (!RootJson->TryGetArrayField(TEXT("graphs"), GraphsArray) || !GraphsArray || GraphsArray->Num() == 0)
	{
		OutStructuralIssues.Add(TEXT("Document has no 'graphs' array"));
		return false;
	}

	RootJson->TryGetStringField(TEXT("format"), OutDocument.Format);
	int32 Version = 0;
	RootJson->TryGetNumberField(TEXT("version"), Version);
	OutDocument.Version = Version;
	RootJson->TryGetStringField(TEXT("engine"), OutDocument.Engine);
	RootJson->TryGetStringField(TEXT("blueprint"), OutDocument.BlueprintPath);
	RootJson->TryGetStringField(TEXT("summary"), OutDocument.Summary);

	const TSharedPtr<FJsonObject>* DeclarationsJson = nullptr;
	if (RootJson->TryGetObjectField(TEXT("declarations"), DeclarationsJson) && DeclarationsJson && DeclarationsJson->IsValid())
	{
		ParseDeclarations(*DeclarationsJson, OutDocument.Declarations);
	}

	int32 GraphIndex = 0;
	for (const TSharedPtr<FJsonValue>& GraphValue : *GraphsArray)
	{
		const TSharedPtr<FJsonObject> GraphJson = GraphValue.IsValid() ? GraphValue->AsObject() : nullptr;
		if (!GraphJson.IsValid())
		{
			OutStructuralIssues.Add(FString::Printf(TEXT("graphs[%d] is not an object, skipped"), GraphIndex));
			++GraphIndex;
			continue;
		}
		FN2CGraphDocGraph Graph;
		ParseGraph(GraphJson, GraphIndex, Graph, OutStructuralIssues);
		OutDocument.Graphs.Add(MoveTemp(Graph));
		++GraphIndex;
	}

	return true;
}
