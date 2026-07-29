// Copyright (c) 2025 GenOrca (by zenoengine). All Rights Reserved.

#include "MCPythonHelper.h"
#include "MCPythonBlueprint2Internal.h"

#include "EdGraphToken.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/SCS_Node.h"
#include "Engine/MemberReference.h"
#include "Engine/SimpleConstructionScript.h"
#include "Internationalization/Text.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_InputKey.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/EngineVersion.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"

namespace
{
FString CompileStatus(const UBlueprint* Blueprint)
{
    if (!Blueprint)
    {
        return TEXT("Unknown");
    }
    switch (Blueprint->Status)
    {
    case BS_UpToDate: return TEXT("UpToDate");
    case BS_UpToDateWithWarnings: return TEXT("UpToDateWithWarnings");
    case BS_Error: return TEXT("Error");
    case BS_Dirty: return TEXT("Dirty");
    case BS_BeingCreated: return TEXT("BeingCreated");
    default: return TEXT("Unknown");
    }
}

FString CompilerTokensUnsupportedResult()
{
    using namespace UE::MCPython::Blueprint2;

    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetStringField(
        TEXT("capability"), TEXT("supports_compiler_tokens"));
    Details->SetStringField(
        TEXT("engine_version"), FEngineVersion::Current().ToString());
    Details->SetArrayField(
        TEXT("supported_engine_versions"),
        {MakeShared<FJsonValueString>(TEXT("5.7"))});
    return SerializeResult(MakeFailure(
        TEXT("UE_VERSION_UNSUPPORTED"),
        TEXT("engine_version"),
        TEXT("Structured Blueprint compiler diagnostics are unavailable on this Unreal Engine version."),
        false,
        TEXT("Use Unreal Engine 5.7 or a plugin build with compiler-token support."),
        Details));
}

void NormalizeDiagnostic(
    const FString& Message,
    const bool bWarning,
    FString& OutCode,
    FString& OutHint)
{
    const FString Lower = Message.ToLower();
    if (Lower.Contains(TEXT("required pin")) ||
        Lower.Contains(TEXT("no value")) ||
        Lower.Contains(TEXT("must be linked")) ||
        Lower.Contains(TEXT("must have a connection")))
    {
        OutCode = TEXT("BP_MISSING_REQUIRED_PIN");
        OutHint = TEXT("Connect the cited pin or assign a valid default value, then compile again.");
    }
    else if (Lower.Contains(TEXT("could not find")) ||
             Lower.Contains(TEXT("unresolved")))
    {
        OutCode = TEXT("BP_UNRESOLVED_MEMBER");
        OutHint = TEXT("Re-inspect the referenced member, replace the stale node, then compile again.");
    }
    else if (Lower.Contains(TEXT("not compatible")) ||
             Lower.Contains(TEXT("type mismatch")) ||
             Lower.Contains(TEXT("incompatible")) ||
             Lower.Contains(TEXT("can't connect pins")))
    {
        OutCode = TEXT("BP_TYPE_MISMATCH");
        OutHint = TEXT("Insert an explicit conversion or change one of the connected pin types.");
    }
    else if (Lower.Contains(TEXT("already exists")) ||
             Lower.Contains(TEXT("duplicate")))
    {
        OutCode = TEXT("BP_DUPLICATE_MEMBER");
        OutHint = TEXT("Rename or remove the duplicate Blueprint member, then compile again.");
    }
    else if (Lower.Contains(TEXT("accessed none")))
    {
        OutCode = TEXT("BP_POSSIBLE_NULL_ACCESS");
        OutHint = TEXT("Validate the referenced object before accessing it.");
    }
    else
    {
        OutCode = bWarning ? TEXT("BP_COMPILE_WARNING") : TEXT("BP_COMPILE_ERROR");
        OutHint = TEXT("Inspect the cited Blueprint node and pins, correct the issue, then compile again.");
    }
}

void ResolveDiagnosticTarget(
    UBlueprint* Blueprint,
    const FTokenizedMessage& Message,
    FString& OutGraphId,
    FString& OutNodeId,
    FString& OutPinId)
{
    const UEdGraphPin* Pin = nullptr;
    const UObject* GraphObject = nullptr;
    for (const TSharedRef<IMessageToken>& Token : Message.GetMessageTokens())
    {
        if (Token->GetType() != EMessageToken::EdGraph)
        {
            continue;
        }
        const FEdGraphToken* EdGraphToken =
            static_cast<const FEdGraphToken*>(&Token.Get());
        if (!Pin)
        {
            Pin = EdGraphToken->GetPin();
        }
        if (!GraphObject)
        {
            GraphObject = EdGraphToken->GetGraphObject();
        }
    }

    const UEdGraphNode* Node = Pin ? Pin->GetOwningNode() : nullptr;
    const UEdGraph* Graph = Node ? Node->GetGraph() : nullptr;
    if (!Node)
    {
        Node = Cast<UEdGraphNode>(GraphObject);
        Graph = Node ? Node->GetGraph() : Cast<UEdGraph>(GraphObject);
    }
    if (Pin)
    {
        OutPinId = UE::MCPython::Blueprint2::DescribePinTarget(
            Blueprint, Pin).Id;
    }
    if (Node)
    {
        OutNodeId = UE::MCPython::Blueprint2::DescribeNodeTarget(
            Blueprint, Node).Id;
    }
    if (Graph)
    {
        OutGraphId = UE::MCPython::Blueprint2::DescribeGraphTarget(
            Blueprint, Graph).Id;
    }
}

TSharedRef<FJsonObject> MakeDiagnostic(
    UBlueprint* Blueprint,
    const TSharedRef<FTokenizedMessage>& Message,
    const bool bWarning)
{
    const FString Text = Message->ToText().ToString();
    FString Code;
    FString Hint;
    NormalizeDiagnostic(Text, bWarning, Code, Hint);

    FString GraphId;
    FString NodeId;
    FString PinId;
    ResolveDiagnosticTarget(
        Blueprint, Message.Get(), GraphId, NodeId, PinId);

    const TSharedRef<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
    Diagnostic->SetStringField(TEXT("code"), Code);
    Diagnostic->SetStringField(
        TEXT("severity"), bWarning ? TEXT("warning") : TEXT("error"));
    Diagnostic->SetStringField(TEXT("message"), Text);
    Diagnostic->SetStringField(TEXT("hint"), Hint);
    Diagnostic->SetStringField(TEXT("graph_id"), GraphId);
    Diagnostic->SetStringField(TEXT("node_id"), NodeId);
    Diagnostic->SetStringField(TEXT("pin_id"), PinId);
    return Diagnostic;
}

TSharedRef<FJsonObject> MakeWarningRecord(
    const TSharedRef<FJsonObject>& Diagnostic)
{
    const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
    Details->SetStringField(
        TEXT("graph_id"), Diagnostic->GetStringField(TEXT("graph_id")));
    Details->SetStringField(
        TEXT("node_id"), Diagnostic->GetStringField(TEXT("node_id")));
    Details->SetStringField(
        TEXT("pin_id"), Diagnostic->GetStringField(TEXT("pin_id")));
    Details->SetStringField(
        TEXT("hint"), Diagnostic->GetStringField(TEXT("hint")));

    const TSharedRef<FJsonObject> Warning = MakeShared<FJsonObject>();
    Warning->SetStringField(
        TEXT("code"), Diagnostic->GetStringField(TEXT("code")));
    Warning->SetStringField(
        TEXT("message"), Diagnostic->GetStringField(TEXT("message")));
    Warning->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
    Warning->SetObjectField(TEXT("details"), Details);
    return Warning;
}

TSharedRef<FJsonObject> MakeNextAction(
    const FString& Action,
    const FString& AssetPath)
{
    const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), AssetPath);
    const TSharedRef<FJsonObject> NextAction = MakeShared<FJsonObject>();
    NextAction->SetStringField(TEXT("domain"), TEXT("blueprint"));
    NextAction->SetStringField(TEXT("action"), Action);
    NextAction->SetObjectField(TEXT("params"), Params);
    return NextAction;
}

TSharedRef<FJsonObject> MakeInspectNextAction(
    const FString& AssetPath,
    const TArray<TSharedPtr<FJsonValue>>& Diagnostics)
{
    const TSharedRef<FJsonObject> NextAction = MakeNextAction(
        TEXT("inspect_blueprint"), AssetPath);
    const TSharedPtr<FJsonObject> Params = NextAction->GetObjectField(
        TEXT("params"));
    TArray<TSharedPtr<FJsonValue>> Queries;

    const TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
    Nodes->SetStringField(TEXT("op"), TEXT("nodes"));
    Nodes->SetStringField(TEXT("detail"), TEXT("detailed"));
    Nodes->SetNumberField(TEXT("limit"), 100);
    Queries.Add(MakeShared<FJsonValueObject>(Nodes));

    FString TargetNodeId;
    for (const TSharedPtr<FJsonValue>& Value : Diagnostics)
    {
        const TSharedPtr<FJsonObject> Diagnostic = Value.IsValid()
            ? Value->AsObject()
            : nullptr;
        if (Diagnostic &&
            Diagnostic->TryGetStringField(TEXT("node_id"), TargetNodeId) &&
            !TargetNodeId.IsEmpty())
        {
            break;
        }
    }
    if (!TargetNodeId.IsEmpty())
    {
        const TSharedRef<FJsonObject> Pins = MakeShared<FJsonObject>();
        Pins->SetStringField(TEXT("op"), TEXT("pins"));
        Pins->SetStringField(TEXT("node_id"), TargetNodeId);
        Pins->SetStringField(TEXT("detail"), TEXT("detailed"));
        Pins->SetNumberField(TEXT("limit"), 100);
        Queries.Add(MakeShared<FJsonValueObject>(Pins));
    }
    Params->SetArrayField(TEXT("queries"), Queries);
    return NextAction;
}

FString InvalidBlueprintResult()
{
    using namespace UE::MCPython::Blueprint2;
    const TSharedRef<FJsonObject> Result = MakeFailure(
        TEXT("INVALID_INPUT"),
        TEXT("params.asset_path"),
        TEXT("Invalid Blueprint."),
        false,
        TEXT("Load a valid Blueprint asset and retry compilation."));
    Result->SetStringField(TEXT("status"), TEXT("Unknown"));
    Result->SetStringField(TEXT("message"), TEXT("Invalid Blueprint."));
    Result->SetArrayField(TEXT("diagnostics"), {});
    return SerializeResult(Result);
}

TSharedPtr<FJsonObject> ParseJsonObject(const FString& Json)
{
    TSharedPtr<FJsonObject> Result;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    return FJsonSerializer::Deserialize(Reader, Result) ? Result : nullptr;
}

FString SnapshotEnabledState(const ENodeEnabledState State)
{
    switch (State)
    {
    case ENodeEnabledState::Enabled: return TEXT("enabled");
    case ENodeEnabledState::Disabled: return TEXT("disabled");
    case ENodeEnabledState::DevelopmentOnly: return TEXT("development_only");
    }
    return TEXT("unknown");
}

TSharedRef<FJsonObject> SnapshotMemberReference(
    const FMemberReference& Reference)
{
    const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(
        TEXT("member_name"), Reference.GetMemberName().ToString());
    Result->SetStringField(
        TEXT("member_guid"),
        Reference.GetMemberGuid().IsValid()
            ? Reference.GetMemberGuid().ToString(
                EGuidFormats::DigitsWithHyphensLower)
            : FString());
    Result->SetStringField(
        TEXT("parent_class_path"),
        GetPathNameSafe(Reference.GetMemberParentClass()));
    Result->SetStringField(
        TEXT("parent_package_path"),
        GetPathNameSafe(Reference.GetMemberParentPackage()));
    Result->SetBoolField(TEXT("self_context"), Reference.IsSelfContext());
    Result->SetBoolField(TEXT("local_scope"), Reference.IsLocalScope());
    return Result;
}

TSharedRef<FJsonObject> SnapshotNodeProperties(const UEdGraphNode* Node)
{
    const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    if (!Node)
    {
        return Properties;
    }
    Properties->SetStringField(
        TEXT("enabled_state"),
        SnapshotEnabledState(Node->GetDesiredEnabledState()));

    if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
    {
        Properties->SetObjectField(
            TEXT("function_reference"),
            SnapshotMemberReference(Call->FunctionReference));
        Properties->SetBoolField(
            TEXT("defaults_to_pure"), Call->bDefaultsToPureFunc != 0);
        Properties->SetBoolField(
            TEXT("enum_exec_expansion"),
            Call->bWantsEnumToExecExpansion != 0);
    }
    if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
    {
        Properties->SetObjectField(
            TEXT("variable_reference"),
            SnapshotMemberReference(Variable->VariableReference));
    }
    if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
    {
        Properties->SetObjectField(
            TEXT("event_reference"),
            SnapshotMemberReference(Event->EventReference));
        Properties->SetStringField(
            TEXT("custom_function_name"),
            Event->CustomFunctionName.ToString());
        Properties->SetBoolField(
            TEXT("override_function"), Event->bOverrideFunction != 0);
        Properties->SetBoolField(
            TEXT("internal_event"), Event->bInternalEvent != 0);
        Properties->SetNumberField(
            TEXT("function_flags"), Event->FunctionFlags);
    }
    if (const UK2Node_CustomEvent* CustomEvent =
        Cast<UK2Node_CustomEvent>(Node))
    {
        Properties->SetBoolField(
            TEXT("call_in_editor"), CustomEvent->bCallInEditor);
        Properties->SetBoolField(
            TEXT("deprecated"), CustomEvent->bIsDeprecated);
    }
    if (const UK2Node_InputKey* InputKey = Cast<UK2Node_InputKey>(Node))
    {
        const TSharedRef<FJsonObject> Input = MakeShared<FJsonObject>();
        Input->SetStringField(TEXT("key"), InputKey->InputKey.GetFName().ToString());
        Input->SetBoolField(TEXT("consume_input"), InputKey->bConsumeInput != 0);
        Input->SetBoolField(
            TEXT("execute_when_paused"), InputKey->bExecuteWhenPaused != 0);
        Input->SetBoolField(
            TEXT("override_parent_binding"),
            InputKey->bOverrideParentBinding != 0);
        Input->SetBoolField(TEXT("control"), InputKey->bControl != 0);
        Input->SetBoolField(TEXT("alt"), InputKey->bAlt != 0);
        Input->SetBoolField(TEXT("shift"), InputKey->bShift != 0);
        Input->SetBoolField(TEXT("command"), InputKey->bCommand != 0);
        Properties->SetObjectField(TEXT("input_key"), Input);
    }
    return Properties;
}

