// Copyright (c) 2025 GenOrca. All Rights Reserved.

#include "MCPythonHelper.h"
#include "MCPythonBlueprint2Internal.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Internationalization/Text.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/CoreNetTypes.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace
{
using namespace UE::MCPython::Blueprint2;

FString Failure(
    const FString& Code,
    const FString& Path,
    const FString& Message,
    const FString& Hint,
    const TSharedPtr<FJsonObject>& Details = nullptr)
{
    return SerializeResult(MakeFailure(
        Code, Path, Message, false, Hint, Details));
}

FString Invalid(const FString& Path, const FString& Message)
{
    return Failure(
        TEXT("INVALID_INPUT"),
        Path,
        Message,
        TEXT("Correct the variable request and retry."));
}

FString Conflict(const FString& Path, const FString& Message)
{
    return Failure(
        TEXT("CONFLICT"),
        Path,
        Message,
        TEXT("Choose a unique Blueprint member name and retry."));
}

FString Precondition(const FString& Path, const FString& Message)
{
    return Failure(
        TEXT("PRECONDITION_FAILED"),
        Path,
        Message,
        TEXT("Inspect the Blueprint again and select a locally declared variable."));
}

FString Unsupported(
    const FString& Path,
    const FString& Message,
    const FString& Capability)
{
    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetStringField(TEXT("capability"), Capability);
    return Failure(
        TEXT("UE_VERSION_UNSUPPORTED"),
        Path,
        Message,
        TEXT("Use an Actor or ActorComponent Blueprint and a supported variable type."),
        Details);
}

bool ParseRequest(
    const FString& RequestJson,
    const TSet<FString>& AllowedFields,
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
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair :
         OutRequest->Values)
    {
        if (!AllowedFields.Contains(Pair.Key))
        {
            OutFailure = Invalid(
                Pair.Key,
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
    const TSharedPtr<FJsonValue>* Value = Request->Values.Find(Field);
    if (!Value || !Value->IsValid() || (*Value)->Type != EJson::String)
    {
        OutFailure = Invalid(
            Field,
            FString::Printf(TEXT("'%s' must be a string."), *Field));
        return false;
    }
    OutValue = (*Value)->AsString();
    if (!bAllowEmpty && OutValue.IsEmpty())
    {
        OutFailure = Invalid(
            Field,
            FString::Printf(TEXT("'%s' must be non-empty."), *Field));
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
    return RequiredString(Request, Field, OutValue, OutFailure, true);
}

bool OptionalBool(
    const TSharedRef<FJsonObject>& Request,
    const FString& Field,
    bool& OutValue,
    bool& bOutProvided,
    FString& OutFailure)
{
    bOutProvided = Request->HasField(Field);
    if (!bOutProvided)
    {
        return true;
    }
    const TSharedPtr<FJsonValue>* Value = Request->Values.Find(Field);
    if (!Value || !Value->IsValid() || (*Value)->Type != EJson::Boolean)
    {
        OutFailure = Invalid(
            Field,
            FString::Printf(TEXT("'%s' must be a boolean."), *Field));
        return false;
    }
    OutValue = (*Value)->AsBool();
    return true;
}

bool ValidMemberName(
    const FString& Candidate,
    const FString& Path,
    FString& OutFailure)
{
    const auto IsLetter = [](const TCHAR Character)
    {
        return (Character >= TEXT('A') && Character <= TEXT('Z')) ||
            (Character >= TEXT('a') && Character <= TEXT('z'));
    };
    const auto IsDigit = [](const TCHAR Character)
    {
        return Character >= TEXT('0') && Character <= TEXT('9');
    };
    bool bValid = !Candidate.IsEmpty() &&
        (Candidate[0] == TEXT('_') || IsLetter(Candidate[0]));
    for (int32 Index = 1; bValid && Index < Candidate.Len(); ++Index)
    {
        bValid = Candidate[Index] == TEXT('_') || IsLetter(Candidate[Index]) ||
            IsDigit(Candidate[Index]);
    }
    if (!bValid ||
        Candidate.Len() > FKismetNameValidator::GetMaximumNameLength())
    {
        OutFailure = Invalid(
            Path,
            TEXT("Blueprint variable names must match "
                 "[A-Za-z_][A-Za-z0-9_]* and contain at most 100 characters."));
        return false;
    }
    return true;
}

bool ParseVariableTarget(
    const TSharedRef<FJsonObject>& Request,
    FTargetRef& OutTarget,
    FString& OutFailure)
{
    if (!RequiredString(
            Request, TEXT("variable_id"), OutTarget.Id, OutFailure))
    {
        return false;
    }
    if (Request->HasField(TEXT("allow_name_fallback")))
    {
        const TSharedPtr<FJsonValue>* Value =
            Request->Values.Find(TEXT("allow_name_fallback"));
        if (!Value || !Value->IsValid() || (*Value)->Type != EJson::Boolean)
        {
            OutFailure = Invalid(
                TEXT("allow_name_fallback"),
                TEXT("'allow_name_fallback' must be a boolean."));
            return false;
        }
        OutTarget.bAllowNameFallback = (*Value)->AsBool();
    }
    if (!OptionalString(
            Request, TEXT("variable_name"), OutTarget.Name, OutFailure) ||
        !OptionalString(
            Request,
            TEXT("variable_owner_id"),
            OutTarget.OwnerId,
            OutFailure) ||
        !OptionalString(
            Request,
            TEXT("variable_type_path"),
            OutTarget.TypePath,
            OutFailure))
    {
        return false;
    }
    if (!OutTarget.Name.IsEmpty() &&
        !ValidMemberName(OutTarget.Name, TEXT("variable_name"), OutFailure))
    {
        return false;
    }
    if (OutTarget.Id.StartsWith(TEXT("fallback:")) &&
        (!OutTarget.bAllowNameFallback || OutTarget.Name.IsEmpty() ||
         OutTarget.OwnerId.IsEmpty() || OutTarget.TypePath.IsEmpty()))
    {
        OutFailure = Invalid(
            TEXT("variable_id"),
            TEXT("An unstable variable id requires explicit, fully qualified "
                 "name fallback."));
        return false;
    }
    return true;
}

bool ResolveLocalVariable(
    UBlueprint* Blueprint,
    const FTargetRef& Target,
    FBPVariableDescription*& OutVariable,
    FString& OutFailure)
{
    FString ResolutionError;
    FResolvedTarget Resolved = ResolveTarget(
        Blueprint, ETargetKind::Variable, Target, ResolutionError);
    if (!Resolved.Variable)
    {
        OutFailure = Precondition(
            TEXT("variable_id"),
            ResolutionError.IsEmpty()
                ? TEXT("The variable is not declared by this Blueprint.")
                : ResolutionError);
        return false;
    }
    const int32 Index = Blueprint->NewVariables.IndexOfByPredicate(
        [Variable = Resolved.Variable](const FBPVariableDescription& Candidate)
        {
            return &Candidate == Variable;
        });
    if (Index == INDEX_NONE)
    {
        OutFailure = Precondition(
            TEXT("variable_id"),
            TEXT("Inherited and external variables cannot be mutated."));
        return false;
    }
    OutVariable = &Blueprint->NewVariables[Index];
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
            TEXT("The variable mutation failed and could not be rolled back."),
            TEXT("Inspect the Blueprint before retrying."));
    }
    return FailureJson;
}

TSharedPtr<FJsonValue> NullIfInvalid(const TSharedPtr<FJsonValue>& Value)
{
    return Value.IsValid() ? Value : MakeShared<FJsonValueNull>();
}

FText ParseStoredText(const FString& Stored)
{
    FText Result;
    if (!Stored.IsEmpty())
    {
        FTextStringHelper::ReadFromBuffer(*Stored, Result);
    }
    return Result;
}

TSharedPtr<FJsonValue> CanonicalStoredDefault(
    const FBPVariableDescription& Variable)
{
    const FText StoredText =
        Variable.VarType.PinCategory == UEdGraphSchema_K2::PC_Text
            ? ParseStoredText(Variable.DefaultValue)
            : FText::GetEmpty();
    return NullIfInvalid(SerializeDefaultValue(
        Variable.VarType,
        Variable.DefaultValue,
        nullptr,
        StoredText));
}

FString StoredDefault(const FNormalizedDefault& Normalized)
{
    if (Normalized.DefaultObject)
    {
        return Normalized.DefaultObject->GetPathName();
    }
    if (!Normalized.DefaultTextValue.IsEmpty())
    {
        FString Stored;
        FTextStringHelper::WriteToBuffer(Stored, Normalized.DefaultTextValue);
        return Stored;
    }
    return Normalized.DefaultValue;
}

TSharedRef<FJsonObject> VariableIdentity(
    UBlueprint* Blueprint,
    const FBPVariableDescription& Variable)
{
    const FTargetRef Target = DescribeVariableTarget(Blueprint, Variable);
    const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("id"), Target.Id);
    Result->SetStringField(TEXT("name"), Variable.VarName.ToString());
    Result->SetStringField(TEXT("owner_id"), Target.OwnerId);
    Result->SetStringField(TEXT("type_path"), Target.TypePath);
    return Result;
}

TSharedPtr<FJsonValue> Change(
    const FString& Kind,
    const FString& TargetId,
    const TSharedRef<FJsonObject>& Details)
{
    const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("kind"), Kind);
    Result->SetStringField(TEXT("target_id"), TargetId);
    Result->SetObjectField(TEXT("details"), Details);
    return MakeShared<FJsonValueObject>(Result);
}

