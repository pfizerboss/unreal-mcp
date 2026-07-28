// Copyright (c) 2025 GenOrca (by zenoengine). All Rights Reserved.

#include "MCPythonHelper.h"
#include "MCPythonHelperInternal.h"
#include "MCPythonBlueprint2Internal.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "BlueprintEditor.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTreeEditor.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTreeGraphNode_SimpleParallel.h"
#include "BehaviorTreeGraphNode_SubtreeTask.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "EdGraph/EdGraph.h"
#include "UObject/UObjectIterator.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
// AnimGraph authoring (editor-only AnimGraph module)
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateEntryNode.h"
#include "AnimationStateMachineGraph.h"
// Editor viewport projection
#include "LevelEditorViewport.h"
#include "EditorViewportClient.h"
#include "SceneView.h"

TArray<UObject*> UMCPythonHelper::GetAllEditedAssets()
{
    if (!GEditor) return {};
    return GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->GetAllEditedAssets();
}

TArray<UObject*> UMCPythonHelper::GetSelectedBlueprintNodes()
{
    TArray<UObject*> Result;
    if (!GEditor) return Result;
    auto* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    for (UObject* Asset : Subsystem->GetAllEditedAssets())
    {
        if (!Cast<UBlueprint>(Asset)) continue;
        IAssetEditorInstance* AssetEditorInstance = Subsystem->FindEditorForAsset(Asset, false);
        if (!AssetEditorInstance ||
            !UE::MCPython::Blueprint2::IsSupportedBlueprintSelectionEditor(
                AssetEditorInstance->GetEditorName()))
        {
            continue;
        }
        TSharedPtr<FTabManager> TabManager =
            AssetEditorInstance->GetAssociatedTabManager();
        if (!TabManager.IsValid()) continue;
        TSharedPtr<SDockTab> Tab = TabManager->GetOwnerTab();
        if (Tab.IsValid() && Tab->IsForeground())
        {
            FBlueprintEditor* BlueprintEditor = static_cast<FBlueprintEditor*>(
                static_cast<FAssetEditorToolkit*>(AssetEditorInstance));
            if (BlueprintEditor)
            {
                FGraphPanelSelectionSet SelectedNodes = BlueprintEditor->GetSelectedNodes();
                for (UObject* Node : SelectedNodes)
                {
                    Result.Add(Node);
                }
            }
        }
    }
    return Result;
}

