// Copyright (c) 2025 GenOrca (by zenoengine). All Rights Reserved.

#include "MCPythonHelper.h"
#include "MCPythonBlueprint2Internal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Tunnel.h"
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

struct FMacroSignature
{
    TArray<FFunctionParameter> Inputs;
    TArray<FFunctionParameter> Outputs;
    bool bPure = false;
    FString Category;
    FString Description;
};

struct FDispatcherSignature
{
    TArray<FFunctionParameter> Parameters;
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

bool RequiredNumber(
    const TSharedRef<FJsonObject>& Request,
    const FString& Field,
    double& OutValue,
    FString& OutFailure)
{
    if (!Request->TryGetNumberField(Field, OutValue) ||
        !FMath::IsFinite(OutValue))
    {
        OutFailure = Invalid(
            Field,
            FString::Printf(TEXT("'%s' must be a finite number."), *Field));
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

bool ParseMacroSignature(
    const TSharedRef<FJsonObject>& Request,
    UBlueprint* Blueprint,
    FMacroSignature& OutSignature,
    FString& OutFailure)
{
    TSet<FName> UsedNames;
    return ParseParameters(
               Request,
               TEXT("inputs"),
               Blueprint,
               UsedNames,
               OutSignature.Inputs,
               OutFailure) &&
        ParseParameters(
               Request,
               TEXT("outputs"),
               Blueprint,
               UsedNames,
               OutSignature.Outputs,
               OutFailure) &&
        RequiredBool(Request, TEXT("pure"), OutSignature.bPure, OutFailure) &&
        RequiredString(
               Request,
               TEXT("category"),
               OutSignature.Category,
               OutFailure,
               true) &&
        RequiredString(
               Request,
               TEXT("description"),
               OutSignature.Description,
               OutFailure,
               true);
}

bool ParseDispatcherSignature(
    const TSharedRef<FJsonObject>& Request,
    UBlueprint* Blueprint,
    FDispatcherSignature& OutSignature,
    FString& OutFailure)
{
    TSet<FName> UsedNames;
    return ParseParameters(
               Request,
               TEXT("parameters"),
               Blueprint,
               UsedNames,
               OutSignature.Parameters,
               OutFailure) &&
        RequiredString(
               Request,
               TEXT("category"),
               OutSignature.Category,
               OutFailure,
               true) &&
        RequiredString(
               Request,
               TEXT("description"),
               OutSignature.Description,
               OutFailure,
               true);
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

bool FindMacroTunnels(
    UEdGraph* Graph,
    UK2Node_Tunnel*& OutEntry,
    UK2Node_Tunnel*& OutExit,
    FString& OutFailure)
{
    TArray<UK2Node_Tunnel*> Tunnels;
    Graph->GetNodesOfClass(Tunnels);
    for (UK2Node_Tunnel* Tunnel : Tunnels)
    {
        if (!Tunnel || !Tunnel->IsEditable())
        {
            continue;
        }
        if (Tunnel->bCanHaveOutputs)
        {
            if (OutEntry)
            {
                OutFailure = Precondition(
                    TEXT("macro_id"),
                    TEXT("The macro has more than one editable entry tunnel."));
                return false;
            }
            OutEntry = Tunnel;
        }
        if (Tunnel->bCanHaveInputs)
        {
            if (OutExit)
            {
                OutFailure = Precondition(
                    TEXT("macro_id"),
                    TEXT("The macro has more than one editable exit tunnel."));
                return false;
            }
            OutExit = Tunnel;
        }
    }
    if (!OutEntry || !OutExit)
    {
        OutFailure = Precondition(
            TEXT("macro_id"),
            TEXT("The macro does not have one editable entry and exit tunnel."));
        return false;
    }
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

bool ParseNamedTarget(
    const TSharedRef<FJsonObject>& Request,
    const FString& IdField,
    const FString& NameField,
    const FString& OwnerField,
    const FString& TypeField,
    FTargetRef& OutTarget,
    FString& OutFailure)
{
    if (!RequiredString(Request, IdField, OutTarget.Id, OutFailure))
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
    if (!OptionalString(Request, NameField, OutTarget.Name, OutFailure) ||
        !OptionalString(Request, OwnerField, OutTarget.OwnerId, OutFailure) ||
        !OptionalString(Request, TypeField, OutTarget.TypePath, OutFailure))
    {
        return false;
    }
    if (!OutTarget.Name.IsEmpty() &&
        !IsValidMemberName(OutTarget.Name, NameField, OutFailure))
    {
        return false;
    }
    if (OutTarget.Id.StartsWith(TEXT("fallback:")) &&
        (!OutTarget.bAllowNameFallback || OutTarget.OwnerId.IsEmpty() ||
         OutTarget.Name.IsEmpty() || OutTarget.TypePath.IsEmpty()))
    {
        OutFailure = Invalid(
            IdField,
            TEXT("An unstable target id requires explicit, fully qualified name fallback."));
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

bool ResolveMemberTarget(
    UBlueprint* Blueprint,
    ETargetKind Kind,
    const FTargetRef& Target,
    const FString& Path,
    FResolvedTarget& OutResolved,
    FString& OutFailure)
{
    FString ResolutionError;
    OutResolved = ResolveTarget(Blueprint, Kind, Target, ResolutionError);
    const bool bFound =
        (Kind == ETargetKind::Graph && OutResolved.Graph) ||
        (Kind == ETargetKind::Node && OutResolved.Node) ||
        (Kind == ETargetKind::Variable && OutResolved.Variable) ||
        (Kind == ETargetKind::Interface && OutResolved.Object);
    if (bFound)
    {
        return true;
    }
    if (ResolutionError.Contains(TEXT("ambiguous")))
    {
        OutFailure = Conflict(Path, ResolutionError);
    }
    else if (
        ResolutionError.Contains(TEXT("no longer exists")) ||
        ResolutionError.Contains(TEXT("not found")) ||
        ResolutionError.Contains(TEXT("does not belong")))
    {
        OutFailure = Precondition(Path, ResolutionError);
    }
    else
    {
        OutFailure = Invalid(Path, ResolutionError);
    }
    return false;
}

TArray<TSharedPtr<FJsonValue>> ParameterPinIds(
    UBlueprint* Blueprint,
    UK2Node_EditablePinBase* Node,
    const TArray<FFunctionParameter>& Parameters)
{
    TArray<TSharedPtr<FJsonValue>> PinIds;
    PinIds.Reserve(Parameters.Num());
    for (const FFunctionParameter& Parameter : Parameters)
    {
        if (const UEdGraphPin* Pin = Node ? Node->FindPin(Parameter.Name) : nullptr)
        {
            PinIds.Add(MakeShared<FJsonValueString>(
                DescribePinTarget(Blueprint, Pin).Id));
        }
    }
    return PinIds;
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

bool CreateOrderedPins(
    UK2Node_EditablePinBase* Node,
    const TArray<FFunctionParameter>& Parameters,
    EEdGraphPinDirection Direction,
    const FString& Path,
    FString& OutFailure)
{
    for (const FFunctionParameter& Parameter : Parameters)
    {
        if (!Node || !Node->CreateUserDefinedPin(
                Parameter.Name, Parameter.Type, Direction, false))
        {
            OutFailure = Failure(
                TEXT("INTERNAL_ERROR"),
                Path,
                FString::Printf(
                    TEXT("Unreal rejected pin '%s'."),
                    *Parameter.Name.ToString()),
                TEXT("Undo the transaction, inspect the member, and retry."));
            return false;
        }
    }
    return true;
}

bool ApplyMacroSignature(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    UK2Node_Tunnel* Entry,
    UK2Node_Tunnel* Exit,
    const FMacroSignature& Signature,
    FMutationScope& Scope,
    FString& OutFailure)
{
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);
    Scope.Modify(Entry);
    Scope.Modify(Exit);
    RemoveUserPins(Entry);
    RemoveUserPins(Exit);

    if (!Signature.bPure)
    {
        FEdGraphPinType ExecType;
        ExecType.PinCategory = UEdGraphSchema_K2::PC_Exec;
        if (!Entry->CreateUserDefinedPin(
                UEdGraphSchema_K2::PN_Execute,
                ExecType,
                EGPD_Output,
                false) ||
            !Exit->CreateUserDefinedPin(
                UEdGraphSchema_K2::PN_Then,
                ExecType,
                EGPD_Input,
                false))
        {
            OutFailure = Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("pure"),
                TEXT("Unreal rejected the macro execution pins."),
                TEXT("Undo the transaction, inspect the macro, and retry."));
            return false;
        }
    }
    if (!CreateOrderedPins(
            Entry, Signature.Inputs, EGPD_Output, TEXT("inputs"), OutFailure) ||
        !CreateOrderedPins(
            Exit, Signature.Outputs, EGPD_Input, TEXT("outputs"), OutFailure))
    {
        return false;
    }

    Entry->MetaData.Category = FText::FromString(Signature.Category);
    Entry->MetaData.ToolTip = FText::FromString(Signature.Description);
    Entry->ReconstructNode();
    Exit->ReconstructNode();
    for (const FFunctionParameter& Parameter : Signature.Inputs)
    {
        ApplyPinDefault(Entry, Parameter);
    }
    for (const FFunctionParameter& Parameter : Signature.Outputs)
    {
        ApplyPinDefault(Exit, Parameter);
    }
    return true;
}

bool ApplyEventParameters(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    UK2Node_CustomEvent* Event,
    const TArray<FFunctionParameter>& Parameters,
    FMutationScope& Scope,
    FString& OutFailure)
{
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);
    Scope.Modify(Event);
    RemoveUserPins(Event);
    if (!CreateOrderedPins(
            Event, Parameters, EGPD_Output, TEXT("parameters"), OutFailure))
    {
        return false;
    }
    Event->ReconstructNode();
    for (const FFunctionParameter& Parameter : Parameters)
    {
        ApplyPinDefault(Event, Parameter);
    }
    return true;
}

bool ApplyDispatcherSignature(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    UK2Node_FunctionEntry* Entry,
    const FDispatcherSignature& Signature,
    FMutationScope& Scope,
    FString& OutFailure)
{
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);
    Scope.Modify(Entry);
    RemoveUserPins(Entry);
    if (!CreateOrderedPins(
            Entry,
            Signature.Parameters,
            EGPD_Output,
            TEXT("parameters"),
            OutFailure))
    {
        return false;
    }
    Entry->MetaData.Category = FText::FromString(Signature.Category);
    Entry->MetaData.ToolTip = FText::FromString(Signature.Description);
    Entry->ReconstructNode();
    for (const FFunctionParameter& Parameter : Signature.Parameters)
    {
        ApplyPinDefault(Entry, Parameter);
    }
    return true;
}

TSharedPtr<FJsonValue> MakeChangeValue(
    const FString& Kind,
    const FString& TargetId,
    const TSharedRef<FJsonObject>& Details)
{
    const TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
    Change->SetStringField(TEXT("kind"), Kind);
    Change->SetStringField(TEXT("target_id"), TargetId);
    Change->SetObjectField(TEXT("details"), Details);
    return MakeShared<FJsonValueObject>(Change);
}

FString AuthoringSuccess(
    UBlueprint* Blueprint,
    const FString& Summary,
    const TSharedRef<FJsonObject>& Data,
    const TArray<TSharedPtr<FJsonValue>>& Changes)
{
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    const TSharedRef<FJsonObject> NextAction = MakeShared<FJsonObject>();
    NextAction->SetStringField(TEXT("domain"), TEXT("blueprint"));
    NextAction->SetStringField(TEXT("action"), TEXT("compile_blueprint"));
    NextAction->SetObjectField(TEXT("params"), Params);

    const TSharedRef<FJsonObject> Response = MakeSuccess(Summary, Data);
    Response->SetArrayField(TEXT("changes"), Changes);
    Response->SetArrayField(
        TEXT("next_actions"), {MakeShared<FJsonValueObject>(NextAction)});
    return SerializeResult(Response);
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

FString UMCPythonHelper::CreateBlueprintMacro(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    if (!Blueprint)
    {
        return Precondition(TEXT("asset_path"), TEXT("Blueprint is required."));
    }
    if (Blueprint->BlueprintType != BPTYPE_Normal &&
        Blueprint->BlueprintType != BPTYPE_MacroLibrary &&
        Blueprint->BlueprintType != BPTYPE_LevelScript)
    {
        return Precondition(
            TEXT("asset_path"),
            TEXT("This Blueprint type does not support user-authored macros."));
    }

    TSharedPtr<FJsonObject> Request;
    FString Error;
    if (!ParseRequestObject(RequestJson, Request, Error) ||
        !ValidateClosedRequest(
            Request.ToSharedRef(),
            {TEXT("macro_name"), TEXT("inputs"), TEXT("outputs"),
             TEXT("pure"), TEXT("category"), TEXT("description")},
            Error))
    {
        return Error;
    }
    FString MacroName;
    FMacroSignature Signature;
    if (!RequiredString(
            Request.ToSharedRef(), TEXT("macro_name"), MacroName, Error) ||
        !IsValidMemberName(MacroName, TEXT("macro_name"), Error) ||
        !ParseMacroSignature(
            Request.ToSharedRef(), Blueprint, Signature, Error) ||
        !ValidateUniqueMemberName(
            Blueprint, MacroName, NAME_None, TEXT("macro_name"), Error))
    {
        return Error;
    }

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython", "CreateBlueprintMacro", "Create Blueprint Macro"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the macro creation transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    Scope.Modify(Blueprint);
    UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint,
        FName(*MacroName),
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());
    if (!Graph)
    {
        return MutationFailure(
            Scope,
            Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("macro_name"),
                TEXT("Unreal could not create the macro graph."),
                TEXT("Inspect the Blueprint and retry.")));
    }
    Scope.Modify(Graph);
    if (Graph->GetFName() != FName(*MacroName))
    {
        return MutationFailure(
            Scope,
            Conflict(
                TEXT("macro_name"),
                TEXT("Unreal could not reserve the requested macro name.")));
    }
    if (!Graph->GraphGuid.IsValid())
    {
        Graph->GraphGuid = FGuid::NewGuid();
    }
    FBlueprintEditorUtils::AddMacroGraph(Blueprint, Graph, true, nullptr);

    UK2Node_Tunnel* Entry = nullptr;
    UK2Node_Tunnel* Exit = nullptr;
    if (!FindMacroTunnels(Graph, Entry, Exit, Error) ||
        !ApplyMacroSignature(
            Blueprint, Graph, Entry, Exit, Signature, Scope, Error))
    {
        return MutationFailure(Scope, Error);
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    const FTargetRef MacroTarget = DescribeGraphTarget(Blueprint, Graph);
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("macro_id"), MacroTarget.Id);
    Data->SetStringField(TEXT("macro_name"), MacroName);
    Data->SetArrayField(
        TEXT("input_pin_ids"),
        ParameterPinIds(Blueprint, Entry, Signature.Inputs));
    Data->SetArrayField(
        TEXT("output_pin_ids"),
        ParameterPinIds(Blueprint, Exit, Signature.Outputs));
    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetStringField(TEXT("macro_name"), MacroName);
    return AuthoringSuccess(
        Blueprint,
        TEXT("Blueprint macro created."),
        Data,
        {MakeChangeValue(TEXT("create"), MacroTarget.Id, Details)});
}

FString UMCPythonHelper::DeleteBlueprintMacro(
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
            {TEXT("macro_id"), TEXT("allow_name_fallback"),
             TEXT("macro_name"), TEXT("macro_owner_id"),
             TEXT("macro_type_path")},
            Error))
    {
        return Error;
    }
    FTargetRef Target;
    FResolvedTarget Resolved;
    if (!ParseNamedTarget(
            Request.ToSharedRef(),
            TEXT("macro_id"),
            TEXT("macro_name"),
            TEXT("macro_owner_id"),
            TEXT("macro_type_path"),
            Target,
            Error) ||
        !ResolveMemberTarget(
            Blueprint,
            ETargetKind::Graph,
            Target,
            TEXT("macro_id"),
            Resolved,
            Error))
    {
        return Error;
    }
    UEdGraph* Graph = Resolved.Graph;
    if (!Blueprint->MacroGraphs.Contains(Graph))
    {
        return Precondition(
            TEXT("macro_id"),
            TEXT("The target is not a user-authored macro graph."));
    }
    const FString MacroId = DescribeGraphTarget(Blueprint, Graph).Id;
    const FString MacroName = Graph->GetName();

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython", "DeleteBlueprintMacro", "Delete Blueprint Macro"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the macro deletion transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        Scope.Modify(Node);
    }
    FBlueprintEditorUtils::RemoveGraph(
        Blueprint, Graph, EGraphRemoveFlags::MarkTransient);
    if (Blueprint->MacroGraphs.Contains(Graph))
    {
        return MutationFailure(
            Scope,
            Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("macro_id"),
                TEXT("Unreal did not remove the macro graph."),
                TEXT("Inspect the Blueprint and retry.")));
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("macro_id"), MacroId);
    Data->SetStringField(TEXT("macro_name"), MacroName);
    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetStringField(TEXT("macro_name"), MacroName);
    return AuthoringSuccess(
        Blueprint,
        TEXT("Blueprint macro deleted."),
        Data,
        {MakeChangeValue(TEXT("delete"), MacroId, Details)});
}

