#if WITH_DEV_AUTOMATION_TESTS
#include "BuildingBlueprintLibrary.h"
#include "Components/ChildActorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DatasmithAssetUserData.h"
#include "DatasmithSceneActor.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/StaticMeshActor.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTemplates/DatasmithActorTemplate.h"
#include "ObjectTemplates/DatasmithSceneComponentTemplate.h"
#include "ObjectTemplates/DatasmithStaticMeshComponentTemplate.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/UnrealType.h"

namespace BBCDatasmithTest
{
static AStaticMeshActor* Mesh(UWorld* World, const FVector& Location)
{
    AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator(0,25,0));
    Actor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
    Actor->GetStaticMeshComponent()->SetMaterial(0, LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial")));
    Actor->SetActorScale3D(FVector(-1.5,2,0.75));
    return Actor;
}

static UDatasmithAssetUserData* AddImportData(AStaticMeshActor* Actor, const FString& Id)
{
    UDatasmithAssetUserData* Data = FDatasmithObjectTemplateUtils::FindOrCreateDatasmithUserData(Actor);
    Data->MetaData.Add(UDatasmithAssetUserData::UniqueIdMetaDataKey, Id);
    Data->MetaData.Add(TEXT("Building"), TEXT("CoastalHotel"));
    Data->MetaData.Add(TEXT("材质说明"), TEXT("外墙"));
    UDatasmithActorTemplate* ActorTemplate = NewObject<UDatasmithActorTemplate>();
    ActorTemplate->Load(Actor);
    FDatasmithObjectTemplateUtils::SetObjectTemplate(Actor, ActorTemplate);
    UDatasmithSceneComponentTemplate* SceneTemplate = NewObject<UDatasmithSceneComponentTemplate>();
    SceneTemplate->Load(Actor->GetRootComponent());
    FDatasmithObjectTemplateUtils::SetObjectTemplate(Actor->GetRootComponent(), SceneTemplate);
    UDatasmithStaticMeshComponentTemplate* MeshTemplate = NewObject<UDatasmithStaticMeshComponentTemplate>();
    MeshTemplate->Load(Actor->GetStaticMeshComponent());
    FDatasmithObjectTemplateUtils::SetObjectTemplate(Actor->GetStaticMeshComponent(), MeshTemplate);
    return Data;
}

static TArray<AActor*> Parts(AActor* Building)
{
    TArray<AActor*> Out;
    TInlineComponentArray<UChildActorComponent*> Components(Building);
    Components.Sort([](const UChildActorComponent& A, const UChildActorComponent& B) { return A.GetName() < B.GetName(); });
    for (UChildActorComponent* Component : Components) if (Component->GetChildActor()) Out.Add(Component->GetChildActor());
    return Out;
}

static void CheckData(FAutomationTestBase& Test, AActor* Building)
{
    TArray<AActor*> Children = Parts(Building);
    Test.TestEqual(TEXT("Both imported mesh actors preserved"), Children.Num(), 2);
    for (AActor* Child : Children)
    {
        UDatasmithAssetUserData* Data = UDatasmithAssetUserData::GetDatasmithUserData(Child);
        if (!Test.TestNotNull(TEXT("Metadata preserved"), Data)) continue;
        Test.TestEqual(TEXT("Ordinary metadata preserved"), Data->MetaData.FindRef(TEXT("Building")), FString(TEXT("CoastalHotel")));
        Test.TestEqual(TEXT("Unicode metadata preserved"), Data->MetaData.FindRef(TEXT("材质说明")), FString(TEXT("外墙")));
        Test.TestFalse(TEXT("Original importer identity not copied"), Data->MetaData.Contains(UDatasmithAssetUserData::UniqueIdMetaDataKey));
        Test.TestEqual(TEXT("Reimport templates not retained"), Data->ObjectTemplates.Num(), 0);
        Test.TestEqual(TEXT("Metadata uses class defaults as archetype"), Data->GetArchetype(), Data->GetClass()->GetDefaultObject());
        Test.TestNotNull(TEXT("Mesh reference retained"), Child->FindComponentByClass<UStaticMeshComponent>()->GetStaticMesh().Get());
        Test.TestNotNull(TEXT("Material reference retained"), Child->FindComponentByClass<UStaticMeshComponent>()->GetMaterial(0));
        Test.TestEqual(TEXT("Exact material reference preserved"), Child->FindComponentByClass<UStaticMeshComponent>()->GetMaterial(0),
            LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial")));
    }
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCDatasmithCompatibilityTest, "BuildingBlueprintCreator.DatasmithCompatibility",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBBCDatasmithCompatibilityTest::RunTest(const FString& Parameters)
{
    using namespace BBCDatasmithTest;
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    const FString Folder = TEXT("/Game/BBCDatasmith_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
    AStaticMeshActor* Parent = Mesh(World, FVector(400,200,100));
    AStaticMeshActor* A = Mesh(World, FVector(600,220,200));
    AStaticMeshActor* B = Mesh(World, FVector(750,320,240));
    A->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
    B->AttachToActor(A, FAttachmentTransformRules::KeepWorldTransform);
    UDatasmithAssetUserData* AData = AddImportData(A, TEXT("A"));
    UDatasmithAssetUserData* BData = AddImportData(B, TEXT("B"));
    const auto ATemplates = AData->ObjectTemplates;
    const auto AMetadata = AData->MetaData;
    ADatasmithSceneActor* Scene = World->SpawnActor<ADatasmithSceneActor>();
    Scene->RelatedActors.Add(TEXT("A"), A);
    Scene->RelatedActors.Add(TEXT("B"), B);
    Scene->RelatedActors.Add(TEXT("Unselected"), Parent);
    Scene->RelatedActors.Add(TEXT("PreviouslyDeleted"), nullptr);
    ADatasmithSceneActor* OtherScene = World->SpawnActor<ADatasmithSceneActor>();
    OtherScene->RelatedActors.Add(TEXT("AliasWithDifferentId"), A);
    const FString SourceMap = Folder / TEXT("SourceMap");
    const FString MapFile = FPackageName::LongPackageNameToFilename(SourceMap, FPackageName::GetMapPackageExtension());
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(MapFile),true);
    if (!TestTrue(TEXT("Real source map saved with Datasmith import templates"), FEditorFileUtils::SaveLevel(World->PersistentLevel, MapFile))) return false;
    const FTransform ABefore = A->GetTransform(), BBefore = B->GetTransform();
    FBuildingBlueprintOptions Options; Options.FolderPath = Folder; Options.AssetName = TEXT("BP_DatasmithBuilding");
    FBuildingBlueprintResult Created = UBuildingBlueprintLibrary::CreateBuildingBlueprint({A,B}, Options);
    if (!TestTrue(TEXT("Create-only can save imported Datasmith meshes: ") + Created.Message, Created.bSuccess)) return false;
    TestTrue(TEXT("Source metadata unchanged"), AData->MetaData.OrderIndependentCompareEqual(AMetadata));
    TestTrue(TEXT("Source reimport templates unchanged"), AData->ObjectTemplates.OrderIndependentCompareEqual(ATemplates));
    TestEqual(TEXT("Source remains managed"), Scene->RelatedActors.FindChecked(TEXT("A")).Get(), static_cast<AActor*>(A));
    TestFalse(TEXT("Create-only does not dirty source map"), World->GetOutermost()->IsDirty());
    UBlueprint* Building = LoadObject<UBlueprint>(nullptr,*Created.AssetObjectPath);
    AActor* Instance = World->SpawnActor<AActor>(Building->GeneratedClass, Created.Pivot, FRotator::ZeroRotator);
    CheckData(*this,Instance);
    TArray<AActor*> Children = Parts(Instance);
    if (Children.Num()==2)
    {
        TestTrue(TEXT("First mesh placement preserved"), Children[0]->GetTransform().Equals(ABefore,0.001));
        TestTrue(TEXT("Second mesh placement preserved"), Children[1]->GetTransform().Equals(BBefore,0.001));
    }
    World->EditorDestroyActor(Instance, false);

    // Datasmith subclass fields remain protected. Only the native management
    // map is exempt, never arbitrary properties on the manager or other actors.
    UPackage* GuardPackage = CreatePackage(*(TEXT("/Temp/BBCReferenceGuard_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    UBlueprint* GuardBP = FKismetEditorUtilities::CreateBlueprint(ADatasmithSceneActor::StaticClass(), GuardPackage, TEXT("BP_Guard"),
        BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    FEdGraphPinType RefType; RefType.PinCategory = UEdGraphSchema_K2::PC_Object; RefType.PinSubCategoryObject = AActor::StaticClass();
    FBlueprintEditorUtils::AddMemberVariable(GuardBP,TEXT("GameplayTarget"),RefType);
    FKismetEditorUtilities::CompileBlueprint(GuardBP);
    AActor* Guard = World->SpawnActor<AActor>(GuardBP->GeneratedClass);
    FObjectProperty* Ref = FindFProperty<FObjectProperty>(GuardBP->GeneratedClass,TEXT("GameplayTarget"));
    Ref->SetObjectPropertyValue_InContainer(Guard,A);
    Options.bReplaceActors = true; Options.AssetName = TEXT("BP_Blocked");
    FBuildingBlueprintResult Blocked = UBuildingBlueprintLibrary::CreateBuildingBlueprint({A,B},Options);
    TestFalse(TEXT("Non-management reference still blocks replacement"),Blocked.bSuccess);
    TestTrue(TEXT("Failure identifies actual protected field"),Blocked.Message.Contains(TEXT("GameplayTarget")));
    TestEqual(TEXT("Failed preflight does not alter management map"),Scene->RelatedActors.FindChecked(TEXT("A")).Get(),static_cast<AActor*>(A));
    Ref->SetObjectPropertyValue_InContainer(Guard,nullptr);
    World->EditorDestroyActor(Guard,false);

    Options.AssetName = TEXT("BP_DatasmithReplacement");
    FBuildingBlueprintResult Replaced = UBuildingBlueprintLibrary::CreateBuildingBlueprint({A,B},Options);
    if (!TestTrue(TEXT("Replacement supports Datasmith management references: ") + Replaced.Message,Replaced.bSuccess)) return false;
    CheckData(*this,Replaced.ReplacementActor);
    TestTrue(TEXT("A key retained as explicitly removed"),Scene->RelatedActors.Contains(TEXT("A")) && Scene->RelatedActors[TEXT("A")].IsNull());
    TestTrue(TEXT("B key retained as explicitly removed"),Scene->RelatedActors.Contains(TEXT("B")) && Scene->RelatedActors[TEXT("B")].IsNull());
    TestTrue(TEXT("All aliases cleared, not only UniqueId match"),OtherScene->RelatedActors[TEXT("AliasWithDifferentId")].IsNull());
    TestEqual(TEXT("Unselected mapping untouched"),Scene->RelatedActors[TEXT("Unselected")].Get(),static_cast<AActor*>(Parent));
    TestEqual(TEXT("No keys lost"),Scene->RelatedActors.Num(),4);
    TestTrue(TEXT("Undo succeeds"),GEditor->UndoTransaction());
    TestEqual(TEXT("Undo restores A association"),Scene->RelatedActors[TEXT("A")].Get(),static_cast<AActor*>(A));
    TestEqual(TEXT("Undo restores B association"),Scene->RelatedActors[TEXT("B")].Get(),static_cast<AActor*>(B));
    TestEqual(TEXT("Undo restores alias in other scene"),OtherScene->RelatedActors[TEXT("AliasWithDifferentId")].Get(),static_cast<AActor*>(A));
    TestTrue(TEXT("Undo preserves original import templates"),AData->ObjectTemplates.OrderIndependentCompareEqual(ATemplates));
    TestTrue(TEXT("Redo succeeds"),GEditor->RedoTransaction());
    TestTrue(TEXT("Redo clears A association"),Scene->RelatedActors[TEXT("A")].IsNull());
    TestTrue(TEXT("Redo clears other-scene alias"),OtherScene->RelatedActors[TEXT("AliasWithDifferentId")].IsNull());
    FString Manifest = Created.AssetObjectPath + TEXT("\n") + SourceMap + TEXT("\n") + Created.Pivot.ToString()
        + TEXT("\n") + ABefore.ToString() + TEXT("\n") + BBefore.ToString();
    const FString Dir = FPaths::ProjectSavedDir() / TEXT("BuildingBlueprintCreator");
    IFileManager::Get().MakeDirectory(*Dir,true);
    FFileHelper::SaveStringToFile(Manifest,*(Dir / TEXT("datasmith_roundtrip.txt")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCDatasmithReloadTest, "BuildingBlueprintCreator.DatasmithReload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBBCDatasmithReloadTest::RunTest(const FString& Parameters)
{
    using namespace BBCDatasmithTest;
    TArray<FString> Lines;
    FFileHelper::LoadFileToStringArray(Lines,*(FPaths::ProjectSavedDir() / TEXT("BuildingBlueprintCreator/datasmith_roundtrip.txt")));
    if (!TestEqual(TEXT("Datasmith roundtrip manifest available"),Lines.Num(),5)) return false;
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    const bool bMapAlreadyLoaded = FindPackage(nullptr,*Lines[1]) != nullptr;
    if (FParse::Param(FCommandLine::Get(), TEXT("BBCFreshReload")))
        TestFalse(TEXT("Fresh process starts without original map"), bMapAlreadyLoaded);
    UBlueprint* BP = LoadObject<UBlueprint>(nullptr,*Lines[0]);
    if (!TestNotNull(TEXT("Independent Datasmith building loads"),BP)) return false;
    FVector Pivot; Pivot.InitFromString(Lines[2]);
    FTransform ABefore,BBefore; ABefore.InitFromString(Lines[3]); BBefore.InitFromString(Lines[4]);
    for (int32 I=0;I<2;++I)
    {
        FVector Offset(2000*I,500*I,0);
        AActor* Instance = World->SpawnActor<AActor>(BP->GeneratedClass,Pivot+Offset,FRotator::ZeroRotator);
        if (!TestNotNull(TEXT("Reusable building spawns"),Instance)) return false;
        CheckData(*this,Instance);
        auto Children=Parts(Instance);
        if (Children.Num()==2)
        {
            FTransform ExpectedA=ABefore,ExpectedB=BBefore;
            ExpectedA.AddToTranslation(Offset); ExpectedB.AddToTranslation(Offset);
            TestTrue(TEXT("First placement survives fresh reload"),Children[0]->GetTransform().Equals(ExpectedA,0.001));
            TestTrue(TEXT("Second placement survives fresh reload"),Children[1]->GetTransform().Equals(ExpectedB,0.001));
        }
    }
    if (!bMapAlreadyLoaded) TestNull(TEXT("Loading the building does not load original map"),FindPackage(nullptr,*Lines[1]));
    return !HasAnyErrors();
}
#endif