void AddCompileNextAction(
    UBlueprint* Blueprint,
    const TSharedRef<FJsonObject>& Response)
{
    const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    const TSharedRef<FJsonObject> Next = MakeShared<FJsonObject>();
    Next->SetStringField(TEXT("domain"), TEXT("blueprint"));
    Next->SetStringField(TEXT("action"), TEXT("compile_blueprint"));
    Next->SetObjectField(TEXT("params"), Params);
    Response->SetArrayField(
        TEXT("next_actions"), {MakeShared<FJsonValueObject>(Next)});
}

FString VariableSuccess(
    UBlueprint* Blueprint,
    const FString& Summary,
    const TSharedRef<FJsonObject>& Data,
    const FString& ChangeKind,
    const FString& TargetId,
    const TSharedRef<FJsonObject>& Details)
{
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    const TSharedRef<FJsonObject> Response = MakeSuccess(Summary, Data);
    Response->SetArrayField(
        TEXT("changes"), {Change(ChangeKind, TargetId, Details)});
    AddCompileNextAction(Blueprint, Response);
    return SerializeResult(Response);
}

void ModifyBlueprintGraphs(UBlueprint* Blueprint, FMutationScope& Scope)
{
    if (!Blueprint)
    {
        return;
    }
    Scope.Modify(Blueprint);
    TArray<UEdGraph*> Graphs;
    Blueprint->GetAllGraphs(Graphs);
    for (UEdGraph* Graph : Graphs)
    {
        Scope.Modify(Graph);
        if (!Graph)
        {
            continue;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            Scope.Modify(Node);
        }
    }
}

bool ValidateUniqueVariableName(
    UBlueprint* Blueprint,
    const FString& Candidate,
    const FName Existing,
    const FString& Path,
    FString& OutFailure)
{
    FKismetNameValidator Validator(Blueprint, Existing);
    const EValidatorResult Result = Validator.IsValid(Candidate);
    if (Result != EValidatorResult::Ok)
    {
        const FString Message = INameValidatorInterface::GetErrorString(
            Candidate, Result);
        OutFailure =
            Result == EValidatorResult::AlreadyInUse ||
                Result == EValidatorResult::ExistingName ||
                Result == EValidatorResult::LocallyInUse
            ? Conflict(Path, Message)
            : Invalid(Path, Message);
        return false;
    }

    for (TObjectIterator<UBlueprint> Iterator; Iterator; ++Iterator)
    {
        UBlueprint* Child = *Iterator;
        if (!Child)
        {
            continue;
        }
        TArray<UBlueprint*> ParentHierarchy;
        UBlueprint::GetBlueprintHierarchyFromClass(
            Child->ParentClass, ParentHierarchy);
        if (!ParentHierarchy.Contains(Blueprint))
        {
            continue;
        }
        FKismetNameValidator ChildValidator(Child);
        const EValidatorResult ChildResult = ChildValidator.IsValid(Candidate);
        if (ChildResult != EValidatorResult::Ok)
        {
            OutFailure = Conflict(
                Path,
                FString::Printf(
                    TEXT("Variable name '%s' conflicts with loaded child Blueprint '%s'."),
                    *Candidate,
                    *Child->GetPathName()));
            return false;
        }
    }
    return true;
}