TArray<FMCPythonBlueprintNodeInfo> UMCPythonHelper::GetSelectedBlueprintNodeInfos()
{
    TArray<FMCPythonBlueprintNodeInfo> Result;
    if (!GEditor) return Result;
    auto* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    for (UObject* Asset : Subsystem->GetAllEditedAssets())
    {
        if (!Cast<UBlueprint>(Asset)) continue;
        IAssetEditorInstance* AssetEditorInstance = Subsystem->FindEditorForAsset(Asset, false);
        if (!AssetEditorInstance ||
            !UE::MCPython::Blueprint2::IsSupportedBlueprintSelectionEditor(
                AssetEditorInstance->GetEditorName()))
        {
            continue;
        }
        TSharedPtr<FTabManager> TabManager =
            AssetEditorInstance->GetAssociatedTabManager();
        if (!TabManager.IsValid()) continue;
        TSharedPtr<SDockTab> Tab = TabManager->GetOwnerTab();
        if (Tab.IsValid() && Tab->IsForeground())
        {
            FBlueprintEditor* BlueprintEditor = static_cast<FBlueprintEditor*>(
                static_cast<FAssetEditorToolkit*>(AssetEditorInstance));
            if (BlueprintEditor)
            {
                FGraphPanelSelectionSet SelectedNodes = BlueprintEditor->GetSelectedNodes();
                for (UObject* NodeObj : SelectedNodes)
                {
                    UEdGraphNode* Node = Cast<UEdGraphNode>(NodeObj);
                    if (!Node) continue;
                    FMCPythonBlueprintNodeInfo NodeInfo;
                    UEdGraph* Graph = Node->GetGraph();
                    UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(Node);
                    const UE::MCPython::Blueprint2::FTargetRef GraphTarget =
                        UE::MCPython::Blueprint2::DescribeGraphTarget(
                            Blueprint, Graph);
                    const UE::MCPython::Blueprint2::FTargetRef NodeTarget =
                        UE::MCPython::Blueprint2::DescribeNodeTarget(
                            Blueprint, Node);
                    NodeInfo.GraphId = GraphTarget.Id;
                    NodeInfo.GraphOwnerId = GraphTarget.OwnerId;
                    NodeInfo.GraphName = GraphTarget.Name;
                    NodeInfo.GraphTypePath = GraphTarget.TypePath;
                    NodeInfo.StableId = NodeTarget.Id;
                    NodeInfo.NodeId = NodeInfo.StableId;
                    NodeInfo.OwnerId = NodeTarget.OwnerId;
                    NodeInfo.TypePath = NodeTarget.TypePath;
                    NodeInfo.NodeName = Node->GetName();
                    NodeInfo.NodeTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
                    NodeInfo.NodeClass = Node->GetClass()->GetName();
                    NodeInfo.ObjectPath = Node->GetPathName();
                    NodeInfo.NodeComment = Node->NodeComment;
                    for (UEdGraphPin* Pin : Node->Pins)
                    {
                        if (!Pin || Pin->bHidden) continue;
                        FMCPythonBlueprintPinInfo PinInfo;
                        PinInfo.GraphId = NodeInfo.GraphId;
                        PinInfo.NodeId = NodeInfo.StableId;
                        const UE::MCPython::Blueprint2::FTargetRef PinTarget =
                            UE::MCPython::Blueprint2::DescribePinTarget(
                                Blueprint, Pin);
                        PinInfo.StableId = PinTarget.Id;
                        PinInfo.PinId = PinInfo.StableId;
                        PinInfo.OwnerId = PinTarget.OwnerId;
                        PinInfo.TypePath = PinTarget.TypePath;
                        FString Friendly = Pin->PinFriendlyName.ToString();
                        PinInfo.PinName = Pin->GetName();
                        PinInfo.FriendlyName = Friendly;
                        PinInfo.Direction = (Pin->Direction == EGPD_Input) ? TEXT("In") : TEXT("Out");
                        PinInfo.PinType = Pin->PinType.PinCategory.ToString();
                        if (Pin->PinType.PinSubCategoryObject.IsValid())
                        {
                            PinInfo.PinSubType = Pin->PinType.PinSubCategoryObject->GetName();
                        }
                        PinInfo.DefaultValue = Pin->DefaultValue;
                        for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
                        {
                            if (LinkedPin && LinkedPin->GetOwningNode())
                            {
                                FMCPythonPinLinkInfo LinkInfo;
                                UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
                                UEdGraph* LinkedGraph = LinkedNode->GetGraph();
                                const UE::MCPython::Blueprint2::FTargetRef LinkedGraphTarget =
                                    UE::MCPython::Blueprint2::DescribeGraphTarget(
                                        Blueprint, LinkedGraph);
                                const UE::MCPython::Blueprint2::FTargetRef LinkedNodeTarget =
                                    UE::MCPython::Blueprint2::DescribeNodeTarget(
                                        Blueprint, LinkedNode);
                                const UE::MCPython::Blueprint2::FTargetRef LinkedPinTarget =
                                    UE::MCPython::Blueprint2::DescribePinTarget(
                                        Blueprint, LinkedPin);
                                LinkInfo.GraphId = LinkedGraphTarget.Id;
                                LinkInfo.GraphOwnerId = LinkedGraphTarget.OwnerId;
                                LinkInfo.GraphName = LinkedGraphTarget.Name;
                                LinkInfo.GraphTypePath = LinkedGraphTarget.TypePath;
                                LinkInfo.NodeId = LinkedNodeTarget.Id;
                                LinkInfo.NodeOwnerId = LinkedNodeTarget.OwnerId;
                                LinkInfo.NodeTypePath = LinkedNodeTarget.TypePath;
                                LinkInfo.PinId = LinkedPinTarget.Id;
                                LinkInfo.OwnerId = LinkedPinTarget.OwnerId;
                                LinkInfo.Name = LinkedPinTarget.Name;
                                LinkInfo.TypePath = LinkedPinTarget.TypePath;
                                LinkInfo.NodeName = LinkedNode->GetName();
                                LinkInfo.NodeTitle = LinkedNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
                                FString LinkedFriendly = LinkedPin->PinFriendlyName.ToString();
                                LinkInfo.PinName = LinkedFriendly.IsEmpty() ? LinkedPin->GetName() : LinkedFriendly;
                                PinInfo.LinkedTo.Add(LinkInfo);
                            }
                        }
                        NodeInfo.Pins.Add(PinInfo);
                    }
                    Result.Add(NodeInfo);
                }
            }
        }
    }
    return Result;
}

