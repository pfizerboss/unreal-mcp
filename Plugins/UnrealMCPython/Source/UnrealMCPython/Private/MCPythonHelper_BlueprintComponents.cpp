// Copyright (c) 2025 GenOrca. All Rights Reserved.

#include "MCPythonHelper.h"
#include "MCPythonBlueprint2Internal.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"

namespace
{
using namespace UE::MCPython::Blueprint2;

constexpr double MaxTransformValue = 1000000000.0;

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
        TEXT("Correct the Blueprint component request and retry."));
}

FString Conflict(const FString& Path, const FString& Message)
{
    return Failure(
        TEXT("CONFLICT"),
        Path,
        Message,
        TEXT("Inspect the component hierarchy and choose a non-conflicting target."));
}

FString Precondition(const FString& Path, const FString& Message)
{
    return Failure(
        TEXT("PRECONDITION_FAILED"),
        Path,
        Message,
        TEXT("Inspect the Blueprint components again and retry with current stable IDs."));
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
        TEXT("Use a supported component arrangement or a different Unreal version."),
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
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : OutRequest->Values)
    {
        if (!AllowedFields.Contains(Pair.Key))
        {
            OutFailure = Invalid(
                Pair.Key,
                FString::Printf(TEXT("Unknown request field '%s'."), *Pair.Key));
            return false;
        }
    }
    return true;
}

bool RequiredString(
    const TSharedRef<FJsonObject>& Request,
    const FString& Field,
    FString& OutValue,
    FString& OutFailure)
{
    const TSharedPtr<FJsonValue>* Value = Request->Values.Find(Field);
    if (!Value || !Value->IsValid() || (*Value)->Type != EJson::String)
    {
        OutFailure = Invalid(
            Field, FString::Printf(TEXT("'%s' must be a string."), *Field));
        return false;
    }
    OutValue = (*Value)->AsString();
    if (OutValue.IsEmpty())
    {
        OutFailure = Invalid(
            Field, FString::Printf(TEXT("'%s' must be non-empty."), *Field));
        return false;
    }
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
    if (!bValid || Candidate.Len() > FKismetNameValidator::GetMaximumNameLength())
    {
        OutFailure = Invalid(
            Path,
            TEXT("Blueprint component names must match "
                 "[A-Za-z_][A-Za-z0-9_]* and contain at most 100 characters."));
        return false;
    }
    return true;
}

bool RequireBlueprintAndSCS(
    UBlueprint* Blueprint,
    USimpleConstructionScript*& OutSCS,
    FString& OutFailure)
{
    if (!Blueprint)
    {
        OutFailure = Precondition(TEXT("asset_path"), TEXT("Blueprint is required."));
        return false;
    }
    OutSCS = Blueprint->SimpleConstructionScript;
    if (!OutSCS)
    {
        OutFailure = Precondition(
            TEXT("asset_path"),
            TEXT("The Blueprint has no SimpleConstructionScript."));
        return false;
    }
    return true;
}

bool ResolveStableComponent(
    UBlueprint* Blueprint,
    const FString& Id,
    const FString& Path,
    USCS_Node*& OutNode,
    FString& OutFailure)
{
    FTargetRef Target;
    Target.Id = Id;
    FString ResolutionError;
    const FResolvedTarget Resolved = ResolveTarget(
        Blueprint, ETargetKind::Component, Target, ResolutionError);
    if (!Resolved.Component || !Resolved.bStable)
    {
        OutFailure = Precondition(
            Path,
            ResolutionError.IsEmpty()
                ? TEXT("The component target no longer exists.")
                : ResolutionError);
        return false;
    }
    OutNode = Resolved.Component;
    return true;
}

USCS_Node* ResolveLegacyComponent(
    UBlueprint* Blueprint,
    const FString& ComponentNameOrId)
{
    if (!Blueprint || !Blueprint->SimpleConstructionScript)
    {
        return nullptr;
    }
    FGuid Guid;
    if (ParseTargetId(ComponentNameOrId, ETargetKind::Component, Guid))
    {
        return Blueprint->SimpleConstructionScript->FindSCSNodeByGuid(Guid);
    }
    for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
    {
        if (Node && Node->GetVariableName().ToString().Equals(
                ComponentNameOrId, ESearchCase::CaseSensitive))
        {
            return Node;
        }
    }
    return nullptr;
}