UEdGraph* FindDispatcherGraph(UBlueprint* Blueprint, const FName VariableName)
{
    UEdGraph* Result = nullptr;
    for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
    {
        if (Graph && Graph->GetFName() == VariableName)
        {
            if (Result)
            {
                return nullptr;
            }
            Result = Graph;
        }
    }
    return Result;
}

void RenameDispatcherGraph(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    const FName OldName,
    const FName NewName,
    FMutationScope& Scope)
{
    if (!Graph)
    {
        return;
    }
    Scope.Modify(Graph);
    FBlueprintEditorUtils::RenameGraph(Graph, NewName.ToString());

    TArray<UK2Node_BaseMCDelegate*> DelegateNodes;
    FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_BaseMCDelegate>(
        Blueprint, DelegateNodes);
    for (UK2Node_BaseMCDelegate* DelegateNode : DelegateNodes)
    {
        if (DelegateNode && DelegateNode->DelegateReference.IsSelfContext() &&
            DelegateNode->DelegateReference.GetMemberName() == OldName)
        {
            Scope.Modify(DelegateNode);
            DelegateNode->DelegateReference.SetSelfMember(NewName);
        }
    }
}

TSharedRef<FJsonObject> MetadataState(
    const FBPVariableDescription& Variable)
{
    const auto HasTrueMetadata = [&Variable](const FName Key)
    {
        return Variable.HasMetaData(Key) &&
            Variable.GetMetaData(Key).ToBool();
    };
    FString Tooltip;
    if (Variable.HasMetaData(FBlueprintMetadata::MD_Tooltip))
    {
        Tooltip = Variable.GetMetaData(FBlueprintMetadata::MD_Tooltip);
    }
    const TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
    State->SetStringField(TEXT("category"), Variable.Category.ToString());
    State->SetStringField(TEXT("tooltip"), Tooltip);
    State->SetBoolField(
        TEXT("visible"),
        (Variable.PropertyFlags & CPF_BlueprintVisible) != 0);
    State->SetBoolField(
        TEXT("instance_editable"),
        (Variable.PropertyFlags & CPF_DisableEditOnInstance) == 0);
    State->SetBoolField(
        TEXT("expose_on_spawn"),
        HasTrueMetadata(FBlueprintMetadata::MD_ExposeOnSpawn));
    State->SetBoolField(
        TEXT("save_game"), (Variable.PropertyFlags & CPF_SaveGame) != 0);
    State->SetBoolField(
        TEXT("cinematic"), (Variable.PropertyFlags & CPF_Interp) != 0);
    return State;
}

void SetBooleanMetadata(
    UBlueprint* Blueprint,
    const FName VariableName,
    const FName Key,
    bool bEnabled)
{
    if (bEnabled)
    {
        FBlueprintEditorUtils::SetBlueprintVariableMetaData(
            Blueprint, VariableName, nullptr, Key, TEXT("true"));
    }
    else
    {
        FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(
            Blueprint, VariableName, nullptr, Key);
    }
}

bool ParseCondition(
    const FString& Name,
    ELifetimeCondition& OutCondition)
{
    static const TMap<FString, ELifetimeCondition> Conditions = {
        {TEXT("none"), COND_None},
        {TEXT("initial_only"), COND_InitialOnly},
        {TEXT("owner_only"), COND_OwnerOnly},
        {TEXT("skip_owner"), COND_SkipOwner},
        {TEXT("simulated_only"), COND_SimulatedOnly},
        {TEXT("autonomous_only"), COND_AutonomousOnly},
        {TEXT("simulated_or_physics"), COND_SimulatedOrPhysics},
        {TEXT("initial_or_owner"), COND_InitialOrOwner},
        {TEXT("custom"), COND_Custom},
        {TEXT("replay_or_owner"), COND_ReplayOrOwner},
        {TEXT("replay_only"), COND_ReplayOnly},
        {TEXT("simulated_only_no_replay"), COND_SimulatedOnlyNoReplay},
        {TEXT("simulated_or_physics_no_replay"),
         COND_SimulatedOrPhysicsNoReplay},
        {TEXT("skip_replay"), COND_SkipReplay},
    };
    const ELifetimeCondition* Found = Conditions.Find(Name);
    if (!Found)
    {
        return false;
    }
    OutCondition = *Found;
    return true;
}

FString ConditionName(ELifetimeCondition Condition)
{
    switch (Condition)
    {
    case COND_None: return TEXT("none");
    case COND_InitialOnly: return TEXT("initial_only");
    case COND_OwnerOnly: return TEXT("owner_only");
    case COND_SkipOwner: return TEXT("skip_owner");
    case COND_SimulatedOnly: return TEXT("simulated_only");
    case COND_AutonomousOnly: return TEXT("autonomous_only");
    case COND_SimulatedOrPhysics: return TEXT("simulated_or_physics");
    case COND_InitialOrOwner: return TEXT("initial_or_owner");
    case COND_Custom: return TEXT("custom");
    case COND_ReplayOrOwner: return TEXT("replay_or_owner");
    case COND_ReplayOnly: return TEXT("replay_only");
    case COND_SimulatedOnlyNoReplay: return TEXT("simulated_only_no_replay");
    case COND_SimulatedOrPhysicsNoReplay:
        return TEXT("simulated_or_physics_no_replay");
    case COND_SkipReplay: return TEXT("skip_replay");
    default: return TEXT("unsupported");
    }
}

