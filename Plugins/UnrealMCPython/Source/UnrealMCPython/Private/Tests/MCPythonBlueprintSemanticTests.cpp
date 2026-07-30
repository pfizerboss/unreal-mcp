// Copyright (c) 2025 GenOrca. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "MCPythonBlueprint2Internal.h"
#include "MCPythonHelper.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNodeUtils.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

namespace UE::MCPython::Blueprint2
{
int32 ClassifySemanticPageCandidateBudget(
    int32 ReturnedBindingCount,
    int32 CandidateBindingCount);
bool CorruptPaletteTemplatePinBindingForTests(const FString& BindingId);
FString InjectStaleDynamicBindingForTests(const FString& ActionId);
}

namespace
{
TSharedPtr<FJsonObject> ParseSemanticResult(const FString& Json)
{
    TSharedPtr<FJsonObject> Result;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    return FJsonSerializer::Deserialize(Reader, Result) ? Result : nullptr;
}

FString SerializeSemanticRequest(const TSharedRef<FJsonObject>& Object)
{
    FString Result;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
    FJsonSerializer::Serialize(Object, Writer);
    return Result;
}

FString SemanticErrorCode(const TSharedPtr<FJsonObject>& Result)
{
    if (!Result || !Result->HasTypedField<EJson::Array>(TEXT("errors")))
    {
        return FString();
    }
    const TArray<TSharedPtr<FJsonValue>>& Errors =
        Result->GetArrayField(TEXT("errors"));
    return Errors.IsEmpty()
        ? FString()
        : Errors[0]->AsObject()->GetStringField(TEXT("code"));
}

FString SemanticErrorPath(const TSharedPtr<FJsonObject>& Result)
{
    if (!Result || !Result->HasTypedField<EJson::Array>(TEXT("errors")))
    {
        return FString();
    }
    const TArray<TSharedPtr<FJsonValue>>& Errors =
        Result->GetArrayField(TEXT("errors"));
    return Errors.IsEmpty()
        ? FString()
        : Errors[0]->AsObject()->GetStringField(TEXT("path"));
}

TSharedPtr<FJsonObject> SemanticErrorDetails(
    const TSharedPtr<FJsonObject>& Result)
{
    if (!Result || !Result->HasTypedField<EJson::Array>(TEXT("errors")))
    {
        return nullptr;
    }
    const TArray<TSharedPtr<FJsonValue>>& Errors =
        Result->GetArrayField(TEXT("errors"));
    return Errors.IsEmpty()
        ? nullptr
        : Errors[0]->AsObject()->GetObjectField(TEXT("details"));
}

void CleanupSemanticPackage(UPackage* Package)
{
    if (!Package)
    {
        return;
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
    CollectGarbage(RF_NoFlags);
}

struct FSemanticFixture
{
    UPackage* Package = nullptr;
    UBlueprint* Blueprint = nullptr;
    UEdGraph* Graph = nullptr;
    UK2Node_CustomEvent* TargetNode = nullptr;
    UEdGraphPin* ExecOutput = nullptr;
    UEdGraphPin* ExecInput = nullptr;
    UEdGraphPin* IntegerOutput = nullptr;
    UEdGraphPin* IntegerInput = nullptr;
    UEdGraphPin* FloatInput = nullptr;
    FString GraphId;
};

FSemanticFixture MakeSemanticFixture(const TCHAR* TestName)
{
    using namespace UE::MCPython::Blueprint2;

    FSemanticFixture Fixture;
    const FString PackageName = FString::Printf(
        TEXT("/Game/__MCPTests/Semantic_%s_%s"),
        TestName,
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    Fixture.Package = CreatePackage(*PackageName);
    Fixture.Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        Fixture.Package,
        TEXT("BP_Semantic"),
        BPTYPE_Normal,
        TestName);
    if (!Fixture.Blueprint || Fixture.Blueprint->UbergraphPages.IsEmpty())
    {
        return Fixture;
    }
    Fixture.Graph = Fixture.Blueprint->UbergraphPages[0];
    Fixture.GraphId = MakeGraphTargetId(Fixture.Blueprint, Fixture.Graph);

    FGraphNodeCreator<UK2Node_CustomEvent> SourceCreator(*Fixture.Graph);
    UK2Node_CustomEvent* SourceNode = SourceCreator.CreateNode(false);
    SourceNode->CustomFunctionName = TEXT("SemanticSource");
    SourceCreator.Finalize();
    Fixture.ExecOutput = SourceNode->FindPin(
        UEdGraphSchema_K2::PN_Then, EGPD_Output);
    Fixture.IntegerOutput = SourceNode->CreatePin(
        EGPD_Output, UEdGraphSchema_K2::PC_Int, TEXT("IntegerOut"));

    FGraphNodeCreator<UK2Node_CustomEvent> TargetCreator(*Fixture.Graph);
    UK2Node_CustomEvent* TargetNode = TargetCreator.CreateNode(false);
    TargetNode->CustomFunctionName = TEXT("SemanticTarget");
    TargetCreator.Finalize();
    Fixture.TargetNode = TargetNode;
    Fixture.ExecInput = TargetNode->CreatePin(
        EGPD_Input, UEdGraphSchema_K2::PC_Exec, TEXT("ExecIn"));
    Fixture.IntegerInput = TargetNode->CreatePin(
        EGPD_Input, UEdGraphSchema_K2::PC_Int, TEXT("IntegerIn"));
    Fixture.FloatInput = TargetNode->CreatePin(
        EGPD_Input,
        UEdGraphSchema_K2::PC_Real,
        UEdGraphSchema_K2::PC_Float,
        TEXT("FloatIn"));
    return Fixture;
}

TSharedRef<FJsonObject> MakeConnectionSuggestionRequest(
    const FSemanticFixture& Fixture,
    UEdGraphPin* SourcePin,
    UEdGraphPin* TargetPin,
    const FString& Query = FString(),
    const bool bAllowConversion = false,
    const FString& Cursor = FString(),
    const int32 Limit = 50)
{
    using namespace UE::MCPython::Blueprint2;

    const TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
    Request->SetStringField(TEXT("graph_id"), Fixture.GraphId);
    Request->SetStringField(
        TEXT("source_pin_id"),
        DescribePinTarget(Fixture.Blueprint, SourcePin).Id);
    Request->SetStringField(
        TEXT("target_pin_id"),
        DescribePinTarget(Fixture.Blueprint, TargetPin).Id);
    Request->SetStringField(TEXT("query"), Query);
    Request->SetObjectField(TEXT("filters"), MakeShared<FJsonObject>());
    Request->SetBoolField(TEXT("allow_conversion"), bAllowConversion);
    Request->SetStringField(TEXT("cursor"), Cursor);
    Request->SetNumberField(TEXT("limit"), Limit);
    return Request;
}

TSharedRef<FJsonObject> MakePinSuggestionRequest(
    const FSemanticFixture& Fixture,
    UEdGraphPin* Pin,
    const FString& Query)
{
    using namespace UE::MCPython::Blueprint2;

    const TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
    Request->SetStringField(TEXT("graph_id"), Fixture.GraphId);
    Request->SetStringField(
        TEXT("pin_id"), DescribePinTarget(Fixture.Blueprint, Pin).Id);
    Request->SetStringField(TEXT("query"), Query);
    Request->SetStringField(TEXT("cursor"), TEXT(""));
    Request->SetNumberField(TEXT("limit"), 50);
    return Request;
}

TSharedRef<FJsonObject> MakeConnectedSpawnRequest(
    const FSemanticFixture& Fixture,
    UEdGraphPin* Pin,
    const FString& ActionId,
    const FString& BindingId,
    const bool bAllowConversion = false)
{
    using namespace UE::MCPython::Blueprint2;

    const TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
    Position->SetNumberField(TEXT("x"), 420.0);
    Position->SetNumberField(TEXT("y"), 160.0);
    const TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
    Request->SetStringField(TEXT("graph_id"), Fixture.GraphId);
    Request->SetStringField(
        TEXT("pin_id"), DescribePinTarget(Fixture.Blueprint, Pin).Id);
    Request->SetStringField(TEXT("action_id"), ActionId);
    Request->SetStringField(TEXT("connection_binding_id"), BindingId);
    Request->SetObjectField(TEXT("position"), Position);
    Request->SetBoolField(TEXT("allow_conversion"), bAllowConversion);
    Request->SetArrayField(TEXT("bindings"), {});
    return Request;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprintSemanticEntryPointsTest,
    "UnrealMCP.Blueprint2.Semantic.EntryPoints",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprintSemanticEntryPointsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const TSharedPtr<FJsonObject> Suggest = ParseSemanticResult(
        UMCPythonHelper::SuggestBlueprintNodesForConnection(nullptr, TEXT("{}")));
    TestTrue(TEXT("connection suggestion returns structured JSON"), Suggest.IsValid());
    TestFalse(
        TEXT("connection suggestion rejects a null Blueprint"),
        Suggest.IsValid() && Suggest->GetBoolField(TEXT("success")));

    const TSharedPtr<FJsonObject> Spawn = ParseSemanticResult(
        UMCPythonHelper::AddBlueprintConnectedActionNode(nullptr, TEXT("{}")));
    TestTrue(TEXT("connected spawn returns structured JSON"), Spawn.IsValid());
    TestFalse(
        TEXT("connected spawn rejects a null Blueprint"),
        Spawn.IsValid() && Spawn->GetBoolField(TEXT("success")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprintSemanticStrictParsingTest,
    "UnrealMCP.Blueprint2.Semantic.StrictParsing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprintSemanticStrictParsingTest::RunTest(
    const FString& Parameters)
{
    (void)Parameters;

    FSemanticFixture Fixture = MakeSemanticFixture(
        TEXT("MCPythonBlueprintSemanticStrictParsingTest"));
    ON_SCOPE_EXIT
    {
        CleanupSemanticPackage(Fixture.Package);
    };
    if (!Fixture.Blueprint || !Fixture.Graph || !Fixture.ExecOutput ||
        !Fixture.ExecInput)
    {
        AddError(TEXT("Strict-parsing fixture could not be created."));
        return false;
    }

    auto ExpectSuggestionFieldError = [&](const FString& Field,
                                           const TSharedPtr<FJsonValue>& Value)
    {
        const TSharedRef<FJsonObject> Request = MakeConnectionSuggestionRequest(
            Fixture, Fixture.ExecOutput, Fixture.ExecInput, TEXT("Sequence"));
        Request->SetField(Field, Value);
        const TSharedPtr<FJsonObject> Result = ParseSemanticResult(
            UMCPythonHelper::SuggestBlueprintNodesForConnection(
                Fixture.Blueprint, SerializeSemanticRequest(Request)));
        TestEqual(
            *FString::Printf(TEXT("%s wrong type is invalid input"), *Field),
            SemanticErrorCode(Result),
            FString(TEXT("INVALID_INPUT")));
        TestEqual(
            *FString::Printf(TEXT("%s reports its exact path"), *Field),
            SemanticErrorPath(Result),
            FString(TEXT("params.")) + Field);
    };
    ExpectSuggestionFieldError(
        TEXT("query"), MakeShared<FJsonValueBoolean>(true));
    ExpectSuggestionFieldError(
        TEXT("cursor"), MakeShared<FJsonValueNumber>(1.0));
    ExpectSuggestionFieldError(
        TEXT("allow_conversion"), MakeShared<FJsonValueString>(TEXT("true")));
    ExpectSuggestionFieldError(
        TEXT("limit"), MakeShared<FJsonValueString>(TEXT("50")));
    ExpectSuggestionFieldError(
        TEXT("filters"), MakeShared<FJsonValueArray>(
            TArray<TSharedPtr<FJsonValue>>()));

    auto ExpectSpawnFieldError = [&](const FString& Field,
                                      const TSharedPtr<FJsonValue>& Value)
    {
        const FString FakeAction = TEXT("action:") + FString::ChrN(40, TEXT('a'));
        const FString FakeBinding =
            TEXT("binding:") + FString::ChrN(40, TEXT('b'));
        const TSharedRef<FJsonObject> Request = MakeConnectedSpawnRequest(
            Fixture, Fixture.ExecOutput, FakeAction, FakeBinding);
        Request->SetField(Field, Value);
        const TSharedPtr<FJsonObject> Result = ParseSemanticResult(
            UMCPythonHelper::AddBlueprintConnectedActionNode(
                Fixture.Blueprint, SerializeSemanticRequest(Request)));
        TestEqual(
            *FString::Printf(TEXT("spawn %s wrong type is invalid input"), *Field),
            SemanticErrorCode(Result),
            FString(TEXT("INVALID_INPUT")));
        TestEqual(
            *FString::Printf(TEXT("spawn %s reports its exact path"), *Field),
            SemanticErrorPath(Result),
            FString(TEXT("params.")) + Field);
    };
    ExpectSpawnFieldError(
        TEXT("allow_conversion"), MakeShared<FJsonValueString>(TEXT("false")));
    ExpectSpawnFieldError(
        TEXT("bindings"), MakeShared<FJsonValueObject>(
            MakeShared<FJsonObject>()));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprintSemanticPageBudgetTest,
    "UnrealMCP.Blueprint2.Semantic.PageBudget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprintSemanticPageBudgetTest::RunTest(
    const FString& Parameters)
{
    (void)Parameters;
    using namespace UE::MCPython::Blueprint2;

    TestEqual(
        TEXT("a representable first card is included"),
        ClassifySemanticPageCandidateBudget(0, 1024),
        1);
    TestEqual(
        TEXT("a representable card that exceeds the page budget stops before it"),
        ClassifySemanticPageCandidateBudget(900, 200),
        0);
    TestEqual(
        TEXT("an individually unrepresentable card is an explicit error"),
        ClassifySemanticPageCandidateBudget(0, 1025),
        -1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprintSemanticSuggestionsTest,
    "UnrealMCP.Blueprint2.Semantic.Suggestions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprintSemanticSuggestionsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    using namespace UE::MCPython::Blueprint2;

    FSemanticFixture Fixture = MakeSemanticFixture(
        TEXT("MCPythonBlueprintSemanticSuggestionsTest"));
    ON_SCOPE_EXIT
    {
        CleanupSemanticPackage(Fixture.Package);
    };
    TestNotNull(TEXT("semantic Blueprint fixture is created"), Fixture.Blueprint);
    TestNotNull(TEXT("semantic graph is created"), Fixture.Graph);
    TestNotNull(TEXT("semantic exec output exists"), Fixture.ExecOutput);
    TestNotNull(TEXT("semantic exec input exists"), Fixture.ExecInput);
    if (!Fixture.Blueprint || !Fixture.Graph || !Fixture.ExecOutput ||
        !Fixture.ExecInput || !Fixture.IntegerOutput || !Fixture.IntegerInput ||
        !Fixture.FloatInput)
    {
        return false;
    }

    const TSharedPtr<FJsonObject> DirectExec = ParseSemanticResult(
        UMCPythonHelper::SuggestBlueprintNodesForConnection(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakeConnectionSuggestionRequest(
                Fixture, Fixture.ExecOutput, Fixture.ExecInput,
                TEXT("Sequence")))));
    TestTrue(
        TEXT("direct exec bridge suggestions succeed"),
        DirectExec.IsValid() && DirectExec->GetBoolField(TEXT("success")));
    if (DirectExec && DirectExec->GetBoolField(TEXT("success")))
    {
        const TSharedPtr<FJsonObject> Data = DirectExec->GetObjectField(TEXT("data"));
        TestTrue(
            TEXT("direct exec bridge returns at least one action"),
            !Data->GetArrayField(TEXT("items")).IsEmpty());
        for (const TSharedPtr<FJsonValue>& ItemValue :
            Data->GetArrayField(TEXT("items")))
        {
            const TSharedPtr<FJsonObject> Item = ItemValue->AsObject();
            const FString ActionId = Item->GetStringField(TEXT("action_id"));
            const TArray<TSharedPtr<FJsonValue>>& Pairs =
                Item->GetArrayField(TEXT("binding_pairs"));
            TestFalse(
                TEXT("every semantic action exposes at least one binding pair"),
                Pairs.IsEmpty());
            bool bSeenConversion = false;
            for (int32 PairIndex = 0; PairIndex < Pairs.Num(); ++PairIndex)
            {
                const TSharedPtr<FJsonObject> Pair = Pairs[PairIndex]->AsObject();
                TestEqual(
                    TEXT("binding pair rank is contiguous"),
                    Pair->GetIntegerField(TEXT("rank")),
                    PairIndex);
                const bool bRequiresConversion =
                    Pair->GetBoolField(TEXT("requires_conversion"));
                TestFalse(
                    TEXT("direct binding pairs precede conversion pairs"),
                    bSeenConversion && !bRequiresConversion);
                bSeenConversion |= bRequiresConversion;

                FPaletteBindingRecord InputBinding;
                FPaletteBindingRecord OutputBinding;
                FError Error;
                TestTrue(
                    TEXT("input binding token resolves for its action"),
                    ResolvePaletteTemplatePinBinding(
                        ActionId,
                        Pair->GetStringField(TEXT("input_binding_id")),
                        InputBinding,
                        Error));
                TestTrue(
                    TEXT("output binding token resolves for its action"),
                    ResolvePaletteTemplatePinBinding(
                        ActionId,
                        Pair->GetStringField(TEXT("output_binding_id")),
                        OutputBinding,
                        Error));
                TestEqual(
                    TEXT("input binding keeps input direction"),
                    InputBinding.PinDirection,
                    FString(TEXT("input")));
                TestEqual(
                    TEXT("output binding keeps output direction"),
                    OutputBinding.PinDirection,
                    FString(TEXT("output")));
            }
        }
    }

    const TSharedPtr<FJsonObject> RepeatedExec = ParseSemanticResult(
        UMCPythonHelper::SuggestBlueprintNodesForConnection(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakeConnectionSuggestionRequest(
                Fixture, Fixture.ExecOutput, Fixture.ExecInput,
                TEXT("Sequence")))));
    if (DirectExec && RepeatedExec &&
        DirectExec->GetBoolField(TEXT("success")) &&
        RepeatedExec->GetBoolField(TEXT("success")))
    {
        const TSharedPtr<FJsonObject> FirstData =
            DirectExec->GetObjectField(TEXT("data"));
        const TSharedPtr<FJsonObject> SecondData =
            RepeatedExec->GetObjectField(TEXT("data"));
        TestEqual(
            TEXT("repeated semantic requests preserve result digest"),
            FirstData->GetStringField(TEXT("result_digest")),
            SecondData->GetStringField(TEXT("result_digest")));
        const FString FirstItemsJson = CanonicalJsonString(
            MakeShared<FJsonValueArray>(
                FirstData->GetArrayField(TEXT("items"))));
        const FString SecondItemsJson = CanonicalJsonString(
            MakeShared<FJsonValueArray>(
                SecondData->GetArrayField(TEXT("items"))));
        TestEqual(
            TEXT("repeated semantic requests preserve cards and token IDs"),
            FirstItemsJson,
            SecondItemsJson);
    }

    const TSharedPtr<FJsonObject> DirectInteger = ParseSemanticResult(
        UMCPythonHelper::SuggestBlueprintNodesForConnection(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakeConnectionSuggestionRequest(
                Fixture, Fixture.IntegerOutput, Fixture.IntegerInput,
                TEXT("Add")))));
    TestTrue(
        TEXT("compatible scalar bridge suggestions succeed"),
        DirectInteger.IsValid() && DirectInteger->GetBoolField(TEXT("success")));
    if (DirectInteger && DirectInteger->GetBoolField(TEXT("success")))
    {
        TestTrue(
            TEXT("compatible scalar bridge returns at least one action"),
            !DirectInteger->GetObjectField(TEXT("data"))
                ->GetArrayField(TEXT("items")).IsEmpty());
    }

    UEdGraphPin* ConversionTarget = nullptr;
    FString ConversionQuery;
    const TSharedPtr<FJsonObject> ConversionPinSuggestions = ParseSemanticResult(
        UMCPythonHelper::SuggestBlueprintNodesForPin(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakePinSuggestionRequest(
                Fixture, Fixture.IntegerOutput, TEXT("")))));
    if (ConversionPinSuggestions &&
        ConversionPinSuggestions->GetBoolField(TEXT("success")))
    {
        for (const TSharedPtr<FJsonValue>& ItemValue :
            ConversionPinSuggestions->GetObjectField(TEXT("data"))
                ->GetArrayField(TEXT("items")))
        {
            const TSharedPtr<FJsonObject> Item = ItemValue->AsObject();
            bool bHasConversionInput = false;
            for (const TSharedPtr<FJsonValue>& BindingValue :
                Item->GetArrayField(TEXT("connection_bindings")))
            {
                bHasConversionInput |= BindingValue->AsObject()
                    ->GetObjectField(TEXT("response"))
                    ->GetBoolField(TEXT("requires_conversion"));
            }
            if (!bHasConversionInput)
            {
                continue;
            }
            const TSharedRef<FJsonObject> DescribeRequest = MakeShared<FJsonObject>();
            DescribeRequest->SetStringField(
                TEXT("action_id"), Item->GetStringField(TEXT("action_id")));
            const TSharedPtr<FJsonObject> Description = ParseSemanticResult(
                UMCPythonHelper::DescribeBlueprintNodeAction(
                    SerializeSemanticRequest(DescribeRequest)));
            if (!Description || !Description->GetBoolField(TEXT("success")))
            {
                continue;
            }
            for (const TSharedPtr<FJsonValue>& PinValue :
                Description->GetObjectField(TEXT("data"))
                    ->GetArrayField(TEXT("template_pins")))
            {
                const TSharedPtr<FJsonObject> Pin = PinValue->AsObject();
                if (Pin->GetStringField(TEXT("direction")) != TEXT("output"))
                {
                    continue;
                }
                const TSharedPtr<FJsonObject> Type = Pin->GetObjectField(TEXT("type"));
                if (Type->GetStringField(TEXT("kind")) == TEXT("exec"))
                {
                    continue;
                }
                FEdGraphPinType OutputType;
                FError TypeError;
                if (ParseTypeSpec(
                        Type.ToSharedRef(),
                        OutputType,
                        TypeError,
                        TEXT("test.output_type")))
                {
                    ConversionTarget = Fixture.TargetNode->CreatePin(
                        EGPD_Input, OutputType, TEXT("ConversionTarget"));
                    ConversionQuery = Item->GetStringField(TEXT("title"));
                    break;
                }
            }
            if (ConversionTarget)
            {
                break;
            }
        }
    }
    TestNotNull(
        TEXT("conversion capability exposes one data output type"),
        ConversionTarget);

    const TSharedPtr<FJsonObject> StrictConversion = ParseSemanticResult(
        UMCPythonHelper::SuggestBlueprintNodesForConnection(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakeConnectionSuggestionRequest(
                Fixture,
                Fixture.IntegerOutput,
                ConversionTarget ? ConversionTarget : Fixture.FloatInput,
                ConversionQuery,
                false))));
    const TSharedPtr<FJsonObject> AllowedConversion = ParseSemanticResult(
        UMCPythonHelper::SuggestBlueprintNodesForConnection(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakeConnectionSuggestionRequest(
                Fixture,
                Fixture.IntegerOutput,
                ConversionTarget ? ConversionTarget : Fixture.FloatInput,
                ConversionQuery,
                true))));
    TestTrue(
        TEXT("strict conversion suggestion request succeeds read-only"),
        StrictConversion.IsValid() &&
            StrictConversion->GetBoolField(TEXT("success")));
    TestTrue(
        TEXT("allowed conversion suggestion request succeeds"),
        AllowedConversion.IsValid() &&
            AllowedConversion->GetBoolField(TEXT("success")));
    if (StrictConversion && AllowedConversion &&
        StrictConversion->GetBoolField(TEXT("success")) &&
        AllowedConversion->GetBoolField(TEXT("success")))
    {
        for (const TSharedPtr<FJsonValue>& ItemValue :
            StrictConversion->GetObjectField(TEXT("data"))
                ->GetArrayField(TEXT("items")))
        {
            for (const TSharedPtr<FJsonValue>& PairValue :
                ItemValue->AsObject()->GetArrayField(TEXT("binding_pairs")))
            {
                TestFalse(
                    TEXT("strict conversion policy removes conversion pairs"),
                    PairValue->AsObject()->GetBoolField(
                        TEXT("requires_conversion")));
            }
        }
        const TArray<TSharedPtr<FJsonValue>>& ConversionItems =
            AllowedConversion->GetObjectField(TEXT("data"))
                ->GetArrayField(TEXT("items"));
        TestFalse(
            TEXT("allow_conversion keeps native Sin bridge pairs"),
            ConversionItems.IsEmpty());
        bool bSawConversionPair = false;
        for (const TSharedPtr<FJsonValue>& ItemValue : ConversionItems)
        {
            for (const TSharedPtr<FJsonValue>& PairValue :
                ItemValue->AsObject()->GetArrayField(TEXT("binding_pairs")))
            {
                bSawConversionPair |= PairValue->AsObject()->GetBoolField(
                    TEXT("requires_conversion"));
            }
        }
        TestTrue(
            TEXT("allow_conversion exposes at least one conversion-aware pair"),
            bSawConversionPair);
    }

    const TSharedPtr<FJsonObject> FirstPage = ParseSemanticResult(
        UMCPythonHelper::SuggestBlueprintNodesForConnection(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakeConnectionSuggestionRequest(
                Fixture, Fixture.ExecOutput, Fixture.ExecInput,
                TEXT(""), false, TEXT(""), 1))));
    TestTrue(
        TEXT("bounded semantic page succeeds"),
        FirstPage.IsValid() && FirstPage->GetBoolField(TEXT("success")));
    FString Cursor;
    FString FirstActionId;
    if (FirstPage && FirstPage->GetBoolField(TEXT("success")))
    {
        const TSharedPtr<FJsonObject> Data = FirstPage->GetObjectField(TEXT("data"));
        Cursor = Data->GetStringField(TEXT("next_cursor"));
        TestFalse(TEXT("bounded semantic page returns a cursor"), Cursor.IsEmpty());
        const TArray<TSharedPtr<FJsonValue>>& Items =
            Data->GetArrayField(TEXT("items"));
        if (!Items.IsEmpty())
        {
            FirstActionId = Items[0]->AsObject()->GetStringField(TEXT("action_id"));
        }
    }
    if (!Cursor.IsEmpty())
    {
        const TSharedPtr<FJsonObject> SecondPage = ParseSemanticResult(
            UMCPythonHelper::SuggestBlueprintNodesForConnection(
                Fixture.Blueprint,
                SerializeSemanticRequest(MakeConnectionSuggestionRequest(
                    Fixture, Fixture.ExecOutput, Fixture.ExecInput,
                    TEXT(""), false, Cursor, 1))));
        TestTrue(
            TEXT("semantic cursor continues the same result set"),
            SecondPage.IsValid() && SecondPage->GetBoolField(TEXT("success")));
        if (SecondPage && SecondPage->GetBoolField(TEXT("success")))
        {
            const TArray<TSharedPtr<FJsonValue>>& Items =
                SecondPage->GetObjectField(TEXT("data"))
                    ->GetArrayField(TEXT("items"));
            if (!Items.IsEmpty())
            {
                TestNotEqual(
                    TEXT("semantic pagination returns no duplicate action"),
                    Items[0]->AsObject()->GetStringField(TEXT("action_id")),
                    FirstActionId);
            }
        }

        const TSharedPtr<FJsonObject> ChangedPolicy = ParseSemanticResult(
            UMCPythonHelper::SuggestBlueprintNodesForConnection(
                Fixture.Blueprint,
                SerializeSemanticRequest(MakeConnectionSuggestionRequest(
                    Fixture, Fixture.ExecOutput, Fixture.ExecInput,
                    TEXT(""), true, Cursor, 1))));
        TestEqual(
            TEXT("changed conversion policy rejects semantic cursor"),
            SemanticErrorCode(ChangedPolicy),
            FString(TEXT("PRECONDITION_FAILED")));

        const TSharedPtr<FJsonObject> ChangedQuery = ParseSemanticResult(
            UMCPythonHelper::SuggestBlueprintNodesForConnection(
                Fixture.Blueprint,
                SerializeSemanticRequest(MakeConnectionSuggestionRequest(
                    Fixture, Fixture.ExecOutput, Fixture.ExecInput,
                    TEXT("Sequence"), false, Cursor, 1))));
        TestEqual(
            TEXT("changed query rejects semantic cursor"),
            SemanticErrorCode(ChangedQuery),
            FString(TEXT("PRECONDITION_FAILED")));

        const TSharedPtr<FJsonObject> ChangedLimit = ParseSemanticResult(
            UMCPythonHelper::SuggestBlueprintNodesForConnection(
                Fixture.Blueprint,
                SerializeSemanticRequest(MakeConnectionSuggestionRequest(
                    Fixture, Fixture.ExecOutput, Fixture.ExecInput,
                    TEXT(""), false, Cursor, 2))));
        TestEqual(
            TEXT("changed limit rejects semantic cursor"),
            SemanticErrorCode(ChangedLimit),
            FString(TEXT("PRECONDITION_FAILED")));

        FString TamperedCursor = Cursor;
        TamperedCursor[0] = TamperedCursor[0] == TEXT('p')
            ? TEXT('q')
            : TEXT('p');
        const TSharedPtr<FJsonObject> Tampered = ParseSemanticResult(
            UMCPythonHelper::SuggestBlueprintNodesForConnection(
                Fixture.Blueprint,
                SerializeSemanticRequest(MakeConnectionSuggestionRequest(
                    Fixture, Fixture.ExecOutput, Fixture.ExecInput,
                    TEXT(""), false, TamperedCursor, 1))));
        TestEqual(
            TEXT("tampered semantic cursor is invalid input"),
            SemanticErrorCode(Tampered),
            FString(TEXT("INVALID_INPUT")));

        const FGuid OriginalSourcePinId = Fixture.ExecOutput->PinId;
        Fixture.ExecOutput->PinId = FGuid::NewGuid();
        const TSharedPtr<FJsonObject> ChangedPinIdentity = ParseSemanticResult(
            UMCPythonHelper::SuggestBlueprintNodesForConnection(
                Fixture.Blueprint,
                SerializeSemanticRequest(MakeConnectionSuggestionRequest(
                    Fixture, Fixture.ExecOutput, Fixture.ExecInput,
                    TEXT(""), false, Cursor, 1))));
        Fixture.ExecOutput->PinId = OriginalSourcePinId;
        TestEqual(
            TEXT("changed stable pin identity rejects semantic cursor"),
            SemanticErrorCode(ChangedPinIdentity),
            FString(TEXT("PRECONDITION_FAILED")));
    }

    const TSharedRef<FJsonObject> ZeroGuidRequest =
        MakeConnectionSuggestionRequest(
            Fixture, Fixture.ExecOutput, Fixture.ExecInput, TEXT("Sequence"));
    ZeroGuidRequest->SetStringField(
        TEXT("source_pin_id"),
        TEXT("pin:00000000-0000-0000-0000-000000000000"));
    const TSharedPtr<FJsonObject> ZeroGuid = ParseSemanticResult(
        UMCPythonHelper::SuggestBlueprintNodesForConnection(
            Fixture.Blueprint, SerializeSemanticRequest(ZeroGuidRequest)));
    TestEqual(
        TEXT("zero GUID pin identity is invalid input"),
        SemanticErrorCode(ZeroGuid),
        FString(TEXT("INVALID_INPUT")));

    UEdGraph* ForeignGraph = NewObject<UEdGraph>(Fixture.Blueprint);
    ForeignGraph->Schema = UEdGraphSchema_K2::StaticClass();
    FGraphNodeCreator<UK2Node_CustomEvent> ForeignCreator(*ForeignGraph);
    UK2Node_CustomEvent* ForeignNode = ForeignCreator.CreateNode(false);
    ForeignNode->CustomFunctionName = TEXT("ForeignSemanticTarget");
    ForeignCreator.Finalize();
    UEdGraphPin* ForeignInput = ForeignNode->CreatePin(
        EGPD_Input, UEdGraphSchema_K2::PC_Exec, TEXT("ForeignExecIn"));
    const TSharedRef<FJsonObject> CrossGraphRequest =
        MakeConnectionSuggestionRequest(
            Fixture, Fixture.ExecOutput, Fixture.ExecInput, TEXT("Sequence"));
    CrossGraphRequest->SetStringField(
        TEXT("target_pin_id"),
        DescribePinTarget(Fixture.Blueprint, ForeignInput).Id);
    const TSharedPtr<FJsonObject> CrossGraph = ParseSemanticResult(
        UMCPythonHelper::SuggestBlueprintNodesForConnection(
            Fixture.Blueprint, SerializeSemanticRequest(CrossGraphRequest)));
    TestEqual(
        TEXT("cross-graph pin is rejected before candidate generation"),
        SemanticErrorCode(CrossGraph),
        FString(TEXT("PRECONDITION_FAILED")));

    UEdGraphPin* WildcardInput = Fixture.TargetNode->CreatePin(
        EGPD_Input, UEdGraphSchema_K2::PC_Wildcard, TEXT("WildcardIn"));
    const TSharedPtr<FJsonObject> Wildcard = ParseSemanticResult(
        UMCPythonHelper::SuggestBlueprintNodesForConnection(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakeConnectionSuggestionRequest(
                Fixture, Fixture.IntegerOutput, WildcardInput, TEXT("Add")))));
    TestTrue(
        TEXT("native wildcard compatibility request succeeds"),
        Wildcard.IsValid() && Wildcard->GetBoolField(TEXT("success")));
    if (Wildcard && Wildcard->GetBoolField(TEXT("success")))
    {
        TestFalse(
            TEXT("native wildcard capability exposes at least one bridge"),
            Wildcard->GetObjectField(TEXT("data"))
                ->GetArrayField(TEXT("items")).IsEmpty());
    }

    const TSharedPtr<FJsonObject> Reversed = ParseSemanticResult(
        UMCPythonHelper::SuggestBlueprintNodesForConnection(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakeConnectionSuggestionRequest(
                Fixture, Fixture.ExecInput, Fixture.ExecOutput))));
    TestEqual(
        TEXT("reversed pins are invalid input"),
        SemanticErrorCode(Reversed),
        FString(TEXT("INVALID_INPUT")));

    const TSharedPtr<FJsonObject> Mixed = ParseSemanticResult(
        UMCPythonHelper::SuggestBlueprintNodesForConnection(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakeConnectionSuggestionRequest(
                Fixture, Fixture.ExecOutput, Fixture.IntegerInput))));
    TestEqual(
        TEXT("exec and data pins cannot share a bridge request"),
        SemanticErrorCode(Mixed),
        FString(TEXT("INVALID_INPUT")));

    const TSharedPtr<FJsonObject> Duplicate = ParseSemanticResult(
        UMCPythonHelper::SuggestBlueprintNodesForConnection(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakeConnectionSuggestionRequest(
                Fixture, Fixture.ExecOutput, Fixture.ExecOutput))));
    TestEqual(
        TEXT("duplicate pin IDs are invalid input"),
        SemanticErrorCode(Duplicate),
        FString(TEXT("INVALID_INPUT")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprintSemanticConnectedSpawnTest,
    "UnrealMCP.Blueprint2.Semantic.ConnectedSpawn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprintSemanticConnectedSpawnTest::RunTest(
    const FString& Parameters)
{
    (void)Parameters;
    using namespace UE::MCPython::Blueprint2;

    FSemanticFixture Fixture = MakeSemanticFixture(
        TEXT("MCPythonBlueprintSemanticConnectedSpawnTest"));
    ON_SCOPE_EXIT
    {
        CleanupSemanticPackage(Fixture.Package);
    };
    if (!Fixture.Blueprint || !Fixture.Graph || !Fixture.ExecOutput)
    {
        AddError(TEXT("Connected-spawn fixture could not be created."));
        return false;
    }

    const TSharedPtr<FJsonObject> Suggestions = ParseSemanticResult(
        UMCPythonHelper::SuggestBlueprintNodesForPin(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakePinSuggestionRequest(
                Fixture, Fixture.ExecOutput, TEXT("Sequence")))));
    TestTrue(
        TEXT("pin-context Sequence suggestion succeeds"),
        Suggestions.IsValid() && Suggestions->GetBoolField(TEXT("success")));
    FString ActionId;
    FString BindingId;
    if (Suggestions && Suggestions->GetBoolField(TEXT("success")))
    {
        for (const TSharedPtr<FJsonValue>& ItemValue :
            Suggestions->GetObjectField(TEXT("data"))
                ->GetArrayField(TEXT("items")))
        {
            const TSharedPtr<FJsonObject> Item = ItemValue->AsObject();
            for (const TSharedPtr<FJsonValue>& BindingValue :
                Item->GetArrayField(TEXT("connection_bindings")))
            {
                const TSharedPtr<FJsonObject> Binding = BindingValue->AsObject();
                if (!Binding->GetObjectField(TEXT("response"))
                        ->GetBoolField(TEXT("requires_conversion")))
                {
                    ActionId = Item->GetStringField(TEXT("action_id"));
                    BindingId = Binding->GetStringField(TEXT("binding_id"));
                    break;
                }
            }
            if (!BindingId.IsEmpty())
            {
                break;
            }
        }
    }
    TestFalse(TEXT("Sequence action ID is available"), ActionId.IsEmpty());
    TestFalse(TEXT("Sequence input binding ID is available"), BindingId.IsEmpty());
    if (ActionId.IsEmpty() || BindingId.IsEmpty())
    {
        return false;
    }

    FString ConnectionActionId;
    FString ForeignBindingId;
    const TSharedPtr<FJsonObject> ConnectionSuggestions = ParseSemanticResult(
        UMCPythonHelper::SuggestBlueprintNodesForConnection(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakeConnectionSuggestionRequest(
                Fixture, Fixture.ExecOutput, Fixture.ExecInput,
                TEXT("Sequence")))));
    if (ConnectionSuggestions &&
        ConnectionSuggestions->GetBoolField(TEXT("success")))
    {
        const TArray<TSharedPtr<FJsonValue>>& Items =
            ConnectionSuggestions->GetObjectField(TEXT("data"))
                ->GetArrayField(TEXT("items"));
        if (!Items.IsEmpty())
        {
            const TSharedPtr<FJsonObject> Item = Items[0]->AsObject();
            const TArray<TSharedPtr<FJsonValue>>& Pairs =
                Item->GetArrayField(TEXT("binding_pairs"));
            if (!Pairs.IsEmpty())
            {
                ConnectionActionId = Item->GetStringField(TEXT("action_id"));
                ForeignBindingId = Pairs[0]->AsObject()->GetStringField(
                    TEXT("input_binding_id"));
            }
        }
    }
    TestFalse(
        TEXT("connection-context action is available for rejection proof"),
        ConnectionActionId.IsEmpty());
    TestFalse(
        TEXT("foreign template binding is available for rejection proof"),
        ForeignBindingId.IsEmpty());

    const FGuid OriginalExecOutputId = Fixture.ExecOutput->PinId;
    Fixture.ExecOutput->PinId = FGuid::NewGuid();
    const TSharedPtr<FJsonObject> ChangedSourceIdentity = ParseSemanticResult(
        UMCPythonHelper::AddBlueprintConnectedActionNode(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakeConnectedSpawnRequest(
                Fixture, Fixture.ExecOutput, ActionId, BindingId))));
    Fixture.ExecOutput->PinId = OriginalExecOutputId;
    TestEqual(
        TEXT("changed source pin identity makes the action stale"),
        SemanticErrorCode(ChangedSourceIdentity),
        FString(TEXT("PRECONDITION_FAILED")));

    const FEdGraphPinType OriginalExecType = Fixture.ExecOutput->PinType;
    Fixture.ExecOutput->PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
    const TSharedPtr<FJsonObject> ChangedResultSet = ParseSemanticResult(
        UMCPythonHelper::AddBlueprintConnectedActionNode(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakeConnectedSpawnRequest(
                Fixture, Fixture.ExecOutput, ActionId, BindingId))));
    Fixture.ExecOutput->PinType = OriginalExecType;
    TestEqual(
        TEXT("changed native source type makes the action result stale"),
        SemanticErrorCode(ChangedResultSet),
        FString(TEXT("PRECONDITION_FAILED")));

    const TSubclassOf<UEdGraphSchema> OriginalSchema = Fixture.Graph->Schema;
    Fixture.Graph->Schema = UEdGraphSchema::StaticClass();
    const TSharedPtr<FJsonObject> ChangedSchema = ParseSemanticResult(
        UMCPythonHelper::AddBlueprintConnectedActionNode(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakeConnectedSpawnRequest(
                Fixture, Fixture.ExecOutput, ActionId, BindingId))));
    Fixture.Graph->Schema = OriginalSchema;
    TestEqual(
        TEXT("changed graph schema makes the action stale"),
        SemanticErrorCode(ChangedSchema),
        FString(TEXT("PRECONDITION_FAILED")));

    TSharedPtr<FJsonObject> BeforeInjectedSnapshot;
    FError InjectionSnapshotError;
    TestTrue(
        TEXT("snapshot before injected connected-spawn failure succeeds"),
        BuildBlueprintGraphSnapshot(
            Fixture.Blueprint,
            {Fixture.GraphId},
            BeforeInjectedSnapshot,
            InjectionSnapshotError));
    const FString BeforeInjectedJson = CanonicalJsonString(
        MakeShared<FJsonValueObject>(BeforeInjectedSnapshot.ToSharedRef()));
    const int32 BeforeInjectedNodeCount = Fixture.Graph->Nodes.Num();
    SetSemanticConnectedSpawnFailurePointForTests(
        ESemanticConnectedSpawnFailurePoint::AfterInvoke);
    const TSharedPtr<FJsonObject> InjectedFailure = ParseSemanticResult(
        UMCPythonHelper::AddBlueprintConnectedActionNode(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakeConnectedSpawnRequest(
                Fixture, Fixture.ExecOutput, ActionId, BindingId))));
    SetSemanticConnectedSpawnFailurePointForTests(
        ESemanticConnectedSpawnFailurePoint::None);
    TestEqual(
        TEXT("post-Invoke injected failure is operation failed"),
        SemanticErrorCode(InjectedFailure),
        FString(TEXT("OPERATION_FAILED")));
    TestEqual(
        TEXT("post-Invoke rollback restores node count"),
        Fixture.Graph->Nodes.Num(),
        BeforeInjectedNodeCount);
    TSharedPtr<FJsonObject> AfterInjectedSnapshot;
    TestTrue(
        TEXT("snapshot after injected connected-spawn failure succeeds"),
        BuildBlueprintGraphSnapshot(
            Fixture.Blueprint,
            {Fixture.GraphId},
            AfterInjectedSnapshot,
            InjectionSnapshotError));
    TestEqual(
        TEXT("post-Invoke rollback restores the exact graph snapshot"),
        CanonicalJsonString(MakeShared<FJsonValueObject>(
            AfterInjectedSnapshot.ToSharedRef())),
        BeforeInjectedJson);

    for (const ESemanticConnectedSpawnFailurePoint FailurePoint : {
            ESemanticConnectedSpawnFailurePoint::OutOfGraphResult,
            ESemanticConnectedSpawnFailurePoint::MissingActualPin,
            ESemanticConnectedSpawnFailurePoint::BeforeTryCreate,
            ESemanticConnectedSpawnFailurePoint::AfterTryCreateBreakTopology,
            ESemanticConnectedSpawnFailurePoint::ZeroVisiblePinGuid})
    {
        SetSemanticConnectedSpawnFailurePointForTests(FailurePoint);
        const TSharedPtr<FJsonObject> Failure = ParseSemanticResult(
            UMCPythonHelper::AddBlueprintConnectedActionNode(
                Fixture.Blueprint,
                SerializeSemanticRequest(MakeConnectedSpawnRequest(
                    Fixture, Fixture.ExecOutput, ActionId, BindingId))));
        SetSemanticConnectedSpawnFailurePointForTests(
            ESemanticConnectedSpawnFailurePoint::None);
        TestEqual(
            TEXT("injected mutation-boundary failure has its canonical code"),
            SemanticErrorCode(Failure),
            FailurePoint ==
                    ESemanticConnectedSpawnFailurePoint::OutOfGraphResult
                ? FString(TEXT("PRECONDITION_FAILED"))
                : FString(TEXT("OPERATION_FAILED")));
        TSharedPtr<FJsonObject> RestoredSnapshot;
        TestTrue(
            TEXT("mutation-boundary rollback snapshot succeeds"),
            BuildBlueprintGraphSnapshot(
                Fixture.Blueprint,
                {Fixture.GraphId},
                RestoredSnapshot,
                InjectionSnapshotError));
        TestEqual(
            TEXT("mutation-boundary rollback is exact"),
            CanonicalJsonString(MakeShared<FJsonValueObject>(
                RestoredSnapshot.ToSharedRef())),
            BeforeInjectedJson);
    }

    SetSemanticConnectedSpawnFailurePointForTests(
        ESemanticConnectedSpawnFailurePoint::AfterRollbackResidual);
    const TSharedPtr<FJsonObject> ResidualFailure = ParseSemanticResult(
        UMCPythonHelper::AddBlueprintConnectedActionNode(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakeConnectedSpawnRequest(
                Fixture, Fixture.ExecOutput, ActionId, BindingId))));
    SetSemanticConnectedSpawnFailurePointForTests(
        ESemanticConnectedSpawnFailurePoint::None);
    TestEqual(
        TEXT("residual rollback mismatch has canonical error code"),
        SemanticErrorCode(ResidualFailure),
        FString(TEXT("ROLLBACK_FAILED")));
    const TSharedPtr<FJsonObject> ResidualDetails =
        SemanticErrorDetails(ResidualFailure);
    TestTrue(
        TEXT("rollback mismatch returns details"),
        ResidualDetails.IsValid());
    if (ResidualDetails)
    {
        TestTrue(
            TEXT("rollback mismatch returns before digest"),
            ResidualDetails->GetStringField(TEXT("before_digest"))
                .StartsWith(TEXT("sha1:")));
        TestTrue(
            TEXT("rollback mismatch returns after digest"),
            ResidualDetails->GetStringField(TEXT("after_digest"))
                .StartsWith(TEXT("sha1:")));
        const TArray<TSharedPtr<FJsonValue>>& AffectedIds =
            ResidualDetails->GetArrayField(TEXT("affected_ids"));
        TestFalse(
            TEXT("rollback mismatch returns stable affected IDs"),
            AffectedIds.IsEmpty());
        for (const TSharedPtr<FJsonValue>& Id : AffectedIds)
        {
            const FString Value = Id->AsString();
            TestTrue(
                TEXT("affected rollback ID is stable"),
                Value.StartsWith(TEXT("graph:")) ||
                    Value.StartsWith(TEXT("node:")) ||
                    Value.StartsWith(TEXT("pin:")));
        }
    }

    const int32 BeforeNodeCount = Fixture.Graph->Nodes.Num();
    const TSharedPtr<FJsonObject> Spawn = ParseSemanticResult(
        UMCPythonHelper::AddBlueprintConnectedActionNode(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakeConnectedSpawnRequest(
                Fixture, Fixture.ExecOutput, ActionId, BindingId))));
    TestTrue(
        TEXT("direct connected spawn succeeds"),
        Spawn.IsValid() && Spawn->GetBoolField(TEXT("success")));
    if (Spawn && Spawn->GetBoolField(TEXT("success")))
    {
        const TSharedPtr<FJsonObject> Data = Spawn->GetObjectField(TEXT("data"));
        TestTrue(
            TEXT("connected spawn returns a stable node ID"),
            Data->GetStringField(TEXT("node_id")).StartsWith(TEXT("node:")));
        TestFalse(
            TEXT("connected spawn returns stable visible pin IDs"),
            Data->GetArrayField(TEXT("pin_ids")).IsEmpty());
        TestFalse(
            TEXT("connected spawn reports its semantic connection"),
            Data->GetArrayField(TEXT("connections")).IsEmpty());
        TestTrue(
            TEXT("connected spawn records a transaction"),
            Data->GetBoolField(TEXT("transaction_recorded")));
        TestFalse(
            TEXT("connected spawn never saves implicitly"),
            Data->GetBoolField(TEXT("saved")));
    }
    TestEqual(
        TEXT("direct connected spawn adds exactly one node"),
        Fixture.Graph->Nodes.Num(),
        BeforeNodeCount + 1);
    TestFalse(
        TEXT("source pin is linked after connected spawn"),
        Fixture.ExecOutput->LinkedTo.IsEmpty());

    const TSharedPtr<FJsonObject> ScalarSuggestions = ParseSemanticResult(
        UMCPythonHelper::SuggestBlueprintNodesForPin(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakePinSuggestionRequest(
                Fixture, Fixture.IntegerOutput, TEXT("Add")))));
    FString ScalarActionId;
    FString ScalarBindingId;
    if (ScalarSuggestions && ScalarSuggestions->GetBoolField(TEXT("success")))
    {
        for (const TSharedPtr<FJsonValue>& ItemValue :
            ScalarSuggestions->GetObjectField(TEXT("data"))
                ->GetArrayField(TEXT("items")))
        {
            const TSharedPtr<FJsonObject> Item = ItemValue->AsObject();
            if (!Item->GetArrayField(TEXT("bindings")).IsEmpty())
            {
                continue;
            }
            for (const TSharedPtr<FJsonValue>& BindingValue :
                Item->GetArrayField(TEXT("connection_bindings")))
            {
                const TSharedPtr<FJsonObject> Binding = BindingValue->AsObject();
                if (!Binding->GetObjectField(TEXT("response"))
                        ->GetBoolField(TEXT("requires_conversion")))
                {
                    ScalarActionId = Item->GetStringField(TEXT("action_id"));
                    ScalarBindingId = Binding->GetStringField(TEXT("binding_id"));
                    break;
                }
            }
            if (!ScalarBindingId.IsEmpty())
            {
                break;
            }
        }
    }
    TestFalse(TEXT("direct scalar action is available"), ScalarActionId.IsEmpty());
    TestFalse(TEXT("direct scalar binding is available"), ScalarBindingId.IsEmpty());
    if (!ScalarActionId.IsEmpty() && !ScalarBindingId.IsEmpty())
    {
        const int32 BeforeScalarNodeCount = Fixture.Graph->Nodes.Num();
        const TSharedPtr<FJsonObject> ScalarSpawn = ParseSemanticResult(
            UMCPythonHelper::AddBlueprintConnectedActionNode(
                Fixture.Blueprint,
                SerializeSemanticRequest(MakeConnectedSpawnRequest(
                    Fixture,
                    Fixture.IntegerOutput,
                    ScalarActionId,
                    ScalarBindingId))));
        TestTrue(
            TEXT("direct scalar connected spawn succeeds"),
            ScalarSpawn.IsValid() && ScalarSpawn->GetBoolField(TEXT("success")));
        TestEqual(
            TEXT("direct scalar connected spawn adds exactly one node"),
            Fixture.Graph->Nodes.Num(),
            BeforeScalarNodeCount + 1);
    }

    TSharedPtr<FJsonObject> BeforeRejectedSnapshot;
    FError SnapshotError;
    TestTrue(
        TEXT("snapshot before rejected connected spawns succeeds"),
        BuildBlueprintGraphSnapshot(
            Fixture.Blueprint,
            {Fixture.GraphId},
            BeforeRejectedSnapshot,
            SnapshotError));
    const FString BeforeRejectedJson = CanonicalJsonString(
        MakeShared<FJsonValueObject>(BeforeRejectedSnapshot.ToSharedRef()));
    const int32 BeforeRejectedNodeCount = Fixture.Graph->Nodes.Num();

    if (!ConnectionActionId.IsEmpty() && !ForeignBindingId.IsEmpty())
    {
        const TSharedPtr<FJsonObject> WrongContext = ParseSemanticResult(
            UMCPythonHelper::AddBlueprintConnectedActionNode(
                Fixture.Blueprint,
                SerializeSemanticRequest(MakeConnectedSpawnRequest(
                    Fixture,
                    Fixture.ExecOutput,
                    ConnectionActionId,
                    ForeignBindingId))));
        TestFalse(
            TEXT("connection-context action cannot drive connected spawn"),
            WrongContext.IsValid() && WrongContext->GetBoolField(TEXT("success")));

        const TSharedPtr<FJsonObject> ForeignBinding = ParseSemanticResult(
            UMCPythonHelper::AddBlueprintConnectedActionNode(
                Fixture.Blueprint,
                SerializeSemanticRequest(MakeConnectedSpawnRequest(
                    Fixture,
                    Fixture.ExecOutput,
                    ActionId,
                    ForeignBindingId))));
        TestEqual(
            TEXT("foreign template binding is invalid input"),
            SemanticErrorCode(ForeignBinding),
            FString(TEXT("INVALID_INPUT")));
    }
    TestEqual(
        TEXT("rejected connected spawns add no nodes"),
        Fixture.Graph->Nodes.Num(),
        BeforeRejectedNodeCount);
    TSharedPtr<FJsonObject> AfterRejectedSnapshot;
    TestTrue(
        TEXT("snapshot after rejected connected spawns succeeds"),
        BuildBlueprintGraphSnapshot(
            Fixture.Blueprint,
            {Fixture.GraphId},
            AfterRejectedSnapshot,
            SnapshotError));
    TestEqual(
        TEXT("rejected connected spawns preserve the exact graph snapshot"),
        CanonicalJsonString(MakeShared<FJsonValueObject>(
            AfterRejectedSnapshot.ToSharedRef())),
        BeforeRejectedJson);

    const TSharedPtr<FJsonObject> ConversionSuggestions = ParseSemanticResult(
        UMCPythonHelper::SuggestBlueprintNodesForPin(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakePinSuggestionRequest(
                Fixture, Fixture.IntegerOutput, TEXT("Sin")))));
    FString ConversionActionId;
    FString ConversionBindingId;
    if (ConversionSuggestions &&
        ConversionSuggestions->GetBoolField(TEXT("success")))
    {
        for (const TSharedPtr<FJsonValue>& ItemValue :
            ConversionSuggestions->GetObjectField(TEXT("data"))
                ->GetArrayField(TEXT("items")))
        {
            const TSharedPtr<FJsonObject> Item = ItemValue->AsObject();
            if (!Item->GetArrayField(TEXT("bindings")).IsEmpty())
            {
                continue;
            }
            for (const TSharedPtr<FJsonValue>& BindingValue :
                Item->GetArrayField(TEXT("connection_bindings")))
            {
                const TSharedPtr<FJsonObject> Binding = BindingValue->AsObject();
                if (Binding->GetObjectField(TEXT("response"))
                        ->GetBoolField(TEXT("requires_conversion")))
                {
                    ConversionActionId = Item->GetStringField(TEXT("action_id"));
                    ConversionBindingId = Binding->GetStringField(TEXT("binding_id"));
                    break;
                }
            }
            if (!ConversionBindingId.IsEmpty())
            {
                break;
            }
        }
    }
    TestFalse(
        TEXT("conversion action is available from native pin suggestions"),
        ConversionActionId.IsEmpty());
    TestFalse(
        TEXT("conversion binding is available from native pin suggestions"),
        ConversionBindingId.IsEmpty());
    if (!ConversionActionId.IsEmpty() && !ConversionBindingId.IsEmpty())
    {
        const int32 BeforeConversionNodeCount = Fixture.Graph->Nodes.Num();
        const TSharedPtr<FJsonObject> ConversionDenied = ParseSemanticResult(
            UMCPythonHelper::AddBlueprintConnectedActionNode(
                Fixture.Blueprint,
                SerializeSemanticRequest(MakeConnectedSpawnRequest(
                    Fixture,
                    Fixture.IntegerOutput,
                    ConversionActionId,
                    ConversionBindingId,
                    false))));
        TestEqual(
            TEXT("conversion requires explicit opt in"),
            SemanticErrorCode(ConversionDenied),
            FString(TEXT("INVALID_INPUT")));
        TestEqual(
            TEXT("denied conversion mutates no nodes"),
            Fixture.Graph->Nodes.Num(),
            BeforeConversionNodeCount);

        const TSharedPtr<FJsonObject> ConversionAllowed = ParseSemanticResult(
            UMCPythonHelper::AddBlueprintConnectedActionNode(
                Fixture.Blueprint,
                SerializeSemanticRequest(MakeConnectedSpawnRequest(
                    Fixture,
                    Fixture.IntegerOutput,
                    ConversionActionId,
                    ConversionBindingId,
                    true))));
        TestTrue(
            TEXT("explicitly allowed conversion spawn succeeds"),
            ConversionAllowed.IsValid() &&
                ConversionAllowed->GetBoolField(TEXT("success")));
        if (ConversionAllowed && ConversionAllowed->GetBoolField(TEXT("success")))
        {
            const TSharedPtr<FJsonObject> Data =
                ConversionAllowed->GetObjectField(TEXT("data"));
            const TArray<TSharedPtr<FJsonValue>>& Connections =
                Data->GetArrayField(TEXT("connections"));
            const TSharedPtr<FJsonObject> Response =
                Connections[0]
                    ->AsObject()->GetObjectField(TEXT("response"));
            TestTrue(
                TEXT("conversion response remains explicitly classified"),
                Response->GetBoolField(TEXT("requires_conversion")));
            TestFalse(
                TEXT("conversion response never reports an implicit save"),
                Data->GetBoolField(TEXT("saved")));
            const TArray<TSharedPtr<FJsonValue>>& AuxiliaryIds =
                Data->GetArrayField(TEXT("auxiliary_node_ids"));
            TestFalse(
                TEXT("real conversion returns its auxiliary node"),
                AuxiliaryIds.IsEmpty());
            for (const TSharedPtr<FJsonValue>& Id : AuxiliaryIds)
            {
                TestTrue(
                    TEXT("every conversion auxiliary node ID is stable"),
                    Id->AsString().StartsWith(TEXT("node:")));
            }
            for (const TSharedPtr<FJsonValue>& Id :
                Data->GetArrayField(TEXT("pin_ids")))
            {
                TestTrue(
                    TEXT("every visible spawned pin ID is stable"),
                    Id->AsString().StartsWith(TEXT("pin:")));
            }

            const FString RequestedSourceId =
                Connections[0]->AsObject()->GetStringField(
                    TEXT("source_pin_id"));
            const FString RequestedTargetId =
                Connections[0]->AsObject()->GetStringField(
                    TEXT("target_pin_id"));
            bool bSourceTouchesAuxiliary = false;
            bool bAuxiliaryTouchesSpawned = false;
            for (int32 EdgeIndex = 1; EdgeIndex < Connections.Num(); ++EdgeIndex)
            {
                const TSharedPtr<FJsonObject> Edge =
                    Connections[EdgeIndex]->AsObject();
                const FString SourceId =
                    Edge->GetStringField(TEXT("source_pin_id"));
                const FString TargetId =
                    Edge->GetStringField(TEXT("target_pin_id"));
                TestTrue(
                    TEXT("every returned topology source endpoint is stable"),
                    SourceId.StartsWith(TEXT("pin:")));
                TestTrue(
                    TEXT("every returned topology target endpoint is stable"),
                    TargetId.StartsWith(TEXT("pin:")));
                const bool bTouchesAuxiliary =
                    !Edge->GetArrayField(TEXT("auxiliary_node_ids")).IsEmpty();
                bSourceTouchesAuxiliary |=
                    SourceId == RequestedSourceId && bTouchesAuxiliary;
                bAuxiliaryTouchesSpawned |=
                    TargetId == RequestedTargetId && bTouchesAuxiliary;
            }
            TestTrue(
                TEXT("physical topology connects source to an auxiliary node"),
                bSourceTouchesAuxiliary);
            TestTrue(
                TEXT("physical topology connects auxiliary node to spawned pin"),
                bAuxiliaryTouchesSpawned);
        }
        TestTrue(
            TEXT("allowed conversion adds the requested node"),
            Fixture.Graph->Nodes.Num() > BeforeConversionNodeCount);
    }

    const TSharedPtr<FJsonObject> FreshTemplateSuggestions = ParseSemanticResult(
        UMCPythonHelper::SuggestBlueprintNodesForPin(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakePinSuggestionRequest(
                Fixture, Fixture.ExecOutput, TEXT("Sequence")))));
    FString FreshActionId;
    FString FreshTemplateBindingId;
    if (FreshTemplateSuggestions &&
        FreshTemplateSuggestions->GetBoolField(TEXT("success")))
    {
        const TArray<TSharedPtr<FJsonValue>>& Items =
            FreshTemplateSuggestions->GetObjectField(TEXT("data"))
                ->GetArrayField(TEXT("items"));
        if (!Items.IsEmpty())
        {
            FreshActionId = Items[0]->AsObject()->GetStringField(TEXT("action_id"));
            const TArray<TSharedPtr<FJsonValue>>& ConnectionBindings =
                Items[0]->AsObject()->GetArrayField(TEXT("connection_bindings"));
            if (!ConnectionBindings.IsEmpty())
            {
                FreshTemplateBindingId = ConnectionBindings[0]->AsObject()
                    ->GetStringField(TEXT("binding_id"));
            }
        }
    }
    TestTrue(
        TEXT("template binding corruption hook accepts current binding"),
        CorruptPaletteTemplatePinBindingForTests(FreshTemplateBindingId));
    const TSharedPtr<FJsonObject> StaleTemplate = ParseSemanticResult(
        UMCPythonHelper::AddBlueprintConnectedActionNode(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakeConnectedSpawnRequest(
                Fixture,
                Fixture.ExecOutput,
                FreshActionId,
                FreshTemplateBindingId))));
    TestEqual(
        TEXT("changed stored template shape is a stale precondition"),
        SemanticErrorCode(StaleTemplate),
        FString(TEXT("PRECONDITION_FAILED")));

    const TSharedPtr<FJsonObject> FreshDynamicSuggestions = ParseSemanticResult(
        UMCPythonHelper::SuggestBlueprintNodesForPin(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakePinSuggestionRequest(
                Fixture, Fixture.ExecOutput, TEXT("Sequence")))));
    FString DynamicActionId;
    FString DynamicTemplateBindingId;
    if (FreshDynamicSuggestions &&
        FreshDynamicSuggestions->GetBoolField(TEXT("success")))
    {
        const TArray<TSharedPtr<FJsonValue>>& Items =
            FreshDynamicSuggestions->GetObjectField(TEXT("data"))
                ->GetArrayField(TEXT("items"));
        if (!Items.IsEmpty())
        {
            DynamicActionId = Items[0]->AsObject()->GetStringField(TEXT("action_id"));
            const TArray<TSharedPtr<FJsonValue>>& ConnectionBindings =
                Items[0]->AsObject()->GetArrayField(TEXT("connection_bindings"));
            if (!ConnectionBindings.IsEmpty())
            {
                DynamicTemplateBindingId = ConnectionBindings[0]->AsObject()
                    ->GetStringField(TEXT("binding_id"));
            }
        }
    }
    const FString StaleDynamicBindingId =
        InjectStaleDynamicBindingForTests(DynamicActionId);
    TestFalse(
        TEXT("stale dynamic binding hook returns a typed binding token"),
        StaleDynamicBindingId.IsEmpty());
    const TSharedRef<FJsonObject> StaleDynamicRequest = MakeConnectedSpawnRequest(
        Fixture,
        Fixture.ExecOutput,
        DynamicActionId,
        DynamicTemplateBindingId);
    StaleDynamicRequest->SetArrayField(
        TEXT("bindings"),
        {MakeShared<FJsonValueString>(StaleDynamicBindingId)});
    const TSharedPtr<FJsonObject> StaleDynamic = ParseSemanticResult(
        UMCPythonHelper::AddBlueprintConnectedActionNode(
            Fixture.Blueprint, SerializeSemanticRequest(StaleDynamicRequest)));
    TestEqual(
        TEXT("missing dynamic binding object is a stale precondition"),
        SemanticErrorCode(StaleDynamic),
        FString(TEXT("PRECONDITION_FAILED")));

    ResetPaletteTokenStateForTests();
    const TSharedPtr<FJsonObject> EvictedAction = ParseSemanticResult(
        UMCPythonHelper::AddBlueprintConnectedActionNode(
            Fixture.Blueprint,
            SerializeSemanticRequest(MakeConnectedSpawnRequest(
                Fixture, Fixture.ExecOutput, ActionId, BindingId))));
    TestEqual(
        TEXT("evicted action capability is invalid input"),
        SemanticErrorCode(EvictedAction),
        FString(TEXT("INVALID_INPUT")));
    return true;
}

#endif
