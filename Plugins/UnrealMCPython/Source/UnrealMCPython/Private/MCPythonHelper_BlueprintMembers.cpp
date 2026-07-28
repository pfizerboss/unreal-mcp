// Copyright (c) 2025 GenOrca (by zenoengine). All Rights Reserved.

#include "MCPythonHelper.h"
#include "MCPythonBlueprint2Internal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"

namespace
{
using namespace UE::MCPython::Blueprint2;

constexpr int32 MaxFunctionParameters = 128;
constexpr int32 ManagedFunctionFlags =
    FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public |
    FUNC_Protected | FUNC_Private | FUNC_BlueprintPure | FUNC_Const;

struct FFunctionParameter
{
    FName Name;
    FEdGraphPinType Type;
    bool bHasDefault = false;
    FNormalizedDefault Default;
};

struct FFunctionSignature
{
    TArray<FFunctionParameter> Inputs;
    TArray<FFunctionParameter> Outputs;
    bool bPure = false;
    bool bConst = false;
    FString Access;
    FString Category;
    FString Description;
};

FString Failure(
    const FString& Code,
    const FString& Path,
    const FString& Message,
    const FString& Hint)
{
    return SerializeResult(MakeFailure(
        Code, Path, Message, false, Hint));
}

FString Invalid(
    const FString& Path,
    const FString& Message,
    const FString& Hint = TEXT("Correct the function request and retry."))
{
    return Failure(TEXT("INVALID_INPUT"), Path, Message, Hint);
}

FString Conflict(const FString& Path, const FString& Message)
{
    return Failure(
        TEXT("CONFLICT"),
        Path,
        Message,
        TEXT("Choose a unique Blueprint member name or refresh the target."));
}

FString Precondition(const FString& Path, const FString& Message)
{
    return Failure(
        TEXT("PRECONDITION_FAILED"),
        Path,
        Message,
        TEXT("Inspect the Blueprint again and select a user-authored function."));
}

bool ParseRequestObject(
    const FString& RequestJson,
    TSharedPtr<FJsonObject>& OutRequest,
    FString& OutFailure)
{
    const TSharedRef<TJsonReader<>> Reader =
        TJsonReaderFactory<>::Create(RequestJson);
    if (!FJsonSerializer::Deserialize(Reader, OutRequest) ||
        !OutRequest.IsValid())
    {
        OutFailure = Invalid(
            TEXT("request"), TEXT("RequestJson must contain one JSON object."));
        return false;
    }
    return true;
}

bool ValidateClosedRequest(
    const TSharedRef<FJsonObject>& Request,
    const TSet<FString>& AllowedFields,
    FString& OutFailure,
    const FString& PathPrefix = FString())
{
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Request->Values)
    {
        if (!AllowedFields.Contains(Pair.Key))
        {
            const FString Path = PathPrefix.IsEmpty()
                ? Pair.Key
                : PathPrefix + TEXT(".") + Pair.Key;
            OutFailure = Invalid(
                Path,
                FString::Printf(
                    TEXT("Unknown request field '%s'."), *Pair.Key));
            return false;
        }
    }
    return true;
}

bool RequiredString(
    const TSharedRef<FJsonObject>& Request,
    const FString& Field,
    FString& OutValue,
    FString& OutFailure,
    bool bAllowEmpty = false)
{
    if (!Request->TryGetStringField(Field, OutValue) ||
        (!bAllowEmpty && OutValue.IsEmpty()))
    {
        OutFailure = Invalid(
            Field,
            FString::Printf(
                TEXT("'%s' must be %sa string."),
                *Field,
                bAllowEmpty ? TEXT("") : TEXT("a non-empty ")));
        return false;
    }
    return true;
}

bool RequiredBool(
    const TSharedRef<FJsonObject>& Request,
    const FString& Field,
    bool& OutValue,
    FString& OutFailure)
{
    if (!Request->TryGetBoolField(Field, OutValue))
    {
        OutFailure = Invalid(
            Field,
            FString::Printf(TEXT("'%s' must be a boolean."), *Field));
        return false;
    }
    return true;
}

