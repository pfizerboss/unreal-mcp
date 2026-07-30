// Copyright (c) 2025 GenOrca. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "MCPythonBlueprint2Internal.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

namespace
{
using namespace UE::MCPython::Blueprint2;

TSharedRef<FJsonObject> KindSpec(const TCHAR* Kind)
{
    const TSharedRef<FJsonObject> Spec = MakeShared<FJsonObject>();
    Spec->SetStringField(TEXT("kind"), Kind);
    return Spec;
}

TSharedRef<FJsonObject> PathSpec(
    const TCHAR* Kind,
    const TCHAR* Field,
    const TCHAR* Path)
{
    const TSharedRef<FJsonObject> Spec = KindSpec(Kind);
    Spec->SetStringField(Field, Path);
    return Spec;
}

TSharedRef<FJsonObject> RealSpec(const TCHAR* Precision)
{
    const TSharedRef<FJsonObject> Spec = KindSpec(TEXT("real"));
    Spec->SetStringField(TEXT("precision"), Precision);
    return Spec;
}

TSharedRef<FJsonObject> ContainerSpec(
    const TCHAR* Kind,
    const TSharedRef<FJsonObject>& Item)
{
    const TSharedRef<FJsonObject> Spec = KindSpec(Kind);
    Spec->SetObjectField(TEXT("item"), Item);
    return Spec;
}

TSharedRef<FJsonObject> MapSpec(
    const TSharedRef<FJsonObject>& Key,
    const TSharedRef<FJsonObject>& Value)
{
    const TSharedRef<FJsonObject> Spec = KindSpec(TEXT("map"));
    Spec->SetObjectField(TEXT("key"), Key);
    Spec->SetObjectField(TEXT("value"), Value);
    return Spec;
}

struct FValidTypeCase
{
    const TCHAR* Label;
    TSharedRef<FJsonObject> Spec;
    FName Category;
    FName SubCategory;
    EPinContainerType Container;
    const TCHAR* ObjectPath;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprint2CanonicalTypesTest,
    "UnrealMCP.Blueprint2.CanonicalTypes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPythonBlueprint2CanonicalTypesTest::RunTest(const FString& Parameters)
{
    using namespace UE::MCPython::Blueprint2;

    (void)Parameters;

    const TArray<FValidTypeCase> Cases = {
        {TEXT("bool"), KindSpec(TEXT("bool")), UEdGraphSchema_K2::PC_Boolean,
            NAME_None, EPinContainerType::None, TEXT("")},
        {TEXT("byte"), KindSpec(TEXT("byte")), UEdGraphSchema_K2::PC_Byte,
            NAME_None, EPinContainerType::None, TEXT("")},
        {TEXT("int"), KindSpec(TEXT("int")), UEdGraphSchema_K2::PC_Int,
            NAME_None, EPinContainerType::None, TEXT("")},
        {TEXT("int64"), KindSpec(TEXT("int64")), UEdGraphSchema_K2::PC_Int64,
            NAME_None, EPinContainerType::None, TEXT("")},
        {TEXT("float"), RealSpec(TEXT("float")), UEdGraphSchema_K2::PC_Real,
            UEdGraphSchema_K2::PC_Float, EPinContainerType::None, TEXT("")},
        {TEXT("double"), RealSpec(TEXT("double")), UEdGraphSchema_K2::PC_Real,
            UEdGraphSchema_K2::PC_Double, EPinContainerType::None, TEXT("")},
        {TEXT("string"), KindSpec(TEXT("string")), UEdGraphSchema_K2::PC_String,
            NAME_None, EPinContainerType::None, TEXT("")},
        {TEXT("name"), KindSpec(TEXT("name")), UEdGraphSchema_K2::PC_Name,
            NAME_None, EPinContainerType::None, TEXT("")},
        {TEXT("text"), KindSpec(TEXT("text")), UEdGraphSchema_K2::PC_Text,
            NAME_None, EPinContainerType::None, TEXT("")},
        {TEXT("enum"),
            PathSpec(TEXT("enum"), TEXT("type_path"),
                TEXT("/Script/Engine.ECollisionChannel")),
            UEdGraphSchema_K2::PC_Byte, NAME_None, EPinContainerType::None,
            TEXT("/Script/Engine.ECollisionChannel")},
        {TEXT("struct"),
            PathSpec(TEXT("struct"), TEXT("type_path"),
                TEXT("/Script/CoreUObject.Vector")),
            UEdGraphSchema_K2::PC_Struct, NAME_None, EPinContainerType::None,
            TEXT("/Script/CoreUObject.Vector")},
        {TEXT("object"),
            PathSpec(TEXT("object"), TEXT("class_path"),
                TEXT("/Script/Engine.Actor")),
            UEdGraphSchema_K2::PC_Object, NAME_None, EPinContainerType::None,
            TEXT("/Script/Engine.Actor")},
        {TEXT("class"),
            PathSpec(TEXT("class"), TEXT("class_path"),
                TEXT("/Script/Engine.Actor")),
            UEdGraphSchema_K2::PC_Class, NAME_None, EPinContainerType::None,
            TEXT("/Script/Engine.Actor")},
        {TEXT("interface"),
            PathSpec(TEXT("interface"), TEXT("class_path"),
                TEXT("/Script/CoreUObject.Interface")),
            UEdGraphSchema_K2::PC_Interface, NAME_None, EPinContainerType::None,
            TEXT("/Script/CoreUObject.Interface")},
        {TEXT("soft object"),
            PathSpec(TEXT("soft_object"), TEXT("class_path"),
                TEXT("/Script/Engine.Texture2D")),
            UEdGraphSchema_K2::PC_SoftObject, NAME_None, EPinContainerType::None,
            TEXT("/Script/Engine.Texture2D")},
        {TEXT("soft class"),
            PathSpec(TEXT("soft_class"), TEXT("class_path"),
                TEXT("/Script/Engine.Actor")),
            UEdGraphSchema_K2::PC_SoftClass, NAME_None, EPinContainerType::None,
            TEXT("/Script/Engine.Actor")},
        {TEXT("array"), ContainerSpec(TEXT("array"), KindSpec(TEXT("int"))),
            UEdGraphSchema_K2::PC_Int, NAME_None, EPinContainerType::Array,
            TEXT("")},
        {TEXT("set"), ContainerSpec(TEXT("set"), KindSpec(TEXT("name"))),
            UEdGraphSchema_K2::PC_Name, NAME_None, EPinContainerType::Set,
            TEXT("")},
        {TEXT("map"),
            MapSpec(KindSpec(TEXT("string")),
                PathSpec(TEXT("object"), TEXT("class_path"),
                    TEXT("/Script/Engine.Actor"))),
            UEdGraphSchema_K2::PC_String, NAME_None, EPinContainerType::Map,
            TEXT("")},
    };

    for (const FValidTypeCase& Case : Cases)
    {
        FEdGraphPinType Type;
        FError Error;
        TestTrue(
            *FString::Printf(TEXT("%s parses"), Case.Label),
            ParseTypeSpec(Case.Spec, Type, Error));
        TestEqual(
            *FString::Printf(TEXT("%s category"), Case.Label),
            Type.PinCategory,
            Case.Category);
        TestEqual(
            *FString::Printf(TEXT("%s subcategory"), Case.Label),
            Type.PinSubCategory,
            Case.SubCategory);
        TestEqual(
            *FString::Printf(TEXT("%s container"), Case.Label),
            Type.ContainerType,
            Case.Container);
        if (FCString::Strlen(Case.ObjectPath) > 0)
        {
            TestEqual(
                *FString::Printf(TEXT("%s object path"), Case.Label),
                GetPathNameSafe(Type.PinSubCategoryObject.Get()),
                FString(Case.ObjectPath));
        }
        const TSharedRef<FJsonObject> Serialized = SerializeTypeSpec(Type);
        FEdGraphPinType RoundTripped;
        FError RoundTripError;
        TestTrue(
            *FString::Printf(TEXT("%s serialized type parses"), Case.Label),
            ParseTypeSpec(Serialized, RoundTripped, RoundTripError));
        TestEqual(
            *FString::Printf(TEXT("%s round-trip category"), Case.Label),
            RoundTripped.PinCategory,
            Type.PinCategory);
        TestEqual(
            *FString::Printf(TEXT("%s round-trip container"), Case.Label),
            RoundTripped.ContainerType,
            Type.ContainerType);
        TestEqual(
            *FString::Printf(TEXT("%s round-trip subcategory"), Case.Label),
            RoundTripped.PinSubCategory,
            Type.PinSubCategory);
        TestEqual(
            *FString::Printf(TEXT("%s round-trip object"), Case.Label),
            GetPathNameSafe(RoundTripped.PinSubCategoryObject.Get()),
            GetPathNameSafe(Type.PinSubCategoryObject.Get()));
        TestEqual(
            *FString::Printf(TEXT("%s round-trip value category"), Case.Label),
            RoundTripped.PinValueType.TerminalCategory,
            Type.PinValueType.TerminalCategory);
        TestEqual(
            *FString::Printf(TEXT("%s round-trip value object"), Case.Label),
            GetPathNameSafe(
                RoundTripped.PinValueType.TerminalSubCategoryObject.Get()),
            GetPathNameSafe(Type.PinValueType.TerminalSubCategoryObject.Get()));
    }

    FEdGraphPinType ExecType;
    ExecType.PinCategory = UEdGraphSchema_K2::PC_Exec;
    const TSharedRef<FJsonObject> SerializedExec = SerializeTypeSpec(ExecType);
    TestEqual(
        TEXT("execution pins serialize with their public snapshot kind"),
        SerializedExec->GetStringField(TEXT("kind")),
        FString(TEXT("exec")));

    auto ExpectInvalid = [this](
        const TCHAR* Label,
        const TSharedRef<FJsonObject>& Spec,
        const TCHAR* ExpectedPath)
    {
        FEdGraphPinType Type;
        FError Error;
        TestFalse(Label, ParseTypeSpec(Spec, Type, Error));
        TestEqual(
            *FString::Printf(TEXT("%s code"), Label),
            Error.Code,
            FString(TEXT("INVALID_INPUT")));
        TestEqual(
            *FString::Printf(TEXT("%s path"), Label),
            Error.Path,
            FString(ExpectedPath));
    };

    ExpectInvalid(
        TEXT("unknown kind"),
        KindSpec(TEXT("vector3")),
        TEXT("params.type.kind"));
    const TSharedRef<FJsonObject> OpenPrimitive = KindSpec(TEXT("int"));
    OpenPrimitive->SetBoolField(TEXT("extra"), true);
    ExpectInvalid(
        TEXT("type objects are closed"),
        OpenPrimitive,
        TEXT("params.type.extra"));
    ExpectInvalid(
        TEXT("enum requires UEnum"),
        PathSpec(TEXT("enum"), TEXT("type_path"), TEXT("/Script/Engine.Actor")),
        TEXT("params.type.type_path"));
    ExpectInvalid(
        TEXT("struct requires UScriptStruct"),
        PathSpec(TEXT("struct"), TEXT("type_path"),
            TEXT("/Script/Engine.Actor")),
        TEXT("params.type.type_path"));
    ExpectInvalid(
        TEXT("object requires UClass"),
        PathSpec(TEXT("object"), TEXT("class_path"),
            TEXT("/Script/Engine.ECollisionChannel")),
        TEXT("params.type.class_path"));
    ExpectInvalid(
        TEXT("interface requires interface class"),
        PathSpec(TEXT("interface"), TEXT("class_path"),
            TEXT("/Script/Engine.Actor")),
        TEXT("params.type.class_path"));
    ExpectInvalid(
        TEXT("map key cannot be a container"),
        MapSpec(
            ContainerSpec(TEXT("array"), KindSpec(TEXT("int"))),
            KindSpec(TEXT("string"))),
        TEXT("params.type.key.kind"));
    ExpectInvalid(
        TEXT("set item cannot be a container"),
        ContainerSpec(
            TEXT("set"),
            MapSpec(KindSpec(TEXT("int")), KindSpec(TEXT("string")))),
        TEXT("params.type.item.kind"));
    ExpectInvalid(
        TEXT("array item cannot be an implicit container"),
        ContainerSpec(
            TEXT("array"),
            ContainerSpec(TEXT("array"), KindSpec(TEXT("int")))),
        TEXT("params.type.item.kind"));
    ExpectInvalid(
        TEXT("map value cannot be an implicit container"),
        MapSpec(
            KindSpec(TEXT("string")),
            ContainerSpec(TEXT("array"), KindSpec(TEXT("int")))),
        TEXT("params.type.value.kind"));

    TSharedRef<FJsonObject> TooDeep = KindSpec(TEXT("int"));
    for (int32 Depth = 0; Depth < 9; ++Depth)
    {
        TooDeep = ContainerSpec(TEXT("array"), TooDeep);
    }
    ExpectInvalid(
        TEXT("container nesting beyond eight"),
        TooDeep,
        TEXT("params.type.item.item.item.item.item.item.item.item.kind"));

    FEdGraphPinType IntType;
    FError IntTypeError;
    TestTrue(
        TEXT("int type for default test parses"),
        ParseTypeSpec(KindSpec(TEXT("int")), IntType, IntTypeError));
    FNormalizedDefault Normalized;
    FError DefaultError;
    TestFalse(
        TEXT("lossy numeric default is rejected"),
        NormalizeDefaultValue(
            IntType,
            MakeShared<FJsonValueNumber>(1.25),
            nullptr,
            Normalized,
            DefaultError,
            TEXT("params.default")));
    TestEqual(
        TEXT("lossy numeric default code"),
        DefaultError.Code,
        FString(TEXT("INVALID_INPUT")));
    TestEqual(
        TEXT("lossy numeric default path"),
        DefaultError.Path,
        FString(TEXT("params.default")));

    UPackage* Package = CreatePackage(TEXT("/Game/__MCPTests/CanonicalTypeDefaults"));
    Package->SetDirtyFlag(false);
    UBlueprint* Owner = NewObject<UBlueprint>(Package, TEXT("BP_DefaultOwner"));
    FNormalizedDefault ValidDefault;
    FError ValidDefaultError;
    TestTrue(
        TEXT("integral numeric default normalizes"),
        NormalizeDefaultValue(
            IntType,
            MakeShared<FJsonValueNumber>(42.0),
            Owner,
            ValidDefault,
            ValidDefaultError,
            TEXT("params.default")));
    TestEqual(
        TEXT("integral numeric default import text"),
        ValidDefault.DefaultValue,
        FString(TEXT("42")));

    auto ParseDefaultType = [this](
        const TCHAR* Label,
        const TSharedRef<FJsonObject>& Spec)
    {
        FEdGraphPinType Type;
        FError Error;
        TestTrue(Label, ParseTypeSpec(Spec, Type, Error));
        return Type;
    };
    auto Normalize = [this, Owner](
        const TCHAR* Label,
        const FEdGraphPinType& Type,
        const TSharedPtr<FJsonValue>& Value,
        const FString& Path = TEXT("params.default"))
    {
        FNormalizedDefault Result;
        FError Error;
        TestTrue(
            Label,
            NormalizeDefaultValue(
                Type, Value, Owner, Result, Error, Path));
        return Result;
    };

    const FEdGraphPinType BoolType = ParseDefaultType(
        TEXT("bool default type parses"), KindSpec(TEXT("bool")));
    const FNormalizedDefault BoolDefault = Normalize(
        TEXT("bool default normalizes"),
        BoolType,
        MakeShared<FJsonValueBoolean>(false));
    TestEqual(
        TEXT("bool import text"),
        BoolDefault.DefaultValue,
        FString(TEXT("false")));

    const FEdGraphPinType RealType = ParseDefaultType(
        TEXT("real default type parses"), RealSpec(TEXT("double")));
    const FNormalizedDefault RealDefault = Normalize(
        TEXT("real default normalizes"),
        RealType,
        MakeShared<FJsonValueNumber>(1.5));
    TestEqual(
        TEXT("real import text"),
        RealDefault.DefaultValue,
        FString(TEXT("1.5")));

    const FEdGraphPinType StringType = ParseDefaultType(
        TEXT("string default type parses"), KindSpec(TEXT("string")));
    const FNormalizedDefault StringDefault = Normalize(
        TEXT("string default normalizes"),
        StringType,
        MakeShared<FJsonValueString>(TEXT("hello, Blueprint")));
    TestEqual(
        TEXT("string import text"),
        StringDefault.DefaultValue,
        FString(TEXT("hello, Blueprint")));

    const FEdGraphPinType NameType = ParseDefaultType(
        TEXT("name default type parses"), KindSpec(TEXT("name")));
    const FNormalizedDefault NameDefault = Normalize(
        TEXT("name default normalizes"),
        NameType,
        MakeShared<FJsonValueString>(TEXT("Player Start")));
    TestEqual(
        TEXT("name import text"),
        NameDefault.DefaultValue,
        FString(TEXT("Player Start")));

    const FEdGraphPinType TextType = ParseDefaultType(
        TEXT("text default type parses"), KindSpec(TEXT("text")));
    const FNormalizedDefault TextDefault = Normalize(
        TEXT("text default normalizes"),
        TextType,
        MakeShared<FJsonValueString>(TEXT("Hello text")));
    TestTrue(TEXT("text uses DefaultTextValue"), TextDefault.DefaultValue.IsEmpty());
    TestEqual(
        TEXT("text display value"),
        TextDefault.DefaultTextValue.ToString(),
        FString(TEXT("Hello text")));

    const FEdGraphPinType ObjectType = ParseDefaultType(
        TEXT("object default type parses"),
        PathSpec(
            TEXT("object"),
            TEXT("class_path"),
            TEXT("/Script/Engine.Actor")));
    const FNormalizedDefault ObjectDefault = Normalize(
        TEXT("hard object path default normalizes"),
        ObjectType,
        MakeShared<FJsonValueString>(TEXT("/Script/Engine.Default__Actor")));
    TestEqual(
        TEXT("hard object path resolves"),
        GetPathNameSafe(ObjectDefault.DefaultObject.Get()),
        FString(TEXT("/Script/Engine.Default__Actor")));

    const FEdGraphPinType SoftObjectType = ParseDefaultType(
        TEXT("soft object default type parses"),
        PathSpec(
            TEXT("soft_object"),
            TEXT("class_path"),
            TEXT("/Script/Engine.Texture2D")));
    const FNormalizedDefault SoftObjectDefault = Normalize(
        TEXT("soft object path default normalizes"),
        SoftObjectType,
        MakeShared<FJsonValueString>(
            TEXT("/Script/Engine.Default__Texture2D")));
    TestEqual(
        TEXT("soft object keeps import path"),
        SoftObjectDefault.DefaultValue,
        FString(TEXT("/Script/Engine.Default__Texture2D")));

    const FEdGraphPinType VectorType = ParseDefaultType(
        TEXT("Vector default type parses"),
        PathSpec(
            TEXT("struct"),
            TEXT("type_path"),
            TEXT("/Script/CoreUObject.Vector")));
    const TSharedRef<FJsonObject> VectorJson = MakeShared<FJsonObject>();
    VectorJson->SetNumberField(TEXT("Z"), 3.0);
    VectorJson->SetNumberField(TEXT("X"), 1.0);
    VectorJson->SetNumberField(TEXT("Y"), 2.0);
    const FNormalizedDefault VectorDefault = Normalize(
        TEXT("Vector object default normalizes"),
        VectorType,
        MakeShared<FJsonValueObject>(VectorJson));
    TestEqual(
        TEXT("struct import follows reflected property order"),
        VectorDefault.DefaultValue,
        FString(TEXT("(X=1,Y=2,Z=3)")));

    const FEdGraphPinType ArrayType = ParseDefaultType(
        TEXT("array default type parses"),
        ContainerSpec(TEXT("array"), KindSpec(TEXT("int"))));
    const TArray<TSharedPtr<FJsonValue>> ArrayJson = {
        MakeShared<FJsonValueNumber>(3.0),
        MakeShared<FJsonValueNumber>(1.0),
        MakeShared<FJsonValueNumber>(2.0)};
    const FNormalizedDefault ArrayDefault = Normalize(
        TEXT("array default normalizes"),
        ArrayType,
        MakeShared<FJsonValueArray>(ArrayJson));
    TestEqual(
        TEXT("container import preserves JSON order"),
        ArrayDefault.DefaultValue,
        FString(TEXT("(3,1,2)")));

    const FEdGraphPinType TextArrayType = ParseDefaultType(
        TEXT("text array default type parses"),
        ContainerSpec(TEXT("array"), KindSpec(TEXT("text"))));
    const TArray<TSharedPtr<FJsonValue>> TextArrayJson = {
        MakeShared<FJsonValueString>(TEXT("first")),
        MakeShared<FJsonValueString>(TEXT("second"))};
    const FNormalizedDefault TextArrayDefault = Normalize(
        TEXT("text array default normalizes as container import text"),
        TextArrayType,
        MakeShared<FJsonValueArray>(TextArrayJson));
    TestEqual(
        TEXT("text array remains in DefaultValue"),
        TextArrayDefault.DefaultValue,
        FString(TEXT("(\"first\",\"second\")")));

    const FEdGraphPinType ObjectArrayType = ParseDefaultType(
        TEXT("object array default type parses"),
        ContainerSpec(
            TEXT("array"),
            PathSpec(
                TEXT("object"),
                TEXT("class_path"),
                TEXT("/Script/Engine.Actor"))));
    const TArray<TSharedPtr<FJsonValue>> ObjectArrayJson = {
        MakeShared<FJsonValueString>(
            TEXT("/Script/Engine.Default__Actor"))};
    const FNormalizedDefault ObjectArrayDefault = Normalize(
        TEXT("object array default normalizes as container import text"),
        ObjectArrayType,
        MakeShared<FJsonValueArray>(ObjectArrayJson));
    TestEqual(
        TEXT("object array remains in DefaultValue"),
        ObjectArrayDefault.DefaultValue,
        FString(TEXT("(/Script/Engine.Default__Actor)")));

    const FNormalizedDefault NullObjectDefault = Normalize(
        TEXT("null object default normalizes"),
        ObjectType,
        MakeShared<FJsonValueNull>());
    TestTrue(
        TEXT("null object has no import string"),
        NullObjectDefault.DefaultValue.IsEmpty());
    TestNull(
        TEXT("null object has no resolved object"),
        NullObjectDefault.DefaultObject.Get());

    auto ExpectInvalidDefault = [this, Owner](
        const TCHAR* Label,
        const FEdGraphPinType& Type,
        const TSharedPtr<FJsonValue>& Value,
        const TCHAR* ExpectedPath)
    {
        FNormalizedDefault Result;
        FError Error;
        TestFalse(
            Label,
            NormalizeDefaultValue(
                Type,
                Value,
                Owner,
                Result,
                Error,
                TEXT("params.default")));
        TestEqual(
            *FString::Printf(TEXT("%s code"), Label),
            Error.Code,
            FString(TEXT("INVALID_INPUT")));
        TestEqual(
            *FString::Printf(TEXT("%s path"), Label),
            Error.Path,
            FString(ExpectedPath));
    };
    ExpectInvalidDefault(
        TEXT("null scalar default is rejected"),
        IntType,
        MakeShared<FJsonValueNull>(),
        TEXT("params.default"));
    ExpectInvalidDefault(
        TEXT("wrong JSON scalar kind is rejected"),
        BoolType,
        MakeShared<FJsonValueString>(TEXT("false")),
        TEXT("params.default"));
    const TSharedRef<FJsonObject> InvalidVectorJson = MakeShared<FJsonObject>();
    InvalidVectorJson->SetNumberField(TEXT("Q"), 1.0);
    ExpectInvalidDefault(
        TEXT("unknown struct field is rejected"),
        VectorType,
        MakeShared<FJsonValueObject>(InvalidVectorJson),
        TEXT("params.default.Q"));
    const TArray<TSharedPtr<FJsonValue>> InvalidArrayJson = {
        MakeShared<FJsonValueNumber>(1.0),
        MakeShared<FJsonValueString>(TEXT("two"))};
    ExpectInvalidDefault(
        TEXT("invalid container item is rejected"),
        ArrayType,
        MakeShared<FJsonValueArray>(InvalidArrayJson),
        TEXT("params.default[1]"));
    const FEdGraphPinType FloatType = ParseDefaultType(
        TEXT("float default type parses"), RealSpec(TEXT("float")));
    const FNormalizedDefault RoundedFloatDefault = Normalize(
        TEXT("ordinary float rounding is accepted"),
        FloatType,
        MakeShared<FJsonValueNumber>(1.1));
    TestEqual(
        TEXT("ordinary float keeps canonical import text"),
        RoundedFloatDefault.DefaultValue,
        FString(TEXT("1.1")));
    ExpectInvalidDefault(
        TEXT("float underflow is rejected as lossy"),
        FloatType,
        MakeShared<FJsonValueNumber>(1.0e-50),
        TEXT("params.default"));
    ExpectInvalidDefault(
        TEXT("float overflow is rejected as lossy"),
        FloatType,
        MakeShared<FJsonValueNumber>(1.0e39),
        TEXT("params.default"));
    const TArray<TSharedPtr<FJsonValue>> WrongObjectArrayJson = {
        MakeShared<FJsonValueString>(
            TEXT("/Script/Engine.Default__Texture2D"))};
    ExpectInvalidDefault(
        TEXT("wrong nested object class is rejected"),
        ObjectArrayType,
        MakeShared<FJsonValueArray>(WrongObjectArrayJson),
        TEXT("params.default[0]"));
    const FEdGraphPinType ClassArrayType = ParseDefaultType(
        TEXT("class array type parses"),
        ContainerSpec(
            TEXT("array"),
            PathSpec(
                TEXT("class"),
                TEXT("class_path"),
                TEXT("/Script/Engine.Actor"))));
    const TArray<TSharedPtr<FJsonValue>> ClassArrayJson = {
        MakeShared<FJsonValueString>(TEXT("/Script/Engine.Character"))};
    const FNormalizedDefault ClassArrayDefault = Normalize(
        TEXT("nested class uses a compatible resolved UClass"),
        ClassArrayType,
        MakeShared<FJsonValueArray>(ClassArrayJson));
    TestEqual(
        TEXT("class array remains canonical container import text"),
        ClassArrayDefault.DefaultValue,
        FString(TEXT("(/Script/Engine.Character)")));
    const TArray<TSharedPtr<FJsonValue>> WrongClassArrayJson = {
        MakeShared<FJsonValueString>(TEXT("/Script/Engine.Texture2D"))};
    ExpectInvalidDefault(
        TEXT("wrong nested class is rejected by the schema"),
        ClassArrayType,
        MakeShared<FJsonValueArray>(WrongClassArrayJson),
        TEXT("params.default[0]"));
    const FEdGraphPinType InterfaceArrayType = ParseDefaultType(
        TEXT("interface array type parses"),
        ContainerSpec(
            TEXT("array"),
            PathSpec(
                TEXT("interface"),
                TEXT("class_path"),
                TEXT("/Script/Engine.NavAgentInterface"))));
    const TArray<TSharedPtr<FJsonValue>> WrongInterfaceArrayJson = {
        MakeShared<FJsonValueString>(
            TEXT("/Script/Engine.Default__Texture2D"))};
    ExpectInvalidDefault(
        TEXT("wrong nested interface object is rejected by the schema"),
        InterfaceArrayType,
        MakeShared<FJsonValueArray>(WrongInterfaceArrayJson),
        TEXT("params.default[0]"));
    const FEdGraphPinType SoftObjectArrayType = ParseDefaultType(
        TEXT("soft object array type parses"),
        ContainerSpec(
            TEXT("array"),
            PathSpec(
                TEXT("soft_object"),
                TEXT("class_path"),
                TEXT("/Script/Engine.Texture2D"))));
    const TArray<TSharedPtr<FJsonValue>> InvalidSoftObjectArrayJson = {
        MakeShared<FJsonValueString>(TEXT("not-an-object-path"))};
    ExpectInvalidDefault(
        TEXT("invalid nested soft object path is rejected"),
        SoftObjectArrayType,
        MakeShared<FJsonValueArray>(InvalidSoftObjectArrayJson),
        TEXT("params.default[0]"));
    const FEdGraphPinType AnimNotifyEventType = ParseDefaultType(
        TEXT("AnimNotifyEvent default type parses"),
        PathSpec(
            TEXT("struct"),
            TEXT("type_path"),
            TEXT("/Script/Engine.AnimNotifyEvent")));
    const TSharedRef<FJsonObject> WrongStructReference =
        MakeShared<FJsonObject>();
    WrongStructReference->SetStringField(
        TEXT("Notify"),
        TEXT("/Script/Engine.Default__Texture2D"));
    ExpectInvalidDefault(
        TEXT("wrong hard reference in a struct is rejected by the schema"),
        AnimNotifyEventType,
        MakeShared<FJsonValueObject>(WrongStructReference),
        TEXT("params.default.Notify"));

    TestFalse(
        TEXT("default validation does not dirty the Blueprint package"),
        Package->IsDirty());
    Package->SetDirtyFlag(false);
    Owner->MarkAsGarbage();
    Package->MarkAsGarbage();

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
