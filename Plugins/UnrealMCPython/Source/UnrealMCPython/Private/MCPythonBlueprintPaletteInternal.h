// Copyright (c) 2025 GenOrca. All Rights Reserved.
#pragma once

#include "MCPythonBlueprint2Internal.h"

#include "BlueprintNodeSpawner.h"
#include "Containers/ArrayView.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

namespace UE::MCPython::Blueprint2::Palette
{
struct FPaletteFilters
{
    TSet<FString> ActionKinds;
    TArray<FString> CategoryPrefixes;
    TSet<FString> OwnerPaths;
    bool bPureOnly = false;
};

struct FPaletteBindingCandidate
{
    FString ObjectPath;
    FString ClassPath;
};

struct FPaletteCandidate
{
    const UObject* Owner = nullptr;
    const UBlueprintNodeSpawner* Spawner = nullptr;
    IBlueprintNodeBinder::FBindingSet Bindings;
    TArray<FPaletteBindingCandidate> BindingDetails;
    FString CandidateKey;
    FString SpawnerSignature;
    FString OwnerPath;
    FString MemberPath;
    FString NodeClassPath;
    FString Title;
    FString Category;
    FString Tooltip;
    FString DocumentationLink;
    FString DocumentationExcerpt;
    TArray<FString> Keywords;
    FString ActionKind;
    TOptional<bool> bPure;
    int32 Score = 0;
    FString SortKey;
};

bool ParseStoredFilters(
    const FString& FiltersJson,
    FPaletteFilters& OutFilters,
    FError& OutError);
bool ResolveStableGraph(
    UBlueprint* Blueprint,
    const FString& GraphId,
    UEdGraph*& OutGraph,
    FError& OutError,
    bool bStaleIsPrecondition);
bool BuildCandidates(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    TConstArrayView<UEdGraphPin*> ContextPins,
    const FString& Query,
    const FPaletteFilters& Filters,
    TArray<FPaletteCandidate>& OutCandidates);
FString ResultDigest(const TArray<FPaletteCandidate>& Candidates);
const FPaletteCandidate* FindExactCandidate(
    const TArray<FPaletteCandidate>& Candidates,
    const FPaletteActionRecord& Record);
bool ResolveDynamicBindingObjects(
    const FString& ActionId,
    const TArray<FString>& BindingIds,
    TArray<FPaletteBindingRecord>& OutRecords,
    IBlueprintNodeBinder::FBindingSet& OutBindings,
    FError& OutError);
UEdGraphNode* GetBoundTemplateNode(
    const FPaletteCandidate& Candidate,
    UEdGraph* Graph,
    const IBlueprintNodeBinder::FBindingSet& Bindings);
}