bool OptionalString(
    const TSharedRef<FJsonObject>& Request,
    const FString& Field,
    FString& OutValue,
    FString& OutFailure)
{
    if (!Request->HasField(Field))
    {
        return true;
    }
    const TSharedPtr<FJsonValue>* Value = Request->Values.Find(Field);
    if (!Value || !Value->IsValid() || (*Value)->Type != EJson::String)
    {
        OutFailure = Invalid(
            Field,
            FString::Printf(TEXT("'%s' must be a string."), *Field));
        return false;
    }
    OutValue = (*Value)->AsString();
    return true;
}

bool IsValidMemberName(
    const FString& Value,
    const FString& Path,
    FString& OutFailure)
{
    const auto IsAsciiLetter = [](const TCHAR Character)
    {
        return (Character >= TEXT('A') && Character <= TEXT('Z')) ||
            (Character >= TEXT('a') && Character <= TEXT('z'));
    };
    const auto IsAsciiDigit = [](const TCHAR Character)
    {
        return Character >= TEXT('0') && Character <= TEXT('9');
    };
    const bool bValidStart = !Value.IsEmpty() &&
        (Value[0] == TEXT('_') || IsAsciiLetter(Value[0]));
    bool bValidBody = bValidStart;
    for (int32 Index = 1; bValidBody && Index < Value.Len(); ++Index)
    {
        const TCHAR Character = Value[Index];
        bValidBody = Character == TEXT('_') || IsAsciiLetter(Character) ||
            IsAsciiDigit(Character);
    }
    if (!bValidBody ||
        Value.Len() > FKismetNameValidator::GetMaximumNameLength())
    {
        OutFailure = Invalid(Path, TEXT(
            "Blueprint member names must match [A-Za-z_][A-Za-z0-9_]* "
            "and contain at most 100 characters."));
        return false;
    }
    return true;
}

bool ParseParameters(
    const TSharedRef<FJsonObject>& Request,
    const FString& Field,
    UBlueprint* Blueprint,
    TSet<FName>& UsedNames,
    TArray<FFunctionParameter>& OutParameters,
    FString& OutFailure)
{
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Request->TryGetArrayField(Field, Values) || !Values)
    {
        OutFailure = Invalid(Field, TEXT("Function parameters must be an array."));
        return false;
    }
    if (Values->Num() > MaxFunctionParameters)
    {
        OutFailure = Invalid(
            Field,
            FString::Printf(
                TEXT("Function signatures support at most %d parameters per direction."),
                MaxFunctionParameters));
        return false;
    }

    OutParameters.Reserve(Values->Num());
    for (int32 Index = 0; Index < Values->Num(); ++Index)
    {
        const FString Path = FString::Printf(TEXT("%s[%d]"), *Field, Index);
        const TSharedPtr<FJsonObject> ParameterObject = (*Values)[Index].IsValid()
            ? (*Values)[Index]->AsObject()
            : nullptr;
        if (!ParameterObject.IsValid())
        {
            OutFailure = Invalid(Path, TEXT("Each parameter must be an object."));
            return false;
        }
        if (!ValidateClosedRequest(
                ParameterObject.ToSharedRef(),
                {TEXT("name"), TEXT("type"), TEXT("default")},
                OutFailure,
                Path))
        {
            return false;
        }

        FString Name;
        if (!RequiredString(
                ParameterObject.ToSharedRef(), TEXT("name"), Name, OutFailure))
        {
            OutFailure = Invalid(
                Path + TEXT(".name"),
                TEXT("Parameter name must be a non-empty string."));
            return false;
        }
        if (!IsValidMemberName(Name, Path + TEXT(".name"), OutFailure))
        {
            return false;
        }
        const FName ParameterName(*Name);
        if (UsedNames.Contains(ParameterName))
        {
            OutFailure = Invalid(
                Path + TEXT(".name"),
                FString::Printf(
                    TEXT("Duplicate parameter name '%s'."), *Name));
            return false;
        }
        UsedNames.Add(ParameterName);

        const TSharedPtr<FJsonObject>* TypeObject = nullptr;
        if (!ParameterObject->TryGetObjectField(TEXT("type"), TypeObject) ||
            !TypeObject || !TypeObject->IsValid())
        {
            OutFailure = Invalid(
                Path + TEXT(".type"),
                TEXT("Parameter type must be a canonical type object."));
            return false;
        }

        FFunctionParameter Parameter;
        Parameter.Name = ParameterName;
        FError TypeError;
        if (!ParseTypeSpec(
                TypeObject->ToSharedRef(),
                Parameter.Type,
                TypeError,
                Path + TEXT(".type")))
        {
            OutFailure = Failure(
                TypeError.Code.IsEmpty() ? TEXT("INVALID_INPUT") : TypeError.Code,
                TypeError.Path,
                TypeError.Message,
                TypeError.Hint.IsEmpty()
                    ? TEXT("Use a supported canonical Blueprint type.")
                    : TypeError.Hint);
            return false;
        }

        if (const TSharedPtr<FJsonValue>* DefaultValue =
                ParameterObject->Values.Find(TEXT("default")))
        {
            FError DefaultError;
            if (!NormalizeDefaultValue(
                    Parameter.Type,
                    *DefaultValue,
                    Blueprint,
                    Parameter.Default,
                    DefaultError,
                    Path + TEXT(".default")))
            {
                OutFailure = Failure(
                    DefaultError.Code.IsEmpty()
                        ? TEXT("INVALID_INPUT")
                        : DefaultError.Code,
                    DefaultError.Path,
                    DefaultError.Message,
                    DefaultError.Hint.IsEmpty()
                        ? TEXT("Use a default compatible with the parameter type.")
                        : DefaultError.Hint);
                return false;
            }
            Parameter.bHasDefault = true;
        }
        OutParameters.Add(MoveTemp(Parameter));
    }
    return true;
}

