// Copyright (c) 2025 GenOrca. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "MCPythonBlueprint2Internal.h"
#include "MCPythonHelper.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Interface.h"
#include "UObject/Package.h"

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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprint2TargetIdsTest,
    "UnrealMCPython.Blueprint2.TargetIds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprint2TargetIdsTest::RunTest(const FString& Parameters)
{
    using namespace UE::MCPython::Blueprint2;

    (void)Parameters;

    UBlueprint* Blueprint = MakeBlueprintFixture(TEXT("/MCPythonTests/StableIds"));
    UEdGraph* Graph = MakeGraphFixture(
        Blueprint,
        TEXT("GraphA"),
        UEdGraphSchema_K2::StaticClass());
    UK2Node_CustomEvent* Node = MakeNodeFixture<UK2Node_CustomEvent>(
        Graph,
        TEXT("NodeA"));
    UEdGraphPin* Pin = MakePinFixture(
        Node,
        TEXT("InputA"),
        UEdGraphSchema_K2::PC_Boolean);

    TestEqualSensitive(
        TEXT("Blueprint path matches the fallback qualification assumption"),
        *Blueprint->GetPathName(),
        TEXT("/MCPythonTests/StableIds.BP_StableIds"));
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

    TestFalse(TEXT("Graph fallback ID is non-empty"), GraphFallback.IsEmpty());
    TestFalse(TEXT("Node fallback ID is non-empty"), NodeFallback.IsEmpty());
    TestFalse(TEXT("Pin fallback ID is non-empty"), PinFallback.IsEmpty());
    TestEqualSensitive(
        TEXT("Graph fallback ID is independently precomputed"),
        *GraphFallback,
        TEXT("fallback:graph:c6afa87de837f324fd224c43d4f24e5fe74ce74d"));
    TestEqualSensitive(
        TEXT("Node fallback ID is independently precomputed"),
        *NodeFallback,
        TEXT("fallback:node:79bfb622f4b03440dc12fba6a89ae53d44b0e46b"));
    TestEqualSensitive(
        TEXT("Pin fallback ID is independently precomputed"),
        *PinFallback,
        TEXT("fallback:pin:8459beff0c6223a4ed7482ad60270a8dc2de4ec6"));
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

    Graph->GraphGuid.Invalidate();
    Node->NodeGuid.Invalidate();
    Pin->PinId.Invalidate();
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

    UBlueprint* GraphOwnerBlueprint = MakeBlueprintFixture(
        TEXT("/MCPythonTests/TargetIdsGraphOwner"));
    UEdGraph* GraphOwnerVariant = MakeGraphFixture(
        GraphOwnerBlueprint,
        TEXT("GraphA"),
        UEdGraphSchema_K2::StaticClass());
    UEdGraph* GraphNameVariant = MakeGraphFixture(
        Blueprint,
        TEXT("GraphB"),
        UEdGraphSchema_K2::StaticClass());
    UBlueprint* GraphSchemaBlueprint = MakeBlueprintFixture(
        TEXT("/MCPythonTests/TargetIdsGraphSchema"));
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

    UK2Node_CustomEvent* NodeGraphOwnerVariant =
        MakeNodeFixture<UK2Node_CustomEvent>(GraphNameVariant, TEXT("NodeA"));
    UK2Node_CustomEvent* NodeNameVariant =
        MakeNodeFixture<UK2Node_CustomEvent>(Graph, TEXT("NodeB"));
    UBlueprint* NodeClassBlueprint = MakeBlueprintFixture(
        TEXT("/MCPythonTests/TargetIdsNodeClass"));
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

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprint2BriefCountsTest,
    "UnrealMCPython.Blueprint2.BriefCounts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprint2BriefCountsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    UPackage* BriefPackage = CreatePackage(
        TEXT("/Game/Tests/MCP/Blueprint2NativeBriefCounts"));
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
    TestTrue(
        TEXT("Brief fixture interface is implemented"),
        FBlueprintEditorUtils::ImplementNewInterface(
            BriefBlueprint,
            UInterface::StaticClass()->GetClassPathName()));

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

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
