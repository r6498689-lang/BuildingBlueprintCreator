#if WITH_DEV_AUTOMATION_TESTS
#include "BuildingBlueprintLibrary.h"
#include "Components/AudioComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "DatasmithAssetUserData.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/StaticMeshActor.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/ObjectThumbnail.h"
#include "ObjectTools.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace BBCISMTest
{
static AActor* Parent(UWorld* World,const TCHAR* Name,FVector Location)
{
    auto* Actor=World->SpawnActor<AActor>();auto* Root=NewObject<USceneComponent>(Actor);
    Actor->AddInstanceComponent(Root);Actor->SetRootComponent(Root);Root->SetMobility(EComponentMobility::Static);Root->RegisterComponent();
    Actor->SetActorLabel(Name);Actor->SetActorLocation(Location);return Actor;
}
static AStaticMeshActor* Mesh(UWorld* World,AActor* Parent,const TCHAR* Name,FVector Local)
{
    auto* A=World->SpawnActor<AStaticMeshActor>();A->SetActorLabel(Name);
    A->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
    A->GetStaticMeshComponent()->SetMaterial(0,LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial")));
    A->AttachToActor(Parent,FAttachmentTransformRules::KeepRelativeTransform);A->SetActorRelativeLocation(Local);return A;
}
static FString Manifest(){return FPaths::ProjectSavedDir()/TEXT("BuildingBlueprintCreator/ism_roundtrip.txt");}
static UChildActorComponent* Child(AActor* Building,const TCHAR* Label)
{
    TInlineComponentArray<UChildActorComponent*> Parts(Building);
    for(auto* Part:Parts)if(Part->GetName().EndsWith(FString(TEXT("_"))+Label))return Part;
    return nullptr;
}
static void Check(FAutomationTestBase& Test,AActor* Building,const TArray<FTransform>& Expected)
{
    TInlineComponentArray<UInstancedStaticMeshComponent*> ISMs(Building);
    if(!Test.TestEqual(TEXT("Exactly one engine ISM"),ISMs.Num(),1))return;
    auto* H=ISMs[0];Test.TestEqual(TEXT("Exactly two repeated windows"),H->GetInstanceCount(),2);
    Test.TestEqual(TEXT("New output is exact ISM, not hierarchical"),H->GetClass(),UInstancedStaticMeshComponent::StaticClass());
    // Attachment enumeration does not promise insertion order. Verify a one-to-one
    // match of geometry placements instead of assigning meaning to instance index.
    TSet<int32> Matched;
    for(int32 I=0;I<2;++I)
    {
        FTransform T;H->GetInstanceTransform(I,T,true);
        int32 Match=INDEX_NONE;
        for(int32 J=0;J<Expected.Num();++J)if(!Matched.Contains(J)&&T.Equals(Expected[J],.001)){Match=J;break;}
        Test.TestTrue(TEXT("ISM keeps unique original world transform: ")+T.ToString(),Match!=INDEX_NONE);
        if(Match!=INDEX_NONE)Matched.Add(Match);
    }
    Test.TestNotNull(TEXT("ISM mesh retained"),H->GetStaticMesh().Get());
    Test.TestEqual(TEXT("ISM material retained"),H->GetMaterial(0),LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial")));
    auto* Empty=Child(Building,TEXT("Empty_Bollard"));
    Test.TestTrue(TEXT("Empty mesh remains independent with children"),Empty&&Empty->GetChildActor()&&!Empty->GetChildActor()->FindComponentByClass<UStaticMeshComponent>()->GetStaticMesh());
    auto* Single=Child(Building,TEXT("UnderEmpty"));
    Test.TestTrue(TEXT("Empty node keeps original hierarchy"),Single&&Single->GetAttachParent()==Empty);
    auto* BP=Child(Building,TEXT("OriginalBlueprint"));Test.TestTrue(TEXT("Existing blueprint is not unpacked"),BP&&BP->GetChildActor()&&BP->GetChildActor()->GetClass()->ClassGeneratedBy);
    TInlineComponentArray<USceneComponent*> Components(Building);
    int32 Metadata=0;
    for(auto* C:Components)
        if(C->GetClass()==USceneComponent::StaticClass()&&C->GetName().Contains(TEXT("Window")))
            if(auto* Data=Cast<UDatasmithAssetUserData>(C->GetAssetUserDataOfClass(UDatasmithAssetUserData::StaticClass())))
            {Test.TestTrue(TEXT("Per-source metadata remains on named anchor"),Data->MetaData.Contains(TEXT("Part")));Test.TestFalse(TEXT("Importer identity not copied"),Data->MetaData.Contains(UDatasmithAssetUserData::UniqueIdMetaDataKey));++Metadata;}
    Test.TestEqual(TEXT("Both original metadata records retained separately"),Metadata,2);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCISMTest,"BuildingBlueprintCreator.ISMConversion",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FBBCISMTest::RunTest(const FString& Parameters)
{
    using namespace BBCISMTest;
    auto* World=FAutomationEditorCommonUtils::CreateNewMap();
    auto* Root=Parent(World,TEXT("ISM_Building"),FVector(1200,400,100));Root->SetActorRotation(FRotator(0,25,0));Root->SetActorScale3D(FVector(1.5,2,1));
    auto* Inner=Parent(World,TEXT("Facade"),FVector::ZeroVector);Inner->AttachToActor(Root,FAttachmentTransformRules::KeepRelativeTransform);Inner->SetActorRelativeLocation(FVector(100,0,100));
    auto* A=Mesh(World,Inner,TEXT("WindowA"),FVector(0,0,100));
    auto* B=Mesh(World,Inner,TEXT("WindowB"),FVector(150,0,100));B->SetActorRelativeScale3D(FVector(1,1,2));
    TArray<FTransform> Expected{A->GetActorTransform(),B->GetActorTransform()};
    for(auto* Actor:{A,B})
    {
        auto* Data=NewObject<UDatasmithAssetUserData>(Actor->GetStaticMeshComponent());Data->MetaData.Add(TEXT("Part"),Actor->GetActorLabel());
        Data->MetaData.Add(UDatasmithAssetUserData::UniqueIdMetaDataKey,Actor->GetActorLabel());Actor->GetStaticMeshComponent()->AddAssetUserData(Data);
    }
    auto* Empty=World->SpawnActor<AStaticMeshActor>();Empty->SetActorLabel(TEXT("Empty_Bollard"));Empty->AttachToActor(Inner,FAttachmentTransformRules::KeepRelativeTransform);
    Mesh(World,Empty,TEXT("UnderEmpty"),FVector(0,150,100));
    auto* OtherParent=Parent(World,TEXT("OtherFacade"),FVector::ZeroVector);OtherParent->AttachToActor(Root,FAttachmentTransformRules::KeepRelativeTransform);
    Mesh(World,OtherParent,TEXT("SameMeshDifferentParent"),FVector(0,300,100));
    auto* Different=Mesh(World,Inner,TEXT("DifferentMaterial"),FVector(300,0,100));Different->GetStaticMeshComponent()->SetMaterial(0,LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial")));
    auto* Shadow=Mesh(World,Inner,TEXT("DifferentShadow"),FVector(450,0,100));Shadow->GetStaticMeshComponent()->SetCastShadow(false);
    auto* Tagged=Mesh(World,Inner,TEXT("Tagged"),FVector(600,0,100));Tagged->Tags.Add(TEXT("GameplayTag"));
    auto* Extra=Mesh(World,Inner,TEXT("ExtraComponent"),FVector(750,0,100));Extra->AddInstanceComponent(NewObject<UAudioComponent>(Extra));
    auto* Mirror=Mesh(World,Inner,TEXT("Mirrored"),FVector(900,0,100));Mirror->SetActorRelativeScale3D(FVector(-1,1,1));

    FBuildingBlueprintOptions O;O.FolderPath=TEXT("/Game/BBCISM_")+FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);O.AssetName=TEXT("Original");
    auto OriginalResult=UBuildingBlueprintLibrary::CreateBuildingBlueprint({Different},O);
    if(!TestTrue(TEXT("Original blueprint fixture saves"),OriginalResult.bSuccess))return false;
    auto* OriginalBP=LoadObject<UBlueprint>(nullptr,*OriginalResult.AssetObjectPath);
    auto* OriginalActor=World->SpawnActor<AActor>(OriginalBP->GeneratedClass);OriginalActor->SetActorLabel(TEXT("OriginalBlueprint"));OriginalActor->AttachToActor(Root,FAttachmentTransformRules::KeepRelativeTransform);

    O.AssetName=TEXT("ISMBuilding");O.bIncludeAllAttachedChildren=true;O.bUseISM=true;
    auto R=UBuildingBlueprintLibrary::CreateBuildingBlueprint({Root},O);
    if(!TestTrue(TEXT("ISM building creates: ")+R.Message,R.bSuccess))return false;
    TestEqual(TEXT("Result reports one ISM"),R.ISMGroupCount,1);TestEqual(TEXT("Result reports two instances"),R.ISMInstanceCount,2);
    TestTrue(TEXT("Empty mesh and fallbacks appear in result"),R.Message.Contains(TEXT("Empty_Bollard"))&&R.Message.Contains(TEXT("Tagged"))&&R.Message.Contains(TEXT("ExtraComponent")));
    TestTrue(TEXT("No-replace leaves originals intact"),IsValid(A)&&IsValid(B)&&IsValid(Empty));
    TestNotNull(TEXT("Original importer metadata not changed"),A->GetStaticMeshComponent()->GetAssetUserDataOfClass(UDatasmithAssetUserData::StaticClass()));
    TestTrue(TEXT("Source importer identity remains"),CastChecked<UDatasmithAssetUserData>(A->GetStaticMeshComponent()->GetAssetUserDataOfClass(UDatasmithAssetUserData::StaticClass()))->MetaData.Contains(UDatasmithAssetUserData::UniqueIdMetaDataKey));
    auto* BP=LoadObject<UBlueprint>(nullptr,*R.AssetObjectPath);auto* Building=World->SpawnActor<AActor>(BP->GeneratedClass,R.Pivot,FRotator::ZeroRotator);
    Check(*this,Building,Expected);
    TestNull(TEXT("Instanced window no longer has a ChildActor"),Child(Building,TEXT("WindowA")));
    for(const TCHAR* Name:{TEXT("SameMeshDifferentParent"),TEXT("DifferentMaterial"),TEXT("DifferentShadow"),TEXT("Tagged"),TEXT("ExtraComponent"),TEXT("Mirrored")})TestNotNull(TEXT("Incompatible/single parts retained"),Child(Building,Name));
    auto* Facade=Child(Building,TEXT("Facade"));TInlineComponentArray<UInstancedStaticMeshComponent*> ISMs(Building);
    if(TestNotNull(TEXT("Original facade hierarchy remains"),Facade)&&ISMs.Num()==1)
    {
        TestEqual(TEXT("ISM attached to exact facade node"),ISMs[0]->GetAttachParent(),static_cast<USceneComponent*>(Facade));
        FTransform Before;ISMs[0]->GetInstanceTransform(0,Before,true);Facade->AddWorldOffset(FVector(100,200,0));
        FTransform After;ISMs[0]->GetInstanceTransform(0,After,true);TestTrue(TEXT("Moving original parent moves all its instances"),After.GetLocation().Equals(Before.GetLocation()+FVector(100,200,0),.01));
    }
    World->EditorDestroyActor(Building,false);
    TArray<FString> Lines{R.AssetObjectPath,R.Pivot.ToString(),Expected[0].ToString(),Expected[1].ToString()};
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Manifest()),true);FFileHelper::SaveStringArrayToFile(Lines,*Manifest());
    O.AssetName=TEXT("ISMOnly");auto Only=UBuildingBlueprintLibrary::CreateBuildingBlueprint({A,B},O);
    if(TestTrue(TEXT("Pure ISM blueprint creates without child actors"),Only.bSuccess))
    {
        auto* OnlyBP=LoadObject<UBlueprint>(nullptr,*Only.AssetObjectPath);
        auto* OnlyActor=World->SpawnActor<AActor>(OnlyBP->GeneratedClass,Only.Pivot,FRotator::ZeroRotator);
        TInlineComponentArray<UChildActorComponent*> Parts(OnlyActor);TestEqual(TEXT("No original Actor overhead for instanced leaves"),Parts.Num(),0);
        TInlineComponentArray<UInstancedStaticMeshComponent*> OnlyISMs(OnlyActor);TestEqual(TEXT("Pure blueprint has one ISM"),OnlyISMs.Num(),1);
        World->EditorDestroyActor(OnlyActor,false);
        FThumbnailMap Thumbnails;const FName Key(*UClass::ConvertFullNameToShortTypeFullName(OnlyBP->GetFullName()));
        const FString File=FPackageName::LongPackageNameToFilename(OnlyBP->GetOutermost()->GetName(),FPackageName::GetAssetPackageExtension());
        if(TestTrue(TEXT("ISM thumbnail saved in asset"),ThumbnailTools::LoadThumbnailsFromPackage(File,{Key},Thumbnails)))
            if(FObjectThumbnail* Thumb=Thumbnails.Find(Key))
            {
                const auto& Bytes=Thumb->GetUncompressedImageData();TArray<FColor> Pixels;
                if(TestTrue(TEXT("ISM thumbnail pixels exist"),Bytes.Num()==Thumb->GetImageWidth()*Thumb->GetImageHeight()*4&&Bytes.Num()>0))
                {
                    Pixels.Append(reinterpret_cast<const FColor*>(Bytes.GetData()),Bytes.Num()/4);
                    TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Thumb->GetImageWidth(),Thumb->GetImageHeight(),Pixels,PNG);
                    FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("BuildingBlueprintCreator_ism_thumbnail.png")));
                }
            }
            else AddError(TEXT("Saved ISM thumbnail entry missing"));
    }
    O.bUseISM=false;O.AssetName=TEXT("Ordinary");auto Plain=UBuildingBlueprintLibrary::CreateBuildingBlueprint({Root},O);
    TestTrue(TEXT("Empty mesh also supported with ISM disabled"),Plain.bSuccess);TestEqual(TEXT("Disabled ISM creates zero groups"),Plain.ISMGroupCount,0);
    FBuildingBlueprintOptions Legacy=O;Legacy.bUseHISM=true;Legacy.AssetName=TEXT("LegacyOption");
    auto LegacyResult=UBuildingBlueprintLibrary::CreateBuildingBlueprint({Root},Legacy);
    TestTrue(TEXT("Old script option remains compatible and produces ISM"),LegacyResult.bSuccess&&LegacyResult.ISMInstanceCount==2);
    O.bUseISM=true;O.bReplaceActors=true;O.AssetName=TEXT("ReplaceISM");auto Replaced=UBuildingBlueprintLibrary::CreateBuildingBlueprint({Root},O);
    if(!TestTrue(TEXT("Replacement ISM succeeds: ")+Replaced.Message,Replaced.bSuccess))return false;
    TestFalse(TEXT("Successful replacement removes original windows"),IsValid(A));TestFalse(TEXT("Successful replacement removes original empty node"),IsValid(Empty));
    Check(*this,Replaced.ReplacementActor,Expected);
    TestTrue(TEXT("Undo restores complete original building"),GEditor->UndoTransaction());TestTrue(TEXT("Restored windows and empty nodes"),IsValid(A)&&IsValid(B)&&IsValid(Empty));
    TestTrue(TEXT("Redo restores ISM replacement"),GEditor->RedoTransaction());

    auto* PA=Parent(World,TEXT("Batch_A"),FVector(5000,0,0));Mesh(World,PA,TEXT("a"),FVector::ZeroVector);
    auto* PB=Parent(World,TEXT("Batch_B"),FVector(7000,0,0));Mesh(World,PB,TEXT("a"),FVector::ZeroVector);Mesh(World,PB,TEXT("d1"),FVector(100,0,0));Mesh(World,PB,TEXT("d2"),FVector(200,0,0));
    // a uses another asset so it cannot join d; equal a across buildings never combine.
    TArray<AActor*> AC;PA->GetAttachedActors(AC);CastChecked<AStaticMeshActor>(AC[0])->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Sphere.Sphere")));
    TArray<AActor*> BC;PB->GetAttachedActors(BC);for(auto* C:BC)if(C->GetActorLabel()==TEXT("a"))CastChecked<AStaticMeshActor>(C)->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Sphere.Sphere")));
    O.bReplaceActors=false;auto Batch=UBuildingBlueprintLibrary::CreateParentBlueprintBatch({PA,PB},O);
    TestTrue(TEXT("Batch ISM creates both buildings"),Batch.CreatedCount==2&&Batch.FailedCount==0);
    if(Batch.Items.Num()==2){TestEqual(TEXT("A singleton stays ordinary"),Batch.Items[0].Result.ISMInstanceCount,0);TestEqual(TEXT("Only B's d objects instance"),Batch.Items[1].Result.ISMInstanceCount,2);}
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCISMReloadTest,"BuildingBlueprintCreator.ISMReload",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FBBCISMReloadTest::RunTest(const FString& Parameters)
{
    using namespace BBCISMTest;TArray<FString> Lines;FFileHelper::LoadFileToStringArray(Lines,*Manifest());
    if(!TestEqual(TEXT("ISM roundtrip manifest exists"),Lines.Num(),4))return false;
    auto* World=FAutomationEditorCommonUtils::CreateNewMap();auto* BP=LoadObject<UBlueprint>(nullptr,*Lines[0]);
    if(!TestNotNull(TEXT("ISM building reloads from disk"),BP))return false;
    FVector Pivot;Pivot.InitFromString(Lines[1]);FTransform A,B;A.InitFromString(Lines[2]);B.InitFromString(Lines[3]);
    auto* Building=World->SpawnActor<AActor>(BP->GeneratedClass,Pivot,FRotator::ZeroRotator);Check(*this,Building,{A,B});
    return !HasAnyErrors();
}
#endif
