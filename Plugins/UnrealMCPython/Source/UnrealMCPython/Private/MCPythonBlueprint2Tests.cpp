// Copyright (c) 2025 GenOrca. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "MCPythonBlueprint2Internal.h"
#include "MCPythonHelper.h"

#include "Dom/JsonObject.h"
#include "AI/Navigation/NavAgentInterface.h"
#include "Audio/SoundSubmixWidgetInterface.h"
#include "Components/SceneComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphNodeUtils.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/EngineTypes.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/Texture2D.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Interface.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

namespace UE::MCPython::Blueprint2
{
FTargetRef DescribeGraphTarget(UBlueprint* Blueprint, const UEdGraph* Graph);
FTargetRef DescribeNodeTarget(UBlueprint* Blueprint, const UEdGraphNode* Node);
FTargetRef DescribePinTarget(UBlueprint* Blueprint, const UEdGraphPin* Pin);
FTargetRef DescribeVariableTarget(
    UBlueprint* Blueprint,
    const FBPVariableDescription& Variable);
bool IsSupportedBlueprintSelectionEditor(const FName& EditorName);
}

namespace
{
template <typename TObjectType>
TObjectType* FindOrCreateFixture(UObject* Outer, const TCHAR* Name)
{
    if (TObjectType* Existing = FindObject<TObjectType>(Outer, Name))
    {
        return Existing;
    }
    return NewObject<TObjectType>(Outer, FName(Name), RF_Transient);
}

UBlueprint* MakeBlueprintFixture(const TCHAR* PackageName)
{
    UPackage* Package = CreatePackage(PackageName);
    return FindOrCreateFixture<UBlueprint>(Package, TEXT("BP_StableIds"));
}

UEdGraph* MakeGraphFixture(
    UBlueprint* Blueprint,
    const TCHAR* Name,
    UClass* SchemaClass)
{
    UEdGraph* Graph = FindOrCreateFixture<UEdGraph>(Blueprint, Name);
    Graph->Schema = SchemaClass;
    Graph->GraphGuid.Invalidate();
    return Graph;
}

template <typename TNodeType>
TNodeType* MakeNodeFixture(UEdGraph* Graph, const TCHAR* Name)
{
    TNodeType* Node = FindOrCreateFixture<TNodeType>(Graph, Name);
    Node->NodeGuid.Invalidate();
    return Node;
}

UEdGraphPin* MakePinFixture(
    UEdGraphNode* Node,
    const TCHAR* Name,
    const FName Category)
{
    UEdGraphPin* Pin = Node->CreatePin(EGPD_Input, Category, Name);
    Pin->PinId.Invalidate();
    return Pin;
}

TSharedPtr<FJsonObject> ParseJsonObject(const FString& Json)
{
    TSharedPtr<FJsonObject> Result;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    return FJsonSerializer::Deserialize(Reader, Result) ? Result : nullptr;
}

FString SerializeJsonObject(const TSharedRef<FJsonObject>& Object)
{
    FString Result;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
    FJsonSerializer::Serialize(Object, Writer);
    return Result;
}

void CleanupFixturePackages(const TArray<UPackage*>& Packages)
{
    for (UPackage* Package : Packages)
    {
        if (!Package)
        {
            continue;
        }
        TArray<UObject*> Objects;
        GetObjectsWithOuter(Package, Objects, true);
        for (int32 Index = Objects.Num() - 1; Index >= 0; --Index)
        {
            Objects[Index]->ClearFlags(RF_Public | RF_Standalone);
            Objects[Index]->MarkAsGarbage();
        }
        Package->SetDirtyFlag(false);
        Package->ClearFlags(RF_Public | RF_Standalone);
        Package->MarkAsGarbage();
    }
    CollectGarbage(RF_NoFlags);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprint2TargetIdsTest,
    "UnrealMCPython.Blueprint2.TargetIds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprint2TargetIdsTest::RunTest(const FString& Parameters)
{
    using namespace UE::MCPython::Blueprint2;

    (void)Parameters;

    const FString TargetIdsRoot = FString::Printf(
        TEXT("/Game/__MCPTests/Blueprint2_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TArray<UPackage*> FixturePackages;
    ON_SCOPE_EXIT
    {
        CleanupFixturePackages(FixturePackages);
    };
    auto MakeTrackedBlueprint = [&TargetIdsRoot, &FixturePackages](
        const TCHAR* Leaf)
    {
        UBlueprint* Result = MakeBlueprintFixture(
            *FString::Printf(TEXT("%s/%s"), *TargetIdsRoot, Leaf));
        FixturePackages.AddUnique(Result->GetOutermost());
        return Result;
    };

    UBlueprint* Blueprint = MakeTrackedBlueprint(TEXT("StableIds"));
    UEdGraph* Graph = MakeGraphFixture(
        Blueprint,
        TEXT("GraphA"),
        UEdGraphSchema_K2::StaticClass());
    Blueprint->UbergraphPages.AddUnique(Graph);
    UK2Node_CustomEvent* Node = MakeNodeFixture<UK2Node_CustomEvent>(
        Graph,
        TEXT("NodeA"));
    Graph->AddNode(Node, false, false);
    UEdGraphPin* Pin = MakePinFixture(
        Node,
        TEXT("InputA"),
        UEdGraphSchema_K2::PC_Boolean);

    TestTrue(
        TEXT("Blueprint fixture uses the unique cleanup root"),
        Blueprint->GetPathName().StartsWith(TargetIdsRoot));
    TestEqualSensitive(
        TEXT("Graph schema class matches the fallback qualification assumption"),
        *Graph->GetSchema()->GetClass()->GetPathName(),
        TEXT("/Script/BlueprintGraph.EdGraphSchema_K2"));
    TestEqualSensitive(
        TEXT("Node class matches the fallback qualification assumption"),
        *Node->GetClass()->GetPathName(),
        TEXT("/Script/BlueprintGraph.K2Node_CustomEvent"));
    TestEqualSensitive(
        TEXT("Pin category matches the fallback qualification assumption"),
        *Pin->PinType.PinCategory.ToString(),
        TEXT("bool"));

    TestFalse(TEXT("Fallback graph fixture has no GUID"), Graph->GraphGuid.IsValid());
    TestFalse(TEXT("Fallback node fixture has no GUID"), Node->NodeGuid.IsValid());
    TestFalse(TEXT("Fallback pin fixture has no GUID"), Pin->PinId.IsValid());

    const FString GraphFallback = MakeGraphTargetId(Blueprint, Graph);
    const FString NodeFallback = MakeNodeTargetId(Blueprint, Node);
    const FString PinFallback = MakePinTargetId(Blueprint, Pin);
    const FTargetRef GraphDescription = DescribeGraphTarget(Blueprint, Graph);
    const FTargetRef NodeDescription = DescribeNodeTarget(Blueprint, Node);
    const FTargetRef PinDescription = DescribePinTarget(Blueprint, Pin);

    TestFalse(TEXT("Graph fallback ID is non-empty"), GraphFallback.IsEmpty());
    TestFalse(TEXT("Node fallback ID is non-empty"), NodeFallback.IsEmpty());
    TestFalse(TEXT("Pin fallback ID is non-empty"), PinFallback.IsEmpty());
    TestEqualSensitive(
        TEXT("Qualified fallback hash is independently precomputed"),
        MakeQualifiedFallbackId(
            ETargetKind::Graph,
            TEXT("/Known/Outer"),
            TEXT("GraphA"),
            TEXT("/Script/BlueprintGraph.EdGraphSchema_K2")),
        TEXT("fallback:graph:42f0c59cd6057cfef73fd05111c93550627c06ba"));
    TestEqualSensitive(
        TEXT("Graph fallback ID is deterministic"),
        MakeGraphTargetId(Blueprint, Graph),
        GraphFallback);
    TestEqualSensitive(
        TEXT("Node fallback ID is deterministic"),
        MakeNodeTargetId(Blueprint, Node),
        NodeFallback);
    TestEqualSensitive(
        TEXT("Pin fallback ID is deterministic"),
        MakePinTargetId(Blueprint, Pin),
        PinFallback);

    TestEqualSensitive(
        TEXT("Graph fallback description carries its actual outer path"),
        GraphDescription.OwnerId,
        Graph->GetOuter()->GetPathName());
    TestEqualSensitive(
        TEXT("Node fallback description carries its graph target ID"),
        NodeDescription.OwnerId,
        GraphFallback);
    TestEqualSensitive(
        TEXT("Pin fallback description carries its node target ID"),
        PinDescription.OwnerId,
        NodeFallback);

    FString FallbackResolveError;
    FTargetRef MissingOwnerGraphTarget = GraphDescription;
    MissingOwnerGraphTarget.OwnerId.Empty();
    MissingOwnerGraphTarget.bAllowNameFallback = true;
    TestFalse(
        TEXT("Graph fallback rejects a missing owner qualification"),
        ResolveTarget(
            Blueprint,
            ETargetKind::Graph,
            MissingOwnerGraphTarget,
            FallbackResolveError).Graph != nullptr);

    FTargetRef QualifiedGraphTarget = GraphDescription;
    QualifiedGraphTarget.bAllowNameFallback = true;
    TestTrue(
        TEXT("Graph fallback replays with complete qualification"),
        ResolveTarget(
            Blueprint,
            ETargetKind::Graph,
            QualifiedGraphTarget,
            FallbackResolveError).Graph == Graph);
    FTargetRef QualifiedNodeTarget = NodeDescription;
    QualifiedNodeTarget.bAllowNameFallback = true;
    TestTrue(
        TEXT("Node fallback replays when its graph owner is also a fallback"),
        ResolveTarget(
            Blueprint,
            ETargetKind::Node,
            QualifiedNodeTarget,
            FallbackResolveError).Node == Node);
    FTargetRef QualifiedPinTarget = PinDescription;
    QualifiedPinTarget.bAllowNameFallback = true;
    TestTrue(
        TEXT("Pin fallback replays when its node owner is also a fallback"),
        ResolveTarget(
            Blueprint,
            ETargetKind::Pin,
            QualifiedPinTarget,
            FallbackResolveError).Pin == Pin);

    const FGuid FixedGuid(0x00112233, 0x44556677, 0x8899aabb, 0xccddeeff);
    Graph->GraphGuid = FixedGuid;
    Node->NodeGuid = FixedGuid;
    Pin->PinId = FixedGuid;

    TestEqualSensitive(
        TEXT("Graph GUID target ID uses lowercase hyphen formatting"),
        *MakeGraphTargetId(Blueprint, Graph),
        TEXT("graph:00112233-4455-6677-8899-aabbccddeeff"));
    TestEqualSensitive(
        TEXT("Node GUID target ID uses lowercase hyphen formatting"),
        *MakeNodeTargetId(Blueprint, Node),
        TEXT("node:00112233-4455-6677-8899-aabbccddeeff"));
    TestEqualSensitive(
        TEXT("Pin GUID target ID uses lowercase hyphen formatting"),
        *MakePinTargetId(Blueprint, Pin),
        TEXT("pin:00112233-4455-6677-8899-aabbccddeeff"));
    FGuid ParsedGuid;
    TestTrue(
        TEXT("Strict target parser accepts lowercase hyphenated GUIDs"),
        ParseTargetId(
            TEXT("graph:00112233-4455-6677-8899-aabbccddeeff"),
            ETargetKind::Graph,
            ParsedGuid));
    TestFalse(
        TEXT("Strict target parser rejects uppercase GUIDs"),
        ParseTargetId(
            TEXT("graph:00112233-4455-6677-8899-AABBCCDDEEFF"),
            ETargetKind::Graph,
            ParsedGuid));
    TestFalse(
        TEXT("Strict target parser rejects compact GUIDs"),
        ParseTargetId(
            TEXT("graph:00112233445566778899aabbccddeeff"),
            ETargetKind::Graph,
            ParsedGuid));

    FString ResolveError;
    FTargetRef StableGraphTarget;
    StableGraphTarget.Id = MakeGraphTargetId(Blueprint, Graph);
    const FResolvedTarget ResolvedGraph = ResolveTarget(
        Blueprint,
        ETargetKind::Graph,
        StableGraphTarget,
        ResolveError);
    TestEqualSensitive(
        TEXT("Resolved graph uses graph_guid id kind"),
        ResolvedGraph.IdKind,
        TEXT("graph_guid"));
    FTargetRef StableNodeTarget;
    StableNodeTarget.Id = MakeNodeTargetId(Blueprint, Node);
    const FResolvedTarget ResolvedNode = ResolveTarget(
        Blueprint,
        ETargetKind::Node,
        StableNodeTarget,
        ResolveError);
    TestEqualSensitive(
        TEXT("Resolved node uses node_guid id kind"),
        ResolvedNode.IdKind,
        TEXT("node_guid"));
    FTargetRef StablePinTarget;
    StablePinTarget.Id = MakePinTargetId(Blueprint, Pin);
    const FResolvedTarget ResolvedPin = ResolveTarget(
        Blueprint,
        ETargetKind::Pin,
        StablePinTarget,
        ResolveError);
    TestEqualSensitive(
        TEXT("Resolved pin uses pin_guid id kind"),
        ResolvedPin.IdKind,
        TEXT("pin_guid"));

    FTargetRef StaleInterfaceTarget;
    StaleInterfaceTarget.Id =
        TEXT("interface:/Script/Engine.MissingInterface");
    const FResolvedTarget ResolvedStaleInterface = ResolveTarget(
        Blueprint,
        ETargetKind::Interface,
        StaleInterfaceTarget,
        ResolveError);
    TestFalse(
        TEXT("Stale interface path does not resolve"),
        ResolvedStaleInterface.bStable);
    TestEqualSensitive(
        TEXT("Stale interface path reports missing stable target"),
        ResolveError,
        TEXT("Stable target no longer exists."));

    Graph->GraphGuid.Invalidate();
    Node->NodeGuid.Invalidate();
    Pin->PinId.Invalidate();
    FTargetRef FallbackGraphTarget;
    FallbackGraphTarget.Id = GraphFallback;
    FallbackGraphTarget.OwnerId = GraphDescription.OwnerId;
    FallbackGraphTarget.Name = Graph->GetName();
    FallbackGraphTarget.TypePath = Graph->GetSchema()->GetClass()->GetPathName();
    FallbackGraphTarget.bAllowNameFallback = true;
    const FResolvedTarget ResolvedFallbackGraph = ResolveTarget(
        Blueprint,
        ETargetKind::Graph,
        FallbackGraphTarget,
        ResolveError);
    TestEqualSensitive(
        TEXT("Resolved graph fallback uses qualified_name_fallback id kind"),
        ResolvedFallbackGraph.IdKind,
        TEXT("qualified_name_fallback"));
    TestFalse(
        TEXT("Resolved graph fallback remains unstable"),
        ResolvedFallbackGraph.bStable);

    Blueprint->FunctionGraphs.Add(Graph);
    Blueprint->FunctionGraphs.Add(Graph);
    const FString AmbiguousDeleteRequest = FString::Printf(
        TEXT("{\"function_id\":\"%s\",\"allow_name_fallback\":true,")
        TEXT("\"function_name\":\"%s\",\"function_owner_id\":\"%s\",")
        TEXT("\"function_type_path\":\"%s\"}"),
        *GraphDescription.Id,
        *GraphDescription.Name,
        *GraphDescription.OwnerId,
        *GraphDescription.TypePath);
    const TSharedPtr<FJsonObject> AmbiguousDelete = ParseJsonObject(
        UMCPythonHelper::DeleteBlueprintFunction(
            Blueprint, AmbiguousDeleteRequest));
    TestTrue(
        TEXT("Ambiguous function fallback returns valid JSON"),
        AmbiguousDelete.IsValid());
    if (AmbiguousDelete)
    {
        TestFalse(
            TEXT("Ambiguous function fallback is rejected"),
            AmbiguousDelete->GetBoolField(TEXT("success")));
        const TArray<TSharedPtr<FJsonValue>>& Errors =
            AmbiguousDelete->GetArrayField(TEXT("errors"));
        TestEqual(
            TEXT("Ambiguous function fallback returns one error"),
            Errors.Num(),
            1);
        if (!Errors.IsEmpty())
        {
            const TSharedPtr<FJsonObject> AmbiguousError =
                Errors[0]->AsObject();
            TestEqualSensitive(
                TEXT("Ambiguous function fallback uses the conflict code"),
                AmbiguousError->GetStringField(TEXT("code")),
                TEXT("CONFLICT"));
            TestEqualSensitive(
                TEXT("Ambiguous function fallback identifies function_id"),
                AmbiguousError->GetStringField(TEXT("path")),
                TEXT("function_id"));
        }
    }
    Blueprint->FunctionGraphs.Remove(Graph);

    TestEqualSensitive(
        TEXT("Graph fallback is restored after clearing its GUID"),
        MakeGraphTargetId(Blueprint, Graph),
        GraphFallback);
    TestEqualSensitive(
        TEXT("Node fallback is restored after clearing its GUID"),
        MakeNodeTargetId(Blueprint, Node),
        NodeFallback);
    TestEqualSensitive(
        TEXT("Pin fallback is restored after clearing its GUID"),
        MakePinTargetId(Blueprint, Pin),
        PinFallback);

    const TSharedPtr<FJsonObject> LegacyGraphInfo = ParseJsonObject(
        UMCPythonHelper::GetBlueprintGraphInfo(Blueprint, TEXT("GraphA")));
    TestTrue(TEXT("Legacy graph info is valid JSON"), LegacyGraphInfo.IsValid());
    if (LegacyGraphInfo)
    {
        TestEqualSensitive(
            TEXT("Legacy graph info classifies fallback graph ID"),
            LegacyGraphInfo->GetStringField(TEXT("id_kind")),
            TEXT("qualified_name_fallback"));
        TestFalse(
            TEXT("Legacy graph info marks fallback graph unstable"),
            LegacyGraphInfo->GetBoolField(TEXT("stable")));
        TestEqualSensitive(
            TEXT("Legacy graph info exposes replay owner"),
            LegacyGraphInfo->GetStringField(TEXT("owner_id")),
            GraphDescription.OwnerId);
        TestEqualSensitive(
            TEXT("Legacy graph info exposes replay name"),
            LegacyGraphInfo->GetStringField(TEXT("name")),
            GraphDescription.Name);
        TestEqualSensitive(
            TEXT("Legacy graph info exposes replay type"),
            LegacyGraphInfo->GetStringField(TEXT("type_path")),
            GraphDescription.TypePath);
        const TArray<TSharedPtr<FJsonValue>>& LegacyNodes =
            LegacyGraphInfo->GetArrayField(TEXT("nodes"));
        TestTrue(
            TEXT("Legacy graph info contains the fixture node"),
            !LegacyNodes.IsEmpty());
        if (!LegacyNodes.IsEmpty())
        {
            const TSharedPtr<FJsonObject> LegacyNode = LegacyNodes[0]->AsObject();
            TestEqualSensitive(
                TEXT("Legacy node classifies fallback ID"),
                LegacyNode->GetStringField(TEXT("id_kind")),
                TEXT("qualified_name_fallback"));
            TestFalse(
                TEXT("Legacy node marks fallback ID unstable"),
                LegacyNode->GetBoolField(TEXT("stable")));
            TestEqualSensitive(
                TEXT("Legacy node exposes replay owner"),
                LegacyNode->GetStringField(TEXT("owner_id")),
                NodeDescription.OwnerId);
            TestEqualSensitive(
                TEXT("Legacy node exposes replay type"),
                LegacyNode->GetStringField(TEXT("type_path")),
                NodeDescription.TypePath);
            const TArray<TSharedPtr<FJsonValue>>& LegacyPins =
                LegacyNode->GetArrayField(TEXT("pins"));
            TestTrue(
                TEXT("Legacy node contains the fixture pin"),
                !LegacyPins.IsEmpty());
            if (!LegacyPins.IsEmpty())
            {
                const TSharedPtr<FJsonObject> LegacyPin =
                    LegacyPins[0]->AsObject();
                TestEqualSensitive(
                    TEXT("Legacy pin classifies fallback ID"),
                    LegacyPin->GetStringField(TEXT("id_kind")),
                    TEXT("qualified_name_fallback"));
                TestFalse(
                    TEXT("Legacy pin marks fallback ID unstable"),
                    LegacyPin->GetBoolField(TEXT("stable")));
                TestEqualSensitive(
                    TEXT("Legacy pin exposes replay owner"),
                    LegacyPin->GetStringField(TEXT("owner_id")),
                    PinDescription.OwnerId);
                TestEqualSensitive(
                    TEXT("Legacy pin exposes raw replay name"),
                    LegacyPin->GetStringField(TEXT("name")),
                    PinDescription.Name);
                TestEqualSensitive(
                    TEXT("Legacy pin exposes canonical replay type"),
                    LegacyPin->GetStringField(TEXT("type_path")),
                    PinDescription.TypePath);
            }
        }
    }

    UBlueprint* GraphOwnerBlueprint = MakeTrackedBlueprint(
        TEXT("TargetIdsGraphOwner"));
    UEdGraph* GraphOwnerVariant = MakeGraphFixture(
        GraphOwnerBlueprint,
        TEXT("GraphA"),
        UEdGraphSchema_K2::StaticClass());
    UEdGraph* GraphNameVariant = MakeGraphFixture(
        Blueprint,
        TEXT("GraphB"),
        UEdGraphSchema_K2::StaticClass());
    UBlueprint* GraphSchemaBlueprint = MakeTrackedBlueprint(
        TEXT("TargetIdsGraphSchema"));
    UEdGraph* GraphSchemaVariant = MakeGraphFixture(
        GraphSchemaBlueprint,
        TEXT("GraphA"),
        UEdGraphSchema::StaticClass());

    TestNotEqualSensitive(
        TEXT("Graph fallback changes with its Blueprint owner path"),
        MakeGraphTargetId(GraphOwnerBlueprint, GraphOwnerVariant),
        GraphFallback);
    TestNotEqualSensitive(
        TEXT("Graph fallback changes with its graph name"),
        MakeGraphTargetId(Blueprint, GraphNameVariant),
        GraphFallback);
    TestNotEqualSensitive(
        TEXT("Graph fallback changes with its schema class"),
        MakeGraphTargetId(Blueprint, GraphSchemaVariant),
        GraphFallback);

    UEdGraph* NestedOwnerA = MakeGraphFixture(
        Blueprint,
        TEXT("NestedOwnerA"),
        UEdGraphSchema_K2::StaticClass());
    UEdGraph* NestedOwnerB = MakeGraphFixture(
        Blueprint,
        TEXT("NestedOwnerB"),
        UEdGraphSchema_K2::StaticClass());
    UEdGraph* NestedGraphA = FindOrCreateFixture<UEdGraph>(
        NestedOwnerA,
        TEXT("NestedGraph"));
    UEdGraph* NestedGraphB = FindOrCreateFixture<UEdGraph>(
        NestedOwnerB,
        TEXT("NestedGraph"));
    NestedGraphA->Schema = UEdGraphSchema_K2::StaticClass();
    NestedGraphB->Schema = UEdGraphSchema_K2::StaticClass();
    NestedGraphA->GraphGuid.Invalidate();
    NestedGraphB->GraphGuid.Invalidate();
    TestNotEqualSensitive(
        TEXT("Graph fallback distinguishes identical nested graph names"),
        MakeGraphTargetId(Blueprint, NestedGraphA),
        MakeGraphTargetId(Blueprint, NestedGraphB));

    UK2Node_CustomEvent* NodeGraphOwnerVariant =
        MakeNodeFixture<UK2Node_CustomEvent>(GraphNameVariant, TEXT("NodeA"));
    UK2Node_CustomEvent* NodeNameVariant =
        MakeNodeFixture<UK2Node_CustomEvent>(Graph, TEXT("NodeB"));
    UBlueprint* NodeClassBlueprint = MakeTrackedBlueprint(
        TEXT("TargetIdsNodeClass"));
    UEdGraph* NodeClassGraph = MakeGraphFixture(
        NodeClassBlueprint,
        TEXT("GraphA"),
        UEdGraphSchema_K2::StaticClass());
    UEdGraphNode* NodeClassVariant = MakeNodeFixture<UEdGraphNode>(
        NodeClassGraph,
        TEXT("NodeA"));

    TestNotEqualSensitive(
        TEXT("Node fallback changes with its graph owner"),
        MakeNodeTargetId(Blueprint, NodeGraphOwnerVariant),
        NodeFallback);
    TestNotEqualSensitive(
        TEXT("Node fallback changes with its node name"),
        MakeNodeTargetId(Blueprint, NodeNameVariant),
        NodeFallback);
    TestNotEqualSensitive(
        TEXT("Node fallback changes with its node class"),
        MakeNodeTargetId(Blueprint, NodeClassVariant),
        NodeFallback);

    UEdGraphPin* PinOwnerVariant = MakePinFixture(
        NodeNameVariant,
        TEXT("InputA"),
        UEdGraphSchema_K2::PC_Boolean);
    UEdGraphPin* PinNameVariant = MakePinFixture(
        Node,
        TEXT("InputB"),
        UEdGraphSchema_K2::PC_Boolean);
    UEdGraphPin* PinTypeVariant = MakePinFixture(
        Node,
        TEXT("InputA"),
        UEdGraphSchema_K2::PC_Int);

    TestNotEqualSensitive(
        TEXT("Pin fallback changes with its owning node"),
        MakePinTargetId(Blueprint, PinOwnerVariant),
        PinFallback);
    TestNotEqualSensitive(
        TEXT("Pin fallback changes with its pin name"),
        MakePinTargetId(Blueprint, PinNameVariant),
        PinFallback);
    TestNotEqualSensitive(
        TEXT("Pin fallback changes with its pin category/type"),
        MakePinTargetId(Blueprint, PinTypeVariant),
        PinFallback);

    UEdGraphPin* PinDirectionVariant = Node->CreatePin(
        EGPD_Output,
        UEdGraphSchema_K2::PC_Boolean,
        TEXT("InputA"));
    PinDirectionVariant->PinId.Invalidate();
    TestNotEqualSensitive(
        TEXT("Pin fallback changes with its direction"),
        MakePinTargetId(Blueprint, PinDirectionVariant),
        PinFallback);

    UEdGraphPin* DuplicatePinA = MakePinFixture(
        Node,
        TEXT("Repeated"),
        UEdGraphSchema_K2::PC_Boolean);
    UEdGraphPin* DuplicatePinB = MakePinFixture(
        Node,
        TEXT("Repeated"),
        UEdGraphSchema_K2::PC_Boolean);
    TestNotEqualSensitive(
        TEXT("Pin fallback distinguishes repeated same-signature pins"),
        MakePinTargetId(Blueprint, DuplicatePinA),
        MakePinTargetId(Blueprint, DuplicatePinB));

    UEdGraphPin* ScalarContainerPin = MakePinFixture(
        Node,
        TEXT("ContainerCollision"),
        UEdGraphSchema_K2::PC_Int);
    UEdGraphPin* ArrayContainerPin = MakePinFixture(
        Node,
        TEXT("ContainerCollision"),
        UEdGraphSchema_K2::PC_Int);
    ArrayContainerPin->PinType.ContainerType = EPinContainerType::Array;
    TestNotEqualSensitive(
        TEXT("Pin fallback distinguishes scalar and array types"),
        MakePinTargetId(Blueprint, ScalarContainerPin),
        MakePinTargetId(Blueprint, ArrayContainerPin));

    UEdGraphPin* IntMapPin = MakePinFixture(
        Node,
        TEXT("MapTerminalCollision"),
        UEdGraphSchema_K2::PC_Int);
    IntMapPin->PinType.ContainerType = EPinContainerType::Map;
    IntMapPin->PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Int;
    UEdGraphPin* BoolMapPin = MakePinFixture(
        Node,
        TEXT("MapTerminalCollision"),
        UEdGraphSchema_K2::PC_Int);
    BoolMapPin->PinType.ContainerType = EPinContainerType::Map;
    BoolMapPin->PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Boolean;
    TestNotEqualSensitive(
        TEXT("Pin fallback distinguishes map terminal types"),
        MakePinTargetId(Blueprint, IntMapPin),
        MakePinTargetId(Blueprint, BoolMapPin));

    FBPVariableDescription VariableDescription;
    VariableDescription.VarName = TEXT("ContainerVariable");
    VariableDescription.VarGuid.Invalidate();
    VariableDescription.VarType.PinCategory = UEdGraphSchema_K2::PC_Int;
    const FString ScalarVariableId =
        DescribeVariableTarget(Blueprint, VariableDescription).Id;
    VariableDescription.VarType.ContainerType = EPinContainerType::Array;
    TestNotEqualSensitive(
        TEXT("Variable fallback changes when its container type changes"),
        DescribeVariableTarget(Blueprint, VariableDescription).Id,
        ScalarVariableId);

    Node->NodeGuid = FixedGuid;
    FTargetRef PinFallbackTarget;
    PinFallbackTarget = DescribePinTarget(Blueprint, Pin);
    PinFallbackTarget.bAllowNameFallback = true;
    const FResolvedTarget ResolvedFallbackPin = ResolveTarget(
        Blueprint,
        ETargetKind::Pin,
        PinFallbackTarget,
        ResolveError);
    TestTrue(
        TEXT("Pin fallback resolution honors its full qualification"),
        ResolvedFallbackPin.Pin == Pin);
    TestEqualSensitive(
        TEXT("Resolved pin fallback uses qualified_name_fallback id kind"),
        ResolvedFallbackPin.IdKind,
        TEXT("qualified_name_fallback"));

    TestTrue(
        TEXT("Standard Blueprint editor is accepted for selection"),
        IsSupportedBlueprintSelectionEditor(TEXT("BlueprintEditor")));
    TestTrue(
        TEXT("Widget Blueprint editor is accepted for selection"),
        IsSupportedBlueprintSelectionEditor(TEXT("WidgetBlueprintEditor")));
    TestTrue(
        TEXT("Animation Blueprint editor is accepted for selection"),
        IsSupportedBlueprintSelectionEditor(TEXT("AnimationBlueprintEditor")));
    TestFalse(
        TEXT("Control Rig Blueprint editor is rejected before casting"),
        IsSupportedBlueprintSelectionEditor(TEXT("ControlRigEditor")));

    const TSharedRef<FJsonObject> GlobalCapabilities = BuildCapabilities(nullptr);
    bool bSupportsK2Schema = false;
    bool bSupportsSCSOperations = false;
    bool bSupportsCompilerTokens = false;
    bool bHasK2Graphs = true;
    TestTrue(
        TEXT("Global capabilities declare K2 schema runtime support"),
        GlobalCapabilities->TryGetBoolField(
            TEXT("supports_k2_schema"),
            bSupportsK2Schema) && bSupportsK2Schema);
    TestTrue(
        TEXT("Global capabilities declare SCS runtime support"),
        GlobalCapabilities->TryGetBoolField(
            TEXT("supports_scs_operations"),
            bSupportsSCSOperations) && bSupportsSCSOperations);
    TestTrue(
        TEXT("UE 5.7 capabilities declare compiler-token runtime support"),
        GlobalCapabilities->TryGetBoolField(
            TEXT("supports_compiler_tokens"),
            bSupportsCompilerTokens) && bSupportsCompilerTokens);
    TestTrue(
        TEXT("Global capabilities distinguish absent asset K2 graphs"),
        GlobalCapabilities->TryGetBoolField(
            TEXT("has_k2_graphs"),
            bHasK2Graphs) && !bHasK2Graphs);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprint2VariableEditingTest,
    "UnrealMCPython.Blueprint2.VariableEditing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprint2VariableEditingTest::RunTest(const FString& Parameters)
{
    using namespace UE::MCPython::Blueprint2;

    (void)Parameters;

    const FString Root = FString::Printf(
        TEXT("/Game/__MCPTests/Blueprint2_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/Variables"), *Root));
    const TArray<UPackage*> FixturePackages = {Package};
    ON_SCOPE_EXIT
    {
        CleanupFixturePackages(FixturePackages);
    };
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        Package,
        TEXT("BP_Variables"),
        BPTYPE_Normal,
        TEXT("MCPythonBlueprint2VariableEditingTest"));
    TestNotNull(TEXT("Variable editing fixture Blueprint is created"), Blueprint);
    if (!Blueprint)
    {
        return false;
    }

    auto Primitive = [](const FName Category, const FName SubCategory = NAME_None)
    {
        FEdGraphPinType Type;
        Type.PinCategory = Category;
        Type.PinSubCategory = SubCategory;
        return Type;
    };
    auto Referenced = [](const FName Category, UObject* TypeObject)
    {
        FEdGraphPinType Type;
        Type.PinCategory = Category;
        Type.PinSubCategoryObject = TypeObject;
        return Type;
    };
    auto JsonArray = [](std::initializer_list<TSharedPtr<FJsonValue>> Values)
    {
        return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>(Values));
    };
    auto JsonObjectValue = [](std::initializer_list<TPair<FString, TSharedPtr<FJsonValue>>> Values)
    {
        const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Values)
        {
            Object->SetField(Pair.Key, Pair.Value);
        }
        return MakeShared<FJsonValueObject>(Object);
    };

    FEdGraphPinType EnumType = Referenced(
        UEdGraphSchema_K2::PC_Byte, StaticEnum<ECollisionChannel>());
    FEdGraphPinType StructType = Referenced(
        UEdGraphSchema_K2::PC_Struct, TBaseStructure<FVector>::Get());
    FEdGraphPinType ObjectType = Referenced(
        UEdGraphSchema_K2::PC_Object, AActor::StaticClass());
    FEdGraphPinType ClassType = Referenced(
        UEdGraphSchema_K2::PC_Class, AActor::StaticClass());
    FEdGraphPinType InterfaceType = Referenced(
        UEdGraphSchema_K2::PC_Interface, UNavAgentInterface::StaticClass());
    FEdGraphPinType SoftObjectType = Referenced(
        UEdGraphSchema_K2::PC_SoftObject, UTexture2D::StaticClass());
    FEdGraphPinType SoftClassType = Referenced(
        UEdGraphSchema_K2::PC_SoftClass, AActor::StaticClass());
    FEdGraphPinType ArrayType = Primitive(UEdGraphSchema_K2::PC_Int);
    ArrayType.ContainerType = EPinContainerType::Array;
    FEdGraphPinType SetType = Primitive(UEdGraphSchema_K2::PC_Name);
    SetType.ContainerType = EPinContainerType::Set;
    FEdGraphPinType MapType = Primitive(UEdGraphSchema_K2::PC_String);
    MapType.ContainerType = EPinContainerType::Map;
    MapType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Boolean;

    struct FDefaultCase
    {
        const TCHAR* Label;
        FEdGraphPinType Type;
        TSharedPtr<FJsonValue> Valid;
        TSharedPtr<FJsonValue> Invalid;
    };
    const TArray<FDefaultCase> Cases = {
        {TEXT("bool"), Primitive(UEdGraphSchema_K2::PC_Boolean),
            MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("false"))},
        {TEXT("byte"), Primitive(UEdGraphSchema_K2::PC_Byte),
            MakeShared<FJsonValueNumber>(200), MakeShared<FJsonValueNumber>(-1)},
        {TEXT("int"), Primitive(UEdGraphSchema_K2::PC_Int),
            MakeShared<FJsonValueNumber>(42), MakeShared<FJsonValueNumber>(1.25)},
        {TEXT("int64"), Primitive(UEdGraphSchema_K2::PC_Int64),
            MakeShared<FJsonValueNumber>(9007199254740991.0), MakeShared<FJsonValueNumber>(1.25)},
        {TEXT("float"), Primitive(UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Float),
            MakeShared<FJsonValueNumber>(1.25), MakeShared<FJsonValueString>(TEXT("bad"))},
        {TEXT("double"), Primitive(UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Double),
            MakeShared<FJsonValueNumber>(2.5), MakeShared<FJsonValueString>(TEXT("bad"))},
        {TEXT("string"), Primitive(UEdGraphSchema_K2::PC_String),
            MakeShared<FJsonValueString>(TEXT("hello")), MakeShared<FJsonValueBoolean>(false)},
        {TEXT("name"), Primitive(UEdGraphSchema_K2::PC_Name),
            MakeShared<FJsonValueString>(TEXT("PlayerStart")), MakeShared<FJsonValueBoolean>(false)},
        {TEXT("text"), Primitive(UEdGraphSchema_K2::PC_Text),
            MakeShared<FJsonValueString>(TEXT("Hello text")), MakeShared<FJsonValueBoolean>(false)},
        {TEXT("enum"), EnumType,
            MakeShared<FJsonValueString>(TEXT("ECC_WorldStatic")), MakeShared<FJsonValueString>(TEXT("Missing"))},
        {TEXT("struct"), StructType,
            JsonObjectValue({
                {TEXT("X"), MakeShared<FJsonValueNumber>(1)},
                {TEXT("Y"), MakeShared<FJsonValueNumber>(2)},
                {TEXT("Z"), MakeShared<FJsonValueNumber>(3)}}),
            JsonObjectValue({{TEXT("Q"), MakeShared<FJsonValueNumber>(1)}})},
        {TEXT("object"), ObjectType,
            MakeShared<FJsonValueString>(AActor::StaticClass()->GetDefaultObject()->GetPathName()),
            MakeShared<FJsonValueString>(UTexture2D::StaticClass()->GetDefaultObject()->GetPathName())},
        {TEXT("class"), ClassType,
            MakeShared<FJsonValueString>(APawn::StaticClass()->GetPathName()),
            MakeShared<FJsonValueString>(UTexture2D::StaticClass()->GetPathName())},
        {TEXT("interface"), InterfaceType,
            MakeShared<FJsonValueString>(APawn::StaticClass()->GetDefaultObject()->GetPathName()),
            MakeShared<FJsonValueString>(UTexture2D::StaticClass()->GetDefaultObject()->GetPathName())},
        {TEXT("soft object"), SoftObjectType,
            MakeShared<FJsonValueString>(UTexture2D::StaticClass()->GetDefaultObject()->GetPathName()),
            MakeShared<FJsonValueString>(TEXT("not-an-object-path"))},
        {TEXT("soft class"), SoftClassType,
            MakeShared<FJsonValueString>(APawn::StaticClass()->GetPathName()),
            MakeShared<FJsonValueString>(TEXT("not-an-object-path"))},
        {TEXT("array"), ArrayType,
            JsonArray({MakeShared<FJsonValueNumber>(3), MakeShared<FJsonValueNumber>(1)}),
            JsonArray({MakeShared<FJsonValueNumber>(1), MakeShared<FJsonValueString>(TEXT("two"))})},
        {TEXT("set"), SetType,
            JsonArray({MakeShared<FJsonValueString>(TEXT("Player")), MakeShared<FJsonValueString>(TEXT("Enemy"))}),
            JsonArray({MakeShared<FJsonValueString>(TEXT("Player")), MakeShared<FJsonValueBoolean>(false)})},
        {TEXT("map"), MapType,
            JsonArray({JsonObjectValue({
                {TEXT("key"), MakeShared<FJsonValueString>(TEXT("Enabled"))},
                {TEXT("value"), MakeShared<FJsonValueBoolean>(true)}})}),
            JsonArray({JsonObjectValue({
                {TEXT("key"), MakeShared<FJsonValueString>(TEXT("Enabled"))},
                {TEXT("value"), MakeShared<FJsonValueString>(TEXT("yes"))}})})},
    };