USCS_Node* FindParent(
    const USimpleConstructionScript* SCS,
    const USCS_Node* Node)
{
    if (!SCS || !Node)
    {
        return nullptr;
    }
    for (USCS_Node* Candidate : SCS->GetAllNodes())
    {
        if (Candidate && Candidate->GetChildNodes().Contains(Node))
        {
            return Candidate;
        }
    }
    return nullptr;
}

bool ContainsDescendant(const USCS_Node* Root, const USCS_Node* Candidate)
{
    if (!Root || !Candidate)
    {
        return false;
    }
    TArray<const USCS_Node*> Pending;
    TSet<const USCS_Node*> Visited;
    Pending.Add(Root);
    while (!Pending.IsEmpty())
    {
        const USCS_Node* Current = Pending.Pop(EAllowShrinking::No);
        if (!Current || Visited.Contains(Current))
        {
            continue;
        }
        Visited.Add(Current);
        for (const USCS_Node* Child : Current->GetChildNodes())
        {
            if (Child == Candidate)
            {
                return true;
            }
            Pending.Add(Child);
        }
    }
    return false;
}

void ModifyHierarchy(
    UBlueprint* Blueprint,
    USimpleConstructionScript* SCS,
    FMutationScope& Scope)
{
    Scope.Modify(Blueprint);
    Scope.Modify(SCS);
    for (USCS_Node* Node : SCS->GetAllNodes())
    {
        Scope.Modify(Node);
        Scope.Modify(Node ? Node->ComponentTemplate : nullptr);
    }
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

FString MutationFailure(FMutationScope& Scope, const FString& FailureJson)
{
    const FRollbackResult Rollback = Scope.Rollback();
    if (!Rollback.bSucceeded && !Rollback.bDeferredToWorkflow)
    {
        return Failure(
            TEXT("ROLLBACK_FAILED"),
            TEXT("transaction"),
            TEXT("The component mutation failed and could not be rolled back."),
            TEXT("Inspect the Blueprint before retrying."));
    }
    return FailureJson;
}

TSharedRef<FJsonObject> VectorJson(const FVector& Value)
{
    const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetNumberField(TEXT("x"), Value.X);
    Json->SetNumberField(TEXT("y"), Value.Y);
    Json->SetNumberField(TEXT("z"), Value.Z);
    return Json;
}

TSharedRef<FJsonObject> RotationJson(const FRotator& Value)
{
    const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetNumberField(TEXT("pitch"), Value.Pitch);
    Json->SetNumberField(TEXT("yaw"), Value.Yaw);
    Json->SetNumberField(TEXT("roll"), Value.Roll);
    return Json;
}

TSharedRef<FJsonObject> ComponentState(
    UBlueprint* Blueprint,
    USimpleConstructionScript* SCS,
    USCS_Node* Node)
{
    const FTargetRef Target = DescribeComponentTarget(Blueprint, Node);
    USCS_Node* Parent = FindParent(SCS, Node);
    const TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
    State->SetStringField(TEXT("component_id"), Target.Id);
    State->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
    State->SetStringField(
        TEXT("parent_id"),
        Parent ? DescribeComponentTarget(Blueprint, Parent).Id : FString());
    const int32 SiblingIndex = Parent
        ? Parent->GetChildNodes().IndexOfByKey(Node)
        : SCS->GetRootNodes().IndexOfByKey(Node);
    State->SetNumberField(TEXT("sibling_index"), SiblingIndex);
    if (const USceneComponent* Scene = Cast<USceneComponent>(Node->ComponentTemplate))
    {
        const TSharedRef<FJsonObject> Transform = MakeShared<FJsonObject>();
        Transform->SetObjectField(
            TEXT("location"), VectorJson(Scene->GetRelativeLocation()));
        Transform->SetObjectField(
            TEXT("rotation"), RotationJson(Scene->GetRelativeRotation()));
        Transform->SetObjectField(
            TEXT("scale"), VectorJson(Scene->GetRelativeScale3D()));
        State->SetObjectField(TEXT("transform"), Transform);
    }
    return State;
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

FString ComponentSuccess(
    UBlueprint* Blueprint,
    const FString& Summary,
    const FString& TargetId,
    const TSharedRef<FJsonObject>& Before,
    const TSharedRef<FJsonObject>& After)
{
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Data->SetStringField(TEXT("component_id"), TargetId);
    Data->SetObjectField(TEXT("before"), Before);
    Data->SetObjectField(TEXT("after"), After);
    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetObjectField(TEXT("before"), Before);
    Details->SetObjectField(TEXT("after"), After);
    const TSharedRef<FJsonObject> Response = MakeSuccess(Summary, Data);
    Response->SetArrayField(
        TEXT("changes"), {Change(TEXT("update"), TargetId, Details)});
    AddCompileNextAction(Blueprint, Response);
    return SerializeResult(Response);
}

bool ValidateUniqueComponentName(
    UBlueprint* Blueprint,
    const FString& Candidate,
    const FName Existing,
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
            ? Conflict(TEXT("new_name"), Message)
            : Invalid(TEXT("new_name"), Message);
        return false;
    }
    for (TObjectIterator<UBlueprint> Iterator; Iterator; ++Iterator)
    {
        UBlueprint* Child = *Iterator;
        if (!Child || !Child->ParentClass)
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
        if (ChildValidator.IsValid(Candidate) != EValidatorResult::Ok)
        {
            OutFailure = Conflict(
                TEXT("new_name"),
                FString::Printf(
                    TEXT("Component name '%s' conflicts with loaded child Blueprint '%s'."),
                    *Candidate,
                    *Child->GetPathName()));
            return false;
        }
    }
    return true;
}

bool ParseVector(
    const TSharedRef<FJsonObject>& Transform,
    const FString& Field,
    FVector& OutValue,
    bool& bOutProvided,
    FString& OutFailure)
{
    bOutProvided = Transform->HasField(Field);
    if (!bOutProvided)
    {
        return true;
    }
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Transform->TryGetArrayField(Field, Values) || !Values || Values->Num() != 3)
    {
        OutFailure = Invalid(
            TEXT("transform.") + Field,
            FString::Printf(TEXT("'%s' must contain exactly three numbers."), *Field));
        return false;
    }
    double Parsed[3] = {};
    for (int32 Index = 0; Index < 3; ++Index)
    {
        const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
        if (!Value.IsValid() || Value->Type != EJson::Number)
        {
            OutFailure = Invalid(
                FString::Printf(TEXT("transform.%s[%d]"), *Field, Index),
                TEXT("Transform vector members must be numbers."));
            return false;
        }
        Parsed[Index] = Value->AsNumber();
        if (!FMath::IsFinite(Parsed[Index]) ||
            FMath::Abs(Parsed[Index]) > MaxTransformValue)
        {
            OutFailure = Invalid(
                FString::Printf(TEXT("transform.%s[%d]"), *Field, Index),
                TEXT("Transform vector members must be finite and within +/-1000000000."));
            return false;
        }
    }
    OutValue = FVector(Parsed[0], Parsed[1], Parsed[2]);
    return true;
}
}

