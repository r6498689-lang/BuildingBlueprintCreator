#include "BuildingBlueprintLibrary.h"
#include "BuildingBackupManager.h"
#include "BuildingOptimizationMemory.h"
#include "BuildingOutlinerVisibility.h"
#include "BuildingThumbnailRenderer.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ChildActorComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "StaticMeshComponentLODInfo.h"
#include "StaticMeshEditorSubsystem.h"
#include "StaticMeshCompiler.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "DatasmithAssetUserData.h"
#include "DatasmithSceneActor.h"
#include "Editor.h"
#include "Editor/GroupActor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Level.h"
#include "Engine/SCS_Node.h"
#include "Engine/Selection.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "LevelUtils.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "UObject/StrongObjectPtr.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/OutputDevice.h"
#include "Misc/FeedbackContext.h"
#include "Materials/MaterialInterface.h"
#include "ObjectTools.h"
#include "PreviewScene.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "BuildingBlueprintCreator"

namespace BBC
{
// These two fields belong to Datasmith's importer, not to the reusable building.
// Never exempt another property merely because it has the same display name.
static bool IsDatasmithRelatedActors(UObject* Object, const FProperty* Property)
{
    return Object->IsA<ADatasmithSceneActor>()
        && Property == FindFProperty<FProperty>(ADatasmithSceneActor::StaticClass(), TEXT("RelatedActors"));
}

static bool MakeDatasmithDataIndependent(AActor* Template, FString& Error)
{
    Template->GroupActor = nullptr;
    // Work on a newly created template only. A clean metadata object also avoids
    // keeping a map component as an archetype through an instanced importer object.
    TArray<UObject*> Objects;
    GetObjectsWithOuter(Template, Objects, true);
    for (UObject* Object : Objects)
    {
        if (AActor* Member = Cast<AActor>(Object)) Member->GroupActor = nullptr;
        UActorComponent* Component = Cast<UActorComponent>(Object);
        if (!Component) continue;
        UDatasmithAssetUserData* Old = Cast<UDatasmithAssetUserData>(
            Component->GetAssetUserDataOfClass(UDatasmithAssetUserData::StaticClass()));
        if (!Old) continue;
        if (!Old->IsIn(Template) || Old->GetClass() != UDatasmithAssetUserData::StaticClass())
        {
            Error = TEXT("Datasmith 元数据不是独立副本或使用了自定义类型，已停止以避免修改原场景或丢失自定义数据。");
            return false;
        }
        UDatasmithAssetUserData* Clean = NewObject<UDatasmithAssetUserData>(Component, NAME_None, RF_Public);
        Clean->MetaData = Old->MetaData;
        Clean->MetaData.Remove(UDatasmithAssetUserData::UniqueIdMetaDataKey);
        Component->RemoveUserDataOfClass(UDatasmithAssetUserData::StaticClass());
        Component->AddAssetUserData(Clean);
        // Only retire objects owned by the copied metadata. Template values may
        // reference original map objects, which must never be modified here.
        TArray<UObject*> Retired;
        GetObjectsWithOuter(Old, Retired, true);
        Retired.Add(Old);
        for (UObject* Item : Retired)
        {
            Item->ClearFlags(RF_Public | RF_Standalone);
            Item->SetFlags(RF_Transient);
        }
        Old->ObjectTemplates.Reset();
    }
    return true;
}

class FSaveErrors final : public FOutputDevice
{
public:
    TArray<FString> Lines;
    virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override
    {
        if (Verbosity <= ELogVerbosity::Warning && Lines.Num() < 8) Lines.Add(FString(Text).Left(1600));
        if (GWarn) GWarn->Serialize(Text, Verbosity, Category);
    }
};

static bool IsBlueprintActor(const AActor* Actor)
{
    return Actor && Cast<UBlueprintGeneratedClass>(Actor->GetClass()) != nullptr;
}

static bool IsHierarchyActor(const AActor* Actor)
{
    // Names such as *_grp are not evidence of a harmless container. Restrict
    // this path to exact native classes with transform-only components.
    if (!Actor || (Actor->GetClass() != AActor::StaticClass() && Actor->GetClass() != AGroupActor::StaticClass())
        || !Actor->GetRootComponent()) return false;
    TInlineComponentArray<UActorComponent*> Components(Actor);
    for (const UActorComponent* Component : Components)
        if (Component && !Component->IsVisualizationComponent())
        {
            if (Component->GetClass() != USceneComponent::StaticClass()) return false;
            // A custom editor group is not necessarily a transform-only wrapper.
            if (Actor->IsA<AGroupActor>() && (Component != Actor->GetRootComponent()
                || (Component->GetAssetUserDataArray() && !Component->GetAssetUserDataArray()->IsEmpty()))) return false;
        }
    return true;
}

static bool IsEmptyMeshNode(const AActor* Actor)
{
    if (!Actor || Actor->GetClass() != AStaticMeshActor::StaticClass()) return false;
    auto* Mesh = CastChecked<AStaticMeshActor>(Actor)->GetStaticMeshComponent();
    if (!Mesh || Mesh->GetStaticMesh()) return false;
    TInlineComponentArray<UActorComponent*> Components(Actor);
    for (const UActorComponent* Component : Components)
        if (Component && Component != Mesh && !Component->IsVisualizationComponent()) return false;
    return true;
}

static TArray<AActor*> GatherActors(const TArray<AActor*>& Input, bool bIncludeAttached)
{
    TArray<AActor*> Result, Queue = Input;
    TSet<AActor*> Visited;
    for (int32 Index = 0; Index < Queue.Num(); ++Index)
    {
        AActor* Actor = Queue[Index];
        if (!IsValid(Actor) || Visited.Contains(Actor)) continue;
        Visited.Add(Actor);
        if (!Actor->IsChildActor() || Input.Contains(Actor)) Result.Add(Actor);
        // Selecting a native editor group always means selecting its members.
        // Group membership is independent of the attachment hierarchy.
        if (AGroupActor* Group = Cast<AGroupActor>(Actor))
        {
            TArray<AActor*> Members;
            Group->GetAllChildren(Members, false);
            Queue.Append(Members);
        }
        if (bIncludeAttached)
        {
            TArray<AActor*> Attached;
            Actor->GetAttachedActors(Attached);
            Queue.Append(Attached);
        }
    }
    return Result;
}

static AActor* GroupAsRuntimeTemplate(AActor* Source)
{
    // GroupActor is editor-only. Represent it with an ordinary Actor containing
    // a clean scene root, rather than serializing editor membership references.
    AActor* Template = NewObject<AActor>(GetTransientPackage(), AActor::StaticClass(), NAME_None, RF_ArchetypeObject | RF_Public);
    USceneComponent* Scene = NewObject<USceneComponent>(Template, TEXT("GroupRoot"), RF_Public | RF_Transactional);
    Template->AddInstanceComponent(Scene);
    Template->SetRootComponent(Scene);
    // Editor groups have movable roots even when all members are static.
    // Their runtime wrapper must permit static members to attach beneath it.
    Scene->SetMobility(EComponentMobility::Static);
    Scene->SetRelativeTransform_Direct(Source->GetActorTransform());
    Template->Tags = Source->Tags;
    Template->SetActorHiddenInGame(Source->IsHidden());
    Template->SetActorEnableCollision(Source->GetActorEnableCollision());
    return Template;
}

static FString Label(const AActor* Actor)
{
    return Actor ? Actor->GetActorLabel() : TEXT("<已失效 Actor>");
}

// Deliberately inspect serialized, user-facing properties, not infrastructure
// references such as Owner/AttachParent/WorldPrivate. Owner is checked separately.
static bool IsSavedUserProperty(const FProperty* Property)
{
    return !Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_Deprecated)
        && (Property->HasAnyPropertyFlags(CPF_Edit) || Cast<UBlueprintGeneratedClass>(Property->GetOwnerClass()));
}