bool IsTransientSnapshotObject(const UObject* Object)
{
    if (!Object || Object->HasAnyFlags(RF_Transient))
    {
        return Object != nullptr;
    }
    const UPackage* Package = Object->GetOutermost();
    return !Package || Package == GetTransientPackage();
}

bool IsTransientSnapshotPath(const FString& Path)
{
    return Path.StartsWith(TEXT("/Engine/Transient.")) ||
        Path.StartsWith(TEXT("/Temp/"));
}

TSharedPtr<FJsonValue> SnapshotPinDefault(const UEdGraphPin* Pin)
{
    using namespace UE::MCPython::Blueprint2;
    if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
    {
        return MakeShared<FJsonValueNull>();
    }
    if (Pin->PinType.ContainerType == EPinContainerType::None &&
        Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
    {
        const FString* Source = FTextInspector::GetSourceString(
            Pin->DefaultTextValue);
        const TOptional<FString> TextNamespace = FTextInspector::GetNamespace(
            Pin->DefaultTextValue);
        const TOptional<FString> TextKey = FTextInspector::GetKey(
            Pin->DefaultTextValue);
        const TSharedRef<FJsonObject> TextDefault = MakeShared<FJsonObject>();
        TextDefault->SetStringField(
            TEXT("source"), Source ? *Source : Pin->DefaultValue);
        TextDefault->SetStringField(
            TEXT("namespace"), TextNamespace.Get(FString()));
        TextDefault->SetStringField(TEXT("key"), TextKey.Get(FString()));
        TextDefault->SetBoolField(
            TEXT("culture_invariant"),
            Pin->DefaultTextValue.IsCultureInvariant());
        return MakeShared<FJsonValueObject>(TextDefault);
    }

    const bool bObjectLike =
        Pin->PinType.ContainerType == EPinContainerType::None &&
        (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object ||
         Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class ||
         Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Interface ||
         Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject ||
         Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass);
    if (bObjectLike && IsTransientSnapshotObject(Pin->DefaultObject.Get()))
    {
        return MakeShared<FJsonValueNull>();
    }
    TSharedPtr<FJsonValue> Result = SerializeDefaultValue(
        Pin->PinType,
        Pin->DefaultValue,
        Pin->DefaultObject,
        Pin->DefaultTextValue);
    if (!Result.IsValid())
    {
        return MakeShared<FJsonValueNull>();
    }
    if (bObjectLike && Result->Type == EJson::String &&
        IsTransientSnapshotPath(Result->AsString()))
    {
        return MakeShared<FJsonValueNull>();
    }
    return Result;
}

struct FGraphDiffError
{
    FString Path;
    FString Message;
    FString Hint;
};

struct FGraphSnapshotIndex
{
    FString Digest;
    TMap<FString, TSharedPtr<FJsonObject>> Nodes;
    TMap<FString, TSharedPtr<FJsonObject>> Pins;
    TMap<FString, TSharedPtr<FJsonObject>> Connections;
};

struct FGraphDiffRecord
{
    FString Id;
    FString Change;
    TArray<FString> ChangedFields;
    TSharedPtr<FJsonValue> Before;
    TSharedPtr<FJsonValue> After;
};

struct FGraphDiffQuery
{
    FString Section;
    FString Detail = TEXT("compact");
    int32 Limit = 100;
    FString Cursor;
    FString LastId;
    FString Digest;
    FString AssetKey;
};

bool SetGraphDiffError(
    FGraphDiffError& OutError,
    const FString& Path,
    const FString& Message,
    const FString& Hint = TEXT("Provide a complete canonical Blueprint graph snapshot and retry."))
{
    OutError.Path = Path;
    OutError.Message = Message;
    OutError.Hint = Hint;
    return false;
}

bool ValidateJsonFields(
    const TSharedRef<FJsonObject>& Object,
    const TSet<FString>& SupportedFields,
    const FString& Path,
    FGraphDiffError& OutError)
{
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
    {
        if (!SupportedFields.Contains(Field.Key))
        {
            return SetGraphDiffError(
                OutError,
                Path + TEXT(".") + Field.Key,
                TEXT("Object contains an unknown field."));
        }
    }
    return true;
}

bool ReadRequiredString(
    const TSharedRef<FJsonObject>& Object,
    const FString& Field,
    const FString& Path,
    FString& OutValue,
    FGraphDiffError& OutError,
    const bool bAllowEmpty = false)
{
    if (!Object->TryGetStringField(Field, OutValue) ||
        (!bAllowEmpty && OutValue.IsEmpty()))
    {
        return SetGraphDiffError(
            OutError,
            Path,
            bAllowEmpty
                ? TEXT("Field must be a string.")
                : TEXT("Field must be a non-empty string."));
    }
    return true;
}

bool IsSnapshotMountedObjectPath(const FString& Path)
{
    if (Path.Len() < 5 || Path[0] != TEXT('/'))
    {
        return false;
    }

    const auto IsAsciiLetter = [](const TCHAR Character)
    {
        return (Character >= TEXT('A') && Character <= TEXT('Z')) ||
            (Character >= TEXT('a') && Character <= TEXT('z'));
    };
    const auto IsAsciiDigit = [](const TCHAR Character)
    {
        return Character >= TEXT('0') && Character <= TEXT('9');
    };

    int32 MountEnd = 1;
    if (!IsAsciiLetter(Path[MountEnd]))
    {
        return false;
    }
    for (++MountEnd; MountEnd < Path.Len() && Path[MountEnd] != TEXT('/');
         ++MountEnd)
    {
        if (!IsAsciiLetter(Path[MountEnd]) &&
            !IsAsciiDigit(Path[MountEnd]) &&
            Path[MountEnd] != TEXT('_'))
        {
            return false;
        }
    }
    if (MountEnd >= Path.Len() || Path[MountEnd] != TEXT('/'))
    {
        return false;
    }

    int32 ObjectSeparator = INDEX_NONE;
    if (!Path.FindLastChar(TEXT('.'), ObjectSeparator) ||
        ObjectSeparator <= MountEnd + 1 ||
        ObjectSeparator >= Path.Len() - 1)
    {
        return false;
    }
    for (int32 Index = MountEnd + 1; Index < ObjectSeparator; ++Index)
    {
        if (FChar::IsWhitespace(Path[Index]) || Path[Index] == TEXT(':'))
        {
            return false;
        }
    }
    for (int32 Index = ObjectSeparator + 1; Index < Path.Len(); ++Index)
    {
        if (FChar::IsWhitespace(Path[Index]) || Path[Index] == TEXT('/') ||
            Path[Index] == TEXT(':') || Path[Index] == TEXT('.'))
        {
            return false;
        }
    }
    return true;
}

bool IsSnapshotFullUnrealPath(const FString& Path)
{
    return Path.Len() >= 9 && Path.StartsWith(TEXT("/Script/"));
}

bool ValidateSnapshotPinType(
    const TSharedPtr<FJsonObject>& Type,
    const FString& Path,
    const bool bAllowOuterOnlyKinds,
    FGraphDiffError& OutError)
{
    if (!Type.IsValid())
    {
        return SetGraphDiffError(
            OutError, Path, TEXT("type must be a canonical pin-type object."));
    }

    FString Kind;
    if (!ReadRequiredString(
            Type.ToSharedRef(), TEXT("kind"), Path + TEXT(".kind"),
            Kind, OutError))
    {
        return false;
    }

    static const TSet<FString> PrimitiveKinds = {
        TEXT("bool"), TEXT("byte"), TEXT("int"), TEXT("int64"),
        TEXT("string"), TEXT("name"), TEXT("text")};
    static const TSet<FString> ReferenceKinds = {
        TEXT("object"), TEXT("class"), TEXT("interface"),
        TEXT("soft_object"), TEXT("soft_class")};
    static const TSet<FString> KindOnlyFields = {TEXT("kind")};
    static const TSet<FString> RealFields = {TEXT("kind"), TEXT("precision")};
    static const TSet<FString> TypePathFields = {TEXT("kind"), TEXT("type_path")};
    static const TSet<FString> ClassPathFields = {TEXT("kind"), TEXT("class_path")};
    static const TSet<FString> ItemFields = {TEXT("kind"), TEXT("item")};
    static const TSet<FString> MapFields = {TEXT("kind"), TEXT("key"), TEXT("value")};

    if (PrimitiveKinds.Contains(Kind))
    {
        return ValidateJsonFields(
            Type.ToSharedRef(), KindOnlyFields, Path, OutError);
    }
    if (Kind == TEXT("exec") || Kind == TEXT("unknown"))
    {
        if (!bAllowOuterOnlyKinds)
        {
            return SetGraphDiffError(
                OutError,
                Path,
                TEXT("Container item, key, and value types must be scalar pin types."));
        }
        return ValidateJsonFields(
            Type.ToSharedRef(), KindOnlyFields, Path, OutError);
    }
    if (Kind == TEXT("real"))
    {
        if (!ValidateJsonFields(
                Type.ToSharedRef(), RealFields, Path, OutError))
        {
            return false;
        }
        FString Precision;
        if (!ReadRequiredString(
                Type.ToSharedRef(), TEXT("precision"),
                Path + TEXT(".precision"), Precision, OutError) ||
            (Precision != TEXT("float") && Precision != TEXT("double")))
        {
            return SetGraphDiffError(
                OutError,
                Path + TEXT(".precision"),
                TEXT("real precision must be either 'float' or 'double'."));
        }
        return true;
    }
    if (Kind == TEXT("enum") || Kind == TEXT("struct"))
    {
        if (!ValidateJsonFields(
                Type.ToSharedRef(), TypePathFields, Path, OutError))
        {
            return false;
        }
        FString TypePath;
        if (!ReadRequiredString(
                Type.ToSharedRef(), TEXT("type_path"),
                Path + TEXT(".type_path"), TypePath, OutError) ||
            !IsSnapshotMountedObjectPath(TypePath))
        {
            return SetGraphDiffError(
                OutError,
                Path + TEXT(".type_path"),
                TEXT("type_path must be a canonical mounted object path."));
        }
        return true;
    }
    if (ReferenceKinds.Contains(Kind))
    {
        if (!ValidateJsonFields(
                Type.ToSharedRef(), ClassPathFields, Path, OutError))
        {
            return false;
        }
        FString ClassPath;
        if (!ReadRequiredString(
                Type.ToSharedRef(), TEXT("class_path"),
                Path + TEXT(".class_path"), ClassPath, OutError) ||
            !IsSnapshotMountedObjectPath(ClassPath))
        {
            return SetGraphDiffError(
                OutError,
                Path + TEXT(".class_path"),
                TEXT("class_path must be a canonical mounted object path."));
        }
        return true;
    }
    if (Kind == TEXT("array") || Kind == TEXT("set"))
    {
        if (!bAllowOuterOnlyKinds)
        {
            return SetGraphDiffError(
                OutError,
                Path,
                TEXT("Nested container pin types are not canonical."));
        }
        if (!ValidateJsonFields(
                Type.ToSharedRef(), ItemFields, Path, OutError))
        {
            return false;
        }
        const TSharedPtr<FJsonObject>* Item = nullptr;
        if (!Type->TryGetObjectField(TEXT("item"), Item) ||
            !Item || !Item->IsValid())
        {
            return SetGraphDiffError(
                OutError,
                Path + TEXT(".item"),
                TEXT("Container item must be a scalar pin-type object."));
        }
        return ValidateSnapshotPinType(
            *Item, Path + TEXT(".item"), false, OutError);
    }
    if (Kind == TEXT("map"))
    {
        if (!bAllowOuterOnlyKinds)
        {
            return SetGraphDiffError(
                OutError,
                Path,
                TEXT("Nested container pin types are not canonical."));
        }
        if (!ValidateJsonFields(
                Type.ToSharedRef(), MapFields, Path, OutError))
        {
            return false;
        }
        const TSharedPtr<FJsonObject>* Key = nullptr;
        const TSharedPtr<FJsonObject>* Value = nullptr;
        if (!Type->TryGetObjectField(TEXT("key"), Key) ||
            !Key || !Key->IsValid())
        {
            return SetGraphDiffError(
                OutError,
                Path + TEXT(".key"),
                TEXT("Map key must be a scalar pin-type object."));
        }
        if (!Type->TryGetObjectField(TEXT("value"), Value) ||
            !Value || !Value->IsValid())
        {
            return SetGraphDiffError(
                OutError,
                Path + TEXT(".value"),
                TEXT("Map value must be a scalar pin-type object."));
        }
        return ValidateSnapshotPinType(
                   *Key, Path + TEXT(".key"), false, OutError) &&
            ValidateSnapshotPinType(
                   *Value, Path + TEXT(".value"), false, OutError);
    }

    return SetGraphDiffError(
        OutError,
        Path + TEXT(".kind"),
        TEXT("kind is not a supported canonical snapshot pin type."));
}

bool ValidateSnapshotTargetId(
    const FString& Id,
    const UE::MCPython::Blueprint2::ETargetKind Kind,
    const FString& Path,
    FGraphDiffError& OutError)
{
    FGuid Guid;
    if (!UE::MCPython::Blueprint2::ParseTargetId(Id, Kind, Guid) ||
        !Guid.IsValid())
    {
        return SetGraphDiffError(
            OutError,
            Path,
            TEXT("Snapshot IDs must use the expected kind and a non-zero lowercase canonical GUID."));
    }
    return true;
}

bool IsCanonicalSha1Digest(const FString& Digest)
{
    if (Digest.Len() != 45 || !Digest.StartsWith(TEXT("sha1:")))
    {
        return false;
    }
    for (int32 Index = 5; Index < Digest.Len(); ++Index)
    {
        const TCHAR Character = Digest[Index];
        if (!((Character >= TEXT('0') && Character <= TEXT('9')) ||
              (Character >= TEXT('a') && Character <= TEXT('f'))))
        {
            return false;
        }
    }
    return true;
}

FString GraphConnectionId(
    const FString& SourcePinId,
    const FString& TargetPinId)
{
    return TEXT("connection:") + SourcePinId + TEXT("->") + TargetPinId;
}

bool ValidateGraphSnapshot(
    const TSharedPtr<FJsonObject>& Snapshot,
    const FString& Path,
    FGraphSnapshotIndex& OutIndex,
    FGraphDiffError& OutError)
{
    using namespace UE::MCPython::Blueprint2;
    if (!Snapshot.IsValid())
    {
        return SetGraphDiffError(
            OutError, Path, TEXT("Snapshot must be a JSON object."));
    }
    static const TSet<FString> SnapshotFields = {
        TEXT("snapshot_version"), TEXT("asset_path"),
        TEXT("blueprint_class"), TEXT("graphs"), TEXT("digest")};
    if (!ValidateJsonFields(
            Snapshot.ToSharedRef(), SnapshotFields, Path, OutError))
    {
        return false;
    }

    double SnapshotVersion = 0.0;
    if (!Snapshot->TryGetNumberField(
            TEXT("snapshot_version"), SnapshotVersion) ||
        SnapshotVersion != 1.0)
    {
        return SetGraphDiffError(
            OutError,
            Path + TEXT(".snapshot_version"),
            TEXT("snapshot_version must be exactly 1."));
    }
    FString Ignored;
    FString BlueprintClass;
    if (!ReadRequiredString(
            Snapshot.ToSharedRef(), TEXT("asset_path"),
            Path + TEXT(".asset_path"), Ignored, OutError) ||
        !ReadRequiredString(
            Snapshot.ToSharedRef(), TEXT("blueprint_class"),
            Path + TEXT(".blueprint_class"), BlueprintClass, OutError) ||
        !ReadRequiredString(
            Snapshot.ToSharedRef(), TEXT("digest"),
            Path + TEXT(".digest"), OutIndex.Digest, OutError))
    {
        return false;
    }
    if (!IsSnapshotFullUnrealPath(BlueprintClass))
    {
        return SetGraphDiffError(
            OutError,
            Path + TEXT(".blueprint_class"),
            TEXT("blueprint_class must be a /Script/ path."));
    }
    if (!IsCanonicalSha1Digest(OutIndex.Digest))
    {
        return SetGraphDiffError(
            OutError,
            Path + TEXT(".digest"),
            TEXT("digest must be sha1: followed by 40 lowercase hexadecimal characters."));
    }

    const TArray<TSharedPtr<FJsonValue>>* GraphValues = nullptr;
    if (!Snapshot->TryGetArrayField(TEXT("graphs"), GraphValues) ||
        !GraphValues)
    {
        return SetGraphDiffError(
            OutError,
            Path + TEXT(".graphs"),
            TEXT("graphs must be an array."));
    }

    static const TSet<FString> GraphFields = {
        TEXT("id"), TEXT("name"), TEXT("schema_path"),
        TEXT("nodes"), TEXT("connections")};
    static const TSet<FString> NodeFields = {
        TEXT("id"), TEXT("class_path"), TEXT("position"),
        TEXT("comment"), TEXT("properties"), TEXT("pins")};
    static const TSet<FString> PositionFields = {TEXT("x"), TEXT("y")};
    static const TSet<FString> PinFields = {
        TEXT("id"), TEXT("name"), TEXT("direction"),
        TEXT("type"), TEXT("default")};
    static const TSet<FString> ConnectionFields = {
        TEXT("source_pin_id"), TEXT("target_pin_id")};
    TSet<FString> GraphIds;
    FString LastGraphId;
    for (int32 GraphIndex = 0; GraphIndex < GraphValues->Num(); ++GraphIndex)
    {
        const FString GraphPath = FString::Printf(
            TEXT("%s.graphs[%d]"), *Path, GraphIndex);
        const TSharedPtr<FJsonValue>& GraphValue = (*GraphValues)[GraphIndex];
        if (!GraphValue.IsValid() || GraphValue->Type != EJson::Object)
        {
            return SetGraphDiffError(
                OutError, GraphPath, TEXT("Each graph must be an object."));
        }
        const TSharedPtr<FJsonObject> Graph = GraphValue->AsObject();
        if (!ValidateJsonFields(
                Graph.ToSharedRef(), GraphFields, GraphPath, OutError))
        {
            return false;
        }
        FString GraphId;
        FString SchemaPath;
        if (!ReadRequiredString(
                Graph.ToSharedRef(), TEXT("id"), GraphPath + TEXT(".id"),
                GraphId, OutError) ||
            !ReadRequiredString(
                Graph.ToSharedRef(), TEXT("name"), GraphPath + TEXT(".name"),
                Ignored, OutError, true) ||
            !ReadRequiredString(
                Graph.ToSharedRef(), TEXT("schema_path"),
                GraphPath + TEXT(".schema_path"), SchemaPath, OutError))
        {
            return false;
        }
        if (!ValidateSnapshotTargetId(
                GraphId, ETargetKind::Graph,
                GraphPath + TEXT(".id"), OutError))
        {
            return false;
        }
        if (!IsSnapshotFullUnrealPath(SchemaPath))
        {
            return SetGraphDiffError(
                OutError,
                GraphPath + TEXT(".schema_path"),
                TEXT("schema_path must be a /Script/ path."));
        }
        if (GraphIds.Contains(GraphId))
        {
            return SetGraphDiffError(
                OutError,
                GraphPath + TEXT(".id"),
                TEXT("Graph IDs must be unique within a snapshot."));
        }
        GraphIds.Add(GraphId);
        if (!LastGraphId.IsEmpty() && !(LastGraphId < GraphId))
        {
            return SetGraphDiffError(
                OutError,
                GraphPath + TEXT(".id"),
                TEXT("graphs must be sorted by strictly ascending stable ID."));
        }
        LastGraphId = GraphId;

        const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* ConnectionValues = nullptr;
        if (!Graph->TryGetArrayField(TEXT("nodes"), NodeValues) || !NodeValues)
        {
            return SetGraphDiffError(
                OutError,
                GraphPath + TEXT(".nodes"),
                TEXT("nodes must be an array."));
        }
        if (!Graph->TryGetArrayField(
                TEXT("connections"), ConnectionValues) || !ConnectionValues)
        {
            return SetGraphDiffError(
                OutError,
                GraphPath + TEXT(".connections"),
                TEXT("connections must be an array."));
        }

        TSet<FString> GraphPinIds;
        TMap<FString, FString> GraphPinDirections;
        FString LastNodeId;
        for (int32 NodeIndex = 0; NodeIndex < NodeValues->Num(); ++NodeIndex)
        {
            const FString NodePath = FString::Printf(
                TEXT("%s.nodes[%d]"), *GraphPath, NodeIndex);
            const TSharedPtr<FJsonValue>& NodeValue = (*NodeValues)[NodeIndex];
            if (!NodeValue.IsValid() || NodeValue->Type != EJson::Object)
            {
                return SetGraphDiffError(
                    OutError, NodePath, TEXT("Each node must be an object."));
            }
            const TSharedPtr<FJsonObject> Node = NodeValue->AsObject();
            if (!ValidateJsonFields(
                    Node.ToSharedRef(), NodeFields, NodePath, OutError))
            {
                return false;
            }
            FString NodeId;
            FString NodeClassPath;
            FString Comment;
            if (!ReadRequiredString(
                    Node.ToSharedRef(), TEXT("id"), NodePath + TEXT(".id"),
                    NodeId, OutError) ||
                !ReadRequiredString(
                    Node.ToSharedRef(), TEXT("class_path"),
                    NodePath + TEXT(".class_path"), NodeClassPath, OutError) ||
                !ReadRequiredString(
                    Node.ToSharedRef(), TEXT("comment"),
                    NodePath + TEXT(".comment"), Comment, OutError, true))
            {
                return false;
            }
            if (!ValidateSnapshotTargetId(
                    NodeId, ETargetKind::Node,
                    NodePath + TEXT(".id"), OutError))
            {
                return false;
            }
            if (!IsSnapshotFullUnrealPath(NodeClassPath))
            {
                return SetGraphDiffError(
                    OutError,
                    NodePath + TEXT(".class_path"),
                    TEXT("class_path must be a /Script/ path."));
            }
            if (OutIndex.Nodes.Contains(NodeId))
            {
                return SetGraphDiffError(
                    OutError,
                    NodePath + TEXT(".id"),
                    TEXT("Node IDs must be unique within a snapshot."));
            }
            if (!LastNodeId.IsEmpty() && !(LastNodeId < NodeId))
            {
                return SetGraphDiffError(
                    OutError,
                    NodePath + TEXT(".id"),
                    TEXT("nodes must be sorted by strictly ascending stable ID."));
            }
            LastNodeId = NodeId;

            const TSharedPtr<FJsonObject>* Position = nullptr;
            const TSharedPtr<FJsonObject>* Properties = nullptr;
            if (!Node->TryGetObjectField(TEXT("position"), Position) ||
                !Position || !Position->IsValid())
            {
                return SetGraphDiffError(
                    OutError,
                    NodePath + TEXT(".position"),
                    TEXT("position must be an object."));
            }
            if (!ValidateJsonFields(
                    Position->ToSharedRef(), PositionFields,
                    NodePath + TEXT(".position"), OutError))
            {
                return false;
            }
            for (const FString& Axis : {TEXT("x"), TEXT("y")})
            {
                double Coordinate = 0.0;
                if (!(*Position)->TryGetNumberField(Axis, Coordinate) ||
                    !FMath::IsFinite(Coordinate) ||
                    Coordinate != FMath::FloorToDouble(Coordinate) ||
                    Coordinate < static_cast<double>(MIN_int32) ||
                    Coordinate > static_cast<double>(MAX_int32))
                {
                    return SetGraphDiffError(
                        OutError,
                        NodePath + TEXT(".position.") + Axis,
                        TEXT("Position coordinates must be finite 32-bit integers."));
                }
            }
            if (!Node->TryGetObjectField(TEXT("properties"), Properties) ||
                !Properties || !Properties->IsValid())
            {
                return SetGraphDiffError(
                    OutError,
                    NodePath + TEXT(".properties"),
                    TEXT("properties must be an object."));
            }

            const TArray<TSharedPtr<FJsonValue>>* PinValues = nullptr;
            if (!Node->TryGetArrayField(TEXT("pins"), PinValues) || !PinValues)
            {
                return SetGraphDiffError(
                    OutError,
                    NodePath + TEXT(".pins"),
                    TEXT("pins must be an array."));
            }
            FString LastPinId;
            for (int32 PinIndex = 0; PinIndex < PinValues->Num(); ++PinIndex)
            {
                const FString PinPath = FString::Printf(
                    TEXT("%s.pins[%d]"), *NodePath, PinIndex);
                const TSharedPtr<FJsonValue>& PinValue = (*PinValues)[PinIndex];
                if (!PinValue.IsValid() || PinValue->Type != EJson::Object)
                {
                    return SetGraphDiffError(
                        OutError, PinPath, TEXT("Each pin must be an object."));
                }
                const TSharedPtr<FJsonObject> Pin = PinValue->AsObject();
                if (!ValidateJsonFields(
                        Pin.ToSharedRef(), PinFields, PinPath, OutError))
                {
                    return false;
                }
                FString PinId;
                FString Direction;
                if (!ReadRequiredString(
                        Pin.ToSharedRef(), TEXT("id"), PinPath + TEXT(".id"),
                        PinId, OutError) ||
                    !ReadRequiredString(
                        Pin.ToSharedRef(), TEXT("name"), PinPath + TEXT(".name"),
                        Ignored, OutError, true) ||
                    !ReadRequiredString(
                        Pin.ToSharedRef(), TEXT("direction"),
                        PinPath + TEXT(".direction"), Direction, OutError))
                {
                    return false;
                }
                if (!ValidateSnapshotTargetId(
                        PinId, ETargetKind::Pin,
                        PinPath + TEXT(".id"), OutError))
                {
                    return false;
                }
                if (Direction != TEXT("input") && Direction != TEXT("output"))
                {
                    return SetGraphDiffError(
                        OutError,
                        PinPath + TEXT(".direction"),
                        TEXT("direction must be either 'input' or 'output'."));
                }
                const TSharedPtr<FJsonObject>* Type = nullptr;
                if (!Pin->TryGetObjectField(TEXT("type"), Type) ||
                    !Type || !Type->IsValid())
                {
                    return SetGraphDiffError(
                        OutError,
                        PinPath + TEXT(".type"),
                        TEXT("type must be a canonical pin-type object."));
                }
                if (!ValidateSnapshotPinType(
                        *Type, PinPath + TEXT(".type"), true, OutError))
                {
                    return false;
                }
                const TSharedPtr<FJsonValue>* Default =
                    Pin->Values.Find(TEXT("default"));
                if (!Default || !Default->IsValid())
                {
                    return SetGraphDiffError(
                        OutError,
                        PinPath + TEXT(".default"),
                        TEXT("default must be present; use null when unsupported."));
                }
                if (OutIndex.Pins.Contains(PinId))
                {
                    return SetGraphDiffError(
                        OutError,
                        PinPath + TEXT(".id"),
                        TEXT("Pin IDs must be unique within a snapshot."));
                }
                if (!LastPinId.IsEmpty() && !(LastPinId < PinId))
                {
                    return SetGraphDiffError(
                        OutError,
                        PinPath + TEXT(".id"),
                        TEXT("pins must be sorted by strictly ascending stable ID."));
                }
                LastPinId = PinId;
                OutIndex.Pins.Add(PinId, Pin);
                GraphPinIds.Add(PinId);
                GraphPinDirections.Add(PinId, Direction);
            }
            OutIndex.Nodes.Add(NodeId, Node);
        }

        FString LastConnectionSource;
        FString LastConnectionTarget;
        for (int32 ConnectionIndex = 0;
             ConnectionIndex < ConnectionValues->Num();
             ++ConnectionIndex)
        {
            const FString ConnectionPath = FString::Printf(
                TEXT("%s.connections[%d]"), *GraphPath, ConnectionIndex);
            const TSharedPtr<FJsonValue>& ConnectionValue =
                (*ConnectionValues)[ConnectionIndex];
            if (!ConnectionValue.IsValid() ||
                ConnectionValue->Type != EJson::Object)
            {
                return SetGraphDiffError(
                    OutError,
                    ConnectionPath,
                    TEXT("Each connection must be an object."));
            }
            const TSharedPtr<FJsonObject> Connection =
                ConnectionValue->AsObject();
            if (!ValidateJsonFields(
                    Connection.ToSharedRef(), ConnectionFields,
                    ConnectionPath, OutError))
            {
                return false;
            }
            FString SourcePinId;
            FString TargetPinId;
            if (!ReadRequiredString(
                    Connection.ToSharedRef(), TEXT("source_pin_id"),
                    ConnectionPath + TEXT(".source_pin_id"),
                    SourcePinId, OutError) ||
                !ReadRequiredString(
                    Connection.ToSharedRef(), TEXT("target_pin_id"),
                    ConnectionPath + TEXT(".target_pin_id"),
                    TargetPinId, OutError))
            {
                return false;
            }
            if (!GraphPinIds.Contains(SourcePinId) ||
                !GraphPinIds.Contains(TargetPinId))
            {
                return SetGraphDiffError(
                    OutError,
                    ConnectionPath,
                    TEXT("Connection endpoints must reference pins in the same graph."));
            }
            if (GraphPinDirections[SourcePinId] != TEXT("output") ||
                GraphPinDirections[TargetPinId] != TEXT("input"))
            {
                return SetGraphDiffError(
                    OutError,
                    ConnectionPath,
                    TEXT("Connections must be oriented from an output pin to an input pin."));
            }
            const FString ConnectionId = GraphConnectionId(
                SourcePinId, TargetPinId);
            if (OutIndex.Connections.Contains(ConnectionId))
            {
                return SetGraphDiffError(
                    OutError,
                    ConnectionPath,
                    TEXT("Connections must be unique within a snapshot."));
            }
            if (!LastConnectionSource.IsEmpty() &&
                (SourcePinId < LastConnectionSource ||
                 (SourcePinId == LastConnectionSource &&
                  !(LastConnectionTarget < TargetPinId))))
            {
                return SetGraphDiffError(
                    OutError,
                    ConnectionPath,
                    TEXT("connections must be sorted by strictly ascending source and target pin IDs."));
            }
            LastConnectionSource = SourcePinId;
            LastConnectionTarget = TargetPinId;
            OutIndex.Connections.Add(ConnectionId, Connection);
        }
    }

    const TSharedRef<FJsonObject> UnsignedSnapshot = MakeShared<FJsonObject>();
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Snapshot->Values)
    {
        if (Field.Key != TEXT("digest"))
        {
            UnsignedSnapshot->SetField(Field.Key, Field.Value);
        }
    }
    const FString ComputedDigest = TEXT("sha1:") + Sha1Hex(
        CanonicalJsonString(MakeShared<FJsonValueObject>(UnsignedSnapshot)));
    if (ComputedDigest != OutIndex.Digest)
    {
        return SetGraphDiffError(
            OutError,
            Path + TEXT(".digest"),
            TEXT("Snapshot digest does not match its canonical contents."),
            TEXT("Create a fresh snapshot with snapshot_blueprint_graph and retry."));
    }
    return true;
}