TSharedRef<FJsonObject> ReplicationState(
    const FBPVariableDescription& Variable)
{
    FString Mode = TEXT("none");
    if ((Variable.PropertyFlags & CPF_Net) != 0)
    {
        Mode = (Variable.PropertyFlags & CPF_RepNotify) != 0
            ? TEXT("rep_notify")
            : TEXT("replicated");
    }
    const TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
    State->SetStringField(TEXT("mode"), Mode);
    State->SetStringField(
        TEXT("notify_function_name"),
        Variable.RepNotifyFunc.IsNone()
            ? FString()
            : Variable.RepNotifyFunc.ToString());
    State->SetStringField(
        TEXT("condition"), ConditionName(Variable.ReplicationCondition));
    return State;
}

bool ValidateRepNotifyFunction(
    UBlueprint* Blueprint,
    const FString& FunctionName,
    FString& OutFailure)
{
    const FName Name(*FunctionName);
    if (const UFunction* Function = Blueprint->SkeletonGeneratedClass
            ? Blueprint->SkeletonGeneratedClass->FindFunctionByName(Name)
            : nullptr)
    {
        if (Function->NumParms == 0 && !Function->GetReturnProperty())
        {
            return true;
        }
        OutFailure = Invalid(
            TEXT("notify_function_name"),
            TEXT("A rep-notify function must have no parameters or return value."));
        return false;
    }

    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (!Graph || Graph->GetFName() != Name)
        {
            continue;
        }
        TArray<UK2Node_FunctionEntry*> Entries;
        TArray<UK2Node_FunctionResult*> Results;
        Graph->GetNodesOfClass(Entries);
        Graph->GetNodesOfClass(Results);
        const bool bNoInputParameters = Entries.Num() == 1 &&
            Entries[0]->UserDefinedPins.IsEmpty();
        bool bNoOutputs = Results.Num() <= 1;
        if (Results.Num() == 1)
        {
            bNoOutputs = Results[0]->UserDefinedPins.IsEmpty();
        }
        if (bNoInputParameters && bNoOutputs)
        {
            return true;
        }
        OutFailure = Invalid(
            TEXT("notify_function_name"),
            TEXT("A rep-notify function must have no parameters or return value."));
        return false;
    }
    OutFailure = Precondition(
        TEXT("notify_function_name"),
        FString::Printf(
            TEXT("Rep-notify function '%s' does not exist."), *FunctionName));
    return false;
}
}

FString UMCPythonHelper::AddBlueprintVariable(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    if (!Blueprint)
    {
        return Precondition(TEXT("asset_path"), TEXT("Blueprint is required."));
    }
    TSharedPtr<FJsonObject> Request;
    FString Error;
    if (!ParseRequest(
            RequestJson,
            {TEXT("asset_path"), TEXT("variable_name"), TEXT("variable_type")},
            Request,
            Error))
    {
        return Error;
    }
    FString VariableName;
    FString VariableType;
    if (!RequiredString(
            Request.ToSharedRef(),
            TEXT("variable_name"),
            VariableName,
            Error) ||
        !RequiredString(
            Request.ToSharedRef(),
            TEXT("variable_type"),
            VariableType,
            Error) ||
        !ValidMemberName(VariableName, TEXT("variable_name"), Error))
    {
        return Error;
    }

    static const TMap<FString, TPair<FName, FName>> Types = {
        {TEXT("int"), {UEdGraphSchema_K2::PC_Int, NAME_None}},
        {TEXT("byte"), {UEdGraphSchema_K2::PC_Byte, NAME_None}},
        {TEXT("bool"), {UEdGraphSchema_K2::PC_Boolean, NAME_None}},
        {TEXT("real"), {UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Double}},
        {TEXT("name"), {UEdGraphSchema_K2::PC_Name, NAME_None}},
        {TEXT("string"), {UEdGraphSchema_K2::PC_String, NAME_None}},
        {TEXT("text"), {UEdGraphSchema_K2::PC_Text, NAME_None}},
    };
    const TPair<FName, FName>* Type = Types.Find(VariableType);
    if (!Type)
    {
        return Invalid(
            TEXT("variable_type"), TEXT("Unsupported legacy variable type."));
    }
    if (!ValidateUniqueVariableName(
            Blueprint,
            VariableName,
            NAME_None,
            TEXT("variable_name"),
            Error))
    {
        return Error;
    }
    FEdGraphPinType PinType;
    PinType.PinCategory = Type->Key;
    PinType.PinSubCategory = Type->Value;

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython", "AddBlueprintVariable", "Add Blueprint Variable"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the variable transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    Scope.Modify(Blueprint);
    if (!FBlueprintEditorUtils::AddMemberVariable(
            Blueprint, FName(*VariableName), PinType))
    {
        return MutationFailure(
            Scope,
            Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("variable_name"),
                TEXT("Unreal rejected the Blueprint variable."),
                TEXT("Inspect the Blueprint and retry.")));
    }
    const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(
        Blueprint, FName(*VariableName));
    if (!Blueprint->NewVariables.IsValidIndex(Index))
    {
        return MutationFailure(
            Scope,
            Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("variable_name"),
                TEXT("Unreal created no addressable Blueprint variable."),
                TEXT("Inspect the Blueprint and retry.")));
    }
    const FBPVariableDescription& Variable = Blueprint->NewVariables[Index];
    const FTargetRef Target = DescribeVariableTarget(Blueprint, Variable);
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("variable_id"), Target.Id);
    Data->SetStringField(TEXT("variable_name"), VariableName);
    Data->SetStringField(TEXT("variable_type"), VariableType);
    const TSharedRef<FJsonObject> Details = VariableIdentity(Blueprint, Variable);
    const TSharedRef<FJsonObject> Response = MakeSuccess(
        TEXT("Blueprint variable added."), Data);
    Response->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Response->SetStringField(TEXT("variable_name"), VariableName);
    Response->SetStringField(TEXT("variable_type"), VariableType);
    Response->SetArrayField(TEXT("changes"), {
        Change(TEXT("create"), Target.Id, Details)});
    AddCompileNextAction(Blueprint, Response);
    return SerializeResult(Response);
}

