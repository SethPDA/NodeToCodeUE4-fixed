// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#include "LLM/Providers/N2CManualService.h"
#include "LLM/Providers/N2CManualResponseParser.h"
#include "LLM/N2CSystemPromptManager.h"
#include "Utils/N2CLogger.h"

#include "HAL/PlatformApplicationMisc.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

void UN2CManualService::SendRequest(
    const FString& JsonPayload,
    const FString& SystemMessage,
    const FOnLLMResponseReceived& OnComplete)
{
    if (!bIsInitialized)
    {
        FN2CLogger::Get().LogError(TEXT("Service not initialized"), TEXT("ManualService"));
        OnComplete.ExecuteIfBound(TEXT("{\"error\": \"Service not initialized\"}"));
        return;
    }

    FN2CLogger::Get().Log(
        FString::Printf(TEXT("Sending request to %s (no API key required)"),
            *UEnum::GetValueAsString(GetProviderType())),
        EN2CLogSeverity::Info,
        TEXT("ManualService")
    );

    // Build the merged prompt exactly as the other "no separate system prompt" providers do (docs §11.1)
    FString FinalUserMessage = JsonPayload;
    PromptManager->PrependSourceFilesToUserMessage(FinalUserMessage);
    const FString MergedPrompt = PromptManager->MergePrompts(SystemMessage, FinalUserMessage);

    FPlatformApplicationMisc::ClipboardCopy(*MergedPrompt);

    // Stash the callback the module chained onto us; SubmitManualResponse resolves it later
    PendingOnComplete = OnComplete;

    FNotificationInfo Info(NSLOCTEXT("NodeToCode", "ManualPromptCopied",
        "Prompt copied to clipboard - paste it into Claude, then paste the reply into the Node to Code window"));
    Info.bFireAndForget = true;
    Info.FadeInDuration = 0.2f;
    Info.FadeOutDuration = 0.5f;
    Info.ExpireDuration = 5.0f;
    FSlateNotificationManager::Get().AddNotification(Info);

    FN2CLogger::Get().Log(TEXT("Manual provider: prompt copied to clipboard, waiting for pasted response"),
        EN2CLogSeverity::Info, TEXT("ManualService"));
}

void UN2CManualService::GetConfiguration(FString& OutEndpoint, FString& OutAuthToken, bool& OutSupportsSystemPrompts)
{
    // Never used - SendRequest is fully overridden and never calls the HTTP handler
    OutEndpoint = TEXT("");
    OutAuthToken = TEXT("");
    OutSupportsSystemPrompts = false;
}

void UN2CManualService::CancelPendingRequest()
{
    if (PendingOnComplete.IsBound())
    {
        PendingOnComplete.Unbind();
        FN2CLogger::Get().Log(TEXT("Manual provider: pending request cancelled"), EN2CLogSeverity::Info, TEXT("ManualService"));
    }
}

void UN2CManualService::SubmitManualResponse(const FString& PastedResponseText)
{
    if (!PendingOnComplete.IsBound())
    {
        FN2CLogger::Get().LogWarning(TEXT("SubmitManualResponse called with no pending request"), TEXT("ManualService"));
        return;
    }

    // Clear before executing in case the callback chain triggers another SendRequest
    FOnLLMResponseReceived Callback = PendingOnComplete;
    PendingOnComplete.Unbind();
    Callback.ExecuteIfBound(PastedResponseText);
}

UN2CResponseParserBase* UN2CManualService::CreateResponseParser()
{
    return NewObject<UN2CManualResponseParser>(this);
}