static void VisitValueReferences(const FProperty* Property, const void* Value,
    const FString& Path, TFunctionRef<void(UObject*, const FString&, const FString&)> Visit, int32 Depth = 0)
{
    if (!Value || Depth > 32) return;
    if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(Property))
    {
        const FSoftObjectPtr& Ref = *static_cast<const FSoftObjectPtr*>(Value);
        Visit(Ref.Get(), Path, Ref.ToSoftObjectPath().ToString());
    }
    else if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Property))
    {
        Visit(Object->GetObjectPropertyValue(Value), Path, FString());
    }
    else if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
    {
        // A soft path stored as a struct is also a level dependency.
        if (Struct->Struct == TBaseStructure<FSoftObjectPath>::Get())
        {
            const FSoftObjectPath& Ref = *static_cast<const FSoftObjectPath*>(Value);
            Visit(Ref.ResolveObject(), Path, Ref.ToString());
        }
        else for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
        {
            for (int32 I = 0; I < It->ArrayDim; ++I)
                VisitValueReferences(*It, It->ContainerPtrToValuePtr<void>(Value, I), Path + TEXT(".") + It->GetName(), Visit, Depth + 1);
        }
    }
    else if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
    {
        FScriptArrayHelper Helper(Array, Value);
        for (int32 I = 0; I < Helper.Num(); ++I)
            VisitValueReferences(Array->Inner, Helper.GetRawPtr(I), Path + FString::Printf(TEXT("[%d]"), I), Visit, Depth + 1);
    }
    else if (const FSetProperty* Set = CastField<FSetProperty>(Property))
    {
        FScriptSetHelper Helper(Set, Value);
        for (int32 I = 0; I < Helper.GetMaxIndex(); ++I) if (Helper.IsValidIndex(I))
            VisitValueReferences(Set->ElementProp, Helper.GetElementPtr(I), Path, Visit, Depth + 1);
    }
    else if (const FMapProperty* Map = CastField<FMapProperty>(Property))
    {
        FScriptMapHelper Helper(Map, Value);
        for (int32 I = 0; I < Helper.GetMaxIndex(); ++I) if (Helper.IsValidIndex(I))
        {
            VisitValueReferences(Map->KeyProp, Helper.GetKeyPtr(I), Path + TEXT(".Key"), Visit, Depth + 1);
            VisitValueReferences(Map->ValueProp, Helper.GetValuePtr(I), Path + TEXT(".Value"), Visit, Depth + 1);
        }
    }
}

static void VisitReferences(UObject* Object, TFunctionRef<void(UObject*, const FString&, const FString&)> Visit,
    bool bSkipDatasmithManagement = false)
{
    for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It) if (IsSavedUserProperty(*It))
    {
        if (bSkipDatasmithManagement && IsDatasmithRelatedActors(Object, *It)) continue;
        for (int32 I = 0; I < It->ArrayDim; ++I)
            VisitValueReferences(*It, It->ContainerPtrToValuePtr<void>(Object, I), Object->GetName() + TEXT(".") + It->GetName(), Visit);
    }
}

static AActor* OwningActor(UObject* Object)
{
    if (AActor* Actor = Cast<AActor>(Object)) return Actor;
    if (UActorComponent* Component = Cast<UActorComponent>(Object)) return Component->GetOwner();
    return Object ? Object->GetTypedOuter<AActor>() : nullptr;
}

static void ActorFamily(AActor* Actor, TArray<AActor*>& Out)
{
    if (!Actor || Out.Contains(Actor)) return;
    Out.Add(Actor);
    TInlineComponentArray<UChildActorComponent*> Components(Actor);
    for (UChildActorComponent* Component : Components) ActorFamily(Component->GetChildActor(), Out);
}

static bool IsLevelReference(UObject* Object, const FString& SoftPath)
{
    return (Object && (OwningActor(Object) || Object->IsA<UWorld>() || Object->IsA<ULevel>()))
        || SoftPath.Contains(TEXT(":PersistentLevel."));
}

static void CheckSavedAsset(UObject* Object, const FString& Context, TSet<UPackage*>& Checked, TArray<FString>& Errors)
{
    if (!Object) return;
    if (UClass* Class = Cast<UClass>(Object)) Object = Class->ClassGeneratedBy;
    if (!Object) return;
    if (Object->IsA<UMaterialInterface>() && !Object->IsAsset())
    {
        Errors.Add(Context + TEXT(": 使用了动态/临时材质，无法作为可复用资产保存：") + Object->GetName()); return;
    }
    if (!Object->IsAsset()) return;
    UPackage* Package = Object->GetOutermost();
    if (Package->HasAnyPackageFlags(PKG_CompiledIn) || Checked.Contains(Package)) return;
    Checked.Add(Package);
    if (Package->IsDirty() || Object->HasAnyFlags(RF_Transient) || !FPackageName::DoesPackageExist(Package->GetName()))
        Errors.Add(Context + TEXT(": 请先保存引用的资源，再创建建筑蓝图：") + Package->GetName());
}