FString UMCPythonHelper::AddComponentToBlueprint(
    UBlueprint* Blueprint,
    const FString& ComponentClassPath,
    const FString& ComponentName,
    float LocationX, float LocationY, float LocationZ,
    float RotationPitch, float RotationYaw, float RotationRoll,
    const FString& ParentComponentName)
{
    USimpleConstructionScript* SCS = nullptr;
    FString Error;
    if (!RequireBlueprintAndSCS(Blueprint, SCS, Error))
    {
        return Error;
    }
    UClass* ComponentClass = LoadClass<UActorComponent>(
        nullptr, *ComponentClassPath);
    if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()) ||
        ComponentClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
    {
        return Invalid(
            TEXT("component_class_path"),
            FString::Printf(
                TEXT("Component class is unavailable or cannot be instantiated: %s"),
                *ComponentClassPath));
    }
    if (!ValidMemberName(ComponentName, TEXT("component_name"), Error) ||
        !ValidateUniqueComponentName(Blueprint, ComponentName, NAME_None, Error))
    {
        return Error;
    }
    USCS_Node* Parent = nullptr;
    if (!ParentComponentName.IsEmpty())
    {
        Parent = ResolveLegacyComponent(Blueprint, ParentComponentName);
        if (!Parent)
        {
            return Precondition(
                TEXT("parent_component_name"),
                FString::Printf(
                    TEXT("Parent component '%s' was not found."),
                    *ParentComponentName));
        }
    }

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython", "AddBlueprintComponent", "Add Blueprint Component"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the component transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    ModifyHierarchy(Blueprint, SCS, Scope);
    USCS_Node* NewNode = SCS->CreateNode(
        ComponentClass, FName(*ComponentName));
    if (!NewNode)
    {
        return MutationFailure(
            Scope,
            Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("component_name"),
                TEXT("Unreal could not create the SCS component node."),
                TEXT("Inspect the Blueprint and retry.")));
    }
    Scope.Modify(NewNode);
    Scope.Modify(NewNode->ComponentTemplate);
    if (USceneComponent* Scene = Cast<USceneComponent>(NewNode->ComponentTemplate))
    {
        Scene->SetRelativeLocation(FVector(LocationX, LocationY, LocationZ));
        Scene->SetRelativeRotation(FRotator(
            RotationPitch, RotationYaw, RotationRoll));
    }
    if (Parent)
    {
        Parent->AddChildNode(NewNode);
    }
    else
    {
        const TArray<USCS_Node*>& Roots = SCS->GetRootNodes();
        if (Roots.Num() > 0)
        {
            Roots[0]->AddChildNode(NewNode);
        }
        else
        {
            SCS->AddNode(NewNode);
        }
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    const FString ComponentId = DescribeComponentTarget(Blueprint, NewNode).Id;
    const TSharedRef<FJsonObject> State = ComponentState(Blueprint, SCS, NewNode);
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Data->SetStringField(TEXT("component_id"), ComponentId);
    Data->SetObjectField(TEXT("component"), State);
    const TSharedRef<FJsonObject> Response = MakeSuccess(
        TEXT("Blueprint component added."), Data);
    Response->SetArrayField(
        TEXT("changes"), {Change(TEXT("create"), ComponentId, State)});
    AddCompileNextAction(Blueprint, Response);
    Response->SetStringField(TEXT("node_name"), NewNode->GetVariableName().ToString());
    Response->SetStringField(TEXT("component_class"), ComponentClass->GetName());
    Response->SetStringField(
        TEXT("message"),
        FString::Printf(
            TEXT("Added '%s' (%s)."), *ComponentName, *ComponentClass->GetName()));
    return SerializeResult(Response);
}