FString UMCPythonHelper::SetBlueprintVariableFlags(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    if (!Blueprint)
    {
        return Precondition(TEXT("asset_path"), TEXT("Blueprint is required."));
    }
    TSharedPtr<FJsonObject> Request;
    FString Error;
    if (!ParseRequest(
            RequestJson,
            {TEXT("asset_path"), TEXT("variable_name"),
             TEXT("instance_editable"), TEXT("expose_on_spawn")},
            Request,
            Error))
    {
        return Error;
    }
    FString VariableName;
    bool bInstanceEditable = false;
    bool bExposeOnSpawn = false;
    bool bHasInstanceEditable = false;
    bool bHasExposeOnSpawn = false;
    if (!RequiredString(
            Request.ToSharedRef(),
            TEXT("variable_name"),
            VariableName,
            Error) ||
        !OptionalBool(
            Request.ToSharedRef(),
            TEXT("instance_editable"),
            bInstanceEditable,
            bHasInstanceEditable,
            Error) ||
        !OptionalBool(
            Request.ToSharedRef(),
            TEXT("expose_on_spawn"),
            bExposeOnSpawn,
            bHasExposeOnSpawn,
            Error))
    {
        return Error;
    }
    if (!bHasInstanceEditable && !bHasExposeOnSpawn)
    {
        return Invalid(
            TEXT("request"),
            TEXT("Provide instance_editable and/or expose_on_spawn."));
    }
    const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(
        Blueprint, FName(*VariableName));
    if (!Blueprint->NewVariables.IsValidIndex(Index))
    {
        return Precondition(
            TEXT("variable_name"),
            TEXT("The variable is not declared by this Blueprint."));
    }
    const FBPVariableDescription& Current = Blueprint->NewVariables[Index];
    const bool bFinalEditable = bHasInstanceEditable
        ? bInstanceEditable
        : (Current.PropertyFlags & CPF_DisableEditOnInstance) == 0;
    const bool bFinalExpose = bHasExposeOnSpawn
        ? bExposeOnSpawn
        : (Current.HasMetaData(FBlueprintMetadata::MD_ExposeOnSpawn) &&
           Current.GetMetaData(FBlueprintMetadata::MD_ExposeOnSpawn).ToBool());
    if (bFinalExpose && !bFinalEditable)
    {
        return Invalid(
            TEXT("expose_on_spawn"),
            TEXT("Expose on Spawn requires instance_editable=true."));
    }

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython",
        "SetBlueprintVariableFlags",
        "Set Blueprint Variable Flags"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the variable transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    Scope.Modify(Blueprint);
    Scope.Modify(Blueprint->SkeletonGeneratedClass);
    Scope.Modify(Blueprint->GeneratedClass);
    if (bHasInstanceEditable)
    {
        FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(
            Blueprint, FName(*VariableName), !bInstanceEditable);
    }
    if (bHasExposeOnSpawn)
    {
        SetBooleanMetadata(
            Blueprint,
            FName(*VariableName),
            FBlueprintMetadata::MD_ExposeOnSpawn,
            bExposeOnSpawn);
    }
    const FBPVariableDescription& After = Blueprint->NewVariables[Index];
    const FTargetRef Target = DescribeVariableTarget(Blueprint, After);
    const TSharedRef<FJsonObject> Applied = MakeShared<FJsonObject>();
    if (bHasInstanceEditable)
    {
        Applied->SetBoolField(TEXT("instance_editable"), bInstanceEditable);
    }
    if (bHasExposeOnSpawn)
    {
        Applied->SetBoolField(TEXT("expose_on_spawn"), bExposeOnSpawn);
    }
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("variable_id"), Target.Id);
    Data->SetStringField(TEXT("variable_name"), VariableName);
    Data->SetObjectField(TEXT("applied"), Applied);
    const TSharedRef<FJsonObject> Response = MakeSuccess(
        TEXT("Blueprint variable flags updated."), Data);
    Response->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Response->SetStringField(TEXT("variable_name"), VariableName);
    Response->SetObjectField(TEXT("applied"), Applied);
    Response->SetArrayField(TEXT("changes"), {
        Change(TEXT("update"), Target.Id, Applied)});
    AddCompileNextAction(Blueprint, Response);
    return SerializeResult(Response);
}

