// Copyright (c) 2025 GenOrca. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"

class FScopedTransaction;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class USCS_Node;
struct FEdGraphPinType;

namespace UE::MCPython::Blueprint2
{
enum class ETargetKind : uint8 { Graph, Node, Pin, Variable, Component, Interface };

struct FTargetRef
{
    FString Id;
    FString OwnerId;
    FString Name;
    FString TypePath;
    bool bAllowNameFallback = false;
};

struct FResolvedTarget
{
    UObject* Object = nullptr;
    UEdGraph* Graph = nullptr;
    UEdGraphNode* Node = nullptr;
    UEdGraphPin* Pin = nullptr;
    FBPVariableDescription* Variable = nullptr;
    USCS_Node* Component = nullptr;
    FString Id;
    FString IdKind;
    bool bStable = false;
};

struct FPageRequest
{
    int32 Limit = 100;
    FString LastId;
    FString QueryDigest;
};

struct FRollbackResult
{
    bool bSucceeded = false;
    bool bDeferredToWorkflow = false;
    TArray<TSharedPtr<FJsonValue>> ResidualChanges;
};

struct FError
{
    FString Code;
    FString Path;
    FString Message;
    FString Hint;
};

struct FNormalizedDefault
{
    FString DefaultValue;
    TObjectPtr<UObject> DefaultObject = nullptr;
    FText DefaultTextValue;
};

class FMutationScope
{
public:
    explicit FMutationScope(const FText& Description);
    ~FMutationScope();
    bool IsValid() const;
    void Modify(UObject* Object);
    FRollbackResult Rollback();
private:
    TUniquePtr<FScopedTransaction> LocalTransaction;
    int32 TransactionIndex = INDEX_NONE;
    FGuid TransactionGuid;
    bool bWorkflowOwned = false;
};

FString MakeTargetId(ETargetKind Kind, const FGuid& Guid);
FString MakeQualifiedFallbackId(
    ETargetKind Kind,
    const FString& Owner,
    const FString& Name,
    const FString& TypePath);
FTargetRef DescribeGraphTarget(UBlueprint* Blueprint, const UEdGraph* Graph);
FTargetRef DescribeNodeTarget(UBlueprint* Blueprint, const UEdGraphNode* Node);
FTargetRef DescribePinTarget(UBlueprint* Blueprint, const UEdGraphPin* Pin);
FTargetRef DescribeVariableTarget(
    UBlueprint* Blueprint,
    const FBPVariableDescription& Variable);
FTargetRef DescribeComponentTarget(UBlueprint* Blueprint, const USCS_Node* Component);
FString MakeGraphTargetId(UBlueprint* Blueprint, const UEdGraph* Graph);
FString MakeNodeTargetId(UBlueprint* Blueprint, const UEdGraphNode* Node);
FString MakePinTargetId(UBlueprint* Blueprint, const UEdGraphPin* Pin);
bool ParseTargetId(
    const FString& Id,
    ETargetKind ExpectedKind,
    FGuid& OutGuid);
FResolvedTarget ResolveTarget(
    UBlueprint* Blueprint,
    ETargetKind Kind,
    const FTargetRef& Target,
    FString& OutError);

FString CanonicalQueryDigest(const TSharedRef<FJsonObject>& Query);
FString CanonicalJsonString(const TSharedPtr<FJsonValue>& Value);
FString Sha1Hex(const FString& Value);
FString EncodeCursor(
    const FString& AssetPath,
    const FPageRequest& Page);
bool DecodeCursor(
    const FString& Cursor,
    const FString& ExpectedAssetPath,
    const FString& ExpectedQueryDigest,
    FPageRequest& OutPage,
    FString& OutError);

TSharedRef<FJsonObject> MakeSuccess(
    const FString& Summary,
    const TSharedPtr<FJsonObject>& Data = nullptr);
TSharedRef<FJsonObject> MakeFailure(
    const FString& Code,
    const FString& Path,
    const FString& Message,
    bool bRetryable,
    const FString& Hint,
    const TSharedPtr<FJsonObject>& Details = nullptr);
FString SerializeResult(const TSharedRef<FJsonObject>& Result);
TSharedRef<FJsonObject> BuildCapabilities(UBlueprint* Blueprint);
bool IsSupportedBlueprintSelectionEditor(const FName& EditorName);
bool ParseTypeSpec(
    const TSharedRef<FJsonObject>& Spec,
    FEdGraphPinType& OutType,
    FError& OutError,
    const FString& Path = TEXT("params.type"),
    int32 Depth = 0);
TSharedRef<FJsonObject> SerializeTypeSpec(const FEdGraphPinType& Type);
TSharedPtr<FJsonValue> SerializeDefaultValue(
    const FEdGraphPinType& Type,
    const FString& DefaultValue,
    UObject* DefaultObject,
    const FText& DefaultTextValue);
bool NormalizeDefaultValue(
    const FEdGraphPinType& Type,
    const TSharedPtr<FJsonValue>& JsonValue,
    UObject* Owner,
    FNormalizedDefault& OutDefault,
    FError& OutError,
    const FString& Path);
void CollectStructuralHealthIssues(
    UBlueprint* Blueprint,
    TArray<TSharedPtr<FJsonValue>>& OutIssues);
void NormalizeHealthIssues(TArray<TSharedPtr<FJsonValue>>& Issues);
}
