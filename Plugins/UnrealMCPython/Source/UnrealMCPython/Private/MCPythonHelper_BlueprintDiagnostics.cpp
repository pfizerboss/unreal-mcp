// Copyright (c) 2025 GenOrca (by zenoengine). All Rights Reserved.

#include "MCPythonHelper.h"
#include "MCPythonBlueprint2Internal.h"

#include "EdGraphToken.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"

namespace
{
FString CompileStatus(const UBlueprint* Blueprint)
{
    if (!Blueprint)
    {
        return TEXT("Unknown");
    }
    switch (Blueprint->Status)
    {
    case BS_UpToDate: return TEXT("UpToDate");
    case BS_UpToDateWithWarnings: return TEXT("UpToDateWithWarnings");
    case BS_Error: return TEXT("Error");
    case BS_Dirty: return TEXT("Dirty");
    case BS_BeingCreated: return TEXT("BeingCreated");
    default: return TEXT("Unknown");
    }
}

void NormalizeDiagnostic(
    const FString& Message,
    const bool bWarning,
    FString& OutCode,
    FString& OutHint)
{
    const FString Lower = Message.ToLower();
    if (Lower.Contains(TEXT("required pin")) ||
        Lower.Contains(TEXT("no value")) ||
        Lower.Contains(TEXT("must be linked")) ||
        Lower.Contains(TEXT("must have a connection")))
    {
        OutCode = TEXT("BP_MISSING_REQUIRED_PIN");
        OutHint = TEXT("Connect the cited pin or assign a valid default value, then compile again.");
    }
    else if (Lower.Contains(TEXT("could not find")) ||
             Lower.Contains(TEXT("unresolved")))
    {
        OutCode = TEXT("BP_UNRESOLVED_MEMBER");
        OutHint = TEXT("Re-inspect the referenced member, replace the stale node, then compile again.");
    }
    else if (Lower.Contains(TEXT("not compatible")) ||
             Lower.Contains(TEXT("type mismatch")) ||
             Lower.Contains(TEXT("incompatible")) ||
             Lower.Contains(TEXT("can't connect pins")))
    {
        OutCode = TEXT("BP_TYPE_MISMATCH");
        OutHint = TEXT("Insert an explicit conversion or change one of the connected pin types.");
    }
    else if (Lower.Contains(TEXT("already exists")) ||
             Lower.Contains(TEXT("duplicate")))
    {
        OutCode = TEXT("BP_DUPLICATE_MEMBER");
        OutHint = TEXT("Rename or remove the duplicate Blueprint member, then compile again.");
    }
    else if (Lower.Contains(TEXT("accessed none")))
    {
        OutCode = TEXT("BP_POSSIBLE_NULL_ACCESS");
        OutHint = TEXT("Validate the referenced object before accessing it.");
    }
    else
    {
        OutCode = bWarning ? TEXT("BP_COMPILE_WARNING") : TEXT("BP_COMPILE_ERROR");
        OutHint = TEXT("Inspect the cited Blueprint node and pins, correct the issue, then compile again.");
    }
}

void ResolveDiagnosticTarget(
    UBlueprint* Blueprint,
    const FTokenizedMessage& Message,
    FString& OutGraphId,
    FString& OutNodeId,
    FString& OutPinId)
{
    const UEdGraphPin* Pin = nullptr;
    const UObject* GraphObject = nullptr;
    for (const TSharedRef<IMessageToken>& Token : Message.GetMessageTokens())
    {
        if (Token->GetType() != EMessageToken::EdGraph)
        {
            continue;
        }
        const FEdGraphToken* EdGraphToken =
            static_cast<const FEdGraphToken*>(&Token.Get());
        if (!Pin)
        {
            Pin = EdGraphToken->GetPin();
        }
        if (!GraphObject)
        {
            GraphObject = EdGraphToken->GetGraphObject();
        }
    }

    const UEdGraphNode* Node = Pin ? Pin->GetOwningNode() : nullptr;
    const UEdGraph* Graph = Node ? Node->GetGraph() : nullptr;
    if (!Node)
    {
        Node = Cast<UEdGraphNode>(GraphObject);
        Graph = Node ? Node->GetGraph() : Cast<UEdGraph>(GraphObject);
    }
    if (Pin)
    {
        OutPinId = UE::MCPython::Blueprint2::DescribePinTarget(
            Blueprint, Pin).Id;
    }
    if (Node)
    {
        OutNodeId = UE::MCPython::Blueprint2::DescribeNodeTarget(
            Blueprint, Node).Id;
    }
    if (Graph)
    {
        OutGraphId = UE::MCPython::Blueprint2::DescribeGraphTarget(
            Blueprint, Graph).Id;
    }
}

TSharedRef<FJsonObject> MakeDiagnostic(
    UBlueprint* Blueprint,
    const TSharedRef<FTokenizedMessage>& Message,
    const bool bWarning)
{
    const FString Text = Message->ToText().ToString();
    FString Code;
    FString Hint;
    NormalizeDiagnostic(Text, bWarning, Code, Hint);

    FString GraphId;
    FString NodeId;
    FString PinId;
    ResolveDiagnosticTarget(
        Blueprint, Message.Get(), GraphId, NodeId, PinId);

    const TSharedRef<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
    Diagnostic->SetStringField(TEXT("code"), Code);
    Diagnostic->SetStringField(
        TEXT("severity"), bWarning ? TEXT("warning") : TEXT("error"));
    Diagnostic->SetStringField(TEXT("message"), Text);
    Diagnostic->SetStringField(TEXT("hint"), Hint);
    Diagnostic->SetStringField(TEXT("graph_id"), GraphId);
    Diagnostic->SetStringField(TEXT("node_id"), NodeId);
    Diagnostic->SetStringField(TEXT("pin_id"), PinId);
    return Diagnostic;
}

TSharedRef<FJsonObject> MakeWarningRecord(
    const TSharedRef<FJsonObject>& Diagnostic)
{
    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetStringField(
        TEXT("graph_id"), Diagnostic->GetStringField(TEXT("graph_id")));
    Details->SetStringField(
        TEXT("node_id"), Diagnostic->GetStringField(TEXT("node_id")));
    Details->SetStringField(
        TEXT("pin_id"), Diagnostic->GetStringField(TEXT("pin_id")));
    Details->SetStringField(
        TEXT("hint"), Diagnostic->GetStringField(TEXT("hint")));

    const TSharedRef<FJsonObject> Warning = MakeShared<FJsonObject>();
    Warning->SetStringField(
        TEXT("code"), Diagnostic->GetStringField(TEXT("code")));
    Warning->SetStringField(
        TEXT("message"), Diagnostic->GetStringField(TEXT("message")));
    Warning->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
    Warning->SetObjectField(TEXT("details"), Details);
    return Warning;
}

TSharedRef<FJsonObject> MakeNextAction(
    const FString& Action,
    const FString& AssetPath)
{
    const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), AssetPath);
    const TSharedRef<FJsonObject> NextAction = MakeShared<FJsonObject>();
    NextAction->SetStringField(TEXT("domain"), TEXT("blueprint"));
    NextAction->SetStringField(TEXT("action"), Action);
    NextAction->SetObjectField(TEXT("params"), Params);
    return NextAction;
}