// ─── CompileBlueprint UFUNCTION ──────────────────────────────────────────────

FString UMCPythonHelper::CompileBlueprint(UBlueprint* Blueprint)
{
    if (!Blueprint)
        return MakeJsonError(TEXT("Invalid Blueprint."));

    FKismetEditorUtilities::CompileBlueprint(Blueprint);

    // Check compile status
    bool bHasError = (Blueprint->Status == BS_Error);
    bool bUpToDate = (Blueprint->Status == BS_UpToDate);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetBoolField(TEXT("success"), !bHasError);

    FString StatusStr;
    switch (Blueprint->Status)
    {
    case BS_UpToDate: StatusStr = TEXT("UpToDate"); break;
    case BS_Error: StatusStr = TEXT("Error"); break;
    case BS_Dirty: StatusStr = TEXT("Dirty"); break;
    case BS_BeingCreated: StatusStr = TEXT("BeingCreated"); break;
    default: StatusStr = TEXT("Unknown"); break;
    }
    Result->SetStringField(TEXT("status"), StatusStr);
    Result->SetStringField(TEXT("message"),
        bHasError ? TEXT("Blueprint compilation failed. Check the output log for details.")
                  : TEXT("Blueprint compiled successfully."));

    return SerializeJsonObj(Result);
}

// ─── SetBlueprintCDOProperty UFUNCTION ───────────────────────────────────────