FString UMCPythonHelper::CreateCustomEvent(
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
            {TEXT("graph_id"), TEXT("event_name"), TEXT("parameters"),
             TEXT("pos_x"), TEXT("pos_y")},
            Error))
    {
        return Error;
    }

    FString GraphId;
    FString EventName;
    double PosX = 0.0;
    double PosY = 0.0;
    TArray<FFunctionParameter> Parameters;
    TSet<FName> UsedNames;
    if (!RequiredString(
            Request.ToSharedRef(), TEXT("graph_id"), GraphId, Error) ||
        !RequiredString(
            Request.ToSharedRef(), TEXT("event_name"), EventName, Error) ||
        !IsValidMemberName(EventName, TEXT("event_name"), Error) ||
        !ParseParameters(
            Request.ToSharedRef(),
            TEXT("parameters"),
            Blueprint,
            UsedNames,
            Parameters,
            Error) ||
        !RequiredNumber(Request.ToSharedRef(), TEXT("pos_x"), PosX, Error) ||
        !RequiredNumber(Request.ToSharedRef(), TEXT("pos_y"), PosY, Error))
    {
        return Error;
    }
    if (PosX < MIN_int32 || PosX > MAX_int32 ||
        PosY < MIN_int32 || PosY > MAX_int32)
    {
        return Invalid(
            TEXT("position"),
            TEXT("Custom event coordinates must fit in a signed 32-bit integer."));
    }

    FTargetRef GraphTarget;
    GraphTarget.Id = GraphId;
    FResolvedTarget Resolved;
    if (!ResolveMemberTarget(
            Blueprint,
            ETargetKind::Graph,
            GraphTarget,
            TEXT("graph_id"),
            Resolved,
            Error))
    {
        return Error;
    }
    UEdGraph* Graph = Resolved.Graph;
    const UEdGraphSchema_K2* Schema = Graph
        ? Cast<UEdGraphSchema_K2>(Graph->GetSchema())
        : nullptr;
    if (!Graph || !Schema || !Blueprint->UbergraphPages.Contains(Graph) ||
        Schema->GetGraphType(Graph) != GT_Ubergraph || !Graph->bEditable)
    {
        return Precondition(
            TEXT("graph_id"),
            TEXT("Custom events can only be created in an editable K2 ubergraph owned by this Blueprint."));
    }
    if (!ValidateUniqueMemberName(
            Blueprint,
            EventName,
            NAME_None,
            TEXT("event_name"),
            Error))
    {
        return Error;
    }

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython", "CreateCustomEvent", "Create Custom Event"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the custom event transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);
    FGraphNodeCreator<UK2Node_CustomEvent> NodeCreator(*Graph);
    UK2Node_CustomEvent* Event = NodeCreator.CreateNode(false);
    Event->CustomFunctionName = FName(*EventName);
    Event->bIsEditable = true;
    Event->NodePosX = FMath::RoundToInt32(PosX);
    Event->NodePosY = FMath::RoundToInt32(PosY);
    NodeCreator.Finalize();
    if (!ApplyEventParameters(
            Blueprint, Graph, Event, Parameters, Scope, Error))
    {
        return MutationFailure(Scope, Error);
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    const FTargetRef EventTarget = DescribeNodeTarget(Blueprint, Event);
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("event_id"), EventTarget.Id);
    Data->SetStringField(TEXT("event_name"), EventName);
    Data->SetStringField(
        TEXT("graph_id"), DescribeGraphTarget(Blueprint, Graph).Id);
    Data->SetArrayField(
        TEXT("pin_ids"), ParameterPinIds(Blueprint, Event, Parameters));
    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetStringField(TEXT("event_name"), EventName);
    return AuthoringSuccess(
        Blueprint,
        TEXT("Custom event created."),
        Data,
        {MakeChangeValue(TEXT("create"), EventTarget.Id, Details)});
}