bool JsonFieldsEqual(
    const TSharedRef<FJsonObject>& Left,
    const TSharedRef<FJsonObject>& Right,
    const FString& Field)
{
    const TSharedPtr<FJsonValue>* LeftValue = Left->Values.Find(Field);
    const TSharedPtr<FJsonValue>* RightValue = Right->Values.Find(Field);
    return LeftValue && RightValue &&
        LeftValue->IsValid() && RightValue->IsValid() &&
        FJsonValue::CompareEqual(**LeftValue, **RightValue);
}

TSharedPtr<FJsonValue> JsonObjectValue(
    const TSharedPtr<FJsonObject>& Object)
{
    return Object.IsValid()
        ? MakeShared<FJsonValueObject>(Object)
        : TSharedPtr<FJsonValue>();
}

TSharedPtr<FJsonValue> ProjectNodeProperties(
    const TSharedPtr<FJsonObject>& Node)
{
    if (!Node.IsValid())
    {
        return nullptr;
    }
    const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    for (const FString& Field : {
             TEXT("class_path"), TEXT("comment"), TEXT("properties")})
    {
        Result->SetField(Field, Node->Values.FindChecked(Field));
    }
    return MakeShared<FJsonValueObject>(Result);
}

TSharedPtr<FJsonValue> NodePositionValue(
    const TSharedPtr<FJsonObject>& Node)
{
    return Node.IsValid() ? Node->Values.FindChecked(TEXT("position")) : nullptr;
}