FString UMCPythonHelper::SetBlueprintCDOProperty(UBlueprint* Blueprint, const FString& PropertyName, const FString& ValueStr)
{
    if (!Blueprint)
        return MakeJsonError(TEXT("Blueprint is null."));

    UClass* GenClass = Blueprint->GeneratedClass;
    if (!GenClass)
        return MakeJsonError(TEXT("Blueprint has no GeneratedClass."));

    UObject* CDO = GenClass->GetDefaultObject(false);
    if (!CDO)
        return MakeJsonError(TEXT("Could not get CDO."));

    FProperty* FoundProp = nullptr;
    for (TFieldIterator<FProperty> It(GenClass, EFieldIterationFlags::IncludeSuper); It; ++It)
    {
        if (It->GetName().Equals(PropertyName, ESearchCase::IgnoreCase))
        {
            FoundProp = *It;
            break;
        }
    }

    if (!FoundProp)
        return MakeJsonError(FString::Printf(TEXT("Property '%s' not found on '%s' or any parent class."), *PropertyName, *Blueprint->GetName()));

    void* PropAddr = FoundProp->ContainerPtrToValuePtr<void>(CDO);

    auto MarkModified = [&]()
    {
        CDO->Modify();
        Blueprint->Modify();
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    };

    // TSubclassOf<T>
    if (FClassProperty* ClassProp = CastField<FClassProperty>(FoundProp))
    {
        UClass* TargetClass = LoadClass<UObject>(nullptr, *ValueStr);
        if (!TargetClass)
        {
            UBlueprint* ValBP = Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *ValueStr));
            if (ValBP) TargetClass = ValBP->GeneratedClass;
        }
        if (!TargetClass)
            return MakeJsonError(FString::Printf(TEXT("Could not resolve class from '%s'."), *ValueStr));
        ClassProp->SetPropertyValue(PropAddr, TargetClass);
        MarkModified();
        TSharedPtr<FJsonObject> R = MakeShareable(new FJsonObject());
        R->SetBoolField(TEXT("success"), true);
        R->SetStringField(TEXT("property"), PropertyName);
        R->SetStringField(TEXT("value"), TargetClass->GetName());
        R->SetStringField(TEXT("message"), FString::Printf(TEXT("ClassProperty '%s' set to '%s'."), *PropertyName, *TargetClass->GetName()));
        return SerializeJsonObj(R);
    }

    // TSoftClassPtr<T>
    if (FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(FoundProp))
    {
        FSoftObjectPath SoftPath(ValueStr);
        FSoftObjectPtr SoftPtr(SoftPath);
        SoftClassProp->SetPropertyValue(PropAddr, SoftPtr);
        MarkModified();
        return MakeJsonSuccess(FString::Printf(TEXT("SoftClassProperty '%s' set to '%s'."), *PropertyName, *ValueStr));
    }

    // UObject* refs (must come after class properties since FClassProperty extends FObjectProperty)
    if (FObjectProperty* ObjProp = CastField<FObjectProperty>(FoundProp))
    {
        UObject* LoadedObj = StaticLoadObject(ObjProp->PropertyClass, nullptr, *ValueStr);
        if (!LoadedObj)
            return MakeJsonError(FString::Printf(TEXT("Could not load object from '%s'."), *ValueStr));
        ObjProp->SetObjectPropertyValue(PropAddr, LoadedObj);
        MarkModified();
        return MakeJsonSuccess(FString::Printf(TEXT("ObjectProperty '%s' set."), *PropertyName));
    }

    // bool
    if (FBoolProperty* BoolProp = CastField<FBoolProperty>(FoundProp))
    {
        bool bVal = (ValueStr == TEXT("true") || ValueStr == TEXT("True") || ValueStr == TEXT("1"));
        BoolProp->SetPropertyValue(PropAddr, bVal);
        MarkModified();
        return MakeJsonSuccess(FString::Printf(TEXT("BoolProperty '%s' set to %s."), *PropertyName, bVal ? TEXT("true") : TEXT("false")));
    }

    // int / float / double
    if (FNumericProperty* NumProp = CastField<FNumericProperty>(FoundProp))
    {
        if (NumProp->IsFloatingPoint())
            NumProp->SetFloatingPointPropertyValue(PropAddr, FCString::Atod(*ValueStr));
        else
            NumProp->SetIntPropertyValue(PropAddr, (int64)FCString::Atoi64(*ValueStr));
        MarkModified();
        return MakeJsonSuccess(FString::Printf(TEXT("NumericProperty '%s' set to '%s'."), *PropertyName, *ValueStr));
    }

    // FString
    if (FStrProperty* StrProp = CastField<FStrProperty>(FoundProp))
    {
        StrProp->SetPropertyValue(PropAddr, ValueStr);
        MarkModified();
        return MakeJsonSuccess(FString::Printf(TEXT("StrProperty '%s' set."), *PropertyName));
    }

    // FName
    if (FNameProperty* NameProp = CastField<FNameProperty>(FoundProp))
    {
        NameProp->SetPropertyValue(PropAddr, FName(*ValueStr));
        MarkModified();
        return MakeJsonSuccess(FString::Printf(TEXT("NameProperty '%s' set."), *PropertyName));
    }

    // FText
    if (FTextProperty* TextProp = CastField<FTextProperty>(FoundProp))
    {
        TextProp->SetPropertyValue(PropAddr, FText::FromString(ValueStr));
        MarkModified();
        return MakeJsonSuccess(FString::Printf(TEXT("TextProperty '%s' set."), *PropertyName));
    }

    return MakeJsonError(FString::Printf(TEXT("Unsupported property type '%s' for property '%s'."),
        *FoundProp->GetClass()->GetName(), *PropertyName));
}

// ─── AddComponentToBlueprint UFUNCTION ───────────────────────────────────────

// ─── ListBlueprintComponents UFUNCTION ───────────────────────────────────────

// ─── RemoveComponentFromBlueprint UFUNCTION ───────────────────────────────────

// ─── SetComponentProperty UFUNCTION ──────────────────────────────────────────

// ─── SkeletalMesh / Skeleton Helpers ──────────────────────────────────────────