FString UMCPythonHelper::RemoveComponentFromBlueprint(
    UBlueprint* Blueprint,
    const FString& ComponentName)
{
    USimpleConstructionScript* SCS = nullptr;
    FString Error;
    if (!RequireBlueprintAndSCS(Blueprint, SCS, Error))
    {
        return Error;
    }
    USCS_Node* Node = ResolveLegacyComponent(Blueprint, ComponentName);
    if (!Node)
    {
        return Precondition(
            TEXT("component_name"),
            FString::Printf(
                TEXT("Component '%s' was not found in the SCS."), *ComponentName));
    }
    const FString ComponentId = DescribeComponentTarget(Blueprint, Node).Id;
    const FString RemovedName = Node->GetVariableName().ToString();
    const FString RemovedClass = Node->ComponentTemplate
        ? Node->ComponentTemplate->GetClass()->GetName()
        : TEXT("Unknown");
    const TSharedRef<FJsonObject> Before = ComponentState(Blueprint, SCS, Node);

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython", "RemoveBlueprintComponent", "Remove Blueprint Component"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the component transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    ModifyHierarchy(Blueprint, SCS, Scope);
    SCS->RemoveNodeAndPromoteChildren(Node);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    if (SCS->GetAllNodes().Contains(Node))
    {
        return MutationFailure(
            Scope,
            Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("component_name"),
                TEXT("Unreal did not remove the SCS component."),
                TEXT("Inspect the Blueprint and retry.")));
    }

    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Data->SetStringField(TEXT("component_id"), ComponentId);
    Data->SetObjectField(TEXT("before"), Before);
    const TSharedRef<FJsonObject> Response = MakeSuccess(
        TEXT("Blueprint component removed."), Data);
    Response->SetArrayField(
        TEXT("changes"), {Change(TEXT("delete"), ComponentId, Before)});
    AddCompileNextAction(Blueprint, Response);
    Response->SetStringField(TEXT("component_name"), RemovedName);
    Response->SetStringField(TEXT("class"), RemovedClass);
    Response->SetStringField(
        TEXT("message"),
        FString::Printf(
            TEXT("Component '%s' (%s) removed."), *RemovedName, *RemovedClass));
    return SerializeResult(Response);
}