FString UMCPythonHelper::RenameBlueprintVariable(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    if (!Blueprint)
    {
        return Precondition(TEXT("asset_path"), TEXT("Blueprint is required."));
    }
    TSharedPtr<FJsonObject> Request;
    FString Error;
    if (!ParseRequest(
            RequestJson,
            {TEXT("variable_id"), TEXT("new_name"),
             TEXT("allow_name_fallback"), TEXT("variable_name"),
             TEXT("variable_owner_id"), TEXT("variable_type_path")},
            Request,
            Error))
    {
        return Error;
    }
    FTargetRef Target;
    FBPVariableDescription* Variable = nullptr;
    FString NewName;
    if (!ParseVariableTarget(Request.ToSharedRef(), Target, Error) ||
        !RequiredString(
            Request.ToSharedRef(), TEXT("new_name"), NewName, Error) ||
        !ValidMemberName(NewName, TEXT("new_name"), Error) ||
        !ResolveLocalVariable(Blueprint, Target, Variable, Error))
    {
        return Error;
    }
    const FName OldFName = Variable->VarName;
    const FName NewFName(*NewName);
    if (OldFName == NewFName)
    {
        return Invalid(
            TEXT("new_name"), TEXT("The new variable name is unchanged."));
    }
    if (!ValidateUniqueVariableName(
            Blueprint, NewName, OldFName, TEXT("new_name"), Error))
    {
        return Error;
    }
    UEdGraph* DispatcherGraph = nullptr;
    if (Variable->VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
    {
        DispatcherGraph = FindDispatcherGraph(Blueprint, OldFName);
        if (!DispatcherGraph)
        {
            return Precondition(
                TEXT("variable_id"),
                TEXT("The dispatcher variable has no unique signature graph."));
        }
        if (!DispatcherGraph->Rename(
                *NewName, DispatcherGraph->GetOuter(), REN_Test))
        {
            return Conflict(
                TEXT("new_name"),
                TEXT("The dispatcher graph cannot reserve the requested name."));
        }
    }

    const FString VariableId = DescribeVariableTarget(Blueprint, *Variable).Id;
    const TSharedRef<FJsonObject> Before = VariableIdentity(Blueprint, *Variable);
    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython",
        "RenameBlueprintVariable",
        "Rename Blueprint Variable"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the variable rename transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    ModifyBlueprintGraphs(Blueprint, Scope);
    TArray<UBlueprint*> Dependents;
    FBlueprintEditorUtils::FindDependentBlueprints(Blueprint, Dependents);
    for (UBlueprint* Dependent : Dependents)
    {
        ModifyBlueprintGraphs(Dependent, Scope);
    }

    Variable->VarName = NewFName;
    Variable->FriendlyName = FName::NameToDisplayString(
        NewName,
        Variable->VarType.PinCategory == UEdGraphSchema_K2::PC_Boolean);
    RenameDispatcherGraph(
        Blueprint, DispatcherGraph, OldFName, NewFName, Scope);
    FBlueprintEditorUtils::ReplaceVariableReferences(
        Blueprint, OldFName, NewFName);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    if (Variable->VarName != NewFName ||
        (DispatcherGraph && DispatcherGraph->GetFName() != NewFName))
    {
        return MutationFailure(
            Scope,
            Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("new_name"),
                TEXT("Unreal did not complete the variable rename."),
                TEXT("Inspect the Blueprint and retry.")));
    }
    const TSharedRef<FJsonObject> After = VariableIdentity(Blueprint, *Variable);
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("variable_id"), VariableId);
    Data->SetObjectField(TEXT("before"), Before);
    Data->SetObjectField(TEXT("after"), After);
    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetObjectField(TEXT("before"), Before);
    Details->SetObjectField(TEXT("after"), After);
    return VariableSuccess(
        Blueprint,
        TEXT("Blueprint variable renamed."),
        Data,
        TEXT("update"),
        VariableId,
        Details);
}
FString UMCPythonHelper::RemoveBlueprintVariable(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    if (!Blueprint)
    {
        return Precondition(TEXT("asset_path"), TEXT("Blueprint is required."));
    }
    TSharedPtr<FJsonObject> Request;
    FString Error;
    if (!ParseRequest(
            RequestJson,
            {TEXT("variable_id"), TEXT("allow_name_fallback"),
             TEXT("variable_name"), TEXT("variable_owner_id"),
             TEXT("variable_type_path")},
            Request,
            Error))
    {
        return Error;
    }
    FTargetRef Target;
    FBPVariableDescription* Variable = nullptr;
    if (!ParseVariableTarget(Request.ToSharedRef(), Target, Error) ||
        !ResolveLocalVariable(Blueprint, Target, Variable, Error))
    {
        return Error;
    }
    if (Variable->VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
    {
        return Precondition(
            TEXT("variable_id"),
            TEXT("Use remove_event_dispatcher for event dispatcher variables."));
    }
    const FName VariableName = Variable->VarName;
    const FString VariableId = DescribeVariableTarget(Blueprint, *Variable).Id;
    const TSharedRef<FJsonObject> Before = VariableIdentity(Blueprint, *Variable);

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython",
        "RemoveBlueprintVariable",
        "Remove Blueprint Variable"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the variable removal transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    ModifyBlueprintGraphs(Blueprint, Scope);
    FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VariableName);
    if (FBlueprintEditorUtils::FindNewVariableIndex(
            Blueprint, VariableName) != INDEX_NONE)
    {
        return MutationFailure(
            Scope,
            Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("variable_id"),
                TEXT("Unreal did not remove the Blueprint variable."),
                TEXT("Inspect the Blueprint and retry.")));
    }
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("variable_id"), VariableId);
    Data->SetStringField(TEXT("variable_name"), VariableName.ToString());
    Data->SetObjectField(TEXT("before"), Before);
    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetObjectField(TEXT("before"), Before);
    return VariableSuccess(
        Blueprint,
        TEXT("Blueprint variable removed."),
        Data,
        TEXT("delete"),
        VariableId,
        Details);
}

FString UMCPythonHelper::SetBlueprintVariableDefault(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    if (!Blueprint)
    {
        return Precondition(TEXT("asset_path"), TEXT("Blueprint is required."));
    }
    TSharedPtr<FJsonObject> Request;
    FString Error;
    if (!ParseRequest(
            RequestJson,
            {TEXT("variable_id"), TEXT("default")},
            Request,
            Error))
    {
        return Error;
    }
    FTargetRef Target;
    FBPVariableDescription* Variable = nullptr;
    if (!ParseVariableTarget(Request.ToSharedRef(), Target, Error) ||
        !ResolveLocalVariable(Blueprint, Target, Variable, Error))
    {
        return Error;
    }
    const TSharedPtr<FJsonValue>* RequestedDefault =
        Request->Values.Find(TEXT("default"));
    if (!RequestedDefault || !RequestedDefault->IsValid())
    {
        return Invalid(TEXT("default"), TEXT("'default' is required."));
    }
    FNormalizedDefault Normalized;
    FError DefaultError;
    if (!NormalizeDefaultValue(
            Variable->VarType,
            *RequestedDefault,
            Blueprint,
            Normalized,
            DefaultError,
            TEXT("default")))
    {
        return Failure(
            DefaultError.Code.IsEmpty()
                ? TEXT("INVALID_INPUT")
                : DefaultError.Code,
            DefaultError.Path,
            DefaultError.Message,
            DefaultError.Hint.IsEmpty()
                ? TEXT("Use a canonical value compatible with the variable type.")
                : DefaultError.Hint);
    }
    const FString NewStoredDefault = StoredDefault(Normalized);
    const TSharedPtr<FJsonValue> Before = CanonicalStoredDefault(*Variable);
    const TSharedPtr<FJsonValue> After = NullIfInvalid(SerializeDefaultValue(
        Variable->VarType,
        Normalized.DefaultValue,
        Normalized.DefaultObject,
        Normalized.DefaultTextValue));
    const FString VariableId = DescribeVariableTarget(Blueprint, *Variable).Id;

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython",
        "SetBlueprintVariableDefault",
        "Set Blueprint Variable Default"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the variable default transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    Scope.Modify(Blueprint);
    Variable->DefaultValue = NewStoredDefault;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("variable_id"), VariableId);
    Data->SetStringField(TEXT("variable_name"), Variable->VarName.ToString());
    Data->SetField(TEXT("before"), Before);
    Data->SetField(TEXT("after"), After);
    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetField(TEXT("before"), Before);
    Details->SetField(TEXT("after"), After);
    return VariableSuccess(
        Blueprint,
        TEXT("Blueprint variable default updated."),
        Data,
        TEXT("update"),
        VariableId,
        Details);
}