TArray<FString> SortedObjectKeys(
    const TMap<FString, TSharedPtr<FJsonObject>>& Before,
    const TMap<FString, TSharedPtr<FJsonObject>>& After)
{
    TSet<FString> KeySet;
    for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : Before)
    {
        KeySet.Add(Pair.Key);
    }
    for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : After)
    {
        KeySet.Add(Pair.Key);
    }
    TArray<FString> Keys = KeySet.Array();
    Keys.Sort();
    return Keys;
}

TArray<FGraphDiffRecord> BuildPresenceDiffRecords(
    const TMap<FString, TSharedPtr<FJsonObject>>& Before,
    const TMap<FString, TSharedPtr<FJsonObject>>& After)
{
    TArray<FGraphDiffRecord> Records;
    for (const FString& Id : SortedObjectKeys(Before, After))
    {
        const TSharedPtr<FJsonObject>* BeforeObject = Before.Find(Id);
        const TSharedPtr<FJsonObject>* AfterObject = After.Find(Id);
        if (!BeforeObject)
        {
            Records.Add({
                Id, TEXT("added"), {}, nullptr,
                JsonObjectValue(*AfterObject)});
        }
        else if (!AfterObject)
        {
            Records.Add({
                Id, TEXT("removed"), {},
                JsonObjectValue(*BeforeObject), nullptr});
        }
    }
    return Records;
}

TArray<FGraphDiffRecord> BuildPinDiffRecords(
    const FGraphSnapshotIndex& Before,
    const FGraphSnapshotIndex& After)
{
    TArray<FGraphDiffRecord> Records = BuildPresenceDiffRecords(
        Before.Pins, After.Pins);
    for (const FString& Id : SortedObjectKeys(Before.Pins, After.Pins))
    {
        const TSharedPtr<FJsonObject>* BeforePin = Before.Pins.Find(Id);
        const TSharedPtr<FJsonObject>* AfterPin = After.Pins.Find(Id);
        if (!BeforePin || !AfterPin)
        {
            continue;
        }
        TArray<FString> ChangedFields;
        for (const FString& Field : {
                 TEXT("name"), TEXT("direction"), TEXT("type"), TEXT("default")})
        {
            if (!JsonFieldsEqual(
                    BeforePin->ToSharedRef(), AfterPin->ToSharedRef(), Field))
            {
                ChangedFields.Add(Field);
            }
        }
        if (!ChangedFields.IsEmpty())
        {
            Records.Add({
                Id, TEXT("changed"), ChangedFields,
                JsonObjectValue(*BeforePin), JsonObjectValue(*AfterPin)});
        }
    }
    Records.Sort([](
        const FGraphDiffRecord& Left,
        const FGraphDiffRecord& Right)
    {
        return Left.Id < Right.Id;
    });
    return Records;
}

TArray<FGraphDiffRecord> BuildPropertyDiffRecords(
    const FGraphSnapshotIndex& Before,
    const FGraphSnapshotIndex& After)
{
    TArray<FGraphDiffRecord> Records;
    for (const FString& Id : SortedObjectKeys(Before.Nodes, After.Nodes))
    {
        const TSharedPtr<FJsonObject>* BeforeNode = Before.Nodes.Find(Id);
        const TSharedPtr<FJsonObject>* AfterNode = After.Nodes.Find(Id);
        if (!BeforeNode || !AfterNode)
        {
            continue;
        }
        TArray<FString> ChangedFields;
        for (const FString& Field : {TEXT("class_path"), TEXT("comment")})
        {
            if (!JsonFieldsEqual(
                    BeforeNode->ToSharedRef(), AfterNode->ToSharedRef(), Field))
            {
                ChangedFields.Add(Field);
            }
        }
        const TSharedPtr<FJsonObject> BeforeProperties =
            BeforeNode->ToSharedRef()->GetObjectField(TEXT("properties"));
        const TSharedPtr<FJsonObject> AfterProperties =
            AfterNode->ToSharedRef()->GetObjectField(TEXT("properties"));
        TSet<FString> PropertyKeySet;
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair :
             BeforeProperties->Values)
        {
            PropertyKeySet.Add(Pair.Key);
        }
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair :
             AfterProperties->Values)
        {
            PropertyKeySet.Add(Pair.Key);
        }
        TArray<FString> PropertyKeys = PropertyKeySet.Array();
        PropertyKeys.Sort();
        for (const FString& Field : PropertyKeys)
        {
            if (!JsonFieldsEqual(
                    BeforeProperties.ToSharedRef(),
                    AfterProperties.ToSharedRef(),
                    Field))
            {
                ChangedFields.Add(Field);
            }
        }
        if (!ChangedFields.IsEmpty())
        {
            Records.Add({
                Id, TEXT("changed"), ChangedFields,
                ProjectNodeProperties(*BeforeNode),
                ProjectNodeProperties(*AfterNode)});
        }
    }
    return Records;
}

TArray<FGraphDiffRecord> BuildPositionDiffRecords(
    const FGraphSnapshotIndex& Before,
    const FGraphSnapshotIndex& After)
{
    TArray<FGraphDiffRecord> Records;
    for (const FString& Id : SortedObjectKeys(Before.Nodes, After.Nodes))
    {
        const TSharedPtr<FJsonObject>* BeforeNode = Before.Nodes.Find(Id);
        const TSharedPtr<FJsonObject>* AfterNode = After.Nodes.Find(Id);
        if (!BeforeNode || !AfterNode)
        {
            continue;
        }
        const TSharedPtr<FJsonValue> BeforePosition =
            NodePositionValue(*BeforeNode);
        const TSharedPtr<FJsonValue> AfterPosition =
            NodePositionValue(*AfterNode);
        if (!FJsonValue::CompareEqual(*BeforePosition, *AfterPosition))
        {
            Records.Add({
                Id, TEXT("changed"), {TEXT("position")},
                BeforePosition, AfterPosition});
        }
    }
    return Records;
}

TArray<FGraphDiffRecord> BuildGraphDiffRecords(
    const FString& Section,
    const FGraphSnapshotIndex& Before,
    const FGraphSnapshotIndex& After)
{
    if (Section == TEXT("nodes"))
    {
        return BuildPresenceDiffRecords(Before.Nodes, After.Nodes);
    }
    if (Section == TEXT("pins"))
    {
        return BuildPinDiffRecords(Before, After);
    }
    if (Section == TEXT("connections"))
    {
        return BuildPresenceDiffRecords(
            Before.Connections, After.Connections);
    }
    if (Section == TEXT("properties"))
    {
        return BuildPropertyDiffRecords(Before, After);
    }
    return BuildPositionDiffRecords(Before, After);
}

TArray<TSharedPtr<FJsonValue>> JsonStringArray(
    const TArray<FString>& Values)
{
    TArray<TSharedPtr<FJsonValue>> Result;
    Result.Reserve(Values.Num());
    for (const FString& Value : Values)
    {
        Result.Add(MakeShared<FJsonValueString>(Value));
    }
    return Result;
}

TSharedRef<FJsonObject> MaterializeGraphDiffRecord(
    const FGraphDiffRecord& Record,
    const bool bDetailed)
{
    const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("id"), Record.Id);
    Result->SetStringField(TEXT("change"), Record.Change);
    Result->SetArrayField(
        TEXT("changed_fields"), JsonStringArray(Record.ChangedFields));
    if (bDetailed)
    {
        Result->SetField(
            TEXT("before"),
            Record.Before.IsValid()
                ? Record.Before
                : MakeShared<FJsonValueNull>());
        Result->SetField(
            TEXT("after"),
            Record.After.IsValid()
                ? Record.After
                : MakeShared<FJsonValueNull>());
    }
    return Result;
}

