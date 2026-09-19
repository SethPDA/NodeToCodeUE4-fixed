// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "LLM/N2CBaseLLMService.h"
#include "N2CManualService.generated.h"

/**
 * @class UN2CManualService
 * @brief "Manual (copy/paste)" provider (docs §11.1) - no API key or HTTP request. Copies the
 * merged prompt to the clipboard and waits for the user to paste the chat AI's reply back in
 * through the Node to Code window's Loading panel.
 */
UCLASS()
class NODETOCODE_API UN2CManualService : public UN2CBaseLLMService
{
    GENERATED_BODY()

public:
    virtual void SendRequest(const FString& JsonPayload, const FString& SystemMessage,
                           const FOnLLMResponseReceived& OnComplete) override;
    virtual void GetConfiguration(FString& OutEndpoint, FString& OutAuthToken,
                              bool& OutSupportsSystemPrompts) override;
    virtual EN2CLLMProvider GetProviderType() const override { return EN2CLLMProvider::Manual; }
    virtual void CancelPendingRequest() override;

    /** Called by the N2C window's Submit (or Load from file) action with the user-pasted reply */
    void SubmitManualResponse(const FString& PastedResponseText);

protected:
    virtual UN2CResponseParserBase* CreateResponseParser() override;

private:
    /** The translation callback stashed by SendRequest, resolved once the user submits a pasted reply */
    FOnLLMResponseReceived PendingOnComplete;
};