TSharedRef<FJsonObject> MakeInspectNextAction(
    const FString& AssetPath,
    const TArray<TSharedPtr<FJsonValue>>& Diagnostics)
{
    const TSharedRef<FJsonObject> NextAction = MakeNextAction(
        TEXT("inspect_blueprint"), AssetPath);
    const TSharedPtr<FJsonObject> Params = NextAction->GetObjectField(
        TEXT("params"));
    TArray<TSharedPtr<FJsonValue>> Queries;

    const TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
    Nodes->SetStringField(TEXT("op"), TEXT("nodes"));
    Nodes->SetStringField(TEXT("detail"), TEXT("detailed"));
    Nodes->SetNumberField(TEXT("limit"), 100);
    Queries.Add(MakeShared<FJsonValueObject>(Nodes));

    FString TargetNodeId;
    for (const TSharedPtr<FJsonValue>& Value : Diagnostics)
    {
        const TSharedPtr<FJsonObject> Diagnostic = Value.IsValid()
            ? Value->AsObject()
            : nullptr;
        if (Diagnostic &&
            Diagnostic->TryGetStringField(TEXT("node_id"), TargetNodeId) &&
            !TargetNodeId.IsEmpty())
        {
            break;
        }
    }
    if (!TargetNodeId.IsEmpty())
    {
        const TSharedRef<FJsonObject> Pins = MakeShared<FJsonObject>();
        Pins->SetStringField(TEXT("op"), TEXT("pins"));
        Pins->SetStringField(TEXT("node_id"), TargetNodeId);
        Pins->SetStringField(TEXT("detail"), TEXT("detailed"));
        Pins->SetNumberField(TEXT("limit"), 100);
        Queries.Add(MakeShared<FJsonValueObject>(Pins));
    }
    Params->SetArrayField(TEXT("queries"), Queries);
    return NextAction;
}

