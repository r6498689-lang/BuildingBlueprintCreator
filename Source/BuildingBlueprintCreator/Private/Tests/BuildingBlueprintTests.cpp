#if WITH_DEV_AUTOMATION_TESTS
#include "BuildingBlueprintLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ChildActorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/PointLight.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMeshActor.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

namespace BBCTest
{
static AStaticMeshActor* Mesh(UWorld* World, const FVector& Location, const FVector& Scale, float Yaw = 0)
{
    AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator(0, Yaw, 0));
    Actor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
    Actor->SetActorScale3D(Scale);
    return Actor;
}

static UChildActorComponent* Part(AActor* Building, int32 Index)
{
    TInlineComponentArray<UChildActorComponent*> Components(Building);
    const FString Prefix = FString::Printf(TEXT("Part_%03d_"), Index + 1);
    for (UChildActorComponent* Component : Components) if (Component->GetName().StartsWith(Prefix)) return Component;
    return nullptr;
}

static bool Save(UBlueprint* BP)
{
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
    const FString Filename = FPackageName::LongPackageNameToFilename(BP->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
    FAssetRegistryModule::AssetCreated(BP);
    return UPackage::SavePackage(BP->GetOutermost(), BP, *Filename, Args);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCBuildingCreateTest, "BuildingBlueprintCreator.CreateAndValidate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBBCBuildingCreateTest::RunTest(const FString& Parameters)
{
    using namespace BBCTest;
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    const FString Folder = TEXT("/Game/BBCValidation_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
    FBuildingBlueprintOptions Options; Options.FolderPath = Folder;
    AStaticMeshActor* A = Mesh(World, FVector(1250,400,300), FVector(-2,1,0.5), 35);
    AStaticMeshActor* B = Mesh(World, FVector(1550,420,350), FVector(1,2,1), -10);
    AStaticMeshActor* C = Mesh(World, FVector(1550,600,450), FVector(0.5), 20);
    B->AttachToActor(A, FAttachmentTransformRules::KeepWorldTransform);
    C->AttachToActor(B, FAttachmentTransformRules::KeepWorldTransform);
    TestEqual(TEXT("Selection only excludes attached descendants"), UBuildingBlueprintLibrary::CollectActors({A}, false).Num(), 1);
    TestEqual(TEXT("Include collects grandchildren too"), UBuildingBlueprintLibrary::CollectActors({A}, true).Num(), 3);

    UPackage* FixturePackage = CreatePackage(*(Folder / TEXT("BP_SourceDoor")));
    UBlueprint* Fixture = FKismetEditorUtilities::CreateBlueprint(AStaticMeshActor::StaticClass(), FixturePackage, TEXT("BP_SourceDoor"),
        BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    FEdGraphPinType NumberType; NumberType.PinCategory = UEdGraphSchema_K2::PC_Real; NumberType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
    FBlueprintEditorUtils::AddMemberVariable(Fixture, TEXT("DoorAngle"), NumberType);
    FEdGraphPinType RefType; RefType.PinCategory = UEdGraphSchema_K2::PC_Object; RefType.PinSubCategoryObject = AActor::StaticClass();
    FBlueprintEditorUtils::AddMemberVariable(Fixture, TEXT("ExternalTarget"), RefType);
    FKismetEditorUtilities::CompileBlueprint(Fixture);
    CastChecked<AStaticMeshActor>(Fixture->GeneratedClass->GetDefaultObject())->GetStaticMeshComponent()->SetStaticMesh(
        LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
    TestTrue(TEXT("Fixture source BP saves"), Save(Fixture));
    AActor* Door = World->SpawnActor<AActor>(Fixture->GeneratedClass, FVector(1300,600,350), FRotator(0,20,0));
    UStaticMeshComponent* DoorMesh = Door->FindComponentByClass<UStaticMeshComponent>();
    DoorMesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
    DoorMesh->SetMaterial(0, LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial")));
    FDoubleProperty* Angle = FindFProperty<FDoubleProperty>(Fixture->GeneratedClass, TEXT("DoorAngle"));
    FObjectPropertyBase* Reference = FindFProperty<FObjectPropertyBase>(Fixture->GeneratedClass, TEXT("ExternalTarget"));
    if (!TestNotNull(TEXT("Fixture BP variable"), Angle) || !TestNotNull(TEXT("Fixture actor reference variable"), Reference)) return false;
    Angle->SetPropertyValue_InContainer(Door, 57.25);

    Reference->SetObjectPropertyValue_InContainer(Door, B);
    Options.AssetName = TEXT("RejectedReference");
    FBuildingBlueprintResult Bad = UBuildingBlueprintLibrary::CreateBuildingBlueprint({A,Door}, Options);
    TestFalse(TEXT("Cross actor references stop whole operation"), Bad.bSuccess);
    TestTrue(TEXT("Reference error identifies parameter"), Bad.Message.Contains(TEXT("ExternalTarget")));
    TestFalse(TEXT("Rejected asset not saved"), FPackageName::DoesPackageExist(Folder / Options.AssetName));
    Reference->SetObjectPropertyValue_InContainer(Door, nullptr);
    APointLight* Light = World->SpawnActor<APointLight>();
    Options.AssetName = TEXT("RejectedLight");
    TestFalse(TEXT("Unsupported actor stops, not skipped"), UBuildingBlueprintLibrary::CreateBuildingBlueprint({A,Light}, Options).bSuccess);
    TestFalse(TEXT("Stale input stops"), UBuildingBlueprintLibrary::CreateBuildingBlueprint({A,nullptr}, Options).bSuccess);
    World->EditorDestroyActor(Light, false);

    Options.AssetName = TEXT("RejectedUnsavedSource");
    FixturePackage->SetDirtyFlag(true);
    FBuildingBlueprintResult Unsaved = UBuildingBlueprintLibrary::CreateBuildingBlueprint({Door}, Options);
    TestFalse(TEXT("Unsaved source BP is rejected before creating a dangling asset"), Unsaved.bSuccess);
    TestTrue(TEXT("Unsaved source error explains saving"), Unsaved.Message.Contains(TEXT("请先保存")));
    TestFalse(TEXT("Unsaved dependency leaves no building file"), FPackageName::DoesPackageExist(Folder / Options.AssetName));
    FixturePackage->SetDirtyFlag(false);

    Options.AssetName = TEXT("BP_Building");
    World->GetCurrentLevel()->GetOutermost()->SetDirtyFlag(false);
    FBuildingBlueprintResult Created = UBuildingBlueprintLibrary::CreateBuildingBlueprint({A,B,Door}, Options);
    if (!TestTrue(TEXT("Mixed static meshes and nested BP create: ") + Created.Message, Created.bSuccess)) return false;
    TestTrue(TEXT("Original actors retained"), IsValid(A) && IsValid(B) && IsValid(Door));
    TestFalse(TEXT("Create-only leaves original level cleanliness"), World->GetCurrentLevel()->GetOutermost()->IsDirty());
    TestEqual(TEXT("Only explicit actors included"), Created.ActorCount, 3);
    FBox Box = A->GetComponentsBoundingBox(true) + B->GetComponentsBoundingBox(true) + Door->GetComponentsBoundingBox(true);
    TestTrue(TEXT("Pivot is world bounds bottom center"), Created.Pivot.Equals(FVector(Box.GetCenter().X,Box.GetCenter().Y,Box.Min.Z),0.01));
    UBlueprint* Building = LoadObject<UBlueprint>(nullptr, *Created.AssetObjectPath);
    if (!TestNotNull(TEXT("Saved building asset"), Building)) return false;
    AActor* Instance = World->SpawnActor<AActor>(Building->GeneratedClass, Created.Pivot, FRotator::ZeroRotator);
    UChildActorComponent* Nested = Part(Instance,2);
    if (!TestNotNull(TEXT("Nested BP exists"), Nested)) return false;
    TestEqual(TEXT("Nested BP keeps original behavior class"), Nested->GetChildActor()->GetClass(), Fixture->GeneratedClass.Get());
    TestEqual(TEXT("Nested BP instance variable preserved"), Angle->GetPropertyValue_InContainer(Nested->GetChildActor()), 57.25);
    TestTrue(TEXT("Negative scale and rotation preserved"), Part(Instance,0)->GetChildActor()->GetTransform().Equals(A->GetActorTransform(), 0.001));
    TestEqual(TEXT("Original material asset referenced"), Nested->GetChildActor()->FindComponentByClass<UStaticMeshComponent>()->GetMaterial(0), DoorMesh->GetMaterial(0));
    World->EditorDestroyActor(Instance, false);
    FBuildingBlueprintResult Duplicate = UBuildingBlueprintLibrary::CreateBuildingBlueprint({A,B,Door}, Options);
    TestTrue(TEXT("Duplicate succeeds"), Duplicate.bSuccess);
    TestTrue(TEXT("Duplicate uses _1 suffix"), Duplicate.AssetObjectPath.EndsWith(TEXT("BP_Building_1.BP_Building_1")));

    Options.AssetName = TEXT("BP_AllDescendants"); Options.bIncludeAllAttachedChildren = true;
    FBuildingBlueprintResult Included = UBuildingBlueprintLibrary::CreateBuildingBlueprint({A}, Options);
    TestTrue(TEXT("Recursive descendants asset saves: ") + Included.Message, Included.bSuccess);
    TestEqual(TEXT("Grandchild collected"), Included.ActorCount, 3);
    Options.bIncludeAllAttachedChildren = false;
    Options.bReplaceActors = true; Options.AssetName = TEXT("BP_IncomingReference");
    Reference->SetObjectPropertyValue_InContainer(Door, A);
    TestFalse(TEXT("Incoming actor reference blocks replacement"), UBuildingBlueprintLibrary::CreateBuildingBlueprint({A}, Options).bSuccess);
    Reference->SetObjectPropertyValue_InContainer(Door, nullptr);
    Options.AssetName = TEXT("BP_Replaced");
    const FTransform BBefore = B->GetTransform();
    const FTransform ABefore = A->GetTransform();
    World->GetCurrentLevel()->GetOutermost()->SetDirtyFlag(false);
    FBuildingBlueprintResult Replaced = UBuildingBlueprintLibrary::CreateBuildingBlueprint({A}, Options);
    if (!TestTrue(TEXT("Replacement succeeds: ") + Replaced.Message, Replaced.bSuccess)) return false;
    TestTrue(TEXT("Excluded child remains in original place"), B->GetTransform().Equals(BBefore, 0.001));
    TestEqual(TEXT("Excluded child reattached to replacement"), B->GetAttachParentActor(), Replaced.ReplacementActor.Get());
    TestEqual(TEXT("Excluded grandchild parent unchanged"), C->GetAttachParentActor(), static_cast<AActor*>(B));
    TestTrue(TEXT("Changed level stays unsaved"), World->GetCurrentLevel()->GetOutermost()->IsDirty());
    TestTrue(TEXT("Replacement is undoable"), GEditor->UndoTransaction());
    TestEqual(TEXT("Undo restores external child attachment"), B->GetAttachParentActor(), static_cast<AActor*>(A));
    TestTrue(TEXT("Undo restores original transform"), A->GetTransform().Equals(ABefore,0.001));
    TestTrue(TEXT("Replacement is redoable"), GEditor->RedoTransaction());

    // Real AActor-based BP with SCS-created components and a BP inside a BP.
    UPackage* SCSPackage = CreatePackage(*(Folder / TEXT("BP_SourceFacade")));
    UBlueprint* SCSFixture = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), SCSPackage, TEXT("BP_SourceFacade"),
        BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    USCS_Node* FacadeRoot = SCSFixture->SimpleConstructionScript->CreateNode(USceneComponent::StaticClass(), TEXT("FacadeRoot"));
    SCSFixture->SimpleConstructionScript->AddNode(FacadeRoot);
    CastChecked<USceneComponent>(FacadeRoot->ComponentTemplate)->SetMobility(EComponentMobility::Static);
    USCS_Node* FacadeMesh = SCSFixture->SimpleConstructionScript->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("FacadeMesh"));
    FacadeRoot->AddChildNode(FacadeMesh);
    CastChecked<UStaticMeshComponent>(FacadeMesh->ComponentTemplate)->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
    CastChecked<USceneComponent>(FacadeMesh->ComponentTemplate)->SetMobility(EComponentMobility::Static);
    USCS_Node* FacadeDoor = SCSFixture->SimpleConstructionScript->CreateNode(UChildActorComponent::StaticClass(), TEXT("InnerDoor"));
    FacadeRoot->AddChildNode(FacadeDoor);
    UChildActorComponent* DoorTemplate = CastChecked<UChildActorComponent>(FacadeDoor->ComponentTemplate);
    DoorTemplate->SetChildActorClass(Fixture->GeneratedClass.Get());
    DoorTemplate->SetMobility(EComponentMobility::Static);
    Angle->SetPropertyValue_InContainer(DoorTemplate->GetChildActorTemplate(), 14.5);
    FKismetEditorUtilities::CompileBlueprint(SCSFixture);
    TestTrue(TEXT("SCS fixture source asset saves"), Save(SCSFixture));
    AActor* Facade = World->SpawnActor<AActor>(SCSFixture->GeneratedClass, FVector(350,450,200), FRotator(0,15,0));
    TestEqual(TEXT("Intrinsic nested BP not collected twice"), UBuildingBlueprintLibrary::CollectActors({Facade}, true).Num(),1);
    Options.bReplaceActors = false; Options.AssetName = TEXT("BP_SCSBuilding");
    FBuildingBlueprintResult SCSCreated = UBuildingBlueprintLibrary::CreateBuildingBlueprint({Facade}, Options);
    if (!TestTrue(TEXT("SCS components and nested BP preserve correctly: ") + SCSCreated.Message, SCSCreated.bSuccess)) return false;

    const FString Manifest = Created.AssetObjectPath + TEXT("\n") + Fixture->GetPathName() + TEXT("\n") + Created.Pivot.ToString() + TEXT("\n") + ABefore.ToString()
        + TEXT("\n") + SCSCreated.AssetObjectPath + TEXT("\n") + SCSFixture->GetPathName();
    const FString Dir = FPaths::ProjectSavedDir() / TEXT("BuildingBlueprintCreator");
    IFileManager::Get().MakeDirectory(*Dir,true);
    FFileHelper::SaveStringToFile(Manifest, *(Dir / TEXT("roundtrip.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    AddInfo(TEXT("Roundtrip manifest: ") + Dir / TEXT("roundtrip.txt"));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCBuildingReloadTest, "BuildingBlueprintCreator.ReloadSavedAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBBCBuildingReloadTest::RunTest(const FString& Parameters)
{
    using namespace BBCTest;
    TArray<FString> Lines;
    FFileHelper::LoadFileToStringArray(Lines, *(FPaths::ProjectSavedDir() / TEXT("BuildingBlueprintCreator/roundtrip.txt")));
    if (!TestEqual(TEXT("Roundtrip manifest available"), Lines.Num(),6)) return false;
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    UBlueprint* Building = LoadObject<UBlueprint>(nullptr,*Lines[0]);
    UBlueprint* Fixture = LoadObject<UBlueprint>(nullptr,*Lines[1]);
    if (!TestNotNull(TEXT("Reloaded building"),Building) || !TestNotNull(TEXT("Reloaded source BP dependency"),Fixture)) return false;
    FVector Pivot; Pivot.InitFromString(Lines[2]); FTransform Original; Original.InitFromString(Lines[3]);
    for (int32 I=0; I<2; ++I)
    {
        const FVector Offset(1000*I,0,0);
        AActor* Instance = World->SpawnActor<AActor>(Building->GeneratedClass,Pivot+Offset,FRotator::ZeroRotator);
        UChildActorComponent* Plain = Part(Instance,0);
        UChildActorComponent* Nested = Part(Instance,2);
        if (!TestNotNull(TEXT("Reloaded plain mesh"),Plain) || !TestNotNull(TEXT("Reloaded nested BP"),Nested)) return false;
        FTransform Expected=Original; Expected.AddToTranslation(Offset);
        TestTrue(TEXT("Saved asset keeps mesh placement on repeated use"),Plain->GetChildActor()->GetTransform().Equals(Expected,0.001));
        TestEqual(TEXT("Original BP class survives disk reload"),Nested->GetChildActor()->GetClass(),Fixture->GeneratedClass.Get());
        TestEqual(TEXT("Instance parameter survives disk reload"),FindFProperty<FDoubleProperty>(Fixture->GeneratedClass,TEXT("DoorAngle"))->GetPropertyValue_InContainer(Nested->GetChildActor()),57.25);
        TestNotNull(TEXT("Material survives disk reload"),Nested->GetChildActor()->FindComponentByClass<UStaticMeshComponent>()->GetMaterial(0));
    }
    UBlueprint* SCSBuilding = LoadObject<UBlueprint>(nullptr,*Lines[4]);
    UBlueprint* SourceFacade = LoadObject<UBlueprint>(nullptr,*Lines[5]);
    if (!TestNotNull(TEXT("SCS building reloads"),SCSBuilding) || !TestNotNull(TEXT("Source facade dependency reloads"),SourceFacade)) return false;
    AActor* SCSInstance = World->SpawnActor<AActor>(SCSBuilding->GeneratedClass);
    UChildActorComponent* FacadePart = Part(SCSInstance,0);
    if (!TestNotNull(TEXT("SCS building contains facade"),FacadePart)) return false;
    AActor* Facade = FacadePart->GetChildActor();
    TestEqual(TEXT("Facade keeps original BP class"),Facade->GetClass(),SourceFacade->GeneratedClass.Get());
    UChildActorComponent* InnerDoor = Facade->FindComponentByClass<UChildActorComponent>();
    if (!TestNotNull(TEXT("Deeply nested door restored after reload"),InnerDoor) || !TestNotNull(TEXT("Deep child exists"),InnerDoor->GetChildActor())) return false;
    TestEqual(TEXT("Deep nested parameter survives reload"),FindFProperty<FDoubleProperty>(Fixture->GeneratedClass,TEXT("DoorAngle"))->GetPropertyValue_InContainer(InnerDoor->GetChildActor()),14.5);
    return !HasAnyErrors();
}
#endif