bool ParseSignature(
    const TSharedRef<FJsonObject>& Request,
    UBlueprint* Blueprint,
    FFunctionSignature& OutSignature,
    FString& OutFailure)
{
    TSet<FName> UsedNames;
    if (!ParseParameters(
            Request,
            TEXT("inputs"),
            Blueprint,
            UsedNames,
            OutSignature.Inputs,
            OutFailure) ||
        !ParseParameters(
            Request,
            TEXT("outputs"),
            Blueprint,
            UsedNames,
            OutSignature.Outputs,
            OutFailure) ||
        !RequiredBool(Request, TEXT("pure"), OutSignature.bPure, OutFailure) ||
        !RequiredBool(Request, TEXT("const"), OutSignature.bConst, OutFailure) ||
        !RequiredString(
            Request, TEXT("access"), OutSignature.Access, OutFailure) ||
        !RequiredString(
            Request,
            TEXT("category"),
            OutSignature.Category,
            OutFailure,
            true) ||
        !RequiredString(
            Request,
            TEXT("description"),
            OutSignature.Description,
            OutFailure,
            true))
    {
        return false;
    }
    if (OutSignature.Access != TEXT("public") &&
        OutSignature.Access != TEXT("protected") &&
        OutSignature.Access != TEXT("private"))
    {
        OutFailure = Invalid(
            TEXT("access"),
            TEXT("Function access must be 'public', 'protected', or 'private'."));
        return false;
    }
    return true;
}

bool IsInterfaceBlueprint(const UBlueprint* Blueprint)
{
    return Blueprint &&
        (Blueprint->BlueprintType == BPTYPE_Interface ||
         (Blueprint->ParentClass &&
          Blueprint->ParentClass->HasAnyClassFlags(CLASS_Interface)));
}

bool IsInterfaceGraph(const UBlueprint* Blueprint, const UEdGraph* Graph)
{
    if (!Blueprint || !Graph)
    {
        return false;
    }
    for (const FBPInterfaceDescription& Interface :
         Blueprint->ImplementedInterfaces)
    {
        if (Interface.Graphs.Contains(Graph))
        {
            return true;
        }
    }
    return false;
}

bool ValidateUniqueMemberName(
    UBlueprint* Blueprint,
    const FString& CandidateName,
    const FName ExistingName,
    const FString& Path,
    FString& OutFailure)
{
    FKismetNameValidator Validator(Blueprint, ExistingName);
    const EValidatorResult Result = Validator.IsValid(CandidateName);
    if (Result == EValidatorResult::Ok)
    {
        return true;
    }

    const FString Message = INameValidatorInterface::GetErrorString(
        CandidateName, Result);
    if (Result == EValidatorResult::AlreadyInUse ||
        Result == EValidatorResult::ExistingName ||
        Result == EValidatorResult::LocallyInUse)
    {
        OutFailure = Conflict(Path, Message);
    }
    else
    {
        OutFailure = Invalid(Path, Message);
    }
    return false;
}