FString InvalidBlueprintResult()
{
    using namespace UE::MCPython::Blueprint2;
    const TSharedRef<FJsonObject> Result = MakeFailure(
        TEXT("INVALID_INPUT"),
        TEXT("params.asset_path"),
        TEXT("Invalid Blueprint."),
        false,
        TEXT("Load a valid Blueprint asset and retry compilation."));
    Result->SetStringField(TEXT("status"), TEXT("Unknown"));
    Result->SetStringField(TEXT("message"), TEXT("Invalid Blueprint."));
    Result->SetArrayField(TEXT("diagnostics"), {});
    return SerializeResult(Result);
}
}

FString UMCPythonHelper::CompileBlueprint(UBlueprint* Blueprint)
{
    using namespace UE::MCPython::Blueprint2;
    if (!Blueprint)
    {
        return InvalidBlueprintResult();
    }

    FCompilerResultsLog Results;
    Results.bSilentMode = true;
    Results.bAnnotateMentionedNodes = false;
    FKismetEditorUtilities::CompileBlueprint(
        Blueprint,
        EBlueprintCompileOptions::None,
        &Results);

    TArray<TSharedPtr<FJsonValue>> Diagnostics;
    TArray<TSharedPtr<FJsonValue>> WarningRecords;
    int32 ErrorCount = 0;
    int32 WarningCount = 0;
    for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
    {
        const EMessageSeverity::Type Severity = Message->GetSeverity();
        const bool bWarning = Severity == EMessageSeverity::Warning ||
            Severity == EMessageSeverity::PerformanceWarning;
        const bool bError = Severity == EMessageSeverity::Error;
        if (!bWarning && !bError)
        {
            continue;
        }
        const TSharedRef<FJsonObject> Diagnostic = MakeDiagnostic(
            Blueprint, Message, bWarning);
        Diagnostics.Add(MakeShared<FJsonValueObject>(Diagnostic));
        if (bWarning)
        {
            ++WarningCount;
            WarningRecords.Add(MakeShared<FJsonValueObject>(
                MakeWarningRecord(Diagnostic)));
        }
        else
        {
            ++ErrorCount;
        }
    }

    const FString Status = CompileStatus(Blueprint);
    const bool bHasErrors = ErrorCount > 0 || Blueprint->Status == BS_Error;
    const FString LegacyMessage = bHasErrors
        ? TEXT("Blueprint compilation failed. Check the output log for details.")
        : TEXT("Blueprint compiled successfully.");
    const FString Summary = bHasErrors
        ? FString::Printf(
            TEXT("Blueprint compilation failed with %d error(s) and %d warning(s)."),
            ErrorCount,
            WarningCount)
        : WarningCount > 0
            ? FString::Printf(
                TEXT("Blueprint compiled with %d warning(s)."), WarningCount)
            : TEXT("Blueprint compiled successfully.");

    TSharedRef<FJsonObject> Result = bHasErrors
        ? MakeFailure(
            TEXT("COMPILE_FAILED"),
            FString(),
            LegacyMessage,
            false,
            TEXT("Inspect the structured diagnostics, repair the cited nodes or pins, and compile again."))
        : MakeSuccess(Summary);
    Result->SetBoolField(TEXT("success"), !bHasErrors);
    Result->SetStringField(TEXT("status"), Status);
    Result->SetStringField(TEXT("message"), LegacyMessage);
    Result->SetStringField(TEXT("summary"), Summary);

    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("result_status"), Status);
    Data->SetNumberField(TEXT("error_count"), ErrorCount);
    Data->SetNumberField(TEXT("warning_count"), WarningCount);
    Data->SetNumberField(TEXT("diagnostic_count"), Diagnostics.Num());
    Result->SetObjectField(TEXT("data"), Data);
    Result->SetArrayField(TEXT("diagnostics"), Diagnostics);
    Result->SetArrayField(TEXT("warnings"), WarningRecords);

    const FString AssetPath = Blueprint->GetPathName();
    TArray<TSharedPtr<FJsonValue>> NextActions;
    if (bHasErrors)
    {
        NextActions.Add(MakeShared<FJsonValueObject>(
            MakeInspectNextAction(AssetPath, Diagnostics)));
        NextActions.Add(MakeShared<FJsonValueObject>(
            MakeNextAction(TEXT("get_blueprint_health"), AssetPath)));
    }
    else
    {
        NextActions.Add(MakeShared<FJsonValueObject>(
            MakeNextAction(TEXT("get_blueprint_health"), AssetPath)));
        NextActions.Add(MakeShared<FJsonValueObject>(
            MakeNextAction(TEXT("snapshot_blueprint_graph"), AssetPath)));
    }
    Result->SetArrayField(TEXT("next_actions"), NextActions);
    return SerializeResult(Result);
}
