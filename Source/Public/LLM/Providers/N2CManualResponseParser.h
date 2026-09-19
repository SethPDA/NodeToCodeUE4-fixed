// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "LLM/N2CResponseParserBase.h"
#include "N2CManualResponseParser.generated.h"

/**
 * @class UN2CManualResponseParser
 * @brief Parser for text the user pastes back in from a chat AI under the Manual (copy/paste)
 * provider - there is no HTTP envelope to unwrap, but the reply may still have prose around the
 * JSON, a fence, or a reasoning model's <think> block, so it needs its own extraction pass
 * before handing off to the base class (docs §11.1).
 */
UCLASS()
class NODETOCODE_API UN2CManualResponseParser : public UN2CResponseParserBase
{
    GENERATED_BODY()

public:
    virtual bool ParseLLMResponse(
        const FString& InJson,
        FN2CTranslationResponse& OutResponse) override;

private:
    /** Strip <think>...</think> blocks and pull out the balanced {...} object containing "graphs" */
    static FString ExtractGraphsJson(const FString& RawText);
};