bool FindTerminators(
    UEdGraph* Graph,
    UK2Node_FunctionEntry*& OutEntry,
    UK2Node_FunctionResult*& OutResult,
    FString& OutFailure)
{
    TArray<UK2Node_FunctionEntry*> Entries;
    TArray<UK2Node_FunctionResult*> Results;
    if (Graph)
    {
        Graph->GetNodesOfClass(Entries);
        Graph->GetNodesOfClass(Results);
    }
    if (Entries.Num() != 1 || Results.Num() > 1)
    {
        OutFailure = Precondition(
            TEXT("function_id"),
            TEXT("A user-authored function must have exactly one entry and at most one result node."));
        return false;
    }
    OutEntry = Entries[0];
    OutResult = Results.IsEmpty() ? nullptr : Results[0];
    return true;
}

bool IsUserFunctionGraph(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    UK2Node_FunctionEntry*& OutEntry,
    UK2Node_FunctionResult*& OutResult,
    FString& OutFailure)
{
    if (!Blueprint || !Graph ||
        !Blueprint->FunctionGraphs.Contains(Graph) ||
        IsInterfaceBlueprint(Blueprint) ||
        IsInterfaceGraph(Blueprint, Graph))
    {
        OutFailure = Precondition(
            TEXT("function_id"),
            TEXT("The target is not a user-authored function graph."));
        return false;
    }
    if (!FindTerminators(Graph, OutEntry, OutResult, OutFailure))
    {
        return false;
    }
    if (!OutEntry->IsEditable())
    {
        OutFailure = Precondition(
            TEXT("function_id"),
            TEXT("Interface and override function graphs cannot be authored by this action."));
        return false;
    }
    return true;
}

bool ParseTarget(
    const TSharedRef<FJsonObject>& Request,
    FTargetRef& OutTarget,
    FString& OutFailure)
{
    if (!RequiredString(
            Request, TEXT("function_id"), OutTarget.Id, OutFailure))
    {
        return false;
    }
    if (Request->HasField(TEXT("allow_name_fallback")) &&
        !Request->TryGetBoolField(
            TEXT("allow_name_fallback"), OutTarget.bAllowNameFallback))
    {
        OutFailure = Invalid(
            TEXT("allow_name_fallback"),
            TEXT("allow_name_fallback must be a boolean."));
        return false;
    }
    if (!OptionalString(
            Request,
            TEXT("function_owner_id"),
            OutTarget.OwnerId,
            OutFailure) ||
        !OptionalString(
            Request, TEXT("function_name"), OutTarget.Name, OutFailure) ||
        !OptionalString(
            Request,
            TEXT("function_type_path"),
            OutTarget.TypePath,
            OutFailure))
    {
        return false;
    }
    if (!OutTarget.Name.IsEmpty() &&
        !IsValidMemberName(
            OutTarget.Name, TEXT("function_name"), OutFailure))
    {
        return false;
    }
    if (OutTarget.Id.StartsWith(TEXT("fallback:")) &&
        (!OutTarget.bAllowNameFallback || OutTarget.OwnerId.IsEmpty() ||
         OutTarget.Name.IsEmpty() || OutTarget.TypePath.IsEmpty()))
    {
        OutFailure = Invalid(
            TEXT("function_id"),
            TEXT("An unstable function id requires explicit, fully qualified name fallback."));
        return false;
    }
    return true;
}

bool ResolveFunction(
    UBlueprint* Blueprint,
    const FTargetRef& Target,
    UEdGraph*& OutGraph,
    UK2Node_FunctionEntry*& OutEntry,
    UK2Node_FunctionResult*& OutResult,
    FString& OutFailure)
{
    FString ResolutionError;
    const FResolvedTarget Resolved = ResolveTarget(
        Blueprint, ETargetKind::Graph, Target, ResolutionError);
    if (!Resolved.Graph)
    {
        if (ResolutionError.Contains(TEXT("ambiguous")))
        {
            OutFailure = Conflict(TEXT("function_id"), ResolutionError);
        }
        else if (ResolutionError.Contains(TEXT("stable target")) ||
                 ResolutionError.Contains(TEXT("not found")))
        {
            OutFailure = Precondition(TEXT("function_id"), ResolutionError);
        }
        else
        {
            OutFailure = Invalid(TEXT("function_id"), ResolutionError);
        }
        return false;
    }
    OutGraph = Resolved.Graph;
    return IsUserFunctionGraph(
        Blueprint, OutGraph, OutEntry, OutResult, OutFailure);
}