FString UMCPythonHelper::SetBlueprintVariableMetadata(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    if (!Blueprint)
    {
        return Precondition(TEXT("asset_path"), TEXT("Blueprint is required."));
    }
    TSharedPtr<FJsonObject> Request;
    FString Error;
    if (!ParseRequest(
            RequestJson,
            {TEXT("variable_id"), TEXT("metadata")},
            Request,
            Error))
    {
        return Error;
    }
    FTargetRef Target;
    FBPVariableDescription* Variable = nullptr;
    if (!ParseVariableTarget(Request.ToSharedRef(), Target, Error) ||
        !ResolveLocalVariable(Blueprint, Target, Variable, Error))
    {
        return Error;
    }
    const TSharedPtr<FJsonObject>* MetadataPointer = nullptr;
    if (!Request->TryGetObjectField(TEXT("metadata"), MetadataPointer) ||
        !MetadataPointer || !MetadataPointer->IsValid())
    {
        return Invalid(TEXT("metadata"), TEXT("'metadata' must be an object."));
    }
    const TSharedRef<FJsonObject> Metadata = MetadataPointer->ToSharedRef();
    static const TSet<FString> Allowed = {
        TEXT("category"), TEXT("tooltip"), TEXT("visible"),
        TEXT("instance_editable"), TEXT("expose_on_spawn"),
        TEXT("save_game"), TEXT("cinematic")};
    if (Metadata->Values.IsEmpty())
    {
        return Invalid(
            TEXT("metadata"), TEXT("Provide at least one metadata field."));
    }
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Metadata->Values)
    {
        if (!Allowed.Contains(Pair.Key))
        {
            const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
            TArray<TSharedPtr<FJsonValue>> AllowedValues;
            for (const FString& Name : Allowed)
            {
                AllowedValues.Add(MakeShared<FJsonValueString>(Name));
            }
            AllowedValues.Sort([](
                const TSharedPtr<FJsonValue>& A,
                const TSharedPtr<FJsonValue>& B)
            {
                return A->AsString() < B->AsString();
            });
            Details->SetArrayField(TEXT("allowed_metadata"), AllowedValues);
            return Failure(
                TEXT("INVALID_INPUT"),
                TEXT("metadata.") + Pair.Key,
                FString::Printf(
                    TEXT("Unknown variable metadata field '%s'."), *Pair.Key),
                TEXT("Use only the documented metadata allowlist."),
                Details);
        }
        const EJson Expected =
            Pair.Key == TEXT("category") || Pair.Key == TEXT("tooltip")
            ? EJson::String
            : EJson::Boolean;
        if (!Pair.Value.IsValid() || Pair.Value->Type != Expected)
        {
            return Invalid(
                TEXT("metadata.") + Pair.Key,
                FString::Printf(
                    TEXT("Metadata '%s' has the wrong JSON type."), *Pair.Key));
        }
    }

    const TSharedRef<FJsonObject> Before = MetadataState(*Variable);
    const bool bCurrentEditable =
        (Variable->PropertyFlags & CPF_DisableEditOnInstance) == 0;
    const bool bCurrentExpose =
        Variable->HasMetaData(FBlueprintMetadata::MD_ExposeOnSpawn) &&
        Variable->GetMetaData(FBlueprintMetadata::MD_ExposeOnSpawn).ToBool();
    bool bFinalEditable = bCurrentEditable;
    bool bFinalExpose = bCurrentExpose;
    if (Metadata->HasField(TEXT("instance_editable")))
    {
        bFinalEditable = Metadata->GetBoolField(TEXT("instance_editable"));
    }
    if (Metadata->HasField(TEXT("expose_on_spawn")))
    {
        bFinalExpose = Metadata->GetBoolField(TEXT("expose_on_spawn"));
    }
    if (bFinalExpose && !bFinalEditable)
    {
        return Invalid(
            TEXT("metadata.expose_on_spawn"),
            TEXT("Expose on Spawn requires instance_editable=true."));
    }
    const FString VariableId = DescribeVariableTarget(Blueprint, *Variable).Id;
    const FName VariableName = Variable->VarName;

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython",
        "SetBlueprintVariableMetadata",
        "Set Blueprint Variable Metadata"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the variable metadata transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    Scope.Modify(Blueprint);
    Scope.Modify(Blueprint->SkeletonGeneratedClass);
    Scope.Modify(Blueprint->GeneratedClass);
    if (Metadata->HasField(TEXT("category")))
    {
        FString Category = Metadata->GetStringField(TEXT("category"));
        if (Category.IsEmpty())
        {
            Category = UEdGraphSchema_K2::VR_DefaultCategory.ToString();
        }
        FBlueprintEditorUtils::SetBlueprintVariableCategory(
            Blueprint, VariableName, nullptr, FText::FromString(Category), true);
    }
    if (Metadata->HasField(TEXT("tooltip")))
    {
        FBlueprintEditorUtils::SetBlueprintVariableMetaData(
            Blueprint,
            VariableName,
            nullptr,
            FBlueprintMetadata::MD_Tooltip,
            Metadata->GetStringField(TEXT("tooltip")));
    }
    if (Metadata->HasField(TEXT("visible")))
    {
        if (Metadata->GetBoolField(TEXT("visible")))
        {
            Variable->PropertyFlags |= CPF_BlueprintVisible;
        }
        else
        {
            Variable->PropertyFlags &= ~CPF_BlueprintVisible;
        }
    }
    if (Metadata->HasField(TEXT("instance_editable")))
    {
        FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(
            Blueprint, VariableName, !bFinalEditable);
    }
    if (Metadata->HasField(TEXT("expose_on_spawn")))
    {
        SetBooleanMetadata(
            Blueprint,
            VariableName,
            FBlueprintMetadata::MD_ExposeOnSpawn,
            bFinalExpose);
    }
    if (Metadata->HasField(TEXT("save_game")))
    {
        FBlueprintEditorUtils::SetVariableSaveGameFlag(
            Blueprint,
            VariableName,
            Metadata->GetBoolField(TEXT("save_game")));
    }
    if (Metadata->HasField(TEXT("cinematic")))
    {
        FBlueprintEditorUtils::SetInterpFlag(
            Blueprint,
            VariableName,
            Metadata->GetBoolField(TEXT("cinematic")));
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    const TSharedRef<FJsonObject> After = MetadataState(*Variable);
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("variable_id"), VariableId);
    Data->SetStringField(TEXT("variable_name"), VariableName.ToString());
    Data->SetObjectField(TEXT("before"), Before);
    Data->SetObjectField(TEXT("after"), After);
    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetObjectField(TEXT("before"), Before);
    Details->SetObjectField(TEXT("after"), After);
    return VariableSuccess(
        Blueprint,
        TEXT("Blueprint variable metadata updated."),
        Data,
        TEXT("update"),
        VariableId,
        Details);
}