FString UMCPythonHelper::DeleteCustomEvent(
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
            {TEXT("event_id"), TEXT("allow_name_fallback"),
             TEXT("event_name"), TEXT("owner_graph_id"),
             TEXT("event_type_path")},
            Error))
    {
        return Error;
    }
    FTargetRef Target;
    FResolvedTarget Resolved;
    if (!ParseNamedTarget(
            Request.ToSharedRef(),
            TEXT("event_id"),
            TEXT("event_name"),
            TEXT("owner_graph_id"),
            TEXT("event_type_path"),
            Target,
            Error) ||
        !ResolveMemberTarget(
            Blueprint,
            ETargetKind::Node,
            Target,
            TEXT("event_id"),
            Resolved,
            Error))
    {
        return Error;
    }
    UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Resolved.Node);
    UEdGraph* Graph = Resolved.Graph;
    if (!Event || !Graph || !Blueprint->UbergraphPages.Contains(Graph))
    {
        return Precondition(
            TEXT("event_id"),
            TEXT("The target is not a custom event in this Blueprint's ubergraph."));
    }
    const FString EventId = DescribeNodeTarget(Blueprint, Event).Id;
    const FString EventName = Event->CustomFunctionName.ToString();

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython", "DeleteCustomEvent", "Delete Custom Event"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the custom event deletion transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);
    Scope.Modify(Event);
    Event->DestroyNode();
    if (Graph->Nodes.Contains(Event))
    {
        return MutationFailure(
            Scope,
            Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("event_id"),
                TEXT("Unreal did not remove the custom event node."),
                TEXT("Inspect the Blueprint and retry.")));
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("event_id"), EventId);
    Data->SetStringField(TEXT("event_name"), EventName);
    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetStringField(TEXT("event_name"), EventName);
    return AuthoringSuccess(
        Blueprint,
        TEXT("Custom event deleted."),
        Data,
        {MakeChangeValue(TEXT("delete"), EventId, Details)});
}