void RemoveUserPins(UK2Node_EditablePinBase* Node)
{
    if (!Node)
    {
        return;
    }
    while (!Node->UserDefinedPins.IsEmpty())
    {
        Node->RemoveUserDefinedPin(Node->UserDefinedPins.Last());
    }
}

void ApplyPinDefault(
    UK2Node_EditablePinBase* Node,
    const FFunctionParameter& Parameter)
{
    if (!Node || !Parameter.bHasDefault)
    {
        return;
    }
    TSharedPtr<FUserPinInfo> MatchingUserPin;
    for (const TSharedPtr<FUserPinInfo>& UserPin : Node->UserDefinedPins)
    {
        if (UserPin.IsValid() && UserPin->PinName == Parameter.Name)
        {
            MatchingUserPin = UserPin;
            break;
        }
    }
    if (UEdGraphPin* Pin = Node->FindPin(Parameter.Name))
    {
        Pin->DefaultValue = Parameter.Default.DefaultValue;
        Pin->DefaultObject = Parameter.Default.DefaultObject;
        Pin->DefaultTextValue = Parameter.Default.DefaultTextValue;
        if (MatchingUserPin.IsValid())
        {
            MatchingUserPin->PinDefaultValue = Pin->GetDefaultAsString();
        }
    }
    else if (MatchingUserPin.IsValid())
    {
        MatchingUserPin->PinDefaultValue = Parameter.Default.DefaultValue;
    }
}

bool ApplySignature(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    UK2Node_FunctionEntry* Entry,
    UK2Node_FunctionResult*& Result,
    const FFunctionSignature& Signature,
    FMutationScope& Scope,
    bool bMarkStructurallyModified,
    FString& OutFailure)
{
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);
    Scope.Modify(Entry);
    if (Result)
    {
        Scope.Modify(Result);
    }

    if (!Signature.Outputs.IsEmpty() && !Result)
    {
        Result = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entry);
        if (!Result)
        {
            OutFailure = Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("outputs"),
                TEXT("Unreal could not create the function result node."),
                TEXT("Undo the transaction, inspect the graph, and retry."));
            return false;
        }
        Scope.Modify(Result);
    }

    RemoveUserPins(Entry);
    RemoveUserPins(Result);
    if (Signature.Outputs.IsEmpty() && Result)
    {
        Result->DestroyNode();
        Result = nullptr;
    }

    int32 Flags = Entry->GetExtraFlags() & ~ManagedFunctionFlags;
    Flags |= FUNC_BlueprintCallable | FUNC_BlueprintEvent;
    if (Signature.Access == TEXT("private"))
    {
        Flags |= FUNC_Private;
    }
    else if (Signature.Access == TEXT("protected"))
    {
        Flags |= FUNC_Protected;
    }
    else
    {
        Flags |= FUNC_Public;
    }
    if (Signature.bPure)
    {
        Flags |= FUNC_BlueprintPure;
    }
    if (Signature.bConst)
    {
        Flags |= FUNC_Const;
    }
    Entry->SetExtraFlags(Flags);
    Entry->MetaData.Category = FText::FromString(Signature.Category);
    Entry->MetaData.ToolTip = FText::FromString(Signature.Description);

    for (const FFunctionParameter& Parameter : Signature.Inputs)
    {
        if (!Entry->CreateUserDefinedPin(
                Parameter.Name, Parameter.Type, EGPD_Output, false))
        {
            OutFailure = Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("inputs"),
                FString::Printf(
                    TEXT("Unreal rejected input pin '%s'."),
                    *Parameter.Name.ToString()),
                TEXT("Undo the transaction, inspect the graph, and retry."));
            return false;
        }
    }
    for (const FFunctionParameter& Parameter : Signature.Outputs)
    {
        if (!Result || !Result->CreateUserDefinedPin(
                Parameter.Name, Parameter.Type, EGPD_Input, false))
        {
            OutFailure = Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("outputs"),
                FString::Printf(
                    TEXT("Unreal rejected output pin '%s'."),
                    *Parameter.Name.ToString()),
                TEXT("Undo the transaction, inspect the graph, and retry."));
            return false;
        }
    }

    Entry->ReconstructNode();
    if (Result)
    {
        Result->ReconstructNode();
    }
    for (const FFunctionParameter& Parameter : Signature.Inputs)
    {
        ApplyPinDefault(Entry, Parameter);
    }
    for (const FFunctionParameter& Parameter : Signature.Outputs)
    {
        ApplyPinDefault(Result, Parameter);
    }
    if (bMarkStructurallyModified)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    }
    return true;
}