FString UMCPythonHelper::SetBlueprintVariableReplication(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    if (!Blueprint)
    {
        return Precondition(TEXT("asset_path"), TEXT("Blueprint is required."));
    }
    TSharedPtr<FJsonObject> Request;
    FString Error;
    if (!ParseRequest(
            RequestJson,
            {TEXT("variable_id"), TEXT("mode"),
             TEXT("notify_function_name"), TEXT("condition")},
            Request,
            Error))
    {
        return Error;
    }
    FTargetRef Target;
    FBPVariableDescription* Variable = nullptr;
    FString Mode;
    FString NotifyFunctionName;
    FString Condition = TEXT("none");
    if (!ParseVariableTarget(Request.ToSharedRef(), Target, Error) ||
        !RequiredString(Request.ToSharedRef(), TEXT("mode"), Mode, Error) ||
        !OptionalString(
            Request.ToSharedRef(),
            TEXT("notify_function_name"),
            NotifyFunctionName,
            Error) ||
        !OptionalString(
            Request.ToSharedRef(), TEXT("condition"), Condition, Error) ||
        !ResolveLocalVariable(Blueprint, Target, Variable, Error))
    {
        return Error;
    }
    if (Mode != TEXT("none") && Mode != TEXT("replicated") &&
        Mode != TEXT("rep_notify"))
    {
        return Invalid(
            TEXT("mode"),
            TEXT("Replication mode must be none, replicated, or rep_notify."));
    }
    ELifetimeCondition ParsedCondition = COND_None;
    if (!ParseCondition(Condition, ParsedCondition))
    {
        return Invalid(
            TEXT("condition"),
            TEXT("Replication condition is not in the supported allowlist."));
    }
    if (Mode == TEXT("none") && ParsedCondition != COND_None)
    {
        return Invalid(
            TEXT("condition"), TEXT("Mode none requires condition none."));
    }
    if (Mode == TEXT("rep_notify"))
    {
        if (NotifyFunctionName.IsEmpty())
        {
            return Invalid(
                TEXT("notify_function_name"),
                TEXT("rep_notify requires a non-empty notify function name."));
        }
        if (!ValidMemberName(
                NotifyFunctionName, TEXT("notify_function_name"), Error) ||
            !ValidateRepNotifyFunction(
                Blueprint, NotifyFunctionName, Error))
        {
            return Error;
        }
    }
    else if (!NotifyFunctionName.IsEmpty())
    {
        return Invalid(
            TEXT("notify_function_name"),
            TEXT("Only rep_notify accepts notify_function_name."));
    }

    const UClass* Parent = Blueprint->ParentClass;
    if (!Parent ||
        (!Parent->IsChildOf(AActor::StaticClass()) &&
         !Parent->IsChildOf(UActorComponent::StaticClass())))
    {
        return Unsupported(
            TEXT("mode"),
            TEXT("This Blueprint class does not support property replication."),
            TEXT("blueprint_variable_replication"));
    }
    if (Variable->VarType.IsSet() || Variable->VarType.IsMap() ||
        Variable->VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
    {
        return Unsupported(
            TEXT("variable_id"),
            TEXT("This variable type cannot be replicated in Unreal 5.7."),
            TEXT("blueprint_variable_replication_type"));
    }
    const FString VariableId = DescribeVariableTarget(Blueprint, *Variable).Id;
    const FName VariableName = Variable->VarName;
    const TSharedRef<FJsonObject> Before = ReplicationState(*Variable);

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython",
        "SetBlueprintVariableReplication",
        "Set Blueprint Variable Replication"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the variable replication transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    Scope.Modify(Blueprint);
    Variable->ReplicationCondition =
        Mode == TEXT("none") ? COND_None : ParsedCondition;
    if (Mode == TEXT("none"))
    {
        Variable->PropertyFlags &= ~(CPF_Net | CPF_RepNotify);
        FBlueprintEditorUtils::SetBlueprintVariableRepNotifyFunc(
            Blueprint, VariableName, NAME_None);
    }
    else if (Mode == TEXT("replicated"))
    {
        Variable->PropertyFlags |= CPF_Net;
        Variable->PropertyFlags &= ~CPF_RepNotify;
        FBlueprintEditorUtils::SetBlueprintVariableRepNotifyFunc(
            Blueprint, VariableName, NAME_None);
    }
    else
    {
        Variable->PropertyFlags |= CPF_Net | CPF_RepNotify;
        FBlueprintEditorUtils::SetBlueprintVariableRepNotifyFunc(
            Blueprint, VariableName, FName(*NotifyFunctionName));
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    const TSharedRef<FJsonObject> After = ReplicationState(*Variable);
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("variable_id"), VariableId);
    Data->SetStringField(TEXT("variable_name"), VariableName.ToString());
    Data->SetObjectField(TEXT("before"), Before);
    Data->SetObjectField(TEXT("after"), After);
    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetObjectField(TEXT("before"), Before);
    Details->SetObjectField(TEXT("after"), After);
    return VariableSuccess(
        Blueprint,
        TEXT("Blueprint variable replication updated."),
        Data,
        TEXT("update"),
        VariableId,
        Details);
}
