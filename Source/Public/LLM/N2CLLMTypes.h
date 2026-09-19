// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Models/N2CTranslation.h"
#include "N2CLLMTypes.generated.h"

/** Delegate for receiving LLM responses */
DECLARE_DELEGATE_OneParam(FOnLLMResponseReceived, const FString& /* Response */);

/** Delegate for receiving parsed translation responses */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnTranslationResponseReceived, const FN2CTranslationResponse&, Response, bool, bSuccess);

/** Delegate for when a translation request is sent */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnTranslationRequestSent);

/** Native (non-dynamic) delegates for C++ Slate widget binding */
DECLARE_MULTICAST_DELEGATE(FOnTranslationRequestSentNative);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnTranslationResponseReceivedNative, const FN2CTranslationResponse&, bool);

/** Available LLM providers */
UENUM(BlueprintType)
enum class EN2CLLMProvider : uint8
{
    OpenAI      UMETA(DisplayName = "OpenAI"),
    Anthropic   UMETA(DisplayName = "Anthropic"),
    Gemini      UMETA(DisplayName = "Gemini"),
    Ollama      UMETA(DisplayName = "Ollama"),
    DeepSeek    UMETA(DisplayName = "DeepSeek"),
    LMStudio    UMETA(DisplayName = "LM Studio"),
    Manual      UMETA(DisplayName = "Manual (Copy/Paste)")
};

/** Status of the Node to Code system */
UENUM(BlueprintType)
enum class EN2CSystemStatus : uint8
{
    Idle        UMETA(DisplayName = "Idle"),
    Processing  UMETA(DisplayName = "Processing Request"),
    Error       UMETA(DisplayName = "Error"),
    Initializing UMETA(DisplayName = "Initializing")
};

/**
 * @struct FN2CLLMConfig
 * @brief Configuration settings for LLM integration
 */
USTRUCT(BlueprintType)
struct FN2CLLMConfig
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LLM Integration")
    EN2CLLMProvider Provider = EN2CLLMProvider::Anthropic;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LLM Integration")
    FString ApiEndpoint;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LLM Integration")
    FString ApiKey;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LLM Integration")
    float TimeoutSeconds = 3600.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LLM Integration")
    bool bUseSystemPrompts = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LLM Integration")
    FString Model;
};