bool ParseGraphDiffQueries(
    const TSharedRef<FJsonObject>& Request,
    const FString& BeforeDigest,
    const FString& AfterDigest,
    TArray<FGraphDiffQuery>& OutQueries,
    FGraphDiffError& OutError)
{
    using namespace UE::MCPython::Blueprint2;
    const TArray<TSharedPtr<FJsonValue>>* QueryValues = nullptr;
    TArray<TSharedPtr<FJsonValue>> DefaultQueryValues;
    if (Request->HasField(TEXT("queries")) &&
        (!Request->TryGetArrayField(TEXT("queries"), QueryValues) ||
         !QueryValues))
    {
        return SetGraphDiffError(
            OutError,
            TEXT("params.queries"),
            TEXT("queries must be an array."));
    }
    if (!QueryValues || QueryValues->IsEmpty())
    {
        for (const FString& Section : {
                 TEXT("nodes"), TEXT("pins"), TEXT("connections"),
                 TEXT("properties"), TEXT("positions")})
        {
            const TSharedRef<FJsonObject> Query = MakeShared<FJsonObject>();
            Query->SetStringField(TEXT("section"), Section);
            DefaultQueryValues.Add(MakeShared<FJsonValueObject>(Query));
        }
        QueryValues = &DefaultQueryValues;
    }
    if (QueryValues->Num() > 5)
    {
        return SetGraphDiffError(
            OutError,
            TEXT("params.queries"),
            TEXT("queries must contain at most five section queries."));
    }

    static const TSet<FString> QueryFields = {
        TEXT("section"), TEXT("detail"), TEXT("limit"), TEXT("cursor")};
    static const TSet<FString> Sections = {
        TEXT("nodes"), TEXT("pins"), TEXT("connections"),
        TEXT("properties"), TEXT("positions")};
    TSet<FString> SeenSections;
    for (int32 Index = 0; Index < QueryValues->Num(); ++Index)
    {
        const FString QueryPath = FString::Printf(
            TEXT("params.queries[%d]"), Index);
        const TSharedPtr<FJsonValue>& QueryValue = (*QueryValues)[Index];
        if (!QueryValue.IsValid() || QueryValue->Type != EJson::Object)
        {
            return SetGraphDiffError(
                OutError, QueryPath, TEXT("Each query must be an object."));
        }
        const TSharedPtr<FJsonObject> QueryObject = QueryValue->AsObject();
        if (!ValidateJsonFields(
                QueryObject.ToSharedRef(), QueryFields, QueryPath, OutError))
        {
            return false;
        }
        FGraphDiffQuery Query;
        if (!ReadRequiredString(
                QueryObject.ToSharedRef(), TEXT("section"),
                QueryPath + TEXT(".section"), Query.Section, OutError))
        {
            return false;
        }
        if (!Sections.Contains(Query.Section))
        {
            return SetGraphDiffError(
                OutError,
                QueryPath + TEXT(".section"),
                TEXT("section must be nodes, pins, connections, properties, or positions."));
        }
        if (SeenSections.Contains(Query.Section))
        {
            return SetGraphDiffError(
                OutError,
                QueryPath + TEXT(".section"),
                TEXT("Each diff section may be queried at most once."));
        }
        SeenSections.Add(Query.Section);

        if (QueryObject->HasField(TEXT("detail")) &&
            (!QueryObject->TryGetStringField(TEXT("detail"), Query.Detail) ||
             (Query.Detail != TEXT("compact") &&
              Query.Detail != TEXT("detailed"))))
        {
            return SetGraphDiffError(
                OutError,
                QueryPath + TEXT(".detail"),
                TEXT("detail must be either 'compact' or 'detailed'."));
        }
        if (QueryObject->HasField(TEXT("limit")))
        {
            double Limit = 0.0;
            if (!QueryObject->TryGetNumberField(TEXT("limit"), Limit) ||
                !FMath::IsFinite(Limit) ||
                Limit != FMath::FloorToDouble(Limit) ||
                Limit < 1.0 || Limit > 500.0)
            {
                return SetGraphDiffError(
                    OutError,
                    QueryPath + TEXT(".limit"),
                    TEXT("limit must be an integer between 1 and 500."));
            }
            Query.Limit = static_cast<int32>(Limit);
        }
        if (QueryObject->HasField(TEXT("cursor")) &&
            !QueryObject->TryGetStringField(TEXT("cursor"), Query.Cursor))
        {
            return SetGraphDiffError(
                OutError,
                QueryPath + TEXT(".cursor"),
                TEXT("cursor must be a string."));
        }

        const TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
        Binding->SetStringField(TEXT("before_digest"), BeforeDigest);
        Binding->SetStringField(TEXT("after_digest"), AfterDigest);
        Binding->SetStringField(TEXT("section"), Query.Section);
        Binding->SetStringField(TEXT("detail"), Query.Detail);
        Query.Digest = CanonicalQueryDigest(Binding);
        Query.AssetKey = FString::Printf(
            TEXT("blueprint-graph-diff|%s|%s|%s|%s"),
            *BeforeDigest, *AfterDigest, *Query.Section, *Query.Detail);
        if (!Query.Cursor.IsEmpty())
        {
            FPageRequest Page;
            FString CursorError;
            if (!DecodeCursor(
                    Query.Cursor,
                    Query.AssetKey,
                    Query.Digest,
                    Page,
                    CursorError))
            {
                return SetGraphDiffError(
                    OutError,
                    QueryPath + TEXT(".cursor"),
                    CursorError,
                    TEXT("Use the cursor returned by this exact diff section in the current editor session."));
            }
            Query.LastId = Page.LastId;
        }
        OutQueries.Add(MoveTemp(Query));
    }
    return true;
}

FString SerializeGraphDiffError(const FGraphDiffError& Error)
{
    using namespace UE::MCPython::Blueprint2;
    return SerializeResult(MakeFailure(
        TEXT("INVALID_INPUT"),
        Error.Path,
        Error.Message,
        false,
        Error.Hint));
}

TSharedRef<FJsonObject> MakeHealthIssueFromDiagnostic(
    const TSharedRef<FJsonObject>& Diagnostic)
{
    const TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
    for (const FString& Field : {
             TEXT("code"),
             TEXT("severity"),
             TEXT("message"),
             TEXT("hint"),
             TEXT("graph_id"),
             TEXT("node_id"),
             TEXT("pin_id")})
    {
        Issue->SetStringField(Field, Diagnostic->GetStringField(Field));
    }
    Issue->SetStringField(TEXT("member_id"), FString());
    return Issue;
}

TSharedRef<FJsonObject> MakeHealthIssue(
    const FString& Code,
    const FString& Severity,
    const FString& Message,
    const FString& Hint,
    const FString& GraphId = FString(),
    const FString& NodeId = FString(),
    const FString& PinId = FString(),
    const FString& MemberId = FString())
{
    const TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
    Issue->SetStringField(TEXT("code"), Code);
    Issue->SetStringField(TEXT("severity"), Severity);
    Issue->SetStringField(TEXT("message"), Message);
    Issue->SetStringField(TEXT("hint"), Hint);
    Issue->SetStringField(TEXT("graph_id"), GraphId);
    Issue->SetStringField(TEXT("node_id"), NodeId);
    Issue->SetStringField(TEXT("pin_id"), PinId);
    Issue->SetStringField(TEXT("member_id"), MemberId);
    return Issue;
}

FString HealthIssueKey(const TSharedRef<FJsonObject>& Issue)
{
    const TArray<FString> Fields = {
        Issue->GetStringField(TEXT("code")),
        Issue->GetStringField(TEXT("graph_id")),
        Issue->GetStringField(TEXT("node_id")),
        Issue->GetStringField(TEXT("pin_id")),
        Issue->GetStringField(TEXT("member_id")),
    };
    return FString::Join(Fields, TEXT("\x1f"));
}

FString HealthIssueSortKey(const TSharedRef<FJsonObject>& Issue)
{
    const TArray<FString> Fields = {
        Issue->GetStringField(TEXT("graph_id")),
        Issue->GetStringField(TEXT("node_id")),
        Issue->GetStringField(TEXT("pin_id")),
        Issue->GetStringField(TEXT("member_id")),
        Issue->GetStringField(TEXT("code")),
        Issue->GetStringField(TEXT("message")),
        Issue->GetStringField(TEXT("hint")),
    };
    return FString::Join(Fields, TEXT("\x1f"));
}

void NormalizeHealthIssuesImpl(TArray<TSharedPtr<FJsonValue>>& Issues)
{
    Issues.Sort([](
        const TSharedPtr<FJsonValue>& LeftValue,
        const TSharedPtr<FJsonValue>& RightValue)
    {
        const TSharedPtr<FJsonObject> Left = LeftValue->AsObject();
        const TSharedPtr<FJsonObject> Right = RightValue->AsObject();
        const int32 LeftSeverity =
            Left->GetStringField(TEXT("severity")) == TEXT("error") ? 0 : 1;
        const int32 RightSeverity =
            Right->GetStringField(TEXT("severity")) == TEXT("error") ? 0 : 1;
        if (LeftSeverity != RightSeverity)
        {
            return LeftSeverity < RightSeverity;
        }
        return HealthIssueSortKey(Left.ToSharedRef()) <
            HealthIssueSortKey(Right.ToSharedRef());
    });

    TSet<FString> Seen;
    Issues.RemoveAll([&Seen](const TSharedPtr<FJsonValue>& Value)
    {
        const TSharedPtr<FJsonObject> Issue = Value.IsValid()
            ? Value->AsObject()
            : nullptr;
        if (!Issue)
        {
            return true;
        }
        const FString Key = HealthIssueKey(Issue.ToSharedRef());
        if (Seen.Contains(Key))
        {
            return true;
        }
        Seen.Add(Key);
        return false;
    });
}

void AddSCSHealthIssues(
    UBlueprint* Blueprint,
    TArray<TSharedPtr<FJsonValue>>& Issues)
{
    if (!Blueprint || !Blueprint->SimpleConstructionScript)
    {
        return;
    }
    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    TSet<USCS_Node*> AllNodeSet;
    TArray<USCS_Node*> Universe;
    for (USCS_Node* Node : SCS->GetAllNodes())
    {
        if (Node)
        {
            AllNodeSet.Add(Node);
            Universe.AddUnique(Node);
        }
    }
    for (USCS_Node* Root : SCS->GetRootNodes())
    {
        if (Root)
        {
            Universe.AddUnique(Root);
        }
    }
    for (int32 Index = 0; Index < Universe.Num(); ++Index)
    {
        for (USCS_Node* Child : Universe[Index]->GetChildNodes())
        {
            if (Child)
            {
                Universe.AddUnique(Child);
            }
        }
    }

    TMap<FGuid, TArray<USCS_Node*>> NodesByGuid;
    for (USCS_Node* Node : Universe)
    {
        if (Node->VariableGuid.IsValid())
        {
            NodesByGuid.FindOrAdd(Node->VariableGuid).Add(Node);
        }
    }
    for (const TPair<FGuid, TArray<USCS_Node*>>& Pair : NodesByGuid)
    {
        if (Pair.Value.Num() < 2)
        {
            continue;
        }
        TArray<FString> Names;
        for (const USCS_Node* Node : Pair.Value)
        {
            Names.Add(Node->GetVariableName().ToString());
        }
        Names.Sort();
        Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
            TEXT("BP_SCS_DUPLICATE_GUID"),
            TEXT("error"),
            FString::Printf(
                TEXT("SCS components share VariableGuid %s: %s."),
                *Pair.Key.ToString(EGuidFormats::DigitsWithHyphensLower),
                *FString::Join(Names, TEXT(", "))),
            TEXT("Assign each SCS component a unique persisted GUID, then inspect the component hierarchy."),
            FString(),
            FString(),
            FString(),
            UE::MCPython::Blueprint2::DescribeComponentTarget(
                Blueprint, Pair.Value[0]).Id)));
    }

    TMap<USCS_Node*, TSet<USCS_Node*>> ParentsByChild;
    for (USCS_Node* Parent : Universe)
    {
        for (USCS_Node* Child : Parent->GetChildNodes())
        {
            if (Child)
            {
                ParentsByChild.FindOrAdd(Child).Add(Parent);
            }
        }
    }
    for (const TPair<USCS_Node*, TSet<USCS_Node*>>& Pair : ParentsByChild)
    {
        if (Pair.Value.Num() > 1)
        {
            Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                TEXT("BP_SCS_MULTIPLE_PARENTS"),
                TEXT("error"),
                FString::Printf(
                    TEXT("SCS component '%s' has %d parents."),
                    *Pair.Key->GetVariableName().ToString(),
                    Pair.Value.Num()),
                TEXT("Reparent the component under exactly one SCS parent."),
                FString(),
                FString(),
                FString(),
                UE::MCPython::Blueprint2::DescribeComponentTarget(
                    Blueprint, Pair.Key).Id)));
        }
    }

    for (USCS_Node* Node : Universe)
    {
        if (!AllNodeSet.Contains(Node))
        {
            Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                TEXT("BP_SCS_NODE_MISSING_FROM_ALL_NODES"),
                TEXT("error"),
                FString::Printf(
                    TEXT("SCS component '%s' is referenced by the hierarchy but missing from AllNodes."),
                    *Node->GetVariableName().ToString()),
                TEXT("Rebuild the SCS hierarchy so every root and child is registered in AllNodes."),
                FString(),
                FString(),
                FString(),
                UE::MCPython::Blueprint2::DescribeComponentTarget(
                    Blueprint, Node).Id)));
        }
    }

    TSet<USCS_Node*> Reachable;
    TArray<USCS_Node*> Pending = SCS->GetRootNodes();
    for (int32 Index = 0; Index < Pending.Num(); ++Index)
    {
        USCS_Node* Node = Pending[Index];
        if (!Node || Reachable.Contains(Node))
        {
            continue;
        }
        Reachable.Add(Node);
        for (USCS_Node* Child : Node->GetChildNodes())
        {
            if (Child && !Reachable.Contains(Child))
            {
                Pending.Add(Child);
            }
        }
    }
    for (USCS_Node* Node : AllNodeSet)
    {
        if (!Reachable.Contains(Node))
        {
            Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                TEXT("BP_SCS_ORPHAN"),
                TEXT("error"),
                FString::Printf(
                    TEXT("SCS component '%s' is registered in AllNodes but unreachable from every root."),
                    *Node->GetVariableName().ToString()),
                TEXT("Attach the component to one valid SCS root or remove the orphaned entry."),
                FString(),
                FString(),
                FString(),
                UE::MCPython::Blueprint2::DescribeComponentTarget(
                    Blueprint, Node).Id)));
        }
    }

    TSet<USCS_Node*> Active;
    TSet<USCS_Node*> Complete;
    TSet<USCS_Node*> ReportedCycles;
    TFunction<void(USCS_Node*)> Visit = [&](USCS_Node* Node)
    {
        if (!Node || Complete.Contains(Node))
        {
            return;
        }
        if (Active.Contains(Node))
        {
            if (!ReportedCycles.Contains(Node))
            {
                ReportedCycles.Add(Node);
                Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                    TEXT("BP_SCS_CYCLE"),
                    TEXT("error"),
                    FString::Printf(
                        TEXT("SCS hierarchy contains a cycle through component '%s'."),
                        *Node->GetVariableName().ToString()),
                    TEXT("Break the component parenting cycle, leaving a directed tree rooted in the SCS root set."),
                    FString(),
                    FString(),
                    FString(),
                    UE::MCPython::Blueprint2::DescribeComponentTarget(
                        Blueprint, Node).Id)));
            }
            return;
        }
        Active.Add(Node);
        for (USCS_Node* Child : Node->GetChildNodes())
        {
            Visit(Child);
        }
        Active.Remove(Node);
        Complete.Add(Node);
    };
    for (USCS_Node* Node : Universe)
    {
        Visit(Node);
    }
}