FString UMCPythonHelper::GetSkeletonBones(USkeletalMesh* Mesh)
{
    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    if (!Mesh)
    {
        R->SetBoolField(TEXT("success"), false);
        R->SetStringField(TEXT("message"), TEXT("SkeletalMesh is null."));
        return SerializeJsonObj(R);
    }

    const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
    TArray<TSharedPtr<FJsonValue>> Bones;
    for (int32 i = 0; i < Ref.GetNum(); ++i)
    {
        TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
        B->SetStringField(TEXT("name"), Ref.GetBoneName(i).ToString());
        B->SetNumberField(TEXT("index"), i);
        const int32 ParentIdx = Ref.GetParentIndex(i);
        B->SetStringField(TEXT("parent"), ParentIdx >= 0 ? Ref.GetBoneName(ParentIdx).ToString() : TEXT(""));
        Bones.Add(MakeShared<FJsonValueObject>(B));
    }

    R->SetBoolField(TEXT("success"), true);
    R->SetNumberField(TEXT("bone_count"), Ref.GetNum());
    R->SetArrayField(TEXT("bones"), Bones);
    return SerializeJsonObj(R);
}

FString UMCPythonHelper::AddSkeletalMeshSocket(USkeletalMesh* Mesh, const FString& SocketName,
    const FString& BoneName,
    float LocationX, float LocationY, float LocationZ,
    float RotationPitch, float RotationYaw, float RotationRoll)
{
    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    if (!Mesh)
    {
        R->SetBoolField(TEXT("success"), false);
        R->SetStringField(TEXT("message"), TEXT("SkeletalMesh is null."));
        return SerializeJsonObj(R);
    }
    if (SocketName.IsEmpty() || BoneName.IsEmpty())
    {
        R->SetBoolField(TEXT("success"), false);
        R->SetStringField(TEXT("message"), TEXT("SocketName and BoneName are required."));
        return SerializeJsonObj(R);
    }
    if (Mesh->FindSocket(FName(*SocketName)) != nullptr)
    {
        R->SetBoolField(TEXT("success"), false);
        R->SetStringField(TEXT("message"), FString::Printf(TEXT("Socket '%s' already exists."), *SocketName));
        return SerializeJsonObj(R);
    }
    if (Mesh->GetRefSkeleton().FindBoneIndex(FName(*BoneName)) == INDEX_NONE)
    {
        R->SetBoolField(TEXT("success"), false);
        R->SetStringField(TEXT("message"), FString::Printf(TEXT("Bone '%s' not found in skeleton."), *BoneName));
        return SerializeJsonObj(R);
    }

    Mesh->Modify();
    USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(Mesh);
    Socket->SocketName = FName(*SocketName);
    Socket->BoneName = FName(*BoneName);
    Socket->RelativeLocation = FVector(LocationX, LocationY, LocationZ);
    Socket->RelativeRotation = FRotator(RotationPitch, RotationYaw, RotationRoll);
    Mesh->AddSocket(Socket, false);
    Mesh->MarkPackageDirty();

    R->SetBoolField(TEXT("success"), true);
    R->SetStringField(TEXT("socket_name"), SocketName);
    R->SetStringField(TEXT("bone_name"), BoneName);
    R->SetStringField(TEXT("message"), FString::Printf(TEXT("Added socket '%s' on bone '%s'."), *SocketName, *BoneName));
    return SerializeJsonObj(R);
}

FString UMCPythonHelper::RemoveSkeletalMeshSocket(USkeletalMesh* Mesh, const FString& SocketName)
{
    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    if (!Mesh)
    {
        R->SetBoolField(TEXT("success"), false);
        R->SetStringField(TEXT("message"), TEXT("SkeletalMesh is null."));
        return SerializeJsonObj(R);
    }
    USkeletalMeshSocket* Socket = Mesh->FindSocket(FName(*SocketName));
    if (!Socket)
    {
        R->SetBoolField(TEXT("success"), false);
        R->SetStringField(TEXT("message"), FString::Printf(TEXT("Socket '%s' not found."), *SocketName));
        return SerializeJsonObj(R);
    }

    Mesh->Modify();
    TArray<TObjectPtr<USkeletalMeshSocket>>& Sockets = Mesh->GetMeshOnlySocketList();
    Sockets.Remove(Socket);
    Mesh->MarkPackageDirty();

    R->SetBoolField(TEXT("success"), true);
    R->SetStringField(TEXT("removed"), SocketName);
    R->SetStringField(TEXT("message"), FString::Printf(TEXT("Removed socket '%s'."), *SocketName));
    return SerializeJsonObj(R);
}

// ─── Response transport (python_call) ─────────────────────────────────────────

static TOptional<FString> GMCPythonSubmittedResult;

