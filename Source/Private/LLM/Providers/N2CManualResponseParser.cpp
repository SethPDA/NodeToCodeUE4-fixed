// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#include "LLM/Providers/N2CManualResponseParser.h"
#include "Utils/N2CLogger.h"

bool UN2CManualResponseParser::ParseLLMResponse(
    const FString& InJson,
    FN2CTranslationResponse& OutResponse)
{
    const FString ExtractedJson = ExtractGraphsJson(InJson);

    FN2CLogger::Get().Log(
        FString::Printf(TEXT("Manual response: extracted %d of %d pasted characters as JSON"),
            ExtractedJson.Len(), InJson.Len()),
        EN2CLogSeverity::Debug,
        TEXT("ManualResponseParser")
    );

    return Super::ParseLLMResponse(ExtractedJson, OutResponse);
}

FString UN2CManualResponseParser::ExtractGraphsJson(const FString& RawText)
{
    FString Text = RawText;

    // Remove <think>...</think> blocks some reasoning models include when pasted manually
    for (;;)
    {
        const int32 ThinkStart = Text.Find(TEXT("<think>"));
        if (ThinkStart == INDEX_NONE)
        {
            break;
        }
        const int32 ThinkEndTag = Text.Find(TEXT("</think>"), ESearchCase::IgnoreCase, ESearchDir::FromStart, ThinkStart);
        if (ThinkEndTag == INDEX_NONE)
        {
            Text = Text.Left(ThinkStart);
            break;
        }
        Text.RemoveAt(ThinkStart, (ThinkEndTag + 8) - ThinkStart); // 8 == len("</think>")
    }

    // Find the first top-level balanced {...} block that contains a "graphs" field, tracking
    // string literals so braces inside JSON string values don't confuse the depth count. This
    // works whether the object sits inside a ```json fence, a bare ``` fence, or loose prose,
    // since it scans the raw text directly instead of relying on fence markers being exact.
    for (int32 i = 0; i < Text.Len(); ++i)
    {
        if (Text[i] != TEXT('{'))
        {
            continue;
        }

        int32 Depth = 0;
        bool bInString = false;
        bool bEscaped = false;
        for (int32 j = i; j < Text.Len(); ++j)
        {
            const TCHAR C = Text[j];
            if (bInString)
            {
                if (bEscaped) { bEscaped = false; }
                else if (C == TEXT('\\')) { bEscaped = true; }
                else if (C == TEXT('"')) { bInString = false; }
                continue;
            }
            if (C == TEXT('"'))
            {
                bInString = true;
                continue;
            }
            if (C == TEXT('{'))
            {
                ++Depth;
            }
            else if (C == TEXT('}'))
            {
                --Depth;
                if (Depth == 0)
                {
                    const FString Candidate = Text.Mid(i, j - i + 1);
                    if (Candidate.Contains(TEXT("\"graphs\"")))
                    {
                        return Candidate;
                    }
                    break; // this brace block wasn't it; keep scanning from the next '{'
                }
            }
        }
    }

    // No embedded JSON object found - return the trimmed text as-is; the base parser's own
    // brace-balance and JSON validity checks will fail loudly and log why.
    return Text.TrimStartAndEnd();
}