void AddGraphHealthIssues(
    UBlueprint* Blueprint,
    TArray<TSharedPtr<FJsonValue>>& Issues)
{
    TArray<UEdGraph*> Graphs;
    Blueprint->GetAllGraphs(Graphs);
    for (UEdGraph* Graph : Graphs)
    {
        const UEdGraphSchema_K2* Schema = Graph
            ? Cast<UEdGraphSchema_K2>(Graph->GetSchema())
            : nullptr;
        if (!Graph || !Schema)
        {
            continue;
        }
        const FString GraphId =
            UE::MCPython::Blueprint2::DescribeGraphTarget(
                Blueprint, Graph).Id;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            const FString NodeId =
                UE::MCPython::Blueprint2::DescribeNodeTarget(
                    Blueprint, Node).Id;
            if (const UK2Node_CallFunction* Call =
                    Cast<UK2Node_CallFunction>(Node))
            {
                if (Call->FunctionReference.GetMemberName() != NAME_None &&
                    !Call->GetTargetFunction())
                {
                    Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                        TEXT("BP_UNRESOLVED_MEMBER"),
                        TEXT("error"),
                        FString::Printf(
                            TEXT("Call node references unresolved function '%s'."),
                            *Call->FunctionReference.GetMemberName().ToString()),
                        TEXT("Re-inspect the callable member and replace or retarget the stale call node."),
                        GraphId,
                        NodeId)));
                }
            }
            if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
            {
                if (Variable->VariableReference.GetMemberName() != NAME_None &&
                    !Variable->VariableReference.ResolveMember<FProperty>(
                        Variable->GetBlueprintClassFromNode()))
                {
                    Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                        TEXT("BP_UNRESOLVED_MEMBER"),
                        TEXT("error"),
                        FString::Printf(
                            TEXT("Variable node references unresolved member '%s'."),
                            *Variable->VariableReference.GetMemberName().ToString()),
                        TEXT("Re-inspect Blueprint variables and replace or retarget the stale variable node."),
                        GraphId,
                        NodeId)));
                }
            }

            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Input || Pin->bHidden ||
                    !Pin->LinkedTo.IsEmpty() ||
                    Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    continue;
                }
                FString SimpleMessage;
                const bool bSimpleValid =
                    Schema->DefaultValueSimpleValidation(
                        Pin->PinType,
                        Pin->PinName,
                        Pin->DefaultValue,
                        Pin->DefaultObject,
                        Pin->DefaultTextValue,
                        &SimpleMessage);
                const FString FullMessage = Schema->IsPinDefaultValid(
                    Pin,
                    Pin->DefaultValue,
                    Pin->DefaultObject,
                    Pin->DefaultTextValue);
                if (bSimpleValid && FullMessage.IsEmpty())
                {
                    continue;
                }
                const FString Message = FullMessage.IsEmpty()
                    ? SimpleMessage
                    : FullMessage;
                const bool bRequired = Pin->PinType.bIsReference ||
                    Pin->PinType.IsContainer() ||
                    Message.Contains(TEXT("must have an input"),
                        ESearchCase::IgnoreCase) ||
                    Message.Contains(TEXT("must be linked"),
                        ESearchCase::IgnoreCase) ||
                    Message.Contains(TEXT("must have a connection"),
                        ESearchCase::IgnoreCase);
                Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                    bRequired
                        ? TEXT("BP_MISSING_REQUIRED_PIN")
                        : TEXT("BP_INVALID_PIN_DEFAULT"),
                    TEXT("error"),
                    Message.IsEmpty()
                        ? FString::Printf(
                            TEXT("Input pin '%s' has no valid connection or default."),
                            *Pin->PinName.ToString())
                        : Message,
                    bRequired
                        ? TEXT("Connect the required input pin or assign a supported default value.")
                        : TEXT("Assign a canonical default compatible with the pin type."),
                    GraphId,
                    NodeId,
                    UE::MCPython::Blueprint2::DescribePinTarget(
                        Blueprint, Pin).Id)));
            }
        }
    }
}

void AddInterfaceHealthIssues(
    UBlueprint* Blueprint,
    TArray<TSharedPtr<FJsonValue>>& Issues)
{
    for (const FBPInterfaceDescription& Description :
         Blueprint->ImplementedInterfaces)
    {
        UClass* InterfaceClass = Description.Interface.Get();
        if (!InterfaceClass)
        {
            continue;
        }
        for (TFieldIterator<UFunction> FunctionIt(
                 InterfaceClass, EFieldIteratorFlags::ExcludeSuper);
             FunctionIt;
             ++FunctionIt)
        {
            UFunction* Function = *FunctionIt;
            if (!Function ||
                !Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
            {
                continue;
            }
            const bool bHasGraph = Description.Graphs.ContainsByPredicate(
                [Function](const UEdGraph* Graph)
                {
                    return Graph && Graph->GetFName() == Function->GetFName();
                });
            const bool bHasEvent =
                FBlueprintEditorUtils::FindOverrideForFunction(
                    Blueprint,
                    InterfaceClass,
                    Function->GetFName()) != nullptr;
            if (!bHasGraph && !bHasEvent)
            {
                Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                    TEXT("BP_MISSING_INTERFACE_IMPLEMENTATION"),
                    TEXT("error"),
                    FString::Printf(
                        TEXT("Interface function '%s.%s' has no implementation graph or event."),
                        *InterfaceClass->GetPathName(),
                        *Function->GetName()),
                    TEXT("Implement the missing interface function as the generated graph or override event."),
                    FString(),
                    FString(),
                    FString(),
                    FString::Printf(
                        TEXT("interface:%s#%s"),
                        *InterfaceClass->GetPathName(),
                        *Function->GetName()))));
            }
        }
    }
}

void AddMemberCollisionHealthIssues(
    UBlueprint* Blueprint,
    TArray<TSharedPtr<FJsonValue>>& Issues)
{
    struct FNamedMember
    {
        FString Name;
        FString Id;
    };
    TMap<FString, TArray<FNamedMember>> MembersByLowerName;
    const auto AddMember = [&MembersByLowerName](
        const FString& Name,
        const FString& Id)
    {
        if (!Name.IsEmpty())
        {
            MembersByLowerName.FindOrAdd(Name.ToLower()).Add({Name, Id});
        }
    };
    for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        AddMember(
            Variable.VarName.ToString(),
            UE::MCPython::Blueprint2::DescribeVariableTarget(
                Blueprint, Variable).Id);
    }
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (Graph)
        {
            AddMember(
                Graph->GetName(),
                UE::MCPython::Blueprint2::DescribeGraphTarget(
                    Blueprint, Graph).Id);
        }
    }
    for (UEdGraph* Graph : Blueprint->MacroGraphs)
    {
        if (Graph)
        {
            AddMember(
                Graph->GetName(),
                UE::MCPython::Blueprint2::DescribeGraphTarget(
                    Blueprint, Graph).Id);
        }
    }
    if (Blueprint->SimpleConstructionScript)
    {
        for (USCS_Node* Component :
             Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (Component)
            {
                AddMember(
                    Component->GetVariableName().ToString(),
                    UE::MCPython::Blueprint2::DescribeComponentTarget(
                        Blueprint, Component).Id);
            }
        }
    }
    TArray<UK2Node_CustomEvent*> CustomEvents;
    FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, CustomEvents);
    for (UK2Node_CustomEvent* Event : CustomEvents)
    {
        if (Event && Event->CustomFunctionName != NAME_None)
        {
            AddMember(
                Event->CustomFunctionName.ToString(),
                UE::MCPython::Blueprint2::DescribeNodeTarget(
                    Blueprint, Event).Id);
        }
    }

    for (TPair<FString, TArray<FNamedMember>>& Pair : MembersByLowerName)
    {
        if (Pair.Value.Num() < 2)
        {
            continue;
        }
        Pair.Value.Sort([](
            const FNamedMember& Left,
            const FNamedMember& Right)
        {
            return Left.Id == Right.Id
                ? Left.Name < Right.Name
                : Left.Id < Right.Id;
        });
        TArray<FString> Names;
        for (const FNamedMember& Member : Pair.Value)
        {
            Names.Add(Member.Name);
        }
        Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
            TEXT("BP_DUPLICATE_MEMBER"),
            TEXT("error"),
            FString::Printf(
                TEXT("Blueprint members collide case-insensitively: %s."),
                *FString::Join(Names, TEXT(", "))),
            TEXT("Rename one colliding variable, function, macro, event, or component."),
            FString(),
            FString(),
            FString(),
            Pair.Value[0].Id)));
    }
}

void AddVariableDefaultHealthIssues(
    UBlueprint* Blueprint,
    TArray<TSharedPtr<FJsonValue>>& Issues)
{
    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        const FEdGraphPinType& Type = Variable.VarType;
        if (Variable.DefaultValue.IsEmpty())
        {
            continue;
        }
        FString DefaultValue = Variable.DefaultValue;
        TObjectPtr<UObject> DefaultObject = nullptr;
        FText DefaultText;
        const bool bHardObject =
            Type.PinCategory == UEdGraphSchema_K2::PC_Object ||
            Type.PinCategory == UEdGraphSchema_K2::PC_Interface;
        const bool bHardClass =
            Type.PinCategory == UEdGraphSchema_K2::PC_Class;
        if (bHardObject || bHardClass)
        {
            Schema->GetPinDefaultValuesFromString(
                Type,
                Blueprint,
                Variable.DefaultValue,
                DefaultValue,
                DefaultObject,
                DefaultText);
            if (!Variable.DefaultValue.IsEmpty() &&
                !Variable.DefaultValue.Equals(
                    TEXT("None"), ESearchCase::IgnoreCase) &&
                !DefaultObject)
            {
                Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                    bHardClass
                        ? TEXT("BP_INVALID_CLASS_DEFAULT")
                        : TEXT("BP_INVALID_OBJECT_DEFAULT"),
                    TEXT("error"),
                    FString::Printf(
                        TEXT("Variable '%s' default path cannot be resolved: %s."),
                        *Variable.VarName.ToString(),
                        *Variable.DefaultValue),
                    bHardClass
                        ? TEXT("Set a loadable class path compatible with the variable type or clear the default.")
                        : TEXT("Set a loadable object path compatible with the variable type or clear the default."),
                    FString(),
                    FString(),
                    FString(),
                    UE::MCPython::Blueprint2::DescribeVariableTarget(
                        Blueprint, Variable).Id)));
                continue;
            }
        }
        else if (Type.PinCategory == UEdGraphSchema_K2::PC_Text)
        {
            const bool bComplexText = FTextStringHelper::IsComplexText(
                *Variable.DefaultValue);
            const TCHAR* TextEnd = FTextStringHelper::ReadFromBuffer(
                *Variable.DefaultValue,
                DefaultText,
                nullptr,
                nullptr,
                bComplexText);
            while (TextEnd && FChar::IsWhitespace(*TextEnd))
            {
                ++TextEnd;
            }
            if (!TextEnd || *TextEnd != TEXT('\0'))
            {
                Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                    TEXT("BP_INVALID_VARIABLE_DEFAULT"),
                    TEXT("error"),
                    FString::Printf(
                        TEXT("Variable '%s' has an invalid serialized text default."),
                        *Variable.VarName.ToString()),
                    TEXT("Set the variable through the canonical Blueprint default schema for its declared type."),
                    FString(),
                    FString(),
                    FString(),
                    UE::MCPython::Blueprint2::DescribeVariableTarget(
                        Blueprint, Variable).Id)));
                continue;
            }
            DefaultValue.Reset();
        }

        const TSharedPtr<FJsonValue> CanonicalJson =
            UE::MCPython::Blueprint2::SerializeDefaultValue(
                Type, DefaultValue, DefaultObject, DefaultText);
        UE::MCPython::Blueprint2::FNormalizedDefault Normalized;
        UE::MCPython::Blueprint2::FError NormalizeError;
        const bool bNormalized = CanonicalJson.IsValid() &&
            UE::MCPython::Blueprint2::NormalizeDefaultValue(
                Type,
                CanonicalJson,
                Blueprint,
                Normalized,
                NormalizeError,
                TEXT("default"));
        FString CanonicalStoredDefault;
        if (bNormalized)
        {
            if (Normalized.DefaultObject)
            {
                CanonicalStoredDefault = Normalized.DefaultObject->GetPathName();
            }
            else if (!Normalized.DefaultTextValue.IsEmpty())
            {
                FTextStringHelper::WriteToBuffer(
                    CanonicalStoredDefault, Normalized.DefaultTextValue);
            }
            else
            {
                CanonicalStoredDefault = Normalized.DefaultValue;
            }
        }
        const bool bCanonicalStoredValue =
            Type.PinCategory == UEdGraphSchema_K2::PC_Text ||
            CanonicalStoredDefault == Variable.DefaultValue;
        if (!bNormalized || !bCanonicalStoredValue)
        {
            const bool bClassPath =
                Type.PinCategory == UEdGraphSchema_K2::PC_Class ||
                Type.PinCategory == UEdGraphSchema_K2::PC_SoftClass;
            const bool bObjectPath =
                Type.PinCategory == UEdGraphSchema_K2::PC_Object ||
                Type.PinCategory == UEdGraphSchema_K2::PC_Interface ||
                Type.PinCategory == UEdGraphSchema_K2::PC_SoftObject;
            Issues.Add(MakeShared<FJsonValueObject>(MakeHealthIssue(
                bClassPath
                    ? TEXT("BP_INVALID_CLASS_DEFAULT")
                    : bObjectPath
                        ? TEXT("BP_INVALID_OBJECT_DEFAULT")
                        : TEXT("BP_INVALID_VARIABLE_DEFAULT"),
                TEXT("error"),
                FString::Printf(
                    TEXT("Variable '%s' has an invalid default: %s"),
                    *Variable.VarName.ToString(),
                    NormalizeError.Message.IsEmpty()
                        ? TEXT("stored value is not canonical for its declared type")
                        : *NormalizeError.Message),
                TEXT("Set the variable through the canonical Blueprint default schema for its declared type."),
                FString(),
                FString(),
                FString(),
                UE::MCPython::Blueprint2::DescribeVariableTarget(
                    Blueprint, Variable).Id)));
        }
    }
}
}