FString MutationFailure(FMutationScope& Scope, const FString& FailureJson)
{
    const FRollbackResult Rollback = Scope.Rollback();
    if (!Rollback.bSucceeded && !Rollback.bDeferredToWorkflow)
    {
        return Failure(
            TEXT("ROLLBACK_FAILED"),
            TEXT("transaction"),
            TEXT("The function mutation failed and its transaction could not be rolled back."),
            TEXT("Inspect the Blueprint before retrying."));
    }
    return FailureJson;
}

FString SuccessResult(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    const FString& Kind,
    const FString& Summary,
    const FString& OldName = FString())
{
    const FTargetRef Target = DescribeGraphTarget(Blueprint, Graph);
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Data->SetStringField(TEXT("function_id"), Target.Id);
    Data->SetStringField(TEXT("function_name"), Graph->GetName());

    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetStringField(TEXT("function_name"), Graph->GetName());
    if (!OldName.IsEmpty())
    {
        Details->SetStringField(TEXT("old_name"), OldName);
    }
    const TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
    Change->SetStringField(TEXT("kind"), Kind);
    Change->SetStringField(TEXT("target_id"), Target.Id);
    Change->SetObjectField(TEXT("details"), Details);

    const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    const TSharedRef<FJsonObject> NextAction = MakeShared<FJsonObject>();
    NextAction->SetStringField(TEXT("domain"), TEXT("blueprint"));
    NextAction->SetStringField(TEXT("action"), TEXT("compile_blueprint"));
    NextAction->SetObjectField(TEXT("params"), Params);

    const TSharedRef<FJsonObject> Response = MakeSuccess(Summary, Data);
    Response->SetArrayField(
        TEXT("changes"), {MakeShared<FJsonValueObject>(Change)});
    Response->SetArrayField(
        TEXT("next_actions"), {MakeShared<FJsonValueObject>(NextAction)});
    return SerializeResult(Response);
}

FString DeleteSuccessResult(
    UBlueprint* Blueprint,
    const FString& FunctionId,
    const FString& FunctionName)
{
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Data->SetStringField(TEXT("function_id"), FunctionId);
    Data->SetStringField(TEXT("function_name"), FunctionName);

    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetStringField(TEXT("function_name"), FunctionName);
    const TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
    Change->SetStringField(TEXT("kind"), TEXT("delete"));
    Change->SetStringField(TEXT("target_id"), FunctionId);
    Change->SetObjectField(TEXT("details"), Details);

    const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    const TSharedRef<FJsonObject> NextAction = MakeShared<FJsonObject>();
    NextAction->SetStringField(TEXT("domain"), TEXT("blueprint"));
    NextAction->SetStringField(TEXT("action"), TEXT("compile_blueprint"));
    NextAction->SetObjectField(TEXT("params"), Params);

    const TSharedRef<FJsonObject> Response = MakeSuccess(
        TEXT("Blueprint function deleted."), Data);
    Response->SetArrayField(
        TEXT("changes"), {MakeShared<FJsonValueObject>(Change)});
    Response->SetArrayField(
        TEXT("next_actions"), {MakeShared<FJsonValueObject>(NextAction)});
    return SerializeResult(Response);
}
}

