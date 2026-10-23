#if WITH_DEV_AUTOMATION_TESTS
#include "BuildingBlueprintLibrary.h"
#include "Components/AudioComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Editor/GroupActor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/PointLight.h"
#include "Engine/StaticMeshActor.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/AutomationEditorCommon.h"

namespace BBCParentBatchTest
{
static AActor* Empty(UWorld* World,const TCHAR* Label,FVector Location)
{
    AActor* A=World->SpawnActor<AActor>();
    USceneComponent* Root=NewObject<USceneComponent>(A,TEXT("ImportedRoot"),RF_Transactional);
    A->AddInstanceComponent(Root);A->SetRootComponent(Root);Root->SetMobility(EComponentMobility::Static);Root->RegisterComponent();
    A->SetActorLabel(Label);A->SetActorLocation(Location);return A;
}
static AStaticMeshActor* Mesh(UWorld* World,const TCHAR* Label,FVector Location,AActor* Parent)
{
    auto* A=World->SpawnActor<AStaticMeshActor>(Location,FRotator(0,15,0));A->SetActorLabel(Label);
    A->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
    A->GetStaticMeshComponent()->SetMaterial(0,LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial")));
    A->AttachToActor(Parent,FAttachmentTransformRules::KeepWorldTransform);return A;
}
static FString Manifest(){return FPaths::ProjectSavedDir()/TEXT("BuildingBlueprintCreator/parent_batch_roundtrip.txt");}
static void CheckWall(FAutomationTestBase& Test,AActor* Building,const TCHAR* Label,const FTransform& Transform)
{
    TInlineComponentArray<UChildActorComponent*> Parts(Building);
    UChildActorComponent** Found=Parts.FindByPredicate([&](UChildActorComponent* Part){return Part->GetName().EndsWith(FString(TEXT("_"))+Label);});
    if(!Test.TestTrue(TEXT("Batch building contains requested wall"),Found&&(*Found)->GetChildActor()))return;
    AActor* Wall=(*Found)->GetChildActor();
    Test.TestTrue(TEXT("Batch keeps world transform"),Wall->GetActorTransform().Equals(Transform,.001));
    Test.TestNotNull(TEXT("Batch keeps mesh"),Wall->FindComponentByClass<UStaticMeshComponent>()->GetStaticMesh().Get());
    Test.TestEqual(TEXT("Batch keeps material"),Wall->FindComponentByClass<UStaticMeshComponent>()->GetMaterial(0),
        LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial")));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCParentBatchTest,"BuildingBlueprintCreator.ParentBatch",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FBBCParentBatchTest::RunTest(const FString& Parameters)
{
    using namespace BBCParentBatchTest;
    UWorld* World=FAutomationEditorCommonUtils::CreateNewMap();
    AActor* A=Empty(World,TEXT("Tower"),FVector(1000,200,100));
    AActor* Inner=Empty(World,TEXT("Inner_grp"),FVector(1050,200,150));Inner->AttachToActor(A,FAttachmentTransformRules::KeepWorldTransform);
    AActor* B=Empty(World,TEXT("Tower"),FVector(3000,500,100));
    AStaticMeshActor* WallA=Mesh(World,TEXT("WallA"),FVector(1100,300,250),Inner);
    AStaticMeshActor* WallB=Mesh(World,TEXT("WallB"),FVector(3100,600,250),B);
    const FTransform TA=WallA->GetActorTransform(),TB=WallB->GetActorTransform();
    TestTrue(TEXT("Plain empty parent is eligible"),UBuildingBlueprintLibrary::IsBatchParentActor(A));
    Mesh(World,TEXT("MeshChild"),FVector(1130,300,280),WallA);
    TestFalse(TEXT("Mesh actor with children is still not a batch parent"),UBuildingBlueprintLibrary::IsBatchParentActor(WallA));
    // Remove just this classification fixture so the saved hierarchy has 3 parts.
    TArray<AActor*> Extra;WallA->GetAttachedActors(Extra);for(AActor* Child:Extra)World->EditorDestroyActor(Child,false);
    TestFalse(TEXT("Mixed parent and mesh selection disables batch"),UBuildingBlueprintLibrary::PlanParentBatch({A,WallA}).IsValid());
    TestFalse(TEXT("Null selection disables batch"),UBuildingBlueprintLibrary::PlanParentBatch({A,nullptr}).IsValid());
    AActor* Audio=Empty(World,TEXT("FunctionalParent"),FVector::ZeroVector);
    Audio->AddInstanceComponent(NewObject<UAudioComponent>(Audio));Mesh(World,TEXT("AudioWall"),FVector(0,0,100),Audio);
    TestFalse(TEXT("Actor with functional component is not an empty parent"),UBuildingBlueprintLibrary::IsBatchParentActor(Audio));
    const auto Plan=UBuildingBlueprintLibrary::PlanParentBatch({Inner,A,B,A});
    TestTrue(TEXT("Parent selection is accepted"),Plan.IsValid());
    TestEqual(TEXT("Duplicate and nested roots produce only two groups"),Plan.Roots.Num(),2);
    TestEqual(TEXT("Nested root is explicitly reported"),Plan.IncludedDescendants.Num(),1);
    FBuildingBlueprintOptions Options;Options.FolderPath=TEXT("/Game/BBCParentBatch_")+FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
    Options.AssetName=TEXT("IgnoredSharedName");Options.bIncludeAllAttachedChildren=false;
    auto Created=UBuildingBlueprintLibrary::CreateParentBlueprintBatch({Inner,A,B,A},Options);
    if(!TestTrue(TEXT("Two independent buildings create: ")+Created.Message,Created.bAccepted&&Created.CreatedCount==2&&Created.FailedCount==0))return false;
    TestEqual(TEXT("First asset uses parent label"),Created.Items[0].Result.AssetObjectPath,Options.FolderPath/TEXT("BP_Tower.BP_Tower"));
    TestEqual(TEXT("Duplicate label gets suffix"),Created.Items[1].Result.AssetObjectPath,Options.FolderPath/TEXT("BP_Tower_1.BP_Tower_1"));
    for(int32 I=0;I<2;++I)
    {
        auto* BP=LoadObject<UBlueprint>(nullptr,*Created.Items[I].Result.AssetObjectPath);
        AActor* Instance=World->SpawnActor<AActor>(BP->GeneratedClass,Created.Items[I].Result.Pivot,FRotator::ZeroRotator);
        CheckWall(*this,Instance,I?TEXT("WallB"):TEXT("WallA"),I?TB:TA);
        TInlineComponentArray<UChildActorComponent*> Parts(Instance);
        TestEqual(TEXT("Each building only contains its own hierarchy"),Parts.Num(),I?2:3);
        TestFalse(TEXT("Existing blueprint actor cannot be a batch parent"),UBuildingBlueprintLibrary::IsBatchParentActor(Instance));
        World->EditorDestroyActor(Instance,false);
    }
    TestTrue(TEXT("No-replace keeps original roots and descendants"),IsValid(A)&&IsValid(Inner)&&IsValid(WallA)&&IsValid(B)&&IsValid(WallB));
    TArray<FString> Lines{Created.Items[0].Result.AssetObjectPath,Created.Items[1].Result.AssetObjectPath,
        Created.Items[0].Result.Pivot.ToString(),Created.Items[1].Result.Pivot.ToString(),TA.ToString(),TB.ToString()};
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Manifest()),true);FFileHelper::SaveStringArrayToFile(Lines,*Manifest());

    AActor* Bad=Empty(World,TEXT("Unsupported"),FVector(6000,0,0));
    auto* Light=World->SpawnActor<APointLight>();Light->AttachToActor(Bad,FAttachmentTransformRules::KeepWorldTransform);
    Options.bReplaceActors=true;
    auto Replaced=UBuildingBlueprintLibrary::CreateParentBlueprintBatch({Bad,A,B},Options);
    TestTrue(TEXT("Batch continues after failed first group"),Replaced.bAccepted&&Replaced.CreatedCount==2&&Replaced.FailedCount==1);
    TestTrue(TEXT("Failed group original actors are retained"),IsValid(Bad)&&IsValid(Light));
    TestFalse(TEXT("Successful A original removed"),IsValid(A));TestFalse(TEXT("Successful B original removed"),IsValid(B));
    TestTrue(TEXT("Successive replacements create independent instances"),IsValid(Replaced.Items[1].Result.ReplacementActor)&&IsValid(Replaced.Items[2].Result.ReplacementActor));
    TestTrue(TEXT("Undo last group"),GEditor->UndoTransaction());
    TestTrue(TEXT("Undo restores only last group's source"),IsValid(B)&&!IsValid(A));
    TestTrue(TEXT("Undo prior successful group"),GEditor->UndoTransaction());
    TestTrue(TEXT("Undo restores complete nested A hierarchy"),IsValid(A)&&IsValid(Inner)&&IsValid(WallA));
    TestTrue(TEXT("Redo A"),GEditor->RedoTransaction());TestTrue(TEXT("Redo B"),GEditor->RedoTransaction());
    FString Report;TestTrue(TEXT("Batch report exists"),FFileHelper::LoadFileToString(Report,*Replaced.ReportFilePath));
    TestTrue(TEXT("Batch report contains successes and failure"),Report.Contains(TEXT("[成功]"))&&Report.Contains(TEXT("[失败]"))&&Report.Contains(TEXT("Unsupported")));

    AActor* G1=Empty(World,TEXT("GroupParentA"),FVector(8000,0,0));
    AActor* G2=Empty(World,TEXT("GroupParentB"),FVector(9000,0,0));
    AStaticMeshActor* Shared=Mesh(World,TEXT("SharedMember"),FVector(9000,0,100),G2);
    AGroupActor* Group=World->SpawnActor<AGroupActor>();
    if(!Group->GetRootComponent())
    {
        auto* Root=NewObject<USceneComponent>(Group);Group->AddInstanceComponent(Root);Group->SetRootComponent(Root);Root->SetMobility(EComponentMobility::Static);Root->RegisterComponent();
    }
    Group->Add(*Shared);Group->AttachToActor(G1,FAttachmentTransformRules::KeepWorldTransform);
    TestFalse(TEXT("Native editor group itself is not a batch parent"),UBuildingBlueprintLibrary::IsBatchParentActor(Group));
    TestFalse(TEXT("Cross-parent native group overlap is refused before conversion"),UBuildingBlueprintLibrary::PlanParentBatch({G1,G2}).IsValid());
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCParentBatchReloadTest,"BuildingBlueprintCreator.ParentBatchReload",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FBBCParentBatchReloadTest::RunTest(const FString& Parameters)
{
    using namespace BBCParentBatchTest;
    TArray<FString> Lines;FFileHelper::LoadFileToStringArray(Lines,*Manifest());
    if(!TestEqual(TEXT("Batch reload manifest"),Lines.Num(),6))return false;
    UWorld* World=FAutomationEditorCommonUtils::CreateNewMap();
    for(int32 I=0;I<2;++I)
    {
        auto* BP=LoadObject<UBlueprint>(nullptr,*Lines[I]);if(!TestNotNull(TEXT("Independent batch blueprint reloads"),BP))return false;
        FVector Pivot;Pivot.InitFromString(Lines[I+2]);FTransform Expected;Expected.InitFromString(Lines[I+4]);
        auto* Instance=World->SpawnActor<AActor>(BP->GeneratedClass,Pivot,FRotator::ZeroRotator);
        CheckWall(*this,Instance,I?TEXT("WallB"):TEXT("WallA"),Expected);
    }
    return !HasAnyErrors();
}
#endif