void UE::MCPython::Blueprint2::CollectStructuralHealthIssues(
    UBlueprint* Blueprint,
    TArray<TSharedPtr<FJsonValue>>& OutIssues)
{
    AddSCSHealthIssues(Blueprint, OutIssues);
    if (!Blueprint)
    {
        return;
    }
    AddGraphHealthIssues(Blueprint, OutIssues);
    AddInterfaceHealthIssues(Blueprint, OutIssues);
    AddMemberCollisionHealthIssues(Blueprint, OutIssues);
    AddVariableDefaultHealthIssues(Blueprint, OutIssues);
}

void UE::MCPython::Blueprint2::NormalizeHealthIssues(
    TArray<TSharedPtr<FJsonValue>>& Issues)
{
    NormalizeHealthIssuesImpl(Issues);
}

FString UMCPythonHelper::CompileBlueprint(UBlueprint* Blueprint)
{
    using namespace UE::MCPython::Blueprint2;
    if (!Blueprint)
    {
        return InvalidBlueprintResult();
    }
    if (!SupportsCompilerTokens())
    {
        return CompilerTokensUnsupportedResult();
    }

    FCompilerResultsLog Results;
    Results.bSilentMode = true;
    Results.bAnnotateMentionedNodes = false;
    FKismetEditorUtilities::CompileBlueprint(
        Blueprint,
        EBlueprintCompileOptions::None,
        &Results);

    TArray<TSharedPtr<FJsonValue>> Diagnostics;
    TArray<TSharedPtr<FJsonValue>> WarningRecords;
    int32 ErrorCount = 0;
    int32 WarningCount = 0;
    for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
    {
        const EMessageSeverity::Type Severity = Message->GetSeverity();
        const bool bWarning = Severity == EMessageSeverity::Warning ||
            Severity == EMessageSeverity::PerformanceWarning;
        const bool bError = Severity == EMessageSeverity::Error;
        if (!bWarning && !bError)
        {
            continue;
        }
        const TSharedRef<FJsonObject> Diagnostic = MakeDiagnostic(
            Blueprint, Message, bWarning);
        Diagnostics.Add(MakeShared<FJsonValueObject>(Diagnostic));
        if (bWarning)
        {
            ++WarningCount;
            WarningRecords.Add(MakeShared<FJsonValueObject>(
                MakeWarningRecord(Diagnostic)));
        }
        else
        {
            ++ErrorCount;
        }
    }

    const FString Status = CompileStatus(Blueprint);
    const bool bHasErrors = ErrorCount > 0 || Blueprint->Status == BS_Error;
    const FString LegacyMessage = bHasErrors
        ? TEXT("Blueprint compilation failed. Check the output log for details.")
        : TEXT("Blueprint compiled successfully.");
    const FString Summary = bHasErrors
        ? FString::Printf(
            TEXT("Blueprint compilation failed with %d error(s) and %d warning(s)."),
            ErrorCount,
            WarningCount)
        : WarningCount > 0
            ? FString::Printf(
                TEXT("Blueprint compiled with %d warning(s)."), WarningCount)
            : TEXT("Blueprint compiled successfully.");

    TSharedRef<FJsonObject> Result = bHasErrors
        ? MakeFailure(
            TEXT("COMPILE_FAILED"),
            FString(),
            LegacyMessage,
            false,
            TEXT("Inspect the structured diagnostics, repair the cited nodes or pins, and compile again."))
        : MakeSuccess(Summary);
    Result->SetBoolField(TEXT("success"), !bHasErrors);
    Result->SetStringField(TEXT("status"), Status);
    Result->SetStringField(TEXT("message"), LegacyMessage);
    Result->SetStringField(TEXT("summary"), Summary);

    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("result_status"), Status);
    Data->SetNumberField(TEXT("error_count"), ErrorCount);
    Data->SetNumberField(TEXT("warning_count"), WarningCount);
    Data->SetNumberField(TEXT("diagnostic_count"), Diagnostics.Num());
    Result->SetObjectField(TEXT("data"), Data);
    Result->SetArrayField(TEXT("diagnostics"), Diagnostics);
    Result->SetArrayField(TEXT("warnings"), WarningRecords);

    const FString AssetPath = Blueprint->GetPathName();
    TArray<TSharedPtr<FJsonValue>> NextActions;
    if (bHasErrors)
    {
        NextActions.Add(MakeShared<FJsonValueObject>(
            MakeInspectNextAction(AssetPath, Diagnostics)));
        NextActions.Add(MakeShared<FJsonValueObject>(
            MakeNextAction(TEXT("get_blueprint_health"), AssetPath)));
    }
    else
    {
        NextActions.Add(MakeShared<FJsonValueObject>(
            MakeNextAction(TEXT("get_blueprint_health"), AssetPath)));
        NextActions.Add(MakeShared<FJsonValueObject>(
            MakeNextAction(TEXT("snapshot_blueprint_graph"), AssetPath)));
    }
    Result->SetArrayField(TEXT("next_actions"), NextActions);
    return SerializeResult(Result);
}

FString UMCPythonHelper::GetBlueprintHealth(UBlueprint* Blueprint)
{
    using namespace UE::MCPython::Blueprint2;
    if (!Blueprint)
    {
        return InvalidBlueprintResult();
    }
    if (!SupportsCompilerTokens())
    {
        return CompilerTokensUnsupportedResult();
    }

    const TSharedPtr<FJsonObject> CompileResult = ParseJsonObject(
        CompileBlueprint(Blueprint));
    if (!CompileResult)
    {
        return SerializeResult(MakeFailure(
            TEXT("INTERNAL_ERROR"),
            TEXT("compile"),
            TEXT("Blueprint compilation returned invalid JSON."),
            false,
            TEXT("Inspect the Unreal editor log and retry the health check.")));
    }

    const bool bCompileSucceeded =
        CompileResult->GetBoolField(TEXT("success"));
    const FString CompileStatus =
        CompileResult->GetStringField(TEXT("status"));
    const TArray<TSharedPtr<FJsonValue>>& Diagnostics =
        CompileResult->GetArrayField(TEXT("diagnostics"));
    TArray<TSharedPtr<FJsonValue>> Issues;
    for (const TSharedPtr<FJsonValue>& Value : Diagnostics)
    {
        const TSharedPtr<FJsonObject> Diagnostic = Value.IsValid()
            ? Value->AsObject()
            : nullptr;
        if (!Diagnostic)
        {
            continue;
        }
        const TSharedRef<FJsonObject> Issue = MakeHealthIssueFromDiagnostic(
            Diagnostic.ToSharedRef());
        Issues.Add(MakeShared<FJsonValueObject>(Issue));
    }

    CollectStructuralHealthIssues(Blueprint, Issues);
    NormalizeHealthIssues(Issues);
    int32 ErrorCount = 0;
    int32 WarningCount = 0;
    for (const TSharedPtr<FJsonValue>& Value : Issues)
    {
        const TSharedPtr<FJsonObject> Issue = Value->AsObject();
        if (Issue->GetStringField(TEXT("severity")) == TEXT("error"))
        {
            ++ErrorCount;
        }
        else
        {
            ++WarningCount;
        }
    }

    const bool bHealthy = ErrorCount == 0;
    const FString Summary = bHealthy
        ? WarningCount > 0
            ? FString::Printf(
                TEXT("Blueprint health check found %d warning(s)."),
                WarningCount)
            : TEXT("Blueprint health check found no issues.")
        : FString::Printf(
            TEXT("Blueprint health check found %d error(s) and %d warning(s)."),
            ErrorCount,
            WarningCount);
    TSharedRef<FJsonObject> Result = bCompileSucceeded
        ? MakeSuccess(Summary)
        : MakeFailure(
            TEXT("COMPILE_FAILED"),
            FString(),
            TEXT("Blueprint compilation failed during the health check."),
            false,
            TEXT("Repair the cited Blueprint issues, then run the health check again."));

    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Data->SetBoolField(TEXT("healthy"), bHealthy);
    Data->SetStringField(TEXT("compile_status"), CompileStatus);
    Data->SetNumberField(TEXT("issue_count"), Issues.Num());
    Data->SetNumberField(TEXT("error_count"), ErrorCount);
    Data->SetNumberField(TEXT("warning_count"), WarningCount);
    Data->SetArrayField(TEXT("issues"), Issues);
    Result->SetObjectField(TEXT("data"), Data);

    const TArray<TSharedPtr<FJsonValue>>* CompileWarnings = nullptr;
    if (CompileResult->TryGetArrayField(TEXT("warnings"), CompileWarnings) &&
        CompileWarnings)
    {
        Result->SetArrayField(TEXT("warnings"), *CompileWarnings);
    }
    const TArray<TSharedPtr<FJsonValue>>* NextActions = nullptr;
    if (CompileResult->TryGetArrayField(TEXT("next_actions"), NextActions) &&
        NextActions)
    {
        Result->SetArrayField(TEXT("next_actions"), *NextActions);
    }
    return SerializeResult(Result);
}