FString UMCPythonHelper::CreateBlueprintFunction(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    if (!Blueprint)
    {
        return Precondition(TEXT("asset_path"), TEXT("Blueprint is required."));
    }
    if (IsInterfaceBlueprint(Blueprint))
    {
        return Precondition(
            TEXT("asset_path"),
            TEXT("Blueprint interface assets do not support user-authored function graphs."));
    }
    if (Blueprint->BlueprintType == BPTYPE_MacroLibrary)
    {
        return Precondition(
            TEXT("asset_path"),
            TEXT("Blueprint macro libraries do not support function graphs."));
    }

    TSharedPtr<FJsonObject> Request;
    FString Error;
    if (!ParseRequestObject(RequestJson, Request, Error) ||
        !ValidateClosedRequest(
            Request.ToSharedRef(),
            {TEXT("function_name"), TEXT("inputs"), TEXT("outputs"),
             TEXT("pure"), TEXT("const"), TEXT("access"), TEXT("category"),
             TEXT("description")},
            Error))
    {
        return Error;
    }
    FString FunctionName;
    if (!RequiredString(
            Request.ToSharedRef(),
            TEXT("function_name"),
            FunctionName,
            Error) ||
        !IsValidMemberName(FunctionName, TEXT("function_name"), Error))
    {
        return Error;
    }

    FFunctionSignature Signature;
    if (!ParseSignature(Request.ToSharedRef(), Blueprint, Signature, Error))
    {
        return Error;
    }
    if (!ValidateUniqueMemberName(
            Blueprint,
            FunctionName,
            NAME_None,
            TEXT("function_name"),
            Error))
    {
        return Error;
    }

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython", "CreateBlueprintFunction", "Create Blueprint Function"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the function creation transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    Scope.Modify(Blueprint);
    UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint,
        FName(*FunctionName),
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());
    if (!Graph)
    {
        return MutationFailure(
            Scope,
            Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("function_name"),
                TEXT("Unreal could not create the function graph."),
                TEXT("Inspect the Blueprint and retry.")));
    }
    if (Graph->GetFName() != FName(*FunctionName))
    {
        return MutationFailure(
            Scope,
            Conflict(
                TEXT("function_name"),
                TEXT("Unreal could not reserve the requested function name.")));
    }
    if (!Graph->GraphGuid.IsValid())
    {
        Graph->GraphGuid = FGuid::NewGuid();
    }
    FBlueprintEditorUtils::AddFunctionGraph<UFunction>(
        Blueprint, Graph, true, nullptr);

    UK2Node_FunctionEntry* Entry = nullptr;
    UK2Node_FunctionResult* Result = nullptr;
    if (!FindTerminators(Graph, Entry, Result, Error) ||
        !ApplySignature(
            Blueprint,
            Graph,
            Entry,
            Result,
            Signature,
            Scope,
            false,
            Error))
    {
        return MutationFailure(Scope, Error);
    }
    return SuccessResult(
        Blueprint,
        Graph,
        TEXT("create"),
        TEXT("Blueprint function created."));
}

FString UMCPythonHelper::RenameBlueprintFunction(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    if (!Blueprint)
    {
        return Precondition(TEXT("asset_path"), TEXT("Blueprint is required."));
    }
    TSharedPtr<FJsonObject> Request;
    FString Error;
    if (!ParseRequestObject(RequestJson, Request, Error) ||
        !ValidateClosedRequest(
            Request.ToSharedRef(),
            {TEXT("function_id"), TEXT("new_name"),
             TEXT("allow_name_fallback"), TEXT("function_name"),
             TEXT("function_owner_id"), TEXT("function_type_path")},
            Error))
    {
        return Error;
    }
    FString NewName;
    FTargetRef Target;
    if (!RequiredString(
            Request.ToSharedRef(), TEXT("new_name"), NewName, Error) ||
        !IsValidMemberName(NewName, TEXT("new_name"), Error) ||
        !ParseTarget(Request.ToSharedRef(), Target, Error))
    {
        return Error;
    }

    UEdGraph* Graph = nullptr;
    UK2Node_FunctionEntry* Entry = nullptr;
    UK2Node_FunctionResult* Result = nullptr;
    if (!ResolveFunction(
            Blueprint, Target, Graph, Entry, Result, Error))
    {
        return Error;
    }
    if (!ValidateUniqueMemberName(
            Blueprint,
            NewName,
            Graph->GetFName(),
            TEXT("new_name"),
            Error))
    {
        return Error;
    }
    const FString OldName = Graph->GetName();
    if (OldName == NewName)
    {
        return Conflict(
            TEXT("new_name"), TEXT("The function already has that name."));
    }

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython", "RenameBlueprintFunction", "Rename Blueprint Function"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the function rename transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);
    Scope.Modify(Entry);
    if (Result)
    {
        Scope.Modify(Result);
    }
    FBlueprintEditorUtils::RenameGraph(Graph, NewName);
    if (Graph->GetName() != NewName)
    {
        return MutationFailure(
            Scope,
            Conflict(TEXT("new_name"), TEXT("Unreal rejected the graph rename.")));
    }
    return SuccessResult(
        Blueprint,
        Graph,
        TEXT("update"),
        TEXT("Blueprint function renamed."),
        OldName);
}