    for (int32 Index = 0; Index < Cases.Num(); ++Index)
    {
        const FDefaultCase& Case = Cases[Index];
        const FName VariableName(*FString::Printf(TEXT("Default_%02d"), Index));
        TestTrue(
            *FString::Printf(TEXT("%s variable is added"), Case.Label),
            FBlueprintEditorUtils::AddMemberVariable(Blueprint, VariableName, Case.Type));
        const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(
            Blueprint, VariableName);
        TestTrue(
            *FString::Printf(TEXT("%s variable is addressable"), Case.Label),
            Blueprint->NewVariables.IsValidIndex(VariableIndex));
        if (!Blueprint->NewVariables.IsValidIndex(VariableIndex))
        {
            continue;
        }
        const FString VariableId = DescribeVariableTarget(
            Blueprint, Blueprint->NewVariables[VariableIndex]).Id;
        const auto Request = [&VariableId](const TSharedPtr<FJsonValue>& Value)
        {
            const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
            Object->SetStringField(TEXT("variable_id"), VariableId);
            Object->SetField(TEXT("default"), Value);
            return SerializeJsonObject(Object);
        };

        const TSharedPtr<FJsonObject> ValidResponse = ParseJsonObject(
            UMCPythonHelper::SetBlueprintVariableDefault(
                Blueprint, Request(Case.Valid)));
        bool bSuccess = false;
        TestTrue(
            *FString::Printf(TEXT("%s action succeeds"), Case.Label),
            ValidResponse.IsValid() &&
                ValidResponse->TryGetBoolField(TEXT("success"), bSuccess) &&
                bSuccess);
        const TSharedPtr<FJsonObject>* Data = nullptr;
        const TSharedPtr<FJsonValue>* After = nullptr;
        if (ValidResponse.IsValid() &&
            ValidResponse->TryGetObjectField(TEXT("data"), Data) && Data)
        {
            After = (*Data)->Values.Find(TEXT("after"));
        }
        TestTrue(
            *FString::Printf(TEXT("%s action round-trips canonical JSON"), Case.Label),
            After &&
                FJsonValue::CompareEqual(*Case.Valid, **After));

        const FString BeforeInvalid =
            Blueprint->NewVariables[VariableIndex].DefaultValue;
        const TSharedPtr<FJsonObject> InvalidResponse = ParseJsonObject(
            UMCPythonHelper::SetBlueprintVariableDefault(
                Blueprint, Request(Case.Invalid)));
        bSuccess = true;
        TestTrue(
            *FString::Printf(TEXT("%s invalid action is rejected"), Case.Label),
            InvalidResponse.IsValid() &&
                InvalidResponse->TryGetBoolField(TEXT("success"), bSuccess) &&
                !bSuccess);
        TestEqual(
            *FString::Printf(TEXT("%s invalid action preserves stored default"), Case.Label),
            Blueprint->NewVariables[VariableIndex].DefaultValue,
            BeforeInvalid);
    }