FString UMCPythonHelper::AddEventDispatcher(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    if (!Blueprint)
    {
        return Precondition(TEXT("asset_path"), TEXT("Blueprint is required."));
    }
    if (Blueprint->BlueprintType == BPTYPE_Interface ||
        Blueprint->BlueprintType == BPTYPE_MacroLibrary ||
        Blueprint->BlueprintType == BPTYPE_FunctionLibrary)
    {
        return Precondition(
            TEXT("asset_path"),
            TEXT("This Blueprint type does not support event dispatchers."));
    }

    TSharedPtr<FJsonObject> Request;
    FString Error;
    if (!ParseRequestObject(RequestJson, Request, Error) ||
        !ValidateClosedRequest(
            Request.ToSharedRef(),
            {TEXT("dispatcher_name"), TEXT("parameters"),
             TEXT("category"), TEXT("description")},
            Error))
    {
        return Error;
    }
    FString DispatcherName;
    FDispatcherSignature Signature;
    if (!RequiredString(
            Request.ToSharedRef(),
            TEXT("dispatcher_name"),
            DispatcherName,
            Error) ||
        !IsValidMemberName(
            DispatcherName, TEXT("dispatcher_name"), Error) ||
        !ParseDispatcherSignature(
            Request.ToSharedRef(), Blueprint, Signature, Error) ||
        !ValidateUniqueMemberName(
            Blueprint,
            DispatcherName,
            NAME_None,
            TEXT("dispatcher_name"),
            Error))
    {
        return Error;
    }

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython", "AddEventDispatcher", "Add Event Dispatcher"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the event dispatcher transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    Scope.Modify(Blueprint);
    FEdGraphPinType DispatcherType;
    DispatcherType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    const FName DispatcherFName(*DispatcherName);
    if (!FBlueprintEditorUtils::AddMemberVariable(
            Blueprint, DispatcherFName, DispatcherType))
    {
        return MutationFailure(
            Scope,
            Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("dispatcher_name"),
                TEXT("Unreal rejected the event dispatcher variable."),
                TEXT("Inspect the Blueprint and retry.")));
    }
    const int32 VariableIndex =
        FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, DispatcherFName);
    if (!Blueprint->NewVariables.IsValidIndex(VariableIndex))
    {
        return MutationFailure(
            Scope,
            Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("dispatcher_name"),
                TEXT("Unreal created no addressable dispatcher variable."),
                TEXT("Inspect the Blueprint and retry.")));
    }
    const FTargetRef DispatcherTarget = DescribeVariableTarget(
        Blueprint, Blueprint->NewVariables[VariableIndex]);

    UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint,
        DispatcherFName,
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());
    if (!Graph)
    {
        return MutationFailure(
            Scope,
            Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("dispatcher_name"),
                TEXT("Unreal could not create the dispatcher signature graph."),
                TEXT("Inspect the Blueprint and retry.")));
    }
    Scope.Modify(Graph);
    if (Graph->GetFName() != DispatcherFName)
    {
        return MutationFailure(
            Scope,
            Conflict(
                TEXT("dispatcher_name"),
                TEXT("Unreal could not reserve the dispatcher graph name.")));
    }
    if (!Graph->GraphGuid.IsValid())
    {
        Graph->GraphGuid = FGuid::NewGuid();
    }

    const UEdGraphSchema_K2* K2Schema =
        Cast<UEdGraphSchema_K2>(Graph->GetSchema());
    if (!K2Schema)
    {
        return MutationFailure(
            Scope,
            Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("dispatcher_name"),
                TEXT("The dispatcher signature graph has no K2 schema."),
                TEXT("Inspect the Blueprint and retry.")));
    }
    Graph->bEditable = false;
    K2Schema->CreateDefaultNodesForGraph(*Graph);
    K2Schema->CreateFunctionGraphTerminators(*Graph, (UClass*)nullptr);
    K2Schema->AddExtraFunctionFlags(
        Graph,
        FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
    K2Schema->MarkFunctionEntryAsEditable(Graph, true);
    Blueprint->DelegateSignatureGraphs.Add(Graph);

    TArray<UK2Node_FunctionEntry*> Entries;
    Graph->GetNodesOfClass(Entries);
    if (Entries.Num() != 1 ||
        !ApplyDispatcherSignature(
            Blueprint,
            Graph,
            Entries.IsEmpty() ? nullptr : Entries[0],
            Signature,
            Scope,
            Error))
    {
        if (Error.IsEmpty())
        {
            Error = Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("dispatcher_name"),
                TEXT("The dispatcher graph does not have exactly one signature entry."),
                TEXT("Inspect the Blueprint and retry."));
        }
        return MutationFailure(Scope, Error);
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    const FTargetRef GraphTarget = DescribeGraphTarget(Blueprint, Graph);
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("dispatcher_id"), DispatcherTarget.Id);
    Data->SetStringField(TEXT("dispatcher_name"), DispatcherName);
    Data->SetStringField(TEXT("signature_graph_id"), GraphTarget.Id);
    Data->SetArrayField(
        TEXT("pin_ids"),
        ParameterPinIds(Blueprint, Entries[0], Signature.Parameters));
    const TSharedRef<FJsonObject> VariableDetails = MakeShared<FJsonObject>();
    VariableDetails->SetStringField(TEXT("dispatcher_name"), DispatcherName);
    const TSharedRef<FJsonObject> GraphDetails = MakeShared<FJsonObject>();
    GraphDetails->SetStringField(TEXT("dispatcher_name"), DispatcherName);
    return AuthoringSuccess(
        Blueprint,
        TEXT("Event dispatcher created."),
        Data,
        {
            MakeChangeValue(
                TEXT("create"), DispatcherTarget.Id, VariableDetails),
            MakeChangeValue(TEXT("create"), GraphTarget.Id, GraphDetails),
        });
}