FString UMCPythonHelper::SnapshotBlueprintGraph(
    UBlueprint* Blueprint,
    const FString& RequestJson)
{
    using namespace UE::MCPython::Blueprint2;
    if (!Blueprint)
    {
        return SerializeResult(MakeFailure(
            TEXT("INVALID_INPUT"),
            TEXT("params.asset_path"),
            TEXT("Invalid Blueprint."),
            false,
            TEXT("Load a valid Blueprint asset and retry the snapshot.")));
    }

    const TSharedPtr<FJsonObject> Request = ParseJsonObject(RequestJson);
    if (!Request)
    {
        return SerializeResult(MakeFailure(
            TEXT("INVALID_INPUT"),
            TEXT("params"),
            TEXT("Snapshot request must be one JSON object."),
            false,
            TEXT("Provide graph_ids as an optional array of stable graph IDs.")));
    }
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Request->Values)
    {
        if (Field.Key != TEXT("graph_ids"))
        {
            return SerializeResult(MakeFailure(
                TEXT("INVALID_INPUT"),
                TEXT("params.") + Field.Key,
                TEXT("Snapshot request contains an unknown field."),
                false,
                TEXT("Only graph_ids is accepted.")));
        }
    }

    TArray<FString> RequestedGraphIds;
    const TArray<TSharedPtr<FJsonValue>>* RequestedValues = nullptr;
    if (Request->TryGetArrayField(TEXT("graph_ids"), RequestedValues))
    {
        if (!RequestedValues || RequestedValues->Num() > 64)
        {
            return SerializeResult(MakeFailure(
                TEXT("INVALID_INPUT"),
                TEXT("params.graph_ids"),
                TEXT("graph_ids must contain at most 64 stable graph IDs."),
                false,
                TEXT("Pass an empty array to snapshot every supported graph.")));
        }
        TSet<FString> SeenIds;
        for (int32 Index = 0; Index < RequestedValues->Num(); ++Index)
        {
            FString GraphId;
            FGuid ParsedGuid;
            if (!(*RequestedValues)[Index].IsValid() ||
                !(*RequestedValues)[Index]->TryGetString(GraphId) ||
                !ParseTargetId(GraphId, ETargetKind::Graph, ParsedGuid) ||
                SeenIds.Contains(GraphId))
            {
                return SerializeResult(MakeFailure(
                    TEXT("INVALID_INPUT"),
                    FString::Printf(TEXT("params.graph_ids[%d]"), Index),
                    TEXT("Each graph ID must be one unique persisted graph:<guid> ID."),
                    false,
                    TEXT("Inspect the Blueprint and pass stable graph IDs exactly as returned.")));
            }
            SeenIds.Add(GraphId);
            RequestedGraphIds.Add(GraphId);
        }
    }
    else if (Request->HasField(TEXT("graph_ids")))
    {
        return SerializeResult(MakeFailure(
            TEXT("INVALID_INPUT"),
            TEXT("params.graph_ids"),
            TEXT("graph_ids must be an array."),
            false,
            TEXT("Pass an array of stable graph IDs or an empty array.")));
    }

    TArray<UEdGraph*> AllGraphs;
    Blueprint->GetAllGraphs(AllGraphs);
    TArray<UEdGraph*> Graphs;
    TSet<FString> MatchedGraphIds;
    TSet<FGuid> SeenGraphGuids;
    for (UEdGraph* Graph : AllGraphs)
    {
        if (!Graph || !Graph->GetSchema() ||
            !Graph->GetSchema()->IsA<UEdGraphSchema_K2>())
        {
            continue;
        }
        const FString GraphPath = FString::Printf(
            TEXT("asset.graphs[%s].id"),
            *Graph->GetName());
        if (!Graph->GraphGuid.IsValid())
        {
            if (!RequestedGraphIds.IsEmpty())
            {
                continue;
            }
            return SerializeResult(MakeFailure(
                TEXT("PRECONDITION_FAILED"),
                GraphPath,
                TEXT("Blueprint graph has no persisted GUID and cannot be snapshotted deterministically."),
                false,
                TEXT("Open and resave or repair the Blueprint so every graph has a valid GUID.")));
        }
        const FString GraphId = MakeTargetId(
            ETargetKind::Graph,
            Graph->GraphGuid);
        if (!RequestedGraphIds.IsEmpty() &&
            !RequestedGraphIds.Contains(GraphId))
        {
            continue;
        }
        if (SeenGraphGuids.Contains(Graph->GraphGuid))
        {
            return SerializeResult(MakeFailure(
                TEXT("PRECONDITION_FAILED"),
                GraphPath,
                TEXT("Blueprint contains duplicate persisted graph GUIDs."),
                false,
                TEXT("Repair the duplicate graph GUIDs before requesting a snapshot.")));
        }
        SeenGraphGuids.Add(Graph->GraphGuid);
        Graphs.Add(Graph);
        MatchedGraphIds.Add(GraphId);
    }
    for (int32 Index = 0; Index < RequestedGraphIds.Num(); ++Index)
    {
        if (!MatchedGraphIds.Contains(RequestedGraphIds[Index]))
        {
            return SerializeResult(MakeFailure(
                TEXT("INVALID_INPUT"),
                FString::Printf(TEXT("params.graph_ids[%d]"), Index),
                TEXT("Requested graph ID was not found in this Blueprint."),
                false,
                TEXT("Re-inspect the Blueprint and use a current supported K2 graph ID.")));
        }
    }
    Graphs.Sort([Blueprint](const UEdGraph& Left, const UEdGraph& Right)
    {
        return DescribeGraphTarget(Blueprint, &Left).Id <
            DescribeGraphTarget(Blueprint, &Right).Id;
    });

    TMap<FGuid, int32> AssetNodeGuidCounts;
    TMap<FGuid, int32> AssetPinGuidCounts;
    TSet<const UEdGraph*> CountedGraphs;
    for (const UEdGraph* Graph : AllGraphs)
    {
        if (!Graph || CountedGraphs.Contains(Graph) ||
            !Graph->GetSchema() ||
            !Graph->GetSchema()->IsA<UEdGraphSchema_K2>())
        {
            continue;
        }
        CountedGraphs.Add(Graph);
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            if (Node->NodeGuid.IsValid())
            {
                ++AssetNodeGuidCounts.FindOrAdd(Node->NodeGuid);
            }
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->PinId.IsValid())
                {
                    ++AssetPinGuidCounts.FindOrAdd(Pin->PinId);
                }
            }
        }
    }

    TSet<FGuid> SeenNodeGuids;
    TSet<FGuid> SeenPinGuids;
    for (const UEdGraph* Graph : Graphs)
    {
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            const FString NodePath = FString::Printf(
                TEXT("asset.graphs[%s].nodes[%s].id"),
                *Graph->GetName(),
                *Node->GetName());
            if (!Node->NodeGuid.IsValid())
            {
                return SerializeResult(MakeFailure(
                    TEXT("PRECONDITION_FAILED"),
                    NodePath,
                    TEXT("Blueprint node has no persisted GUID and cannot be snapshotted deterministically."),
                    false,
                    TEXT("Open and resave or repair the Blueprint so every node has a valid GUID.")));
            }
            if (SeenNodeGuids.Contains(Node->NodeGuid) ||
                AssetNodeGuidCounts.FindRef(Node->NodeGuid) > 1)
            {
                return SerializeResult(MakeFailure(
                    TEXT("PRECONDITION_FAILED"),
                    NodePath,
                    TEXT("Blueprint contains duplicate persisted node GUIDs for a selected node."),
                    false,
                    TEXT("Repair the duplicate node GUIDs before requesting a snapshot.")));
            }
            SeenNodeGuids.Add(Node->NodeGuid);

            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin)
                {
                    continue;
                }
                const FString PinPath = FString::Printf(
                    TEXT("asset.graphs[%s].nodes[%s].pins[%s].id"),
                    *Graph->GetName(),
                    *Node->GetName(),
                    *Pin->PinName.ToString());
                if (!Pin->PinId.IsValid())
                {
                    return SerializeResult(MakeFailure(
                        TEXT("PRECONDITION_FAILED"),
                        PinPath,
                        TEXT("Blueprint pin has no persisted GUID and cannot be snapshotted deterministically."),
                        false,
                        TEXT("Open and resave or repair the Blueprint so every pin has a valid GUID.")));
                }
                if (SeenPinGuids.Contains(Pin->PinId) ||
                    AssetPinGuidCounts.FindRef(Pin->PinId) > 1)
                {
                    return SerializeResult(MakeFailure(
                        TEXT("PRECONDITION_FAILED"),
                        PinPath,
                        TEXT("Blueprint contains duplicate persisted pin GUIDs for a selected pin."),
                        false,
                        TEXT("Repair the duplicate pin GUIDs before requesting a snapshot.")));
                }
                SeenPinGuids.Add(Pin->PinId);
            }
        }
    }

    TArray<TSharedPtr<FJsonValue>> GraphValues;
    for (UEdGraph* Graph : Graphs)
    {
        const TSharedRef<FJsonObject> GraphJson = MakeShared<FJsonObject>();
        GraphJson->SetStringField(
            TEXT("id"), DescribeGraphTarget(Blueprint, Graph).Id);
        GraphJson->SetStringField(TEXT("name"), Graph->GetName());
        GraphJson->SetStringField(
            TEXT("schema_path"), Graph->GetSchema()->GetClass()->GetPathName());

        TArray<UEdGraphNode*> Nodes;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node)
            {
                Nodes.Add(Node);
            }
        }
        Nodes.Sort([Blueprint](const UEdGraphNode& Left, const UEdGraphNode& Right)
        {
            return DescribeNodeTarget(Blueprint, &Left).Id <
                DescribeNodeTarget(Blueprint, &Right).Id;
        });

        TArray<TSharedPtr<FJsonValue>> NodeValues;
        TArray<TSharedPtr<FJsonValue>> ConnectionValues;
        TSet<TPair<FString, FString>> UniqueConnections;
        for (UEdGraphNode* Node : Nodes)
        {
            const TSharedRef<FJsonObject> NodeJson = MakeShared<FJsonObject>();
            NodeJson->SetStringField(
                TEXT("id"), DescribeNodeTarget(Blueprint, Node).Id);
            NodeJson->SetStringField(
                TEXT("class_path"), Node->GetClass()->GetPathName());
            const TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
            Position->SetNumberField(TEXT("x"), Node->NodePosX);
            Position->SetNumberField(TEXT("y"), Node->NodePosY);
            NodeJson->SetObjectField(TEXT("position"), Position);
            NodeJson->SetStringField(TEXT("comment"), Node->NodeComment);
            NodeJson->SetObjectField(
                TEXT("properties"), SnapshotNodeProperties(Node));

            TArray<UEdGraphPin*> Pins;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin)
                {
                    Pins.Add(Pin);
                }
            }
            Pins.Sort([Blueprint](const UEdGraphPin& Left, const UEdGraphPin& Right)
            {
                return DescribePinTarget(Blueprint, &Left).Id <
                    DescribePinTarget(Blueprint, &Right).Id;
            });
            TArray<TSharedPtr<FJsonValue>> PinValues;
            for (UEdGraphPin* Pin : Pins)
            {
                const TSharedRef<FJsonObject> PinJson = MakeShared<FJsonObject>();
                const FString PinId = DescribePinTarget(Blueprint, Pin).Id;
                PinJson->SetStringField(TEXT("id"), PinId);
                PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
                PinJson->SetStringField(
                    TEXT("direction"),
                    Pin->Direction == EGPD_Output ? TEXT("output") : TEXT("input"));
                if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    const TSharedRef<FJsonObject> ExecType =
                        MakeShared<FJsonObject>();
                    ExecType->SetStringField(TEXT("kind"), TEXT("exec"));
                    PinJson->SetObjectField(TEXT("type"), ExecType);
                }
                else
                {
                    PinJson->SetObjectField(
                        TEXT("type"), SerializeTypeSpec(Pin->PinType));
                }
                PinJson->SetField(TEXT("default"), SnapshotPinDefault(Pin));
                PinValues.Add(MakeShared<FJsonValueObject>(PinJson));

                if (Pin->Direction == EGPD_Output)
                {
                    for (const UEdGraphPin* Linked : Pin->LinkedTo)
                    {
                        if (Linked && Linked->Direction == EGPD_Input &&
                            Linked->GetOwningNode() &&
                            Linked->GetOwningNode()->GetGraph() == Graph)
                        {
                            UniqueConnections.Add(TPair<FString, FString>(
                                PinId,
                                DescribePinTarget(Blueprint, Linked).Id));
                        }
                    }
                }
            }
            NodeJson->SetArrayField(TEXT("pins"), PinValues);
            NodeValues.Add(MakeShared<FJsonValueObject>(NodeJson));
        }
        TArray<TPair<FString, FString>> Connections = UniqueConnections.Array();
        Connections.Sort([](
            const TPair<FString, FString>& Left,
            const TPair<FString, FString>& Right)
        {
            return Left.Key == Right.Key
                ? Left.Value < Right.Value
                : Left.Key < Right.Key;
        });
        for (const TPair<FString, FString>& Connection : Connections)
        {
            const TSharedRef<FJsonObject> ConnectionJson = MakeShared<FJsonObject>();
            ConnectionJson->SetStringField(
                TEXT("source_pin_id"), Connection.Key);
            ConnectionJson->SetStringField(
                TEXT("target_pin_id"), Connection.Value);
            ConnectionValues.Add(MakeShared<FJsonValueObject>(ConnectionJson));
        }
        GraphJson->SetArrayField(TEXT("nodes"), NodeValues);
        GraphJson->SetArrayField(TEXT("connections"), ConnectionValues);
        GraphValues.Add(MakeShared<FJsonValueObject>(GraphJson));
    }

    const TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
    Snapshot->SetNumberField(TEXT("snapshot_version"), 1);
    Snapshot->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
    Snapshot->SetStringField(
        TEXT("blueprint_class"), Blueprint->GetClass()->GetPathName());
    Snapshot->SetArrayField(TEXT("graphs"), GraphValues);
    const FString Digest = Sha1Hex(CanonicalJsonString(
        MakeShared<FJsonValueObject>(Snapshot)));
    Snapshot->SetStringField(TEXT("digest"), TEXT("sha1:") + Digest);

    return SerializeResult(MakeSuccess(
        FString::Printf(
            TEXT("Snapshotted %d Blueprint graph(s)."), GraphValues.Num()),
        Snapshot));
}

FString UMCPythonHelper::DiffBlueprintGraphs(const FString& RequestJson)
{
    using namespace UE::MCPython::Blueprint2;
    const TSharedPtr<FJsonObject> Request = ParseJsonObject(RequestJson);
    if (!Request.IsValid())
    {
        return SerializeGraphDiffError({
            TEXT("params"),
            TEXT("Diff request must be one JSON object."),
            TEXT("Provide before_snapshot, after_snapshot, and optional queries.")});
    }
    static const TSet<FString> RequestFields = {
        TEXT("before_snapshot"), TEXT("after_snapshot"), TEXT("queries")};
    FGraphDiffError Error;
    if (!ValidateJsonFields(
            Request.ToSharedRef(), RequestFields, TEXT("params"), Error))
    {
        return SerializeGraphDiffError(Error);
    }

    const TSharedPtr<FJsonObject>* BeforeObject = nullptr;
    const TSharedPtr<FJsonObject>* AfterObject = nullptr;
    if (!Request->TryGetObjectField(TEXT("before_snapshot"), BeforeObject) ||
        !BeforeObject || !BeforeObject->IsValid())
    {
        return SerializeGraphDiffError({
            TEXT("params.before_snapshot"),
            TEXT("before_snapshot must be a complete snapshot object."),
            TEXT("Pass data returned by snapshot_blueprint_graph.")});
    }
    if (!Request->TryGetObjectField(TEXT("after_snapshot"), AfterObject) ||
        !AfterObject || !AfterObject->IsValid())
    {
        return SerializeGraphDiffError({
            TEXT("params.after_snapshot"),
            TEXT("after_snapshot must be a complete snapshot object."),
            TEXT("Pass data returned by snapshot_blueprint_graph.")});
    }

    FGraphSnapshotIndex Before;
    FGraphSnapshotIndex After;
    if (!ValidateGraphSnapshot(
            *BeforeObject, TEXT("params.before_snapshot"), Before, Error) ||
        !ValidateGraphSnapshot(
            *AfterObject, TEXT("params.after_snapshot"), After, Error))
    {
        return SerializeGraphDiffError(Error);
    }

    TArray<FGraphDiffQuery> Queries;
    if (!ParseGraphDiffQueries(
            Request.ToSharedRef(),
            Before.Digest,
            After.Digest,
            Queries,
            Error))
    {
        return SerializeGraphDiffError(Error);
    }

    TArray<TSharedPtr<FJsonValue>> SectionValues;
    for (int32 QueryIndex = 0; QueryIndex < Queries.Num(); ++QueryIndex)
    {
        const FGraphDiffQuery& Query = Queries[QueryIndex];
        const TArray<FGraphDiffRecord> Records = BuildGraphDiffRecords(
            Query.Section, Before, After);
        int32 Start = 0;
        if (!Query.LastId.IsEmpty())
        {
            const int32 LastIndex = Records.IndexOfByPredicate(
                [&Query](const FGraphDiffRecord& Record)
                {
                    return Record.Id.Equals(
                        Query.LastId, ESearchCase::CaseSensitive);
                });
            if (LastIndex == INDEX_NONE)
            {
                return SerializeGraphDiffError({
                    FString::Printf(
                        TEXT("params.queries[%d].cursor"), QueryIndex),
                    TEXT("Cursor is stale because its last diff ID no longer exists."),
                    TEXT("Restart pagination from the first page of this diff section.")});
            }
            Start = LastIndex + 1;
        }
        const int32 End = FMath::Min(Start + Query.Limit, Records.Num());
        TArray<TSharedPtr<FJsonValue>> Items;
        for (int32 RecordIndex = Start; RecordIndex < End; ++RecordIndex)
        {
            Items.Add(MakeShared<FJsonValueObject>(
                MaterializeGraphDiffRecord(
                    Records[RecordIndex], Query.Detail == TEXT("detailed"))));
        }

        FString NextCursor;
        if (End > Start && End < Records.Num())
        {
            FPageRequest Page;
            Page.Limit = Query.Limit;
            Page.LastId = Records[End - 1].Id;
            Page.QueryDigest = Query.Digest;
            NextCursor = EncodeCursor(Query.AssetKey, Page);
        }
        const TSharedRef<FJsonObject> Section = MakeShared<FJsonObject>();
        Section->SetStringField(TEXT("section"), Query.Section);
        Section->SetStringField(TEXT("detail"), Query.Detail);
        Section->SetNumberField(TEXT("total_count"), Records.Num());
        Section->SetNumberField(TEXT("returned_count"), Items.Num());
        Section->SetArrayField(TEXT("items"), Items);
        Section->SetStringField(TEXT("next_cursor"), NextCursor);
        SectionValues.Add(MakeShared<FJsonValueObject>(Section));
    }

    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("before_digest"), Before.Digest);
    Data->SetStringField(TEXT("after_digest"), After.Digest);
    Data->SetArrayField(TEXT("sections"), SectionValues);
    return SerializeResult(MakeSuccess(
        FString::Printf(
            TEXT("Blueprint graph diff returned %d section%s."),
            SectionValues.Num(), SectionValues.Num() == 1 ? TEXT("") : TEXT("s")),
        Data));
}
