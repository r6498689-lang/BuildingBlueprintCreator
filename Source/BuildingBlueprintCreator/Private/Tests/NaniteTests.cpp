#if WITH_DEV_AUTOMATION_TESTS
#include "BuildingBlueprintLibrary.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "StaticMeshCompiler.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace BBCNaniteTest
{
static bool Save(UObject* Asset)
{
    FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;
    return UPackage::SavePackage(Asset->GetOutermost(),Asset,*FPackageName::LongPackageNameToFilename(Asset->GetOutermost()->GetName(),FPackageName::GetAssetPackageExtension()),Args);
}
static UStaticMesh* MeshAsset(const FString& Folder,const TCHAR* Name)
{
    UStaticMesh* Mesh=DuplicateObject<UStaticMesh>(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")),CreatePackage(*(Folder/Name)),FName(Name));
    Mesh->SetFlags(RF_Public|RF_Standalone|RF_Transactional);
    Mesh->GetStaticMaterials().Reset();Mesh->GetStaticMaterials().Add(FStaticMaterial(LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"))));
    Mesh->NaniteSettings.bEnabled=false;Mesh->PostEditChange();TArray<UStaticMesh*> One{Mesh};FStaticMeshCompilingManager::Get().FinishCompilation(One);Save(Mesh);return Mesh;
}
static AStaticMeshActor* Place(UWorld* World,UStaticMesh* Mesh,FVector Position,AActor* Parent=nullptr)
{
    auto* A=World->SpawnActor<AStaticMeshActor>(Position,FRotator::ZeroRotator);A->GetStaticMeshComponent()->SetStaticMesh(Mesh);
    if(Parent)A->AttachToActor(Parent,FAttachmentTransformRules::KeepWorldTransform);return A;
}
static FString Manifest(){return FPaths::ProjectSavedDir()/TEXT("BuildingBlueprintCreator/nanite_roundtrip.txt");}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCNaniteTest,"BuildingBlueprintCreator.NaniteEnable",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FBBCNaniteTest::RunTest(const FString& Parameters)
{
    using namespace BBCNaniteTest;auto* W=FAutomationEditorCommonUtils::CreateNewMap();
    const FString Folder=TEXT("/Game/BBCNanite_")+FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
    auto* EngineCube=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));const bool EngineEnabled=EngineCube->NaniteSettings.bEnabled;
    const bool EngineDirty=EngineCube->GetOutermost()->IsDirty();
    auto* Mesh=MeshAsset(Folder,TEXT("SharedMesh"));auto* ReadOnlyMesh=MeshAsset(Folder,TEXT("ReadOnlyMesh"));
    const FString ReadOnlyFile=FPackageName::LongPackageNameToFilename(ReadOnlyMesh->GetOutermost()->GetName(),FPackageName::GetAssetPackageExtension());
    ON_SCOPE_EXIT{FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*ReadOnlyFile,false);};
    auto* Root=W->SpawnActor<AActor>();auto* R=NewObject<USceneComponent>(Root);Root->AddInstanceComponent(R);Root->SetRootComponent(R);R->SetMobility(EComponentMobility::Static);R->RegisterComponent();
    auto* A=Place(W,Mesh,FVector(100,0,100),Root);Place(W,Mesh,FVector(300,0,100),Root);
    auto* Outside=Place(W,Mesh,FVector(3000,0,100));auto* Blocked=Place(W,ReadOnlyMesh,FVector(6000,0,100));
    FBuildingBlueprintOptions O;O.FolderPath=Folder;O.AssetName=TEXT("NoNanite");
    auto Plain=UBuildingBlueprintLibrary::CreateBuildingBlueprint({A},O);
    TestTrue(TEXT("Ordinary creation succeeds"),Plain.bSuccess);TestFalse(TEXT("Unselected Nanite leaves asset setting unchanged"),Mesh->NaniteSettings.bEnabled);
    O.bEnableNanite=true;O.AssetName=TEXT("Blocked");FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*ReadOnlyFile,true);
    auto Fail=UBuildingBlueprintLibrary::CreateBuildingBlueprint({A,Blocked},O);
    TestFalse(TEXT("Read-only project asset is refused before changes"),Fail.bSuccess);
    TestFalse(TEXT("Earlier writable candidate is unchanged by failed preflight"),Mesh->NaniteSettings.bEnabled);
    TestFalse(TEXT("No blueprint created for failed Nanite preflight"),FPackageName::DoesPackageExist(Folder/TEXT("Blocked")));
    FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*ReadOnlyFile,false);
    auto* Nested=LoadObject<UBlueprint>(nullptr,*Plain.AssetObjectPath);
    auto* Node=Nested->SimpleConstructionScript->CreateNode(UInstancedStaticMeshComponent::StaticClass(),TEXT("NestedInstances"));
    auto* ISM=CastChecked<UInstancedStaticMeshComponent>(Node->ComponentTemplate);ISM->SetMobility(EComponentMobility::Static);ISM->SetStaticMesh(Mesh);
    ISM->AddInstance(FTransform(FVector(0,500,100)));ISM->AddInstance(FTransform(FVector(200,500,100)));Nested->SimpleConstructionScript->GetRootNodes()[0]->AddChildNode(Node);
    FKismetEditorUtilities::CompileBlueprint(Nested);Save(Nested);
    auto* NestedActor=W->SpawnActor<AActor>(Nested->GeneratedClass);NestedActor->AttachToActor(Root,FAttachmentTransformRules::KeepWorldTransform);
    O.bIncludeAllAttachedChildren=true;O.bUseISM=true;O.AssetName=TEXT("NaniteBuilding");
    auto Created=UBuildingBlueprintLibrary::CreateBuildingBlueprint({Root},O);
    if(!TestTrue(TEXT("Nanite and ISM create together: ")+Created.Message,Created.bSuccess))return false;
    TestTrue(TEXT("Nanite processing recorded"),Created.bNaniteProcessed);
    TestEqual(TEXT("Shared nested/ISM/ordinary mesh enabled only once"),Created.NaniteEnabledCount,1);
    TestEqual(TEXT("No originally enabled project mesh"),Created.NaniteAlreadyEnabledCount,0);
    TestTrue(TEXT("Original project asset now enabled"),Mesh->NaniteSettings.bEnabled);
    TestTrue(TEXT("Nanite geometry built, not just a flag"),Mesh->HasValidNaniteData());
    TestFalse(TEXT("Original project asset saved clean"),Mesh->GetOutermost()->IsDirty());
    TestTrue(TEXT("Other references see original mesh setting"),Outside->GetStaticMeshComponent()->GetStaticMesh()->NaniteSettings.bEnabled);
    TestTrue(TEXT("Original actors remain"),IsValid(A)&&IsValid(Outside));
    TestEqual(TEXT("Engine mesh setting untouched"),EngineCube->NaniteSettings.bEnabled,EngineEnabled);
    TestEqual(TEXT("Engine mesh dirty state untouched"),EngineCube->GetOutermost()->IsDirty(),EngineDirty);
    TestTrue(TEXT("Asset Nanite setting has independent undo"),GEditor->UndoTransaction());
    TestFalse(TEXT("Undo restores original Nanite flag"),Mesh->NaniteSettings.bEnabled);
    TArray<UStaticMesh*> One{Mesh};FStaticMeshCompilingManager::Get().FinishCompilation(One);
    TestTrue(TEXT("Nanite redo works"),GEditor->RedoTransaction());FStaticMeshCompilingManager::Get().FinishCompilation(One);
    TestTrue(TEXT("Redo restores enabled Nanite"),Mesh->NaniteSettings.bEnabled);Save(Mesh);
    O.AssetName=TEXT("AlreadyEnabled");O.bIncludeAllAttachedChildren=false;
    auto Again=UBuildingBlueprintLibrary::CreateBuildingBlueprint({A},O);
    TestTrue(TEXT("Already enabled asset can create again"),Again.bSuccess);
    TestEqual(TEXT("Already enabled mesh not rebuilt"),Again.NaniteEnabledCount,0);TestEqual(TEXT("Already enabled mesh counted once"),Again.NaniteAlreadyEnabledCount,1);
    TArray<FString> Lines{Mesh->GetPathName(),Created.AssetObjectPath};IFileManager::Get().MakeDirectory(*FPaths::GetPath(Manifest()),true);FFileHelper::SaveStringArrayToFile(Lines,*Manifest());
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCNaniteReloadTest,"BuildingBlueprintCreator.NaniteReload",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FBBCNaniteReloadTest::RunTest(const FString& Parameters)
{
    using namespace BBCNaniteTest;TArray<FString> Lines;FFileHelper::LoadFileToStringArray(Lines,*Manifest());
    if(!TestEqual(TEXT("Nanite roundtrip manifest saved"),Lines.Num(),2))return false;
    auto* Mesh=LoadObject<UStaticMesh>(nullptr,*Lines[0]);if(!TestNotNull(TEXT("Modified original mesh reloads"),Mesh))return false;
    TArray<UStaticMesh*> One{Mesh};FStaticMeshCompilingManager::Get().FinishCompilation(One);
    TestTrue(TEXT("Enabled Nanite setting saved to disk"),Mesh->NaniteSettings.bEnabled);TestTrue(TEXT("Valid Nanite resources survive reload"),Mesh->HasValidNaniteData());
    auto* BP=LoadObject<UBlueprint>(nullptr,*Lines[1]);if(!TestNotNull(TEXT("Nanite building reloads"),BP))return false;
    auto* World=FAutomationEditorCommonUtils::CreateNewMap();auto* Actor=World->SpawnActor<AActor>(BP->GeneratedClass);
    TInlineComponentArray<UInstancedStaticMeshComponent*> ISMs(Actor);TestTrue(TEXT("Created building uses real ISM"),ISMs.Num()>0&&ISMs[0]->GetClass()==UInstancedStaticMeshComponent::StaticClass());
    if(!ISMs.IsEmpty())TestEqual(TEXT("Original model resource reference retained"),ISMs[0]->GetStaticMesh().Get(),Mesh);
    return !HasAnyErrors();
}
#endif