FString UMCPythonHelper::SetComponentProperty(
    UBlueprint* Blueprint,
    const FString& ComponentName,
    const FString& PropertyName,
    const FString& Value)
{
    USimpleConstructionScript* SCS = nullptr;
    FString Error;
    if (!RequireBlueprintAndSCS(Blueprint, SCS, Error))
    {
        return Error;
    }
    USCS_Node* Node = ResolveLegacyComponent(Blueprint, ComponentName);
    if (!Node)
    {
        return Precondition(
            TEXT("component_name"),
            FString::Printf(
                TEXT("Component '%s' was not found in the SCS."), *ComponentName));
    }
    UObject* Template = Node->ComponentTemplate;
    if (!Template)
    {
        return Precondition(
            TEXT("component_name"),
            TEXT("The component has no editable template."));
    }
    FProperty* Property = Template->GetClass()->FindPropertyByName(
        FName(*PropertyName));
    if (!Property)
    {
        return Invalid(
            TEXT("property_name"),
            FString::Printf(
                TEXT("Property '%s' was not found on component '%s'."),
                *PropertyName,
                *ComponentName));
    }
    if (Property->HasAnyPropertyFlags(CPF_EditConst))
    {
        return Precondition(
            TEXT("property_name"), TEXT("Read-only component properties cannot be changed."));
    }
    if (Property->HasAnyPropertyFlags(CPF_Transient))
    {
        return Precondition(
            TEXT("property_name"), TEXT("Transient component properties cannot be authored."));
    }
    if (CastField<FDelegateProperty>(Property) ||
        CastField<FMulticastDelegateProperty>(Property))
    {
        return Precondition(
            TEXT("property_name"), TEXT("Delegate component properties cannot be imported from text."));
    }

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython", "SetBlueprintComponentProperty", "Set Blueprint Component Property"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the component transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    ModifyHierarchy(Blueprint, SCS, Scope);
    void* ValueAddress = Property->ContainerPtrToValuePtr<void>(Template);
    const TCHAR* ImportResult = Property->ImportText_Direct(
        *Value, ValueAddress, Template, PPF_None);
    if (!ImportResult)
    {
        return MutationFailure(
            Scope,
            Invalid(
                TEXT("value"),
                FString::Printf(
                    TEXT("Failed to import property '%s' from '%s'."),
                    *PropertyName,
                    *Value)));
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    const FString ComponentId = DescribeComponentTarget(Blueprint, Node).Id;
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Data->SetStringField(TEXT("component_id"), ComponentId);
    Data->SetStringField(TEXT("property"), PropertyName);
    Data->SetStringField(TEXT("value"), Value);
    const TSharedRef<FJsonObject> Response = MakeSuccess(
        TEXT("Blueprint component property updated."), Data);
    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetStringField(TEXT("property"), PropertyName);
    Details->SetStringField(TEXT("value"), Value);
    Response->SetArrayField(
        TEXT("changes"), {Change(TEXT("update"), ComponentId, Details)});
    AddCompileNextAction(Blueprint, Response);
    Response->SetStringField(TEXT("component"), Node->GetVariableName().ToString());
    Response->SetStringField(TEXT("property"), PropertyName);
    Response->SetStringField(TEXT("value"), Value);
    return SerializeResult(Response);
}

FString UMCPythonHelper::RenameBlueprintComponent(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    USimpleConstructionScript* SCS = nullptr;
    FString Error;
    if (!RequireBlueprintAndSCS(Blueprint, SCS, Error))
    {
        return Error;
    }
    TSharedPtr<FJsonObject> Request;
    if (!ParseRequest(
            RequestJson,
            {TEXT("component_id"), TEXT("new_name")},
            Request,
            Error))
    {
        return Error;
    }
    FString ComponentId;
    FString NewName;
    USCS_Node* Node = nullptr;
    if (!RequiredString(Request.ToSharedRef(), TEXT("component_id"), ComponentId, Error) ||
        !RequiredString(Request.ToSharedRef(), TEXT("new_name"), NewName, Error) ||
        !ValidMemberName(NewName, TEXT("new_name"), Error) ||
        !ResolveStableComponent(Blueprint, ComponentId, TEXT("component_id"), Node, Error))
    {
        return Error;
    }
    const FName OldName = Node->GetVariableName();
    const FName NewFName(*NewName);
    if (OldName == NewFName)
    {
        return Invalid(TEXT("new_name"), TEXT("The new component name is unchanged."));
    }
    if (!ValidateUniqueComponentName(Blueprint, NewName, OldName, Error))
    {
        return Error;
    }
    const TSharedRef<FJsonObject> Before = ComponentState(Blueprint, SCS, Node);

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython", "RenameBlueprintComponent", "Rename Blueprint Component"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the component transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    ModifyHierarchy(Blueprint, SCS, Scope);
    ModifyBlueprintGraphs(Blueprint, Scope);
    TArray<UBlueprint*> Dependents;
    FBlueprintEditorUtils::FindDependentBlueprints(Blueprint, Dependents);
    for (UBlueprint* Dependent : Dependents)
    {
        ModifyBlueprintGraphs(Dependent, Scope);
    }
    FBlueprintEditorUtils::RenameComponentMemberVariable(
        Blueprint, Node, NewFName);
    if (Node->GetVariableName() != NewFName ||
        DescribeComponentTarget(Blueprint, Node).Id != ComponentId)
    {
        return MutationFailure(
            Scope,
            Failure(
                TEXT("INTERNAL_ERROR"),
                TEXT("new_name"),
                TEXT("Unreal did not complete the component rename without changing identity."),
                TEXT("Inspect the Blueprint and retry.")));
    }
    const TSharedRef<FJsonObject> After = ComponentState(Blueprint, SCS, Node);
    return ComponentSuccess(
        Blueprint, TEXT("Blueprint component renamed."), ComponentId, Before, After);
}

FString UMCPythonHelper::ReparentBlueprintComponent(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    USimpleConstructionScript* SCS = nullptr;
    FString Error;
    if (!RequireBlueprintAndSCS(Blueprint, SCS, Error))
    {
        return Error;
    }
    TSharedPtr<FJsonObject> Request;
    if (!ParseRequest(
            RequestJson,
            {TEXT("component_id"), TEXT("parent_component_id")},
            Request,
            Error))
    {
        return Error;
    }
    FString ComponentId;
    USCS_Node* Node = nullptr;
    if (!RequiredString(Request.ToSharedRef(), TEXT("component_id"), ComponentId, Error) ||
        !ResolveStableComponent(Blueprint, ComponentId, TEXT("component_id"), Node, Error))
    {
        return Error;
    }
    const TSharedPtr<FJsonValue>* ParentValue =
        Request->Values.Find(TEXT("parent_component_id"));
    if (!ParentValue || !ParentValue->IsValid())
    {
        return Invalid(
            TEXT("parent_component_id"),
            TEXT("'parent_component_id' must be a component ID or null for the SCS root."));
    }
    USCS_Node* NewParent = nullptr;
    if ((*ParentValue)->Type == EJson::String)
    {
        const FString ParentId = (*ParentValue)->AsString();
        if (ParentId.IsEmpty() ||
            !ResolveStableComponent(
                Blueprint,
                ParentId,
                TEXT("parent_component_id"),
                NewParent,
                Error))
        {
            return ParentId.IsEmpty()
                ? Invalid(
                    TEXT("parent_component_id"),
                    TEXT("Use null, not an empty string, for the SCS root."))
                : Error;
        }
    }
    else if ((*ParentValue)->Type != EJson::Null)
    {
        return Invalid(
            TEXT("parent_component_id"),
            TEXT("'parent_component_id' must be a component ID or null for the SCS root."));
    }
    USCS_Node* OldParent = FindParent(SCS, Node);
    if (NewParent == Node)
    {
        return Conflict(
            TEXT("parent_component_id"), TEXT("A component cannot parent itself."));
    }
    if (NewParent && ContainsDescendant(Node, NewParent))
    {
        return Conflict(
            TEXT("parent_component_id"),
            TEXT("Reparenting beneath a descendant would create an SCS cycle."));
    }
    if (OldParent == NewParent &&
        (NewParent || SCS->GetRootNodes().Contains(Node)))
    {
        return Invalid(
            TEXT("parent_component_id"), TEXT("The requested component parent is unchanged."));
    }
    const TSharedRef<FJsonObject> Before = ComponentState(Blueprint, SCS, Node);

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython", "ReparentBlueprintComponent", "Reparent Blueprint Component"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the component transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    ModifyHierarchy(Blueprint, SCS, Scope);
    if (OldParent)
    {
        OldParent->RemoveChildNode(Node, true);
    }
    else
    {
        SCS->RemoveNode(Node, false);
    }
    if (NewParent)
    {
        NewParent->AddChildNode(Node, true);
    }
    else
    {
        SCS->AddNode(Node);
        SCS->ValidateSceneRootNodes();
    }
    if (FindParent(SCS, Node) != NewParent ||
        (!NewParent && !SCS->GetRootNodes().Contains(Node)))
    {
        return MutationFailure(
            Scope,
            Failure(
                TEXT("VERIFICATION_FAILED"),
                TEXT("parent_component_id"),
                TEXT("Unreal did not retain the requested component parent."),
                TEXT("Inspect the SCS hierarchy and retry.")));
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    const TSharedRef<FJsonObject> After = ComponentState(Blueprint, SCS, Node);
    return ComponentSuccess(
        Blueprint, TEXT("Blueprint component reparented."), ComponentId, Before, After);
}

FString UMCPythonHelper::ReorderBlueprintComponent(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    USimpleConstructionScript* SCS = nullptr;
    FString Error;
    if (!RequireBlueprintAndSCS(Blueprint, SCS, Error))
    {
        return Error;
    }
    TSharedPtr<FJsonObject> Request;
    if (!ParseRequest(
            RequestJson,
            {TEXT("component_id"), TEXT("sibling_index")},
            Request,
            Error))
    {
        return Error;
    }
    FString ComponentId;
    USCS_Node* Node = nullptr;
    if (!RequiredString(Request.ToSharedRef(), TEXT("component_id"), ComponentId, Error) ||
        !ResolveStableComponent(Blueprint, ComponentId, TEXT("component_id"), Node, Error))
    {
        return Error;
    }
    const TSharedPtr<FJsonValue>* IndexValue = Request->Values.Find(TEXT("sibling_index"));
    if (!IndexValue || !IndexValue->IsValid() || (*IndexValue)->Type != EJson::Number)
    {
        return Invalid(TEXT("sibling_index"), TEXT("'sibling_index' must be an integer."));
    }
    const double RequestedNumber = (*IndexValue)->AsNumber();
    if (!FMath::IsFinite(RequestedNumber) ||
        RequestedNumber < 0.0 ||
        RequestedNumber > static_cast<double>(MAX_int32) ||
        FMath::FloorToDouble(RequestedNumber) != RequestedNumber)
    {
        return Invalid(
            TEXT("sibling_index"),
            TEXT("'sibling_index' must be a non-negative 32-bit integer."));
    }
    const int32 RequestedIndex = static_cast<int32>(RequestedNumber);
    USCS_Node* Parent = FindParent(SCS, Node);
    TArray<USCS_Node*> Ordered = Parent
        ? Parent->GetChildNodes()
        : SCS->GetRootNodes();
    const int32 OldIndex = Ordered.IndexOfByKey(Node);
    if (OldIndex == INDEX_NONE)
    {
        return Precondition(
            TEXT("component_id"),
            TEXT("The component is not present in its expected sibling collection."));
    }
    if (RequestedIndex >= Ordered.Num())
    {
        return Invalid(
            TEXT("sibling_index"),
            FString::Printf(
                TEXT("'sibling_index' must be between 0 and %d."), Ordered.Num() - 1));
    }
    if (RequestedIndex == OldIndex)
    {
        return Invalid(TEXT("sibling_index"), TEXT("The requested sibling index is unchanged."));
    }
    const TSharedRef<FJsonObject> Before = ComponentState(Blueprint, SCS, Node);
    Ordered.RemoveAt(OldIndex);
    Ordered.Insert(Node, RequestedIndex);

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython", "ReorderBlueprintComponent", "Reorder Blueprint Component"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the component transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    ModifyHierarchy(Blueprint, SCS, Scope);
    if (Parent)
    {
        const TArray<USCS_Node*> Existing = Parent->GetChildNodes();
        for (USCS_Node* Child : Existing)
        {
            Parent->RemoveChildNode(Child, false);
        }
        for (USCS_Node* Child : Ordered)
        {
            Parent->AddChildNode(Child, false);
        }
    }
    else
    {
        const TArray<USCS_Node*> Existing = SCS->GetRootNodes();
        for (USCS_Node* Root : Existing)
        {
            SCS->RemoveNode(Root, false);
        }
        for (USCS_Node* Root : Ordered)
        {
            SCS->AddNode(Root);
        }
        SCS->ValidateSceneRootNodes();
    }
    const TArray<USCS_Node*>& ResultOrder = Parent
        ? Parent->GetChildNodes()
        : SCS->GetRootNodes();
    if (ResultOrder != Ordered)
    {
        const FString Result = Parent
            ? Failure(
                TEXT("VERIFICATION_FAILED"),
                TEXT("sibling_index"),
                TEXT("Unreal did not retain the requested child component order."),
                TEXT("Inspect the SCS hierarchy and retry."))
            : Unsupported(
                TEXT("sibling_index"),
                TEXT("This Unreal version normalized the requested root component order."),
                TEXT("root_component_reorder"));
        return MutationFailure(Scope, Result);
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    const TSharedRef<FJsonObject> After = ComponentState(Blueprint, SCS, Node);
    return ComponentSuccess(
        Blueprint, TEXT("Blueprint component reordered."), ComponentId, Before, After);
}

FString UMCPythonHelper::SetBlueprintComponentTransform(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    USimpleConstructionScript* SCS = nullptr;
    FString Error;
    if (!RequireBlueprintAndSCS(Blueprint, SCS, Error))
    {
        return Error;
    }
    TSharedPtr<FJsonObject> Request;
    if (!ParseRequest(
            RequestJson,
            {TEXT("component_id"), TEXT("transform")},
            Request,
            Error))
    {
        return Error;
    }
    FString ComponentId;
    USCS_Node* Node = nullptr;
    if (!RequiredString(Request.ToSharedRef(), TEXT("component_id"), ComponentId, Error) ||
        !ResolveStableComponent(Blueprint, ComponentId, TEXT("component_id"), Node, Error))
    {
        return Error;
    }
    const TSharedPtr<FJsonObject>* TransformPtr = nullptr;
    if (!Request->TryGetObjectField(TEXT("transform"), TransformPtr) ||
        !TransformPtr || !TransformPtr->IsValid())
    {
        return Invalid(TEXT("transform"), TEXT("'transform' must be an object."));
    }
    const TSharedRef<FJsonObject> Transform = (*TransformPtr).ToSharedRef();
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Transform->Values)
    {
        if (Pair.Key != TEXT("location") &&
            Pair.Key != TEXT("rotation") &&
            Pair.Key != TEXT("scale"))
        {
            return Invalid(
                TEXT("transform.") + Pair.Key,
                FString::Printf(
                    TEXT("Unknown transform field '%s'."), *Pair.Key));
        }
    }
    FVector Location = FVector::ZeroVector;
    FVector Rotation = FVector::ZeroVector;
    FVector Scale = FVector::OneVector;
    bool bHasLocation = false;
    bool bHasRotation = false;
    bool bHasScale = false;
    if (!ParseVector(Transform, TEXT("location"), Location, bHasLocation, Error) ||
        !ParseVector(Transform, TEXT("rotation"), Rotation, bHasRotation, Error) ||
        !ParseVector(Transform, TEXT("scale"), Scale, bHasScale, Error))
    {
        return Error;
    }
    if (!bHasLocation && !bHasRotation && !bHasScale)
    {
        return Invalid(
            TEXT("transform"),
            TEXT("Provide at least one of location, rotation, or scale."));
    }
    USceneComponent* Scene = Cast<USceneComponent>(Node->ComponentTemplate);
    if (!Scene)
    {
        return Precondition(
            TEXT("component_id"),
            TEXT("Relative transforms are supported only for scene component templates."));
    }
    const TSharedRef<FJsonObject> Before = ComponentState(Blueprint, SCS, Node);

    FMutationScope Scope(NSLOCTEXT(
        "UnrealMCPython", "SetBlueprintComponentTransform", "Set Blueprint Component Transform"));
    if (!Scope.IsValid())
    {
        return Failure(
            TEXT("TRANSACTION_FAILED"),
            TEXT("transaction"),
            TEXT("Unreal could not start the component transaction."),
            TEXT("Finish the active editor operation and retry."));
    }
    ModifyHierarchy(Blueprint, SCS, Scope);
    if (bHasLocation)
    {
        Scene->SetRelativeLocation(Location);
    }
    if (bHasRotation)
    {
        Scene->SetRelativeRotation(FRotator(
            Rotation.X, Rotation.Y, Rotation.Z));
    }
    if (bHasScale)
    {
        Scene->SetRelativeScale3D(Scale);
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    const TSharedRef<FJsonObject> After = ComponentState(Blueprint, SCS, Node);
    return ComponentSuccess(
        Blueprint, TEXT("Blueprint component transform updated."), ComponentId, Before, After);
}