void UMCPythonHelper::SubmitResult(const FString& ResultJson)
{
    GMCPythonSubmittedResult = ResultJson;
}

bool UMCPythonHelper::ConsumeSubmittedResult(FString& OutResult)
{
    if (GMCPythonSubmittedResult.IsSet())
    {
        OutResult = MoveTemp(GMCPythonSubmittedResult.GetValue());
        GMCPythonSubmittedResult.Reset();
        return true;
    }
    return false;
}

void UMCPythonHelper::ClearSubmittedResult()
{
    GMCPythonSubmittedResult.Reset();
}

// ─── Editor viewport projection ──────────────────────────────────────────────

static FLevelEditorViewportClient* GetActiveLevelViewportClient()
{
    if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->Viewport)
        return GCurrentLevelEditingViewportClient;
    if (GEditor)
    {
        for (FLevelEditorViewportClient* VC : GEditor->GetLevelViewportClients())
            if (VC && VC->Viewport && VC->Viewport->GetSizeXY().X > 0)
                return VC;
    }
    return nullptr;
}

static TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& V)
{
    TArray<TSharedPtr<FJsonValue>> A;
    A.Add(MakeShareable(new FJsonValueNumber(V.X)));
    A.Add(MakeShareable(new FJsonValueNumber(V.Y)));
    A.Add(MakeShareable(new FJsonValueNumber(V.Z)));
    return A;
}

FString UMCPythonHelper::WorldToScreen(FVector WorldLocation)
{
    FLevelEditorViewportClient* VC = GetActiveLevelViewportClient();
    if (!VC)
        return MakeJsonError(TEXT("No active level viewport."));

    FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
        VC->Viewport, VC->GetScene(), VC->EngineShowFlags).SetRealtimeUpdate(VC->IsRealtime()));
    FSceneView* View = VC->CalcSceneView(&ViewFamily);
    if (!View)
        return MakeJsonError(TEXT("Could not calculate the scene view."));

    FVector2D Pixel;
    const bool bInFront = View->WorldToPixel(WorldLocation, Pixel);
    const FIntPoint Size = VC->Viewport->GetSizeXY();
    const bool bOnScreen = bInFront && Pixel.X >= 0 && Pixel.Y >= 0 && Pixel.X <= Size.X && Pixel.Y <= Size.Y;

    TSharedPtr<FJsonObject> R = MakeShareable(new FJsonObject());
    R->SetBoolField(TEXT("success"), true);
    R->SetNumberField(TEXT("x"), Pixel.X);
    R->SetNumberField(TEXT("y"), Pixel.Y);
    R->SetBoolField(TEXT("visible"), bInFront);      // in front of the camera (not clipped)
    R->SetBoolField(TEXT("on_screen"), bOnScreen);   // also within the viewport rect
    R->SetNumberField(TEXT("viewport_width"), Size.X);
    R->SetNumberField(TEXT("viewport_height"), Size.Y);
    return SerializeJsonObj(R);
}

FString UMCPythonHelper::ScreenToWorld(float ScreenX, float ScreenY, float Distance)
{
    FLevelEditorViewportClient* VC = GetActiveLevelViewportClient();
    if (!VC)
        return MakeJsonError(TEXT("No active level viewport."));

    FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
        VC->Viewport, VC->GetScene(), VC->EngineShowFlags).SetRealtimeUpdate(VC->IsRealtime()));
    FSceneView* View = VC->CalcSceneView(&ViewFamily);
    if (!View)
        return MakeJsonError(TEXT("Could not calculate the scene view."));

    FVector Origin, Direction;
    View->DeprojectFVector2D(FVector2D(ScreenX, ScreenY), Origin, Direction);
    const FVector Location = Origin + Direction * Distance;

    TSharedPtr<FJsonObject> R = MakeShareable(new FJsonObject());
    R->SetBoolField(TEXT("success"), true);
    R->SetArrayField(TEXT("location"), VectorToJsonArray(Location));
    R->SetArrayField(TEXT("origin"), VectorToJsonArray(Origin));
    R->SetArrayField(TEXT("direction"), VectorToJsonArray(Direction));
    R->SetNumberField(TEXT("distance"), Distance);
    return SerializeJsonObj(R);
}