FString UMCPythonHelper::RemoveEventDispatcher(
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
            {TEXT("dispatcher_id"), TEXT("allow_name_fallback"),
             TEXT("dispatcher_name"), TEXT("dispatcher_owner_id"),
             TEXT("dispatcher_type_path")},
            Error))
    {
        return Error;
    }
    FTargetRef Target;
    FResolvedTarget Resolved;
    if (!ParseNamedTarget(
            Request.ToSharedRef(),
            TEXT("dispatcher_id"),
            TEXT("dispatcher_name"),
            TEXT("dispatcher_owner_id"),
            TEXT("dispatcher_type_path"),
            Target,
            Error) ||
        !ResolveMemberTarget(
            Blueprint,
            ETargetKind::Variable,
            Target,
            TEXT("dispatcher_id"),
            Resolved,
            Error))
    {
        return Error;
    }
    FBPVariableDescription* Variable = Resolved.Variable;
    if (!Variable ||
        Variable->VarType.PinCategory != UEdGraphSchema_K2::PC_MCDelegate)
    {
        return Precondition(
            TEXT("dispatcher_id"),
            TEXT("The target is not an event dispatcher declared by this Blueprint."));
    }
    const FName DispatcherName = Variable->VarName;
    const FString DispatcherId =
        DescribeVariableTarget(Blueprint, *Variable).Id;
    TArray<UEdGraph*> MatchingGraphs;
    for (UEdGraph* Candidate : Blueprint->DelegateSignatureGraphs)
    {
        if (Candidate && Candidate->GetFName() == DispatcherName)
        {
            MatchingGraphs.Add(Candidate);
        }
    }
    if (MatchingGraphs.Num() != 1)
    {
        return Precondition(
            TEXT("dispatcher_id"),
            TEXT("The dispatcher must have exactly one same-name signature graph."));
    }
    UEdGraph* Graph = MatchingGraphs[0];
    const FString GraphId = DescribeGraphTarget(Blueprint, Graph).Id;

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython", "RemoveEventDispatcher", "Remove Event Dispatcher"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the dispatcher removal transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    Scope.Modify(Blueprint);
    Scope.Modify(Graph);
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        Scope.Modify(Node);
    }
    FBlueprintEditorUtils::RemoveGraph(
        Blueprint, Graph, EGraphRemoveFlags::MarkTransient);
    FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, DispatcherName);
    if (Blueprint->DelegateSignatureGraphs.Contains(Graph) ||
        FBlueprintEditorUtils::FindNewVariableIndex(
            Blueprint, DispatcherName) != INDEX_NONE)
    {
        return MutationFailure(
            Scope,
            Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("dispatcher_id"),
                TEXT("Unreal did not fully remove the event dispatcher."),
                TEXT("Inspect the Blueprint and retry.")));
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("dispatcher_id"), DispatcherId);
    Data->SetStringField(TEXT("dispatcher_name"), DispatcherName.ToString());
    Data->SetStringField(TEXT("signature_graph_id"), GraphId);
    const TSharedRef<FJsonObject> GraphDetails = MakeShared<FJsonObject>();
    GraphDetails->SetStringField(
        TEXT("dispatcher_name"), DispatcherName.ToString());
    const TSharedRef<FJsonObject> VariableDetails = MakeShared<FJsonObject>();
    VariableDetails->SetStringField(
        TEXT("dispatcher_name"), DispatcherName.ToString());
    return AuthoringSuccess(
        Blueprint,
        TEXT("Event dispatcher removed."),
        Data,
        {
            MakeChangeValue(TEXT("delete"), GraphId, GraphDetails),
            MakeChangeValue(TEXT("delete"), DispatcherId, VariableDetails),
        });
}