static void CheckReferences(AActor* Actor, TSet<UPackage*>& CheckedPackages, TArray<FString>& Errors)
{
    TArray<AActor*> Family;
    ActorFamily(Actor, Family);
    for (AActor* Member : Family)
    {
        CheckSavedAsset(Member->GetClass(), Label(Actor), CheckedPackages, Errors);
        if (Member->GetOwner() && !Family.Contains(Member->GetOwner()))
            Errors.Add(Label(Actor) + TEXT(": Owner 引用了关卡中的其他 Actor，无法可靠保存。"));
        auto Check = [&](UObject* Target, const FString& Path, const FString& SoftPath)
        {
            CheckSavedAsset(Target, Label(Actor) + TEXT(": ") + Path, CheckedPackages, Errors);
            if (!IsLevelReference(Target, SoftPath)) return;
            AActor* TargetActor = OwningActor(Target);
            if (TargetActor && Family.Contains(TargetActor) && SoftPath.IsEmpty()) return;
            Errors.Add(Label(Actor) + TEXT(": ") + Path + TEXT(" 引用了关卡对象 ")
                + (TargetActor ? Label(TargetActor) : SoftPath) + TEXT("。暂不支持跨 Actor 引用重绑定；请先解除此引用。"));
        };
        VisitReferences(Member, Check);
        TInlineComponentArray<UActorComponent*> Components(Member);
        for (UActorComponent* Component : Components) if (Component && !Component->IsVisualizationComponent())
            VisitReferences(Component, Check);
    }
}

struct FMeshState
{
    UClass* Class = nullptr;
    UStaticMesh* Mesh = nullptr;
    TArray<UMaterialInterface*> Materials;
    FTransform Transform;
    TArray<FTransform> Instances;
    FCollisionResponseContainer Responses;
    ECollisionEnabled::Type Collision = ECollisionEnabled::NoCollision;
    EComponentMobility::Type Mobility = EComponentMobility::Static;
    bool bVisible = false, bHidden = false, bShadow = false;
};

struct FActorState
{
    UClass* Class = nullptr;
    TMap<FString, FString> Values;
    TMap<FString, FMeshState> Meshes;
    TMap<FString, TSharedPtr<FActorState>> Children;
    TArray<FName> Tags;
    bool bHidden = false, bCollision = false;
};

static FActorState Capture(AActor* Actor)
{
    FActorState Result;
    Result.Class = Actor->GetClass();
    Result.Tags = Actor->Tags;
    Result.bHidden = Actor->IsHidden();
    Result.bCollision = Actor->GetActorEnableCollision();
    // All serialized BP variables, including inherited BP instance variables.
    for (TFieldIterator<FProperty> It(Actor->GetClass()); It; ++It)
    {
        if (!Cast<UBlueprintGeneratedClass>(It->GetOwnerClass()) || !IsSavedUserProperty(*It)) continue;
        FString Value;
        It->ExportText_InContainer(0, Value, Actor, nullptr, Actor, PPF_None);
        Value.ReplaceInline(*Actor->GetPathName(), TEXT("#SELF"));
        Result.Values.Add(It->GetName(), MoveTemp(Value));
    }
    TInlineComponentArray<UStaticMeshComponent*> Meshes(Actor);
    for (UStaticMeshComponent* Mesh : Meshes)
    {
        if (!Mesh || Mesh->IsVisualizationComponent() || Mesh->IsEditorOnly()) continue;
        FMeshState State;
        State.Class = Mesh->GetClass(); State.Mesh = Mesh->GetStaticMesh(); State.Transform = Mesh->GetComponentTransform();
        State.Collision = Mesh->GetCollisionEnabled(); State.Responses = Mesh->GetCollisionResponseToChannels();
        State.Mobility = Mesh->Mobility; State.bVisible = Mesh->IsVisible(); State.bHidden = Mesh->bHiddenInGame; State.bShadow = Mesh->CastShadow;
        for (int32 I = 0; I < Mesh->GetNumMaterials(); ++I) State.Materials.Add(Mesh->GetMaterial(I));
        if (UInstancedStaticMeshComponent* ISM = Cast<UInstancedStaticMeshComponent>(Mesh))
            for (int32 I = 0; I < ISM->GetInstanceCount(); ++I)
            {
                FTransform Transform; ISM->GetInstanceTransform(I, Transform, true); State.Instances.Add(Transform);
            }
        Result.Meshes.Add(Mesh->GetName(), MoveTemp(State));
    }
    TInlineComponentArray<UChildActorComponent*> Children(Actor);
    for (UChildActorComponent* Component : Children) if (AActor* Child = Component->GetChildActor())
        Result.Children.Add(Component->GetName(), MakeShared<FActorState>(Capture(Child)));
    return Result;
}

static bool SameTransform(const FTransform& A, const FTransform& B)
{
    return A.GetLocation().Equals(B.GetLocation(), 0.05) && A.GetScale3D().Equals(B.GetScale3D(), 0.0001)
        && A.GetRotation().Equals(B.GetRotation(), 0.0001);
}

static bool Compare(const FActorState& Before, const FActorState& After, const FString& Path, FString& Error)
{
    auto Fail = [&](const FString& Detail) { Error = Path + TEXT(": ") + Detail; return false; };
    if (Before.Class != After.Class || Before.Tags != After.Tags || Before.bHidden != After.bHidden || Before.bCollision != After.bCollision)
        return Fail(TEXT("Actor 类型、标签、可见性或碰撞状态与原对象不一致。"));
    if (Before.Values.Num() != After.Values.Num()) return Fail(TEXT("蓝图实例参数数量发生变化。"));
    for (const auto& Pair : Before.Values)
    {
        const FString* Other = After.Values.Find(Pair.Key);
        if (!Other || Pair.Value != *Other) return Fail(TEXT("蓝图实例参数未能保留：") + Pair.Key);
    }
    if (Before.Meshes.Num() != After.Meshes.Num() || Before.Children.Num() != After.Children.Num())
        return Fail(TEXT("生成后的模型或嵌套蓝图数量与原对象不一致；请检查构造脚本或实例添加的组件。"));
    for (const auto& Pair : Before.Meshes)
    {
        const FMeshState* Other = After.Meshes.Find(Pair.Key);
        const FMeshState& Original = Pair.Value;
        if (!Other || Original.Class != Other->Class || Original.Mesh != Other->Mesh || Original.Materials != Other->Materials)
            return Fail(TEXT("模型或材质未能完整保留：") + Pair.Key);
        if (!SameTransform(Original.Transform, Other->Transform)) return Fail(TEXT("模型的世界位置、旋转或缩放发生变化：") + Pair.Key
            + TEXT("\n原始：") + Original.Transform.ToString() + TEXT("\n生成：") + Other->Transform.ToString());
        if (Original.Mobility != Other->Mobility || Original.Collision != Other->Collision || Original.Responses != Other->Responses
            || Original.bVisible != Other->bVisible || Original.bHidden != Other->bHidden || Original.bShadow != Other->bShadow)
            return Fail(TEXT("模型的移动性、碰撞、可见性或投影设置发生变化：") + Pair.Key);
        if (Original.Instances.Num() != Other->Instances.Num()) return Fail(TEXT("实例化网格体数量发生变化：") + Pair.Key);
        for (int32 I = 0; I < Original.Instances.Num(); ++I)
            if (!SameTransform(Original.Instances[I], Other->Instances[I])) return Fail(TEXT("实例化网格体变换发生变化：") + Pair.Key);
    }
    for (const auto& Pair : Before.Children)
    {
        const TSharedPtr<FActorState>* Other = After.Children.Find(Pair.Key);
        if (!Other) return Fail(TEXT("嵌套蓝图丢失：") + Pair.Key);
        if (!Compare(*Pair.Value, **Other, Path + TEXT("/") + Pair.Key, Error)) return false;
    }
    return true;
}