FString UMCPythonHelper::SetBlueprintFunctionSignature(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    if (!Blueprint)
    {
        return Precondition(TEXT("asset_path"), TEXT("Blueprint is required."));
    }
    TSharedPtr<FJsonObject> Request;
    FString Error;
    if (!ParseRequestObject(RequestJson, Request, Error) ||
        !ValidateClosedRequest(
            Request.ToSharedRef(),
            {TEXT("function_id"), TEXT("inputs"), TEXT("outputs"),
             TEXT("pure"), TEXT("const"), TEXT("access"), TEXT("category"),
             TEXT("description"), TEXT("allow_name_fallback"),
             TEXT("function_name"), TEXT("function_owner_id"),
             TEXT("function_type_path")},
            Error))
    {
        return Error;
    }

    FTargetRef Target;
    FFunctionSignature Signature;
    if (!ParseTarget(Request.ToSharedRef(), Target, Error) ||
        !ParseSignature(Request.ToSharedRef(), Blueprint, Signature, Error))
    {
        return Error;
    }
    UEdGraph* Graph = nullptr;
    UK2Node_FunctionEntry* Entry = nullptr;
    UK2Node_FunctionResult* Result = nullptr;
    if (!ResolveFunction(
            Blueprint, Target, Graph, Entry, Result, Error))
    {
        return Error;
    }

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython",
        "SetBlueprintFunctionSignature",
        "Set Blueprint Function Signature"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the signature transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    if (!ApplySignature(
            Blueprint,
            Graph,
            Entry,
            Result,
            Signature,
            Scope,
            true,
            Error))
    {
        return MutationFailure(Scope, Error);
    }
    return SuccessResult(
        Blueprint,
        Graph,
        TEXT("update"),
        TEXT("Blueprint function signature replaced."));
}

FString UMCPythonHelper::DeleteBlueprintFunction(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    if (!Blueprint)
    {
        return Precondition(TEXT("asset_path"), TEXT("Blueprint is required."));
    }
    TSharedPtr<FJsonObject> Request;
    FString Error;
    if (!ParseRequestObject(RequestJson, Request, Error) ||
        !ValidateClosedRequest(
            Request.ToSharedRef(),
            {TEXT("function_id"), TEXT("allow_name_fallback"),
             TEXT("function_name"), TEXT("function_owner_id"),
             TEXT("function_type_path")},
            Error))
    {
        return Error;
    }
    FTargetRef Target;
    if (!ParseTarget(Request.ToSharedRef(), Target, Error))
    {
        return Error;
    }
    UEdGraph* Graph = nullptr;
    UK2Node_FunctionEntry* Entry = nullptr;
    UK2Node_FunctionResult* Result = nullptr;
    if (!ResolveFunction(
            Blueprint, Target, Graph, Entry, Result, Error))
    {
        return Error;
    }
    const FString FunctionId = DescribeGraphTarget(Blueprint, Graph).Id;
    const FString FunctionName = Graph->GetName();

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython", "DeleteBlueprintFunction", "Delete Blueprint Function"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the function deletion transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);
    Scope.Modify(Entry);
    if (Result)
    {
        Scope.Modify(Result);
    }
    FBlueprintEditorUtils::RemoveGraph(
        Blueprint, Graph, EGraphRemoveFlags::MarkTransient);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    return DeleteSuccessResult(Blueprint, FunctionId, FunctionName);
}