FString UMCPythonHelper::AddBlueprintInterface(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    if (!Blueprint)
    {
        return Precondition(TEXT("asset_path"), TEXT("Blueprint is required."));
    }
    if (!FBlueprintEditorUtils::DoesSupportImplementingInterfaces(Blueprint))
    {
        return Precondition(
            TEXT("asset_path"),
            TEXT("This Blueprint type does not support implemented interfaces."));
    }

    TSharedPtr<FJsonObject> Request;
    FString Error;
    if (!ParseRequestObject(RequestJson, Request, Error) ||
        !ValidateClosedRequest(
            Request.ToSharedRef(), {TEXT("interface_path")}, Error))
    {
        return Error;
    }
    FString InterfacePath;
    if (!RequiredString(
            Request.ToSharedRef(),
            TEXT("interface_path"),
            InterfacePath,
            Error))
    {
        return Error;
    }
    UClass* InterfaceClass = LoadObject<UClass>(nullptr, *InterfacePath);
    if (!InterfaceClass || InterfaceClass->GetPathName() != InterfacePath ||
        !InterfaceClass->HasAnyClassFlags(CLASS_Interface))
    {
        return Invalid(
            TEXT("interface_path"),
            TEXT("interface_path must resolve exactly to a reflected interface class."),
            TEXT("Use the full /Script/... or generated /Game/..._C class path."));
    }
    if (FBlueprintEditorUtils::ImplementsInterface(
            Blueprint, true, InterfaceClass))
    {
        return Conflict(
            TEXT("interface_path"),
            TEXT("The Blueprint already implements this interface directly or through a parent."));
    }

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython", "AddBlueprintInterface", "Add Blueprint Interface"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the interface implementation transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    Scope.Modify(Blueprint);
    if (!FBlueprintEditorUtils::ImplementNewInterface(
            Blueprint, InterfaceClass->GetClassPathName()))
    {
        return MutationFailure(
            Scope,
            Conflict(
                TEXT("interface_path"),
                TEXT("Unreal could not implement the interface because one or more functions conflict with this Blueprint.")));
    }

    FBPInterfaceDescription* AddedDescription = nullptr;
    for (FBPInterfaceDescription& Description :
         Blueprint->ImplementedInterfaces)
    {
        if (Description.Interface.Get() == InterfaceClass)
        {
            if (AddedDescription)
            {
                return MutationFailure(
                    Scope,
                    Failure(
                        TEXT("INTERNAL_ERROR"),
                        TEXT("interface_path"),
                        TEXT("Unreal created duplicate interface descriptions."),
                        TEXT("Inspect the Blueprint and retry.")));
            }
            AddedDescription = &Description;
        }
    }
    if (!AddedDescription)
    {
        return MutationFailure(
            Scope,
            Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("interface_path"),
                TEXT("Unreal reported success without creating an interface description."),
                TEXT("Inspect the Blueprint and retry.")));
    }

    const FString InterfaceId = TEXT("interface:") + InterfacePath;
    TArray<TSharedPtr<FJsonValue>> GraphIds;
    TArray<TSharedPtr<FJsonValue>> Changes;
    for (UEdGraph* Graph : AddedDescription->Graphs)
    {
        if (!Graph)
        {
            return MutationFailure(
                Scope,
                Failure(
                    TEXT("INTERNAL_ERROR"),
                    TEXT("interface_path"),
                    TEXT("The interface contains a null implementation graph."),
                    TEXT("Inspect the Blueprint and retry.")));
        }
        Scope.Modify(Graph);
        if (!Graph->GraphGuid.IsValid())
        {
            Graph->GraphGuid = FGuid::NewGuid();
        }
        const FString GraphId = DescribeGraphTarget(Blueprint, Graph).Id;
        GraphIds.Add(MakeShared<FJsonValueString>(GraphId));
        const TSharedRef<FJsonObject> GraphDetails = MakeShared<FJsonObject>();
        GraphDetails->SetStringField(TEXT("interface_path"), InterfacePath);
        GraphDetails->SetStringField(TEXT("graph_name"), Graph->GetName());
        Changes.Add(MakeChangeValue(TEXT("create"), GraphId, GraphDetails));
    }
    GraphIds.Sort([](
        const TSharedPtr<FJsonValue>& A,
        const TSharedPtr<FJsonValue>& B)
    {
        return A->AsString() < B->AsString();
    });
    const TSharedRef<FJsonObject> InterfaceDetails = MakeShared<FJsonObject>();
    InterfaceDetails->SetStringField(TEXT("interface_path"), InterfacePath);
    Changes.Insert(
        MakeChangeValue(TEXT("create"), InterfaceId, InterfaceDetails), 0);

    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("interface_id"), InterfaceId);
    Data->SetStringField(TEXT("interface_path"), InterfacePath);
    Data->SetArrayField(TEXT("implementation_graph_ids"), GraphIds);
    return AuthoringSuccess(
        Blueprint,
        TEXT("Blueprint interface added."),
        Data,
        Changes);
}