static void DiscardPackage(UPackage* Package)
{
    ForEachObjectWithPackage(Package, [](UObject* Object)
    {
        Object->ClearFlags(RF_Public | RF_Standalone);
        Object->SetFlags(RF_Transient);
        return true;
    }, true);
    Package->SetDirtyFlag(false);
    Package->Rename(*FString::Printf(TEXT("/Temp/RejectedBuilding_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)), nullptr,
        REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional);
}

#include "BuildingInstancing.inl"
#include "BuildingNanite.inl"
}

TArray<AActor*> UBuildingBlueprintLibrary::CollectActors(const TArray<AActor*>& Actors, bool bIncludeAllAttachedChildren)
{
    return BBC::GatherActors(Actors, bIncludeAllAttachedChildren);
}

FString UBuildingBlueprintLibrary::FindAvailableAssetName(const FString& FolderPath, const FString& DesiredName)
{
    for (int32 Suffix = 0; Suffix < 100000; ++Suffix)
    {
        const FString Name = DesiredName + (Suffix ? FString::Printf(TEXT("_%d"), Suffix) : FString());
        const FString PackageName = FolderPath / Name;
        if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName)) return Name;
    }
    return FString();
}

static FBuildingBlueprintResult CreateBuildingBlueprintInternal(const TArray<AActor*>& Input, const FBuildingBlueprintOptions& Options)
{
    using namespace BBC;
    const bool bUseInstances = Options.bUseISM || Options.bUseHISM;
    FBuildingBlueprintResult Result;
    TArray<FString> Errors;
    if (!GEditor || GEditor->PlayWorld)
    {
        Result.Message = TEXT("请先退出运行或模拟模式，在关卡编辑器中创建蓝图。"); return Result;
    }
    TArray<AActor*> Actors = UBuildingBlueprintLibrary::CollectActors(Input, Options.bIncludeAllAttachedChildren);
    // A stale selection must not silently become a partial building.
    if (Input.IsEmpty() || Input.ContainsByPredicate([](AActor* A) { return !IsValid(A); }) || Actors.IsEmpty())
    {
        Result.Message = TEXT("选择为空或其中有已失效对象，请重新选择建筑 Actor。"); return Result;
    }
    UWorld* World = Actors[0]->GetWorld();
    if (!World || World != GEditor->GetEditorWorldContext().World())
    {
        Result.Message = TEXT("只能转换当前编辑关卡中已加载的 Actor。"); return Result;
    }
    FString Folder = Options.FolderPath;
    Folder.RemoveFromEnd(TEXT("/"));
    FText PathReason;
    if (!(Folder == TEXT("/Game") || Folder.StartsWith(TEXT("/Game/"))) || Options.AssetName.IsEmpty()
        || !FPackageName::IsValidLongPackageName(Folder / Options.AssetName, false, &PathReason)
        || !FPackageName::IsValidObjectPath((Folder / Options.AssetName) + TEXT(".") + Options.AssetName, &PathReason))
    {
        Result.Message = TEXT("请使用 /Game/ 下的 UE 保存目录和有效蓝图名称。") + PathReason.ToString(); return Result;
    }
    FBox Bounds(ForceInit);
    TSet<UPackage*> CheckedAssetPackages;
    AActor** SettingsPtr = Actors.FindByPredicate([](AActor* Actor) { return !IsHierarchyActor(Actor) && !IsEmptyMeshNode(Actor); });
    if (!SettingsPtr)
    {
        Result.Message = TEXT("所选内容只有空层级节点，没有模型或蓝图。请勾选包含全部子 Actor，或选择组内的模型。"); return Result;
    }
    AActor* SettingsActor = *SettingsPtr;
    TSet<AActor*> CollectedSet(Actors);
    // Track group parents explicitly: editor group membership is not AttachTo.
    TMap<AActor*, AActor*> GroupParents;
    for (AActor* Actor : Actors) if (AGroupActor* Group = Cast<AGroupActor>(Actor))
    {
        TArray<AActor*> Members;
        Group->GetAllChildren(Members, false);
        for (AActor* Member : Members) if (CollectedSet.Contains(Member))
        {
            if (AActor** Existing = GroupParents.Find(Member); Existing && *Existing != Group)
                Errors.Add(Label(Member) + TEXT(": 同时属于多个编辑器分组，请先修复分组关系。"));
            GroupParents.Add(Member, Group);
        }
    }
    TMap<AActor*, AActor*> LayoutParents;
    for (AActor* Actor : Actors)
    {
        AActor* Parent = Actor->GetAttachParentActor();
        TSet<AActor*> Seen;
        while (Parent && !CollectedSet.Contains(Parent) && !Seen.Contains(Parent))
        { Seen.Add(Parent); Parent = Parent->GetAttachParentActor(); }
        LayoutParents.Add(Actor, Parent && CollectedSet.Contains(Parent) ? Parent : GroupParents.FindRef(Actor));
    }
    for (AActor* Actor : Actors)
    {
        TSet<AActor*> Seen;
        for (AActor* Cursor = Actor; Cursor; Cursor = LayoutParents.FindRef(Cursor))
        {
            if (Seen.Contains(Cursor)) { Errors.Add(Label(Actor) + TEXT(": 分组与挂接关系形成循环，请先整理层级。")); break; }
            Seen.Add(Cursor);
        }
    }
    for (AActor* Actor : Actors)
    {
        if (Actor->GetWorld() != World || !Actor->GetRootComponent() || Actor->IsChildActor()
            || (!Actor->IsA<AStaticMeshActor>() && !IsBlueprintActor(Actor) && !IsHierarchyActor(Actor)))
        {
            FString ComponentDetail;
            if (Actor->GetClass() == AActor::StaticClass() || Actor->IsA<AGroupActor>())
            {
                TInlineComponentArray<UActorComponent*> Components(Actor);
                for (const UActorComponent* Component : Components)
                    if (Component && !Component->IsVisualizationComponent() && Component->GetClass() != USceneComponent::StaticClass())
                        ComponentDetail += TEXT("\n组件：") + Component->GetName() + TEXT(" / ") + Component->GetClass()->GetPathName();
            }
            Errors.Add(Label(Actor) + TEXT(": 暂不支持此对象类型或组件：") + Actor->GetClass()->GetPathName() + ComponentDetail
                + TEXT("。支持静态网格体、蓝图、原生组及仅含普通场景组件的空 Actor；蓝图内部子 Actor 请通过所属蓝图收集。")); continue;
        }
        if (Actor->GetClass()->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
            || Actor->GetRootComponent()->IsUsingAbsoluteLocation() || Actor->GetRootComponent()->IsUsingAbsoluteRotation()
            || Actor->GetRootComponent()->IsUsingAbsoluteScale() || !Actor->GetAttachParentSocketName().IsNone())
            Errors.Add(Label(Actor) + TEXT(": 类状态、根组件绝对变换或插槽挂接暂不支持可靠转换。"));
        if (Options.bReplaceActors && (FLevelUtils::IsLevelLocked(Actor) || Actor->GetLevel() != Actors[0]->GetLevel()))
            Errors.Add(Label(Actor) + TEXT(": 替换要求对象位于同一未锁定关卡。"));
        if (Options.bReplaceActors && !IsHierarchyActor(Actor) && !IsEmptyMeshNode(Actor) && (Actor->GetDataLayerInstanceNames() != SettingsActor->GetDataLayerInstanceNames()
            || Actor->GetRuntimeGrid() != SettingsActor->GetRuntimeGrid()
            || Actor->GetIsSpatiallyLoaded() != SettingsActor->GetIsSpatiallyLoaded()))
            Errors.Add(Label(Actor) + TEXT(": 待替换对象的数据层、运行时网格或空间加载设置不一致，无法合并成一个关卡实例。请取消替换。"));
        if (Actor->IsA<AStaticMeshActor>() && !CastChecked<AStaticMeshActor>(Actor)->GetStaticMeshComponent()->GetStaticMesh())
            Result.Warnings.Add(Label(Actor) + TEXT("：静态网格体为空，保留节点、变换和子级；未自动补回模型。"));
        if (UBlueprint* SourceBP = Cast<UBlueprint>(Actor->GetClass()->ClassGeneratedBy))
            if (SourceBP->Status == BS_Error || SourceBP->Status == BS_Dirty)
                Errors.Add(Label(Actor) + TEXT(": 原蓝图需要先编译成功。"));
        CheckReferences(Actor, CheckedAssetPackages, Errors);
        TArray<AActor*> Family; ActorFamily(Actor, Family);
        for (AActor* Member : Family)
        {
            TInlineComponentArray<UStaticMeshComponent*> Meshes(Member);
            for (UStaticMeshComponent* Mesh : Meshes) if (Mesh && Mesh->GetStaticMesh() && !Mesh->IsVisualizationComponent())
            {
                CheckSavedAsset(Mesh->GetStaticMesh(), Label(Actor), CheckedAssetPackages, Errors);
                for (int32 MaterialIndex = 0; MaterialIndex < Mesh->GetNumMaterials(); ++MaterialIndex)
                    CheckSavedAsset(Mesh->GetMaterial(MaterialIndex), Label(Actor), CheckedAssetPackages, Errors);
                Bounds += Mesh->Bounds.GetBox();
            }
        }
    }
    if (Options.bReplaceActors)
    {
        TSet<AActor*> Replaced;
        for (AActor* Actor : Actors) { TArray<AActor*> Family; ActorFamily(Actor, Family); Replaced.Append(Family); }
        for (TActorIterator<AActor> It(World); It; ++It) if (!Replaced.Contains(*It))
        {
            if (AGroupActor* Group = Cast<AGroupActor>(*It))
            {
                TArray<AActor*> Members;
                Group->GetAllChildren(Members, false);
                if (Members.ContainsByPredicate([&](AActor* Member) { return Replaced.Contains(Member); }) && FLevelUtils::IsLevelLocked(Group))
                    Errors.Add(Label(Group) + TEXT(": 分组位于锁定关卡，无法更新成员；请解锁关卡或取消替换。"));
            }
            if (ADatasmithSceneActor* Scene = Cast<ADatasmithSceneActor>(*It))
                for (const auto& Pair : Scene->RelatedActors)
                    if (Replaced.Contains(Pair.Value.Get()) && FLevelUtils::IsLevelLocked(Scene))
                    {
                        Errors.Add(Label(Scene) + TEXT(": Datasmith 管理对象位于锁定关卡，无法更新关联；请解锁关卡或取消替换。"));
                        break;
                    }
            auto Check = [&](UObject* Target, const FString& Path, const FString& SoftPath)
            {
                if (Replaced.Contains(OwningActor(Target)))
                    Errors.Add(Label(*It) + TEXT(": ") + Path + TEXT(" 引用了待替换对象，请先解除引用或取消替换。"));
                else for (AActor* Source : Replaced)
                    if (!SoftPath.IsEmpty() && SoftPath == FSoftObjectPath(Source).ToString())
                        Errors.Add(Label(*It) + TEXT(": ") + Path + TEXT(" 软引用了待替换对象，请先解除引用或取消替换。"));
            };
            VisitReferences(*It, Check, true);
            TInlineComponentArray<UActorComponent*> Components(*It);
            for (UActorComponent* Component : Components) if (Component && !Component->IsVisualizationComponent()) VisitReferences(Component, Check);
        }
    }
    if (!Errors.IsEmpty()) { Result.Message = FString::Join(Errors, TEXT("\n")); return Result; }
    if (Options.bEnableNanite && !EnableBuildingNanite(Actors, Result)) return Result;
    TArray<FISMGroup> ISMs;
    TSet<int32> Instanced;
    if (bUseInstances)
    {
        ISMs = PlanISM(Actors, LayoutParents, Result.Warnings);
        for (const auto& Group : ISMs) for (int32 Index : Group.Members) Instanced.Add(Index);
    }
    Result.ISMGroupCount = ISMs.Num(); Result.ISMInstanceCount = Instanced.Num();
    Result.HISMGroupCount = Result.ISMGroupCount; Result.HISMInstanceCount = Result.ISMInstanceCount;
    if (!Bounds.IsValid) for (AActor* Actor : Actors) Bounds += Actor->GetActorLocation();
    Result.Pivot = FVector(Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.Min.Z);
    Result.ActorCount = Actors.Num();
    const FTransform PivotTransform(FQuat::Identity, Result.Pivot);
    TArray<FActorState> States;
    for (AActor* Actor : Actors)
    {
        FActorState State = Capture(Actor);
        if (Actor->IsA<AGroupActor>()) State.Class = AActor::StaticClass();
        States.Add(MoveTemp(State));
    }
    const FString AssetName = UBuildingBlueprintLibrary::FindAvailableAssetName(Folder, Options.AssetName);
    if (AssetName.IsEmpty()) { Result.Message = TEXT("无法找到可用的蓝图名称。"); return Result; }
    const FString PackageName = Folder / AssetName;
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    FScopedSlowTask Progress(4.0f, LOCTEXT("Creating", "正在保存建筑蓝图…"));
    Progress.MakeDialog(); Progress.EnterProgressFrame(1);
    UPackage* Package = CreatePackage(*PackageName);
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, FName(*AssetName),
        BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    if (!Blueprint) { DiscardPackage(Package); Result.Message = TEXT("无法创建蓝图。"); return Result; }
    BBCOutliner::MarkBuilding(Blueprint);
    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    USCS_Node* Root = SCS->CreateNode(USceneComponent::StaticClass(), TEXT("BuildingRoot"));
    SCS->AddNode(Root);
    CastChecked<USceneComponent>(Root->ComponentTemplate)->SetMobility(EComponentMobility::Static);
    TMap<AActor*, USCS_Node*> Nodes;
    TArray<FName> NodeNames;
    for (int32 I = 0; I < Actors.Num(); ++I)
    {
        AActor* Actor = Actors[I];
        const FString PartName = FString::Printf(TEXT("Part_%03d_%s"), I + 1, *ObjectTools::SanitizeObjectName(Actor->GetActorLabel()));
        if (Instanced.Contains(I))
        {
            // A lightweight name/metadata anchor preserves source provenance,
            // without retaining a child actor and a second render component.
            USCS_Node* Anchor = SCS->CreateNode(USceneComponent::StaticClass(), FName(*PartName));
            auto* Scene = CastChecked<USceneComponent>(Anchor->ComponentTemplate);
            Scene->SetMobility(EComponentMobility::Static);
            CopyISMSourceMetadata(CastChecked<AStaticMeshActor>(Actor), Scene);
            Nodes.Add(Actor, Anchor); NodeNames.Add(Anchor->GetVariableName());
            continue;
        }
        USCS_Node* Node = SCS->CreateNode(UChildActorComponent::StaticClass(), FName(*PartName));
        UChildActorComponent* CAC = CastChecked<UChildActorComponent>(Node->ComponentTemplate);
        // Match native AddComponentsToBlueprint: first prepare a transient
        // child template, then deep-copy into a clean SCS template. Directly
        // instancing the map Actor here would retain map archetypes.
        UChildActorComponent* Staging = NewObject<UChildActorComponent>(GetTransientPackage(), NAME_None, RF_ArchetypeObject);
        AActor* CopySource = Actor->IsA<AGroupActor>() ? GroupAsRuntimeTemplate(Actor) : Actor;
        Staging->SetChildActorClass(CopySource->GetClass(), CopySource);
        FString DatasmithError;
        if (!Staging->GetChildActorTemplate() || !MakeDatasmithDataIndependent(Staging->GetChildActorTemplate(), DatasmithError))
        {
            DiscardPackage(Package); Result.Message = Label(Actor) + TEXT(": ") + DatasmithError; return Result;
        }
        UEngine::FCopyPropertiesForUnrelatedObjectsParams CopyParams;
        CopyParams.bDoDelta = false;
        UEngine::CopyPropertiesForUnrelatedObjects(Staging, CAC, CopyParams);
        CAC->CreationMethod = EComponentCreationMethod::Native;
        CAC->SetMobility(Actor->IsA<AGroupActor>() ? EComponentMobility::Static : Actor->GetRootComponent()->Mobility.GetValue());
        AActor* Template = CAC->GetChildActorTemplate();
        if (!Template || !Template->GetRootComponent())
        {
            DiscardPackage(Package); Result.Message = Label(Actor) + TEXT(": 无法创建子 Actor 模板。"); return Result;
        }
        if (!MakeDatasmithDataIndependent(Template, DatasmithError))
        {
            DiscardPackage(Package); Result.Message = Label(Actor) + TEXT(": ") + DatasmithError; return Result;
        }
        // Match UE's native ChildActor conversion: the CAC owns the placement;
        // the actor template itself must start at identity.
        Template->GetRootComponent()->SetRelativeLocation_Direct(FVector::ZeroVector);
        Template->GetRootComponent()->SetRelativeRotation_Direct(FRotator::ZeroRotator);
        Template->GetRootComponent()->SetRelativeScale3D_Direct(FVector::OneVector);
        // Attachment links crossing Actor boundaries belong to the level;
        // the new SCS hierarchy represents them instead. Mutate copied arrays
        // directly so that no detach notification changes source level actors.
        TInlineComponentArray<USceneComponent*> TemplateComponents(Template);
        FArrayProperty* ChildrenProperty = FindFProperty<FArrayProperty>(USceneComponent::StaticClass(), TEXT("AttachChildren"));
        for (USceneComponent* Component : TemplateComponents)
        {
            if (Component->GetAttachParent() && !Component->GetAttachParent()->IsIn(Template))
            {
                Component->SetupAttachment(nullptr);
            }
            if (ChildrenProperty)
            {
                FScriptArrayHelper Children(ChildrenProperty, ChildrenProperty->ContainerPtrToValuePtr<void>(Component));
                FObjectPropertyBase* ChildProperty = CastFieldChecked<FObjectPropertyBase>(ChildrenProperty->Inner);
                for (int32 ChildIndex = Children.Num() - 1; ChildIndex >= 0; --ChildIndex)
                {
                    UObject* Child = ChildProperty->GetObjectPropertyValue(Children.GetRawPtr(ChildIndex));
                    if (Child && !Child->IsIn(Template)) Children.RemoveValues(ChildIndex);
                }
            }
        }
        Template->ClearActorLabel();
        Nodes.Add(Actor, Node); NodeNames.Add(Node->GetVariableName());
    }
    for (AActor* Actor : Actors)
    {
        AActor* Parent = LayoutParents.FindRef(Actor);
        USCS_Node* Node = Nodes.FindChecked(Actor);
        (Parent ? Nodes.FindChecked(Parent) : Root)->AddChildNode(Node);
        // Reusing the original local transform avoids an extra matrix
        // decomposition for mirrored/nonuniformly-scaled attachment chains.
        // A world-to-parent roundtrip is not generally lossless in that case.
        const bool bDirectParentRoot = Parent && Actor->GetRootComponent()->GetAttachParent() == Parent->GetRootComponent();
        const FTransform Relative = bDirectParentRoot ? Actor->GetRootComponent()->GetRelativeTransform()
            : Actor->GetActorTransform().GetRelativeTransform(Parent ? Parent->GetActorTransform() : PivotTransform);
        CastChecked<USceneComponent>(Node->ComponentTemplate)->SetRelativeTransform_Direct(Relative);
    }
    for (int32 I = 0; I < ISMs.Num(); ++I)
    {
        FISMGroup& Group = ISMs[I];
        USCS_Node* Node = SCS->CreateNode(UInstancedStaticMeshComponent::StaticClass(),
            FName(*FString::Printf(TEXT("ISM_%03d"), I + 1)));
        auto* ISM = CastChecked<UInstancedStaticMeshComponent>(Node->ComponentTemplate);
        FString Error;
        if (!InitializeISM(ISM, CastChecked<AStaticMeshActor>(Actors[Group.Members[0]]), Error))
        { DiscardPackage(Package); Result.Message = TEXT("无法创建 ISM：") + Error; return Result; }
        (Group.Parent ? Nodes.FindChecked(Group.Parent) : Root)->AddChildNode(Node);
        Group.NodeName = Node->GetVariableName();
        for (int32 InstanceIndex = 0; InstanceIndex < Group.Members.Num(); ++InstanceIndex)
        {
            const int32 Index = Group.Members[InstanceIndex];
            AActor* Actor = Actors[Index];
            const bool bDirect = Group.Parent && Actor->GetRootComponent()->GetAttachParent() == Group.Parent->GetRootComponent();
            const FTransform Local = bDirect ? Actor->GetRootComponent()->GetRelativeTransform()
                : Actor->GetActorTransform().GetRelativeTransform(Group.Parent ? Group.Parent->GetActorTransform() : PivotTransform);
            ISM->AddInstance(Local);
            CastChecked<USceneComponent>(Nodes.FindChecked(Actor)->ComponentTemplate)->ComponentTags.Add(
                FName(*FString::Printf(TEXT("BBC.ISM=%s;Instance=%d"), *Group.NodeName.ToString(), InstanceIndex)));
        }
    }
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    if (Blueprint->Status == BS_Error || !Blueprint->GeneratedClass)
    {
        DiscardPackage(Package); Result.Message = TEXT("生成的蓝图编译失败，原建筑已保留。"); return Result;
    }
    Progress.EnterProgressFrame(1, LOCTEXT("Checking", "正在检查模型、材质、变换和蓝图参数…"));
    auto Validate = [&](AActor* Building, FString& Error)
    {
        if (!Building) { Error = TEXT("无法生成建筑验证实例。"); return false; }
        for (int32 I = 0; I < Actors.Num(); ++I)
        {
            if (Instanced.Contains(I))
            {
                auto* Anchor = FindObject<USceneComponent>(Building, *NodeNames[I].ToString());
                if (!Anchor || !SameTransform(Actors[I]->GetActorTransform(), Anchor->GetComponentTransform()))
                { Error = Label(Actors[I]) + TEXT("：ISM 来源节点变换未保留。"); return false; }
                continue;
            }
            UChildActorComponent* Found = FindObject<UChildActorComponent>(Building, *NodeNames[I].ToString());
            if (!Found || !Found->GetChildActor()) { Error = Label(Actors[I]) + TEXT(": 生成后找不到子对象。"); return false; }
            if (!Compare(States[I], Capture(Found->GetChildActor()), Label(Actors[I]), Error))
            {
                Error += TEXT("\n源相对变换：") + Actors[I]->GetRootComponent()->GetRelativeTransform().ToString()
                    + TEXT("\n子组件相对变换：") + Found->GetRelativeTransform().ToString();
                return false;
            }
        }
        return ValidateISM(Building, Actors, ISMs, Error);
    };
    {
        FPreviewScene Preview(FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(false).SetTransactional(false));
        FActorSpawnParameters Spawn; Spawn.ObjectFlags = RF_Transient; Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AActor* PreviewActor = Preview.GetWorld()->SpawnActor<AActor>(Blueprint->GeneratedClass, PivotTransform, Spawn);
        FString Error;
        if (!Validate(PreviewActor, Error))
        {
            DiscardPackage(Package); Result.Message = Error + TEXT("\n已停止创建，原建筑未替换。"); return Result;
        }
    }
    Progress.EnterProgressFrame(1, LOCTEXT("Saving", "正在保存蓝图资产…"));
    if (IFileManager::Get().FileExists(*Filename))
    {
        DiscardPackage(Package); Result.Message = TEXT("保存目标已被其他操作创建，请重新创建以选择新名称。"); return Result;
    }
    BBCThumbnails::CacheForSave(Blueprint);
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone; SaveArgs.SaveFlags = SAVE_NoError;
    FSaveErrors SaveErrors;
    SaveArgs.Error = &SaveErrors;
    if (!UPackage::SavePackage(Package, Blueprint, *Filename, SaveArgs))
    {
        if (IFileManager::Get().FileExists(*Filename)) IFileManager::Get().Delete(*Filename);
        DiscardPackage(Package);
        Result.Message = TEXT("蓝图保存失败，原建筑已保留。\n")
            + (SaveErrors.Lines.IsEmpty() ? TEXT("请查看输出日志中的 LogSavePackage 记录，检查资源引用及文件写入情况。")
                : FString::Join(SaveErrors.Lines, TEXT("\n")));
        return Result;
    }
    Progress.EnterProgressFrame(1);
    if (Options.bReplaceActors)
    {
        bool bFailed = false;
        FString Failure;
        UPackage* LevelPackage = Actors[0]->GetLevel()->GetOutermost();
        const bool bWasDirty = LevelPackage->IsDirty();
        {
            FScopedTransaction Transaction(LOCTEXT("ReplaceTransaction", "将建筑替换为蓝图 Actor"));
            World->Modify();
            Actors[0]->GetLevel()->Modify();
            GEditor->GetSelectedActors()->Modify();
            FActorSpawnParameters Spawn; Spawn.OverrideLevel = Actors[0]->GetLevel(); Spawn.ObjectFlags = RF_Transactional;
            Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            AActor* NewActor = World->SpawnActor<AActor>(Blueprint->GeneratedClass, PivotTransform, Spawn);
            if (!Validate(NewActor, Failure)) bFailed = true;
            if (!bFailed)
            {
                NewActor->SetActorLabel(AssetName);
                NewActor->SetFolderPath(Actors[0]->GetFolderPath());
                NewActor->SetRuntimeGrid(SettingsActor->GetRuntimeGrid());
                NewActor->SetIsSpatiallyLoaded(SettingsActor->GetIsSpatiallyLoaded());
                if (FProperty* DataLayers = FindFProperty<FProperty>(AActor::StaticClass(), AActor::GetDataLayerAssetsPropertyName()))
                    DataLayers->CopyCompleteValue_InContainer(NewActor, SettingsActor);
                // Preserve any common external parent of the selected hierarchy.
                AActor* Parent = Actors[0]->GetAttachParentActor();
                while (Parent && Actors.Contains(Parent)) Parent = Parent->GetAttachParentActor();
                if (Parent && Actors.ContainsByPredicate([&](AActor* A) { return A != Parent && !A->IsAttachedTo(Parent); })) Parent = nullptr;
                if (Parent) NewActor->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
                TSet<AActor*> Collected(Actors);
                for (AActor* Source : Actors)
                {
                    TArray<AActor*> Family; ActorFamily(Source, Family);
                    for (AActor* Member : Family)
                    {
                        TArray<AActor*> Attached; Member->GetAttachedActors(Attached);
                        for (AActor* Child : Attached) if (IsValid(Child) && !Collected.Contains(Child) && !Child->IsChildActor())
                        {
                            Child->Modify(); Child->GetRootComponent()->Modify();
                            if (!Child->AttachToActor(NewActor, FAttachmentTransformRules::KeepWorldTransform))
                            { bFailed = true; Failure = Label(Child) + TEXT(": 无法重新挂接未收集的子 Actor。"); break; }
                        }
                    }
                    if (bFailed) break;
                }
                if (!bFailed)
                {
                    // Preserve the keys with empty values, like deleting imported
                    // actors in the editor. Rebinding many source IDs to one new
                    // Actor would corrupt future reimport matching. Modify before
                    // changing the map so Undo/Redo restores the exact associations.
                    TSet<AActor*> Replaced;
                    for (AActor* Source : Actors) { TArray<AActor*> Family; ActorFamily(Source, Family); Replaced.Append(Family); }
                    // If all converted actors belong to the same unselected
                    // direct group, keep the replacement in that outer group.
                    AGroupActor* CommonOuterGroup = nullptr;
                    bool bCommonGroup = true;
                    for (AActor* Source : Actors)
                    {
                        AGroupActor* Group = Cast<AGroupActor>(Source->GroupActor);
                        if (!Group || Replaced.Contains(Group)) { bCommonGroup = false; break; }
                        if (!CommonOuterGroup) CommonOuterGroup = Group;
                        else if (CommonOuterGroup != Group) { bCommonGroup = false; break; }
                    }
                    for (TActorIterator<ADatasmithSceneActor> It(World); It; ++It)
                    {
                        bool bModified = false;
                        for (auto& Pair : It->RelatedActors)
                        {
                            if (!Replaced.Contains(Pair.Value.Get())) continue;
                            if (!bModified) { It->Modify(); bModified = true; }
                            Pair.Value.Reset();
                        }
                    }
                    // Remove editor membership references before deleting actors.
                    // Do not use Group.Remove: its automatic group deletion can
                    // affect unrelated groups while an operation is in progress.
                    // Keep unselected members and restore everything through Undo.
                    for (TActorIterator<AGroupActor> It(World); It; ++It)
                    {
                        bool bTouches = Replaced.Contains(*It);
                        for (AActor* Member : It->GroupActors) bTouches |= Replaced.Contains(Member);
                        for (AGroupActor* Group : It->SubGroups) bTouches |= Replaced.Contains(Group);
                        if (!bTouches) continue;
                        It->Modify();
                        It->GroupActors.RemoveAll([&](AActor* Member) { return Replaced.Contains(Member); });
                        It->SubGroups.RemoveAll([&](AGroupActor* Group) { return Replaced.Contains(Group); });
                        It->PostEditChange();
                    }
                    for (AActor* Source : Actors)
                    {
                        Source->Modify();
                        Source->GroupActor = nullptr;
                    }
                    if (bCommonGroup && CommonOuterGroup)
                    {
                        CommonOuterGroup->Add(*NewActor);
                        CommonOuterGroup->PostEditChange();
                    }
                    GEditor->SelectNone(false, true, false);
                    for (AActor* Source : Actors)
                    {
                        Source->Modify();
                        if (!World->EditorDestroyActor(Source, true)) { bFailed = true; Failure = TEXT("原 Actor 无法删除。"); break; }
                    }
                    if (!bFailed)
                    {
                        Result.ReplacementActor = NewActor;
                        GEditor->SelectActor(NewActor, true, true); LevelPackage->MarkPackageDirty();
                    }
                }
            }
        }
        if (bFailed)
        {
            GEditor->UndoTransaction();
            LevelPackage->SetDirtyFlag(bWasDirty);
            IFileManager::Get().Delete(*Filename);
            DiscardPackage(Package);
            Result.Message = Failure + TEXT("\n替换未完成，已撤销本次关卡操作。"); return Result;
        }
    }
    FAssetRegistryModule::AssetCreated(Blueprint);
    Result.bSuccess = true;
    Result.AssetObjectPath = Blueprint->GetPathName();
    Result.Message = FString::Printf(TEXT("已保存 %s，共 %d 个 Actor。%s"), *Result.AssetObjectPath, Actors.Num(),
        Options.bReplaceActors ? TEXT("原建筑已替换，可撤销；请检查后按 Ctrl+S 保存关卡。") : TEXT("原建筑已保留。"));
    if (bUseInstances) Result.Message += FString::Printf(TEXT("\nISM：%d 个组件，%d 个实例；其他零件按原方式保留。"), Result.ISMGroupCount, Result.ISMInstanceCount);
    if (!Result.Warnings.IsEmpty()) Result.Message += TEXT("\n\n提示：\n") + FString::Join(Result.Warnings, TEXT("\n"));
    return Result;
}

FBuildingBlueprintResult UBuildingBlueprintLibrary::CreateBuildingBlueprint(const TArray<AActor*>& Input, const FBuildingBlueprintOptions& Options)
{
    FBuildingBlueprintResult Result = CreateBuildingBlueprintInternal(Input, Options);
    if (Options.bEnableNanite && Result.bNaniteProcessed)
    {
        Result.Message += FString::Printf(TEXT("\nNanite 原模型资产：本次启用并保存 %d 个，原本开启 %d 个。"),
            Result.NaniteEnabledCount, Result.NaniteAlreadyEnabledCount);
        if (Result.NaniteEnabledCount)
            Result.Message += TEXT("\nNanite 修改影响模型的所有引用，有独立撤销步骤；撤销后需保存模型。建筑创建/替换失败不会自动撤销已成功保存的模型设置。");
    }
    return Result;
}

#include "BuildingAssetReplacement.inl"
#include "BuildingAssetOptimizer.inl"

#undef LOCTEXT_NAMESPACE