    const TSharedRef<FJsonObject> DispatcherRequest = MakeShared<FJsonObject>();
    DispatcherRequest->SetStringField(
        TEXT("dispatcher_name"), TEXT("BeforeRenameDispatcher"));
    DispatcherRequest->SetArrayField(TEXT("parameters"), {});
    DispatcherRequest->SetStringField(TEXT("category"), TEXT(""));
    DispatcherRequest->SetStringField(TEXT("description"), TEXT(""));
    const TSharedPtr<FJsonObject> DispatcherResponse = ParseJsonObject(
        UMCPythonHelper::AddEventDispatcher(
            Blueprint, SerializeJsonObject(DispatcherRequest)));
    bool bDispatcherSuccess = false;
    TestTrue(
        TEXT("dispatcher fixture is created"),
        DispatcherResponse.IsValid() &&
            DispatcherResponse->TryGetBoolField(
                TEXT("success"), bDispatcherSuccess) &&
            bDispatcherSuccess);
    const TSharedPtr<FJsonObject>* DispatcherData = nullptr;
    FString DispatcherId;
    if (DispatcherResponse.IsValid() &&
        DispatcherResponse->TryGetObjectField(TEXT("data"), DispatcherData) &&
        DispatcherData)
    {
        (*DispatcherData)->TryGetStringField(TEXT("dispatcher_id"), DispatcherId);
    }
    UEdGraph* EventGraph = Blueprint->UbergraphPages.IsEmpty()
        ? nullptr
        : Blueprint->UbergraphPages[0];
    TestNotNull(TEXT("dispatcher reference graph exists"), EventGraph);
    UK2Node_CallDelegate* DelegateNode = EventGraph
        ? NewObject<UK2Node_CallDelegate>(EventGraph)
        : nullptr;
    TestNotNull(TEXT("dispatcher reference node is created"), DelegateNode);
    if (DelegateNode && EventGraph)
    {
        DelegateNode->DelegateReference.SetSelfMember(TEXT("BeforeRenameDispatcher"));
        EventGraph->AddNode(DelegateNode, false, false);
    }
    const TSharedRef<FJsonObject> RenameRequest = MakeShared<FJsonObject>();
    RenameRequest->SetStringField(TEXT("variable_id"), DispatcherId);
    RenameRequest->SetStringField(
        TEXT("new_name"), TEXT("AfterRenameDispatcher"));
    const TSharedPtr<FJsonObject> RenameResponse = ParseJsonObject(
        UMCPythonHelper::RenameBlueprintVariable(
            Blueprint, SerializeJsonObject(RenameRequest)));
    bool bRenameSuccess = false;
    TestTrue(
        TEXT("dispatcher rename succeeds"),
        RenameResponse.IsValid() &&
            RenameResponse->TryGetBoolField(TEXT("success"), bRenameSuccess) &&
            bRenameSuccess);
    if (DelegateNode)
    {
        TestEqual(
            TEXT("legacy delegate reference follows dispatcher rename"),
            DelegateNode->DelegateReference.GetMemberName(),
            FName(TEXT("AfterRenameDispatcher")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprint2BriefCountsTest,
    "UnrealMCPython.Blueprint2.BriefCounts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprint2BriefCountsTest::RunTest(const FString& Parameters)
{
    using namespace UE::MCPython::Blueprint2;

    (void)Parameters;

    const FString BriefRoot = FString::Printf(
        TEXT("/Game/__MCPTests/Blueprint2_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UPackage* BriefPackage = CreatePackage(
        *FString::Printf(TEXT("%s/BriefCounts"), *BriefRoot));
    const TArray<UPackage*> FixturePackages = {BriefPackage};
    ON_SCOPE_EXIT
    {
        CleanupFixturePackages(FixturePackages);
    };
    UBlueprint* BriefBlueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        BriefPackage,
        TEXT("BP_BriefCounts"),
        BPTYPE_Normal,
        TEXT("MCPythonBlueprint2BriefCountsTest"));
    TestNotNull(TEXT("Brief count fixture Blueprint is created"), BriefBlueprint);
    if (!BriefBlueprint)
    {
        return false;
    }

    FEdGraphPinType DispatcherType;
    DispatcherType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    TestTrue(
        TEXT("Brief fixture dispatcher variable is added"),
        FBlueprintEditorUtils::AddMemberVariable(
            BriefBlueprint,
            TEXT("OnBriefEvent"),
            DispatcherType));

    UEdGraph* MacroGraph = FBlueprintEditorUtils::CreateNewGraph(
        BriefBlueprint,
        TEXT("BriefMacro"),
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddMacroGraph(
        BriefBlueprint,
        MacroGraph,
        true,
        nullptr);
    UEdGraph* NestedBriefGraph = NewObject<UEdGraph>(
        MacroGraph,
        TEXT("NestedBriefGraph"),
        RF_Transient);
    NestedBriefGraph->Schema = UEdGraphSchema_K2::StaticClass();
    NestedBriefGraph->GraphGuid = FGuid::NewGuid();
    MacroGraph->SubGraphs.Add(NestedBriefGraph);
    TestTrue(
        TEXT("Brief fixture interface is implemented"),
        FBlueprintEditorUtils::ImplementNewInterface(
            BriefBlueprint,
            UInterface::StaticClass()->GetClassPathName()));
    BriefBlueprint->ImplementedInterfaces.AddDefaulted();

    FString ResolveError;
    FTargetRef InterfaceTarget;
    InterfaceTarget.Id = FString::Printf(
        TEXT("interface:%s"),
        *UInterface::StaticClass()->GetPathName());
    const FResolvedTarget ResolvedInterface = ResolveTarget(
        BriefBlueprint,
        ETargetKind::Interface,
        InterfaceTarget,
        ResolveError);
    TestTrue(
        TEXT("Interface path target resolves as stable"),
        ResolvedInterface.bStable);
    TestEqualSensitive(
        TEXT("Resolved interface uses interface_path id kind"),
        ResolvedInterface.IdKind,
        TEXT("interface_path"));

    TestTrue(
        TEXT("Dispatcher fixture has a persisted variable GUID"),
        BriefBlueprint->NewVariables[0].VarGuid.IsValid());
    FTargetRef VariableTarget;
    VariableTarget.Id = MakeTargetId(
        ETargetKind::Variable,
        BriefBlueprint->NewVariables[0].VarGuid);
    const FResolvedTarget ResolvedVariable = ResolveTarget(
        BriefBlueprint,
        ETargetKind::Variable,
        VariableTarget,
        ResolveError);
    TestEqualSensitive(
        TEXT("Resolved variable uses variable_guid id kind"),
        ResolvedVariable.IdKind,
        TEXT("variable_guid"));

    const TSharedPtr<FJsonObject> LegacyVariables = ParseJsonObject(
        UMCPythonHelper::ListBlueprintVariables(BriefBlueprint));
    const TSharedPtr<FJsonObject> LegacyVariable =
        LegacyVariables->GetArrayField(TEXT("variables"))[0]->AsObject();
    TestEqualSensitive(
        TEXT("Legacy variable emits variable_guid id kind"),
        LegacyVariable->GetStringField(TEXT("id_kind")),
        TEXT("variable_guid"));
    TestTrue(
        TEXT("Legacy variable emits stable persisted target"),
        LegacyVariable->GetBoolField(TEXT("stable")));

    USCS_Node* BriefComponent =
        BriefBlueprint->SimpleConstructionScript->CreateNode(
            USceneComponent::StaticClass(),
            TEXT("BriefComponent"));
    BriefBlueprint->SimpleConstructionScript->AddNode(BriefComponent);
    TestTrue(
        TEXT("Brief component fixture has a persisted SCS variable GUID"),
        BriefComponent->VariableGuid.IsValid());
    FTargetRef ComponentTarget;
    ComponentTarget.Id = MakeTargetId(
        ETargetKind::Component,
        BriefComponent->VariableGuid);
    const FResolvedTarget ResolvedComponent = ResolveTarget(
        BriefBlueprint,
        ETargetKind::Component,
        ComponentTarget,
        ResolveError);
    TestEqualSensitive(
        TEXT("Resolved component uses scs_variable_guid id kind"),
        ResolvedComponent.IdKind,
        TEXT("scs_variable_guid"));
    const TSharedPtr<FJsonObject> LegacyComponents = ParseJsonObject(
        UMCPythonHelper::ListBlueprintComponents(BriefBlueprint));
    const TSharedPtr<FJsonObject> LegacyComponent =
        LegacyComponents->GetArrayField(TEXT("components"))[0]->AsObject();
    TestEqualSensitive(
        TEXT("Legacy component emits scs_variable_guid id kind"),
        LegacyComponent->GetStringField(TEXT("id_kind")),
        TEXT("scs_variable_guid"));
    TestTrue(
        TEXT("Legacy component emits stable persisted target"),
        LegacyComponent->GetBoolField(TEXT("stable")));

    const TSharedPtr<FJsonObject> Brief = ParseJsonObject(
        UMCPythonHelper::GetBlueprintBrief(BriefBlueprint));
    TestTrue(TEXT("Blueprint brief is valid JSON"), Brief.IsValid());
    if (!Brief)
    {
        return false;
    }
    TestTrue(TEXT("Blueprint brief succeeds"), Brief->GetBoolField(TEXT("success")));
    const TSharedPtr<FJsonObject> Data = Brief->GetObjectField(TEXT("data"));
    const TSharedPtr<FJsonObject> Counts = Data->GetObjectField(TEXT("counts"));
    const TSharedPtr<FJsonObject> Capabilities =
        Data->GetObjectField(TEXT("capabilities"));
    TestTrue(
        TEXT("Brief capabilities report K2 graph presence"),
        Capabilities->GetBoolField(TEXT("has_k2_graphs")));
    TestTrue(
        TEXT("Brief capabilities report all fixture graphs use K2 schema"),
        Capabilities->GetBoolField(TEXT("all_graphs_k2_schema")));
    TestEqual(
        TEXT("Blueprint brief counts one variable"),
        Counts->GetIntegerField(TEXT("variables")),
        int32(1));
    TestEqual(
        TEXT("Blueprint brief counts one dispatcher"),
        Counts->GetIntegerField(TEXT("dispatchers")),
        int32(1));
    TestEqual(
        TEXT("Blueprint brief counts one macro"),
        Counts->GetIntegerField(TEXT("macros")),
        int32(1));
    TestEqual(
        TEXT("Blueprint brief counts one implemented interface"),
        Counts->GetIntegerField(TEXT("interfaces")),
        int32(1));
    const TArray<TSharedPtr<FJsonValue>>& Interfaces =
        Data->GetArrayField(TEXT("interfaces"));
    TestEqual(TEXT("Blueprint brief emits one interface path"), Interfaces.Num(), 1);
    if (Interfaces.Num() == 1)
    {
        TestEqualSensitive(
            TEXT("Blueprint brief emits the implemented interface path"),
            Interfaces[0]->AsString(),
            UInterface::StaticClass()->GetPathName());
    }
    const TArray<TSharedPtr<FJsonValue>>& Graphs =
        Data->GetArrayField(TEXT("graphs"));
    const bool bContainsNestedGraph = Graphs.ContainsByPredicate(
        [](const TSharedPtr<FJsonValue>& Value)
        {
            return Value && Value->AsString() == TEXT("NestedBriefGraph");
        });
    TestFalse(
        TEXT("Blueprint brief excludes nested graph names"),
        bContainsNestedGraph);

    const TSharedPtr<FJsonObject> Inspection = ParseJsonObject(
        UMCPythonHelper::InspectBlueprint(
            BriefBlueprint,
            TEXT("{\"queries\":["
                 "{\"op\":\"macros\",\"detail\":\"detailed\"},"
                 "{\"op\":\"dispatchers\"},"
                 "{\"op\":\"interfaces\"}]}")));
    TestTrue(
        TEXT("Blueprint inspection fixture succeeds"),
        Inspection.IsValid() && Inspection->GetBoolField(TEXT("success")));
    if (Inspection && Inspection->GetBoolField(TEXT("success")))
    {
        const TArray<TSharedPtr<FJsonValue>>& Results =
            Inspection->GetObjectField(TEXT("data"))->GetArrayField(
                TEXT("results"));
        TestEqual(
            TEXT("Blueprint inspection emits all fixture query results"),
            Results.Num(),
            int32(3));
        if (Results.Num() == 3)
        {
            for (int32 Index = 0; Index < Results.Num(); ++Index)
            {
                const TSharedPtr<FJsonObject> Result = Results[Index]->AsObject();
                TestEqual(
                    *FString::Printf(
                        TEXT("Blueprint inspection query %d is non-empty"),
                        Index),
                    Result->GetIntegerField(TEXT("returned_count")),
                    int32(1));
            }
            const TSharedPtr<FJsonObject> Macro =
                Results[0]->AsObject()->GetArrayField(TEXT("items"))[0]->AsObject();
            TestTrue(
                TEXT("Detailed macro inspection emits source metadata"),
                Macro->HasTypedField<EJson::Object>(TEXT("metadata")));
            const TSharedPtr<FJsonObject> Interface =
                Results[2]->AsObject()->GetArrayField(TEXT("items"))[0]->AsObject();
            TestEqualSensitive(
                TEXT("Interface inspection emits the implemented class path"),
                Interface->GetStringField(TEXT("class_path")),
                UInterface::StaticClass()->GetPathName());
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprint2CompileDiagnosticsTest,
    "UnrealMCPython.Blueprint2.CompileDiagnostics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprint2CompileDiagnosticsTest::RunTest(
    const FString& Parameters)
{
    (void)Parameters;

    const FString Root = FString::Printf(
        TEXT("/Game/__MCPTests/Blueprint2_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UPackage* Package = CreatePackage(
        *FString::Printf(TEXT("%s/CompileDiagnostics"), *Root));
    const TArray<UPackage*> FixturePackages = {Package};
    ON_SCOPE_EXIT
    {
        CleanupFixturePackages(FixturePackages);
    };
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        Package,
        TEXT("BP_CompileDiagnostics"),
        BPTYPE_Normal,
        TEXT("MCPythonBlueprint2CompileDiagnosticsTest"));
    TestNotNull(TEXT("Compile diagnostic fixture Blueprint is created"), Blueprint);
    if (!Blueprint || Blueprint->UbergraphPages.IsEmpty())
    {
        return false;
    }
    UEdGraph* Graph = Blueprint->UbergraphPages[0];

    FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*Graph);
    UK2Node_CustomEvent* Event = EventCreator.CreateNode(false);
    Event->CustomFunctionName = TEXT("RunCompileDiagnosticFixture");
    Event->NodePosX = -160;
    Event->NodePosY = 160;
    EventCreator.Finalize();

    FGraphNodeCreator<UK2Node_CallFunction> SourceCreator(*Graph);
    UK2Node_CallFunction* Source = SourceCreator.CreateNode(false);
    Source->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, RandomInteger),
        UKismetMathLibrary::StaticClass());
    Source->NodePosX = 160;
    Source->NodePosY = 160;
    SourceCreator.Finalize();

    FGraphNodeCreator<UK2Node_IfThenElse> BranchCreator(*Graph);
    UK2Node_IfThenElse* Branch = BranchCreator.CreateNode(false);
    Branch->NodePosX = 480;
    Branch->NodePosY = 160;
    BranchCreator.Finalize();

    UEdGraphPin* IntegerOutput = Source->FindPin(
        UEdGraphSchema_K2::PN_ReturnValue,
        EGPD_Output);
    UEdGraphPin* BooleanInput = Branch->GetConditionPin();
    UEdGraphPin* EventOutput = Event->FindPin(
        UEdGraphSchema_K2::PN_Then,
        EGPD_Output);
    UEdGraphPin* BranchInput = Branch->FindPin(
        UEdGraphSchema_K2::PN_Execute,
        EGPD_Input);
    TestNotNull(TEXT("Type mismatch source pin exists"), IntegerOutput);
    TestNotNull(TEXT("Type mismatch target pin exists"), BooleanInput);
    TestNotNull(TEXT("Diagnostic event output pin exists"), EventOutput);
    TestNotNull(TEXT("Diagnostic branch input pin exists"), BranchInput);
    if (!IntegerOutput || !BooleanInput || !EventOutput || !BranchInput)
    {
        return false;
    }
    EventOutput->MakeLinkTo(BranchInput);
    IntegerOutput->MakeLinkTo(BooleanInput);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    const FString ResponseJson = UMCPythonHelper::CompileBlueprint(Blueprint);
    const TSharedPtr<FJsonObject> Response = ParseJsonObject(ResponseJson);
    TestNotNull(TEXT("Compile diagnostic response is JSON"), Response.Get());
    if (!Response)
    {
        return false;
    }
    TestFalse(
        TEXT("Incompatible pins fail Blueprint compilation"),
        Response->GetBoolField(TEXT("success")));
    TestEqualSensitive(
        TEXT("Compile failure preserves the legacy status"),
        Response->GetStringField(TEXT("status")),
        TEXT("Error"));
    const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
    TestTrue(
        TEXT("Compile response contains structured diagnostics"),
        Response->TryGetArrayField(TEXT("diagnostics"), Diagnostics) &&
            Diagnostics);
    if (!Diagnostics)
    {
        return false;
    }
    const TSharedPtr<FJsonValue>* TypeMismatchValue = Diagnostics->FindByPredicate(
        [](const TSharedPtr<FJsonValue>& Value)
        {
            const TSharedPtr<FJsonObject> Diagnostic =
                Value.IsValid() ? Value->AsObject() : nullptr;
            return Diagnostic &&
                Diagnostic->GetStringField(TEXT("code")) ==
                    TEXT("BP_TYPE_MISMATCH");
        });
    TestTrue(
        TEXT("Incompatible pins emit BP_TYPE_MISMATCH"),
        TypeMismatchValue != nullptr);
    if (!TypeMismatchValue)
    {
        AddError(FString::Printf(
            TEXT("Compile diagnostics response: %s"), *ResponseJson));
    }
    if (TypeMismatchValue)
    {
        const TSharedPtr<FJsonObject> TypeMismatch =
            (*TypeMismatchValue)->AsObject();
        TestEqualSensitive(
            TEXT("Type mismatch diagnostic targets the source graph"),
            TypeMismatch->GetStringField(TEXT("graph_id")),
            UE::MCPython::Blueprint2::DescribeGraphTarget(Blueprint, Graph).Id);
        TestTrue(
            TEXT("Type mismatch diagnostic includes a stable node ID"),
            TypeMismatch->GetStringField(TEXT("node_id")).StartsWith(
                TEXT("node:")));
        TestTrue(
            TEXT("Type mismatch diagnostic includes a stable pin ID"),
            TypeMismatch->GetStringField(TEXT("pin_id")).StartsWith(
                TEXT("pin:")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprint2HealthStructuralTest,
    "UnrealMCPython.Blueprint2.HealthStructural",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprint2HealthStructuralTest::RunTest(
    const FString& Parameters)
{
    (void)Parameters;

    const FString Root = FString::Printf(
        TEXT("/Game/__MCPTests/Blueprint2_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UPackage* Package = CreatePackage(
        *FString::Printf(TEXT("%s/HealthStructural"), *Root));
    const TArray<UPackage*> FixturePackages = {Package};
    ON_SCOPE_EXIT
    {
        CleanupFixturePackages(FixturePackages);
    };
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        Package,
        TEXT("BP_HealthStructural"),
        BPTYPE_Normal,
        TEXT("MCPythonBlueprint2HealthStructuralTest"));
    TestNotNull(TEXT("Health fixture Blueprint is created"), Blueprint);
    if (!Blueprint || !Blueprint->SimpleConstructionScript)
    {
        return false;
    }

    USCS_Node* First = Blueprint->SimpleConstructionScript->CreateNode(
        USceneComponent::StaticClass(), TEXT("HealthFirst"));
    USCS_Node* Second = Blueprint->SimpleConstructionScript->CreateNode(
        USceneComponent::StaticClass(), TEXT("HealthSecond"));
    Blueprint->SimpleConstructionScript->AddNode(First);
    Blueprint->SimpleConstructionScript->AddNode(Second);
    const FGuid DuplicateGuid = FGuid::NewGuid();
    First->VariableGuid = DuplicateGuid;
    Second->VariableGuid = DuplicateGuid;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    const FString ResponseJson = UMCPythonHelper::GetBlueprintHealth(Blueprint);
    const TSharedPtr<FJsonObject> Response = ParseJsonObject(ResponseJson);
    TestNotNull(TEXT("Health response is JSON"), Response.Get());
    if (!Response)
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Data = Response->GetObjectField(TEXT("data"));
    const TArray<TSharedPtr<FJsonValue>>& Issues =
        Data->GetArrayField(TEXT("issues"));
    const TSharedPtr<FJsonValue>* DuplicateIssue = Issues.FindByPredicate(
        [](const TSharedPtr<FJsonValue>& Value)
        {
            const TSharedPtr<FJsonObject> Issue =
                Value.IsValid() ? Value->AsObject() : nullptr;
            return Issue &&
                Issue->GetStringField(TEXT("code")) ==
                    TEXT("BP_SCS_DUPLICATE_GUID");
        });
    TestTrue(
        TEXT("Duplicate SCS GUID emits BP_SCS_DUPLICATE_GUID"),
        DuplicateIssue != nullptr);
    if (DuplicateIssue)
    {
        const TSharedPtr<FJsonObject> Issue = (*DuplicateIssue)->AsObject();
        TestEqualSensitive(
            TEXT("Duplicate SCS GUID issue is an error"),
            Issue->GetStringField(TEXT("severity")),
            TEXT("error"));
        TestTrue(
            TEXT("Duplicate SCS GUID issue targets a stable component"),
            Issue->GetStringField(TEXT("member_id")).StartsWith(
                TEXT("component:")));
    }
    TestFalse(
        TEXT("Duplicate SCS GUID makes the Blueprint unhealthy"),
        Data->GetBoolField(TEXT("healthy")));

    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    USCS_Node* ParentA = SCS->CreateNode(
        USceneComponent::StaticClass(), TEXT("HealthParentA"));
    USCS_Node* ParentB = SCS->CreateNode(
        USceneComponent::StaticClass(), TEXT("HealthParentB"));
    SCS->AddNode(ParentA);
    SCS->AddNode(ParentB);

    USCS_Node* SharedChild = SCS->CreateNode(
        USceneComponent::StaticClass(), TEXT("HealthSharedChild"));
    ParentA->AddChildNode(SharedChild, true);
    ParentB->AddChildNode(SharedChild, false);

    USCS_Node* MissingFromAllNodes = SCS->CreateNode(
        USceneComponent::StaticClass(), TEXT("HealthMissingFromAllNodes"));
    ParentA->AddChildNode(MissingFromAllNodes, false);

    USCS_Node* Orphan = SCS->CreateNode(
        USceneComponent::StaticClass(), TEXT("HealthOrphan"));
    ParentA->AddChildNode(Orphan, true);
    ParentA->RemoveChildNode(Orphan, false);

    USCS_Node* CycleA = SCS->CreateNode(
        USceneComponent::StaticClass(), TEXT("HealthCycleA"));
    USCS_Node* CycleB = SCS->CreateNode(
        USceneComponent::StaticClass(), TEXT("HealthCycleB"));
    SCS->AddNode(CycleA);
    CycleA->AddChildNode(CycleB, true);
    CycleB->AddChildNode(CycleA, false);

    FBPVariableDescription CollisionVariable;
    CollisionVariable.VarName = TEXT("healthfirst");
    CollisionVariable.VarGuid = FGuid::NewGuid();
    CollisionVariable.VarType.PinCategory = UEdGraphSchema_K2::PC_Int;
    CollisionVariable.DefaultValue = TEXT("0");
    Blueprint->NewVariables.Add(CollisionVariable);

    FBPVariableDescription InvalidInteger;
    InvalidInteger.VarName = TEXT("BrokenIntegerDefault");
    InvalidInteger.VarGuid = FGuid::NewGuid();
    InvalidInteger.VarType.PinCategory = UEdGraphSchema_K2::PC_Int;
    InvalidInteger.DefaultValue = TEXT("not-an-integer");
    Blueprint->NewVariables.Add(InvalidInteger);

    FBPVariableDescription InvalidObject;
    InvalidObject.VarName = TEXT("BrokenObjectDefault");
    InvalidObject.VarGuid = FGuid::NewGuid();
    InvalidObject.VarType.PinCategory = UEdGraphSchema_K2::PC_Object;
    InvalidObject.VarType.PinSubCategoryObject = UObject::StaticClass();
    InvalidObject.DefaultValue = TEXT("/Game/Missing.HealthObject");
    Blueprint->NewVariables.Add(InvalidObject);

    FBPVariableDescription InvalidClass;
    InvalidClass.VarName = TEXT("BrokenClassDefault");
    InvalidClass.VarGuid = FGuid::NewGuid();
    InvalidClass.VarType.PinCategory = UEdGraphSchema_K2::PC_Class;
    InvalidClass.VarType.PinSubCategoryObject = UObject::StaticClass();
    InvalidClass.DefaultValue = TEXT("/Game/Missing.HealthClass_C");
    Blueprint->NewVariables.Add(InvalidClass);

    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    FGraphNodeCreator<UK2Node_CallFunction> MissingCallCreator(*Graph);
    UK2Node_CallFunction* MissingCall = MissingCallCreator.CreateNode(false);
    MissingCall->FunctionReference.SetSelfMember(TEXT("MissingHealthFunction"));
    MissingCallCreator.Finalize();

    FGraphNodeCreator<UK2Node_VariableGet> MissingVariableCreator(*Graph);
    UK2Node_VariableGet* MissingVariable =
        MissingVariableCreator.CreateNode(false);
    MissingVariable->VariableReference.SetSelfMember(
        TEXT("MissingHealthVariable"));
    MissingVariableCreator.Finalize();

    UEdGraphNode* RequiredNode = NewObject<UEdGraphNode>(Graph);
    RequiredNode->CreateNewGuid();
    Graph->AddNode(RequiredNode, false, false);
    UEdGraphPin* RequiredPin = RequiredNode->CreatePin(
        EGPD_Input,
        UEdGraphSchema_K2::PC_Int,
        TEXT("RequiredReference"));
    RequiredPin->PinType.bIsReference = true;

    FBPInterfaceDescription MissingInterface;
    MissingInterface.Interface = USoundSubmixWidgetInterface::StaticClass();
    Blueprint->ImplementedInterfaces.Add(MissingInterface);

    TArray<TSharedPtr<FJsonValue>> StructuralIssues;
    UE::MCPython::Blueprint2::CollectStructuralHealthIssues(
        Blueprint, StructuralIssues);
    const auto MakeSortIssue = [](
        const FString& Code,
        const FString& MemberId)
    {
        const TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
        Issue->SetStringField(TEXT("code"), Code);
        Issue->SetStringField(TEXT("severity"), TEXT("error"));
        Issue->SetStringField(TEXT("message"), TEXT("sort fixture"));
        Issue->SetStringField(TEXT("hint"), TEXT("sort fixture"));
        Issue->SetStringField(TEXT("graph_id"), FString());
        Issue->SetStringField(TEXT("node_id"), FString());
        Issue->SetStringField(TEXT("pin_id"), FString());
        Issue->SetStringField(TEXT("member_id"), MemberId);
        return MakeShared<FJsonValueObject>(Issue);
    };
    TArray<TSharedPtr<FJsonValue>> SortIssues = {
        MakeSortIssue(TEXT("BP_A_CODE_FIRST"), TEXT("component:ffffffff-ffff-ffff-ffff-ffffffffffff")),
        MakeSortIssue(TEXT("BP_Z_CODE_LAST"), TEXT("component:00000000-0000-0000-0000-000000000001")),
    };
    UE::MCPython::Blueprint2::NormalizeHealthIssues(SortIssues);
    TestEqualSensitive(
        TEXT("Equal-severity health issues sort by stable target ID"),
        SortIssues[0]->AsObject()->GetStringField(TEXT("member_id")),
        TEXT("component:00000000-0000-0000-0000-000000000001"));
    const auto MakeDuplicateIssue = [](const FString& Message)
    {
        const TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
        Issue->SetStringField(TEXT("code"), TEXT("BP_DUPLICATE_TEST"));
        Issue->SetStringField(TEXT("severity"), TEXT("error"));
        Issue->SetStringField(TEXT("message"), Message);
        Issue->SetStringField(TEXT("hint"), Message);
        Issue->SetStringField(TEXT("graph_id"), FString());
        Issue->SetStringField(TEXT("node_id"), TEXT("node:11111111-1111-1111-1111-111111111111"));
        Issue->SetStringField(TEXT("pin_id"), FString());
        Issue->SetStringField(TEXT("member_id"), FString());
        return MakeShared<FJsonValueObject>(Issue);
    };
    TArray<TSharedPtr<FJsonValue>> ForwardDuplicates = {
        MakeDuplicateIssue(TEXT("z-source")),
        MakeDuplicateIssue(TEXT("a-source")),
    };
    TArray<TSharedPtr<FJsonValue>> ReverseDuplicates = {
        MakeDuplicateIssue(TEXT("a-source")),
        MakeDuplicateIssue(TEXT("z-source")),
    };
    UE::MCPython::Blueprint2::NormalizeHealthIssues(ForwardDuplicates);
    UE::MCPython::Blueprint2::NormalizeHealthIssues(ReverseDuplicates);
    TestEqual(
        TEXT("Equivalent health issues deduplicate to one record"),
        ForwardDuplicates.Num(),
        int32(1));
    TestEqualSensitive(
        TEXT("Health deduplication is independent of source order"),
        ForwardDuplicates[0]->AsObject()->GetStringField(TEXT("message")),
        ReverseDuplicates[0]->AsObject()->GetStringField(TEXT("message")));
    const auto CountCode = [&StructuralIssues](const FString& Code)
    {
        return StructuralIssues.FilterByPredicate(
            [&Code](const TSharedPtr<FJsonValue>& Value)
            {
                const TSharedPtr<FJsonObject> Issue =
                    Value.IsValid() ? Value->AsObject() : nullptr;
                return Issue &&
                    Issue->GetStringField(TEXT("code")) == Code;
            }).Num();
    };
    TestEqual(
        TEXT("SCS cycle emits one normalized issue"),
        CountCode(TEXT("BP_SCS_CYCLE")),
        int32(1));
    TestEqual(
        TEXT("SCS orphan emits one normalized issue"),
        CountCode(TEXT("BP_SCS_ORPHAN")),
        int32(1));
    TestEqual(
        TEXT("SCS child missing from AllNodes emits one normalized issue"),
        CountCode(TEXT("BP_SCS_NODE_MISSING_FROM_ALL_NODES")),
        int32(1));
    TestEqual(
        TEXT("SCS multiple parent emits one normalized issue"),
        CountCode(TEXT("BP_SCS_MULTIPLE_PARENTS")),
        int32(1));
    TestEqual(
        TEXT("Case-insensitive member collision emits one normalized issue"),
        CountCode(TEXT("BP_DUPLICATE_MEMBER")),
        int32(1));
    TestEqual(
        TEXT("Invalid scalar variable default emits one normalized issue"),
        CountCode(TEXT("BP_INVALID_VARIABLE_DEFAULT")),
        int32(1));
    TestEqual(
        TEXT("Invalid object path emits one normalized issue"),
        CountCode(TEXT("BP_INVALID_OBJECT_DEFAULT")),
        int32(1));
    TestEqual(
        TEXT("Invalid class path emits one normalized issue"),
        CountCode(TEXT("BP_INVALID_CLASS_DEFAULT")),
        int32(1));
    TestEqual(
        TEXT("Disconnected required pin emits one normalized issue"),
        CountCode(TEXT("BP_MISSING_REQUIRED_PIN")),
        int32(1));
    TestEqual(
        TEXT("Unresolved call and variable emit two normalized issues"),
        CountCode(TEXT("BP_UNRESOLVED_MEMBER")),
        int32(2));
    TestEqual(
        TEXT("Missing interface function emits one normalized issue"),
        CountCode(TEXT("BP_MISSING_INTERFACE_IMPLEMENTATION")),
        int32(1));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprint2HealthInvalidVariableDefaultsTest,
    "UnrealMCPython.Blueprint2.HealthInvalidVariableDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprint2HealthInvalidVariableDefaultsTest::RunTest(
    const FString& Parameters)
{
    (void)Parameters;

    const FString Root = FString::Printf(
        TEXT("/Game/__MCPTests/Blueprint2_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UPackage* Package = CreatePackage(
        *FString::Printf(TEXT("%s/HealthInvalidVariableDefaults"), *Root));
    const TArray<UPackage*> FixturePackages = {Package};
    ON_SCOPE_EXIT
    {
        CleanupFixturePackages(FixturePackages);
    };
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        Package,
        TEXT("BP_HealthInvalidVariableDefaults"),
        BPTYPE_Normal,
        TEXT("MCPythonBlueprint2HealthInvalidVariableDefaultsTest"));
    TestNotNull(TEXT("Invalid-default health fixture is created"), Blueprint);
    if (!Blueprint)
    {
        return false;
    }

    auto AddVariable = [Blueprint](
        const FName Name,
        const FName Category,
        const EPinContainerType Container,
        const FString& DefaultValue)
    {
        FBPVariableDescription Variable;
        Variable.VarName = Name;
        Variable.VarGuid = FGuid::NewGuid();
        Variable.VarType.PinCategory = Category;
        Variable.VarType.ContainerType = Container;
        Variable.DefaultValue = DefaultValue;
        Blueprint->NewVariables.Add(MoveTemp(Variable));
    };
    AddVariable(
        TEXT("BrokenBooleanDefault"),
        UEdGraphSchema_K2::PC_Boolean,
        EPinContainerType::None,
        TEXT("definitely"));
    AddVariable(
        TEXT("BrokenArrayDefault"),
        UEdGraphSchema_K2::PC_Int,
        EPinContainerType::Array,
        TEXT("not-an-array"));
    AddVariable(
        TEXT("BrokenSetDefault"),
        UEdGraphSchema_K2::PC_Int,
        EPinContainerType::Set,
        TEXT("not-a-set"));
    AddVariable(
        TEXT("BrokenMapDefault"),
        UEdGraphSchema_K2::PC_String,
        EPinContainerType::Map,
        TEXT("not-a-map"));
    Blueprint->NewVariables.Last().VarType.PinValueType.TerminalCategory =
        UEdGraphSchema_K2::PC_Int;
    AddVariable(
        TEXT("BrokenTextDefault"),
        UEdGraphSchema_K2::PC_Text,
        EPinContainerType::None,
        TEXT("NSLOCTEXT(\"HealthNamespace\", \"HealthKey\", \"unterminated\""));

    TArray<TSharedPtr<FJsonValue>> Issues;
    UE::MCPython::Blueprint2::CollectStructuralHealthIssues(Blueprint, Issues);
    const TArray<FName> InvalidNames = {
        TEXT("BrokenBooleanDefault"),
        TEXT("BrokenArrayDefault"),
        TEXT("BrokenSetDefault"),
        TEXT("BrokenMapDefault"),
        TEXT("BrokenTextDefault"),
    };
    for (const FName InvalidName : InvalidNames)
    {
        const FBPVariableDescription* Variable = Blueprint->NewVariables.FindByPredicate(
            [InvalidName](const FBPVariableDescription& Candidate)
            {
                return Candidate.VarName == InvalidName;
            });
        TestNotNull(
            *FString::Printf(TEXT("%s fixture variable exists"), *InvalidName.ToString()),
            Variable);
        if (!Variable)
        {
            continue;
        }
        const FString VariableId =
            UE::MCPython::Blueprint2::DescribeVariableTarget(
                Blueprint, *Variable).Id;
        const bool bHasInvalidDefaultIssue = Issues.ContainsByPredicate(
            [&VariableId](const TSharedPtr<FJsonValue>& Value)
            {
                const TSharedPtr<FJsonObject> Issue =
                    Value.IsValid() ? Value->AsObject() : nullptr;
                return Issue &&
                    Issue->GetStringField(TEXT("code")) ==
                        TEXT("BP_INVALID_VARIABLE_DEFAULT") &&
                    Issue->GetStringField(TEXT("member_id")) == VariableId;
            });
        TestTrue(
            *FString::Printf(
                TEXT("%s emits BP_INVALID_VARIABLE_DEFAULT"),
                *InvalidName.ToString()),
            bHasInvalidDefaultIssue);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprint2HealthHierarchyOnlyDuplicateGuidTest,
    "UnrealMCPython.Blueprint2.HealthHierarchyOnlyDuplicateGuid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprint2HealthHierarchyOnlyDuplicateGuidTest::RunTest(
    const FString& Parameters)
{
    (void)Parameters;

    const FString Root = FString::Printf(
        TEXT("/Game/__MCPTests/Blueprint2_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UPackage* Package = CreatePackage(
        *FString::Printf(TEXT("%s/HealthHierarchyOnlyDuplicateGuid"), *Root));
    const TArray<UPackage*> FixturePackages = {Package};
    ON_SCOPE_EXIT
    {
        CleanupFixturePackages(FixturePackages);
    };
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        Package,
        TEXT("BP_HealthHierarchyOnlyDuplicateGuid"),
        BPTYPE_Normal,
        TEXT("MCPythonBlueprint2HealthHierarchyOnlyDuplicateGuidTest"));
    TestNotNull(TEXT("Hierarchy-only GUID fixture is created"), Blueprint);
    if (!Blueprint || !Blueprint->SimpleConstructionScript)
    {
        return false;
    }

    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    USCS_Node* Parent = SCS->CreateNode(
        USceneComponent::StaticClass(), TEXT("HierarchyGuidParent"));
    SCS->AddNode(Parent);
    USCS_Node* RegisteredChild = SCS->CreateNode(
        USceneComponent::StaticClass(), TEXT("RegisteredGuidChild"));
    Parent->AddChildNode(RegisteredChild, true);
    USCS_Node* HierarchyOnlyChild = SCS->CreateNode(
        USceneComponent::StaticClass(), TEXT("HierarchyOnlyGuidChild"));
    Parent->AddChildNode(HierarchyOnlyChild, false);
    const FGuid DuplicateGuid = FGuid::NewGuid();
    RegisteredChild->VariableGuid = DuplicateGuid;
    HierarchyOnlyChild->VariableGuid = DuplicateGuid;

    TArray<TSharedPtr<FJsonValue>> Issues;
    UE::MCPython::Blueprint2::CollectStructuralHealthIssues(Blueprint, Issues);
    const auto CountCode = [&Issues](const FString& Code)
    {
        return Issues.FilterByPredicate(
            [&Code](const TSharedPtr<FJsonValue>& Value)
            {
                const TSharedPtr<FJsonObject> Issue =
                    Value.IsValid() ? Value->AsObject() : nullptr;
                return Issue &&
                    Issue->GetStringField(TEXT("code")) == Code;
            }).Num();
    };
    TestEqual(
        TEXT("Hierarchy-only child emits missing-from-AllNodes"),
        CountCode(TEXT("BP_SCS_NODE_MISSING_FROM_ALL_NODES")),
        int32(1));
    TestEqual(
        TEXT("GUID collision includes hierarchy-only child"),
        CountCode(TEXT("BP_SCS_DUPLICATE_GUID")),
        int32(1));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprint2HealthDistinctSCSParentsTest,
    "UnrealMCPython.Blueprint2.HealthDistinctSCSParents",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprint2HealthDistinctSCSParentsTest::RunTest(
    const FString& Parameters)
{
    (void)Parameters;

    const FString Root = FString::Printf(
        TEXT("/Game/__MCPTests/Blueprint2_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UPackage* Package = CreatePackage(
        *FString::Printf(TEXT("%s/HealthDistinctSCSParents"), *Root));
    const TArray<UPackage*> FixturePackages = {Package};
    ON_SCOPE_EXIT
    {
        CleanupFixturePackages(FixturePackages);
    };
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        Package,
        TEXT("BP_HealthDistinctSCSParents"),
        BPTYPE_Normal,
        TEXT("MCPythonBlueprint2HealthDistinctSCSParentsTest"));
    TestNotNull(TEXT("Distinct-parent health fixture is created"), Blueprint);
    if (!Blueprint || !Blueprint->SimpleConstructionScript)
    {
        return false;
    }

    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    USCS_Node* RepeatedParent = SCS->CreateNode(
        USceneComponent::StaticClass(), TEXT("RepeatedChildParent"));
    USCS_Node* SharedParentA = SCS->CreateNode(
        USceneComponent::StaticClass(), TEXT("SharedChildParentA"));
    USCS_Node* SharedParentB = SCS->CreateNode(
        USceneComponent::StaticClass(), TEXT("SharedChildParentB"));
    SCS->AddNode(RepeatedParent);
    SCS->AddNode(SharedParentA);
    SCS->AddNode(SharedParentB);

    USCS_Node* RepeatedChild = SCS->CreateNode(
        USceneComponent::StaticClass(), TEXT("RepeatedChild"));
    RepeatedParent->AddChildNode(RepeatedChild, true);
    const FArrayProperty* ChildNodesProperty = FindFProperty<FArrayProperty>(
        USCS_Node::StaticClass(), TEXT("ChildNodes"));
    TestNotNull(TEXT("USCS_Node ChildNodes property is reflected"), ChildNodesProperty);
    if (!ChildNodesProperty)
    {
        return false;
    }
    FScriptArrayHelper ChildNodesHelper(
        ChildNodesProperty,
        ChildNodesProperty->ContainerPtrToValuePtr<void>(RepeatedParent));
    const int32 RepeatedIndex = ChildNodesHelper.AddValue();
    FObjectPropertyBase* ChildProperty = CastFieldChecked<FObjectPropertyBase>(
        ChildNodesProperty->Inner);
    ChildProperty->SetObjectPropertyValue(
        ChildNodesHelper.GetRawPtr(RepeatedIndex), RepeatedChild);

    USCS_Node* SharedChild = SCS->CreateNode(
        USceneComponent::StaticClass(), TEXT("SharedChild"));
    SharedParentA->AddChildNode(SharedChild, true);
    SharedParentB->AddChildNode(SharedChild, false);

    TArray<TSharedPtr<FJsonValue>> Issues;
    UE::MCPython::Blueprint2::CollectStructuralHealthIssues(Blueprint, Issues);
    const FString RepeatedChildId =
        UE::MCPython::Blueprint2::DescribeComponentTarget(
            Blueprint, RepeatedChild).Id;
    const FString SharedChildId =
        UE::MCPython::Blueprint2::DescribeComponentTarget(
            Blueprint, SharedChild).Id;
    const auto HasMultipleParentsIssue = [&Issues](const FString& MemberId)
    {
        return Issues.ContainsByPredicate(
            [&MemberId](const TSharedPtr<FJsonValue>& Value)
            {
                const TSharedPtr<FJsonObject> Issue =
                    Value.IsValid() ? Value->AsObject() : nullptr;
                return Issue &&
                    Issue->GetStringField(TEXT("code")) ==
                        TEXT("BP_SCS_MULTIPLE_PARENTS") &&
                    Issue->GetStringField(TEXT("member_id")) == MemberId;
            });
    };
    TestFalse(
        TEXT("Repeated child entry in one parent is not multiple parents"),
        HasMultipleParentsIssue(RepeatedChildId));
    TestTrue(
        TEXT("Child under two distinct parents is multiple parents"),
        HasMultipleParentsIssue(SharedChildId));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