FString UMCPythonHelper::RemoveBlueprintInterface(
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
            Request.ToSharedRef(), {TEXT("interface_id")}, Error))
    {
        return Error;
    }
    FString InterfaceId;
    if (!RequiredString(
            Request.ToSharedRef(),
            TEXT("interface_id"),
            InterfaceId,
            Error))
    {
        return Error;
    }
    FTargetRef Target;
    Target.Id = InterfaceId;
    FResolvedTarget Resolved;
    if (!ResolveMemberTarget(
            Blueprint,
            ETargetKind::Interface,
            Target,
            TEXT("interface_id"),
            Resolved,
            Error))
    {
        return Error;
    }
    UClass* InterfaceClass = Cast<UClass>(Resolved.Object);
    int32 InterfaceIndex = INDEX_NONE;
    for (int32 Index = 0;
         Index < Blueprint->ImplementedInterfaces.Num();
         ++Index)
    {
        if (Blueprint->ImplementedInterfaces[Index].Interface.Get() ==
            InterfaceClass)
        {
            InterfaceIndex = Index;
            break;
        }
    }
    if (!InterfaceClass || InterfaceIndex == INDEX_NONE)
    {
        return Precondition(
            TEXT("interface_id"),
            TEXT("The target is not an interface directly implemented by this Blueprint."));
    }
    const FString InterfacePath = InterfaceClass->GetPathName();
    FBPInterfaceDescription& Description =
        Blueprint->ImplementedInterfaces[InterfaceIndex];
    const TArray<UEdGraph*> InterfaceGraphs = Description.Graphs;
    TArray<FString> InterfaceGraphIds;
    InterfaceGraphIds.Reserve(InterfaceGraphs.Num());
    for (UEdGraph* Graph : InterfaceGraphs)
    {
        if (!Graph)
        {
            return Precondition(
                TEXT("interface_id"),
                TEXT("The interface description contains an invalid implementation graph."));
        }
        InterfaceGraphIds.Add(DescribeGraphTarget(Blueprint, Graph).Id);
    }

    TArray<UK2Node_Event*> InterfaceEvents;
    TArray<UK2Node_Event*> AllEvents;
    FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, AllEvents);
    for (UK2Node_Event* Event : AllEvents)
    {
        if (Event &&
            Event->EventReference.GetMemberParentClass(
                Event->GetBlueprintClassFromNode()) == InterfaceClass)
        {
            InterfaceEvents.Add(Event);
        }
    }
    TArray<UEdGraph*> AllGraphs;
    Blueprint->GetAllGraphs(AllGraphs);

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython",
        "RemoveBlueprintInterface",
        "Remove Blueprint Interface"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the interface removal transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    Scope.Modify(Blueprint);
    for (UEdGraph* Graph : AllGraphs)
    {
        Scope.Modify(Graph);
        if (Graph)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                Scope.Modify(Node);
            }
        }
    }

    for (TFieldIterator<UFunction> FunctionIt(InterfaceClass);
         FunctionIt;
         ++FunctionIt)
    {
        UFunction* Function = *FunctionIt;
        if (Function &&
            Function->GetFName() != UEdGraphSchema_K2::FN_ExecuteUbergraphBase)
        {
            FBlueprintEditorUtils::RemoveInterfaceFunction(
                Blueprint, Description, Function, false);
        }
    }
    if (!Description.Graphs.IsEmpty())
    {
        return MutationFailure(
            Scope,
            Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("interface_id"),
                TEXT("Unreal did not remove every interface implementation graph."),
                TEXT("Inspect the Blueprint and retry.")));
    }
    for (UK2Node_Event* Event : InterfaceEvents)
    {
        if (Event && Event->GetGraph())
        {
            Event->GetGraph()->RemoveNode(Event);
        }
    }
    Blueprint->ImplementedInterfaces.RemoveAt(InterfaceIndex, 1);
    FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    if (Blueprint->ImplementedInterfaces.ContainsByPredicate(
            [InterfaceClass](const FBPInterfaceDescription& Candidate)
            {
                return Candidate.Interface.Get() == InterfaceClass;
            }))
    {
        return MutationFailure(
            Scope,
            Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("interface_id"),
                TEXT("Unreal did not remove the interface description."),
                TEXT("Inspect the Blueprint and retry.")));
    }

    TArray<TSharedPtr<FJsonValue>> GraphIdValues;
    TArray<TSharedPtr<FJsonValue>> Changes;
    for (const FString& GraphId : InterfaceGraphIds)
    {
        GraphIdValues.Add(MakeShared<FJsonValueString>(GraphId));
        const TSharedRef<FJsonObject> GraphDetails = MakeShared<FJsonObject>();
        GraphDetails->SetStringField(TEXT("interface_path"), InterfacePath);
        Changes.Add(MakeChangeValue(TEXT("delete"), GraphId, GraphDetails));
    }
    GraphIdValues.Sort([](
        const TSharedPtr<FJsonValue>& A,
        const TSharedPtr<FJsonValue>& B)
    {
        return A->AsString() < B->AsString();
    });
    const TSharedRef<FJsonObject> InterfaceDetails = MakeShared<FJsonObject>();
    InterfaceDetails->SetStringField(TEXT("interface_path"), InterfacePath);
    Changes.Add(
        MakeChangeValue(TEXT("delete"), InterfaceId, InterfaceDetails));

    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("interface_id"), InterfaceId);
    Data->SetStringField(TEXT("interface_path"), InterfacePath);
    Data->SetArrayField(TEXT("implementation_graph_ids"), GraphIdValues);
    return AuthoringSuccess(
        Blueprint,
        TEXT("Blueprint interface removed."),
        Data,
        Changes);
}
