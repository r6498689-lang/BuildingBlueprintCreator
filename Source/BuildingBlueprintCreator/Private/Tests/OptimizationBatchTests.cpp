#if WITH_DEV_AUTOMATION_TESTS
#include "BuildingOptimizationBatch.h"
#include "BuildingOptimizationMemory.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/StaticMeshActor.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Tests/AutomationEditorCommon.h"

class FWaitOptimizationBatch : public IAutomationLatentCommand
{
    FAutomationTestBase* Test;double Started=FPlatformTime::Seconds(),Finished=0;FString Folder;int32 Phase=0;TArray<FAssetData> Sources;
public:
    FWaitOptimizationBatch(FAutomationTestBase* In,const FString& F):Test(In),Folder(F){}
    bool Update()override
    {
        if(FPlatformTime::Seconds()-Started>120){BBCOptimizationBatch::Stop();Test->AddError(TEXT("Optimization batch timeout"));return true;}
        if(BBCOptimizationBatch::IsRunning())return false;
        if(Phase==1)
        {
            const auto& R=BBCOptimizationBatch::LastResult();Test->TestEqual(TEXT("Batch replacement succeeds for both sources"),R.CreatedCount,2);
            for(const auto& I:R.Items){Test->TestEqual(TEXT("Batch replacement keeps original path"),I.Result.AssetObjectPath,I.SourceObjectPath);Test->TestTrue(TEXT("Every replacement has a recorded backup"),!I.Result.BackupDirectory.IsEmpty());}
            FBuildingBlueprintOptimizationOptions O;
            Test->TestTrue(TEXT("Another batch can start after completion"),BBCOptimizationBatch::Start(Sources,Folder/TEXT("Cancelled"),O,{}));BBCOptimizationBatch::Cancel();Phase=2;return false;
        }
        if(Phase==2)
        {
            const auto& R=BBCOptimizationBatch::LastResult();Test->TestEqual(TEXT("Cancel prevents queued asset creation"),R.CreatedCount,0);
            Test->TestEqual(TEXT("Cancelled items explicitly reported"),R.FailedCount,2);
            Test->TestFalse(TEXT("Cancelled destination has no new asset"),FPackageName::DoesPackageExist(Folder/TEXT("Cancelled/House_ISM")));
            return true;
        }
        if(!Finished){Finished=FPlatformTime::Seconds();return false;}
        if(FPlatformTime::Seconds()-Finished<16)return false;
        const auto& R=BBCOptimizationBatch::LastResult();
        Test->TestEqual(TEXT("Two compatible assets succeed"),R.CreatedCount,2);Test->TestEqual(TEXT("Invalid blueprint fails without stopping next item"),R.FailedCount,1);
        Test->TestEqual(TEXT("Duplicate source is not processed twice"),R.Items.Num(),3);
        Test->TestTrue(TEXT("First copy uses shared destination"),FPackageName::DoesPackageExist(Folder/TEXT("House_ISM")));
        Test->TestTrue(TEXT("Same source names get unique numbered outputs"),FPackageName::DoesPackageExist(Folder/TEXT("House_ISM_1")));
        FString Report;Test->TestTrue(TEXT("Batch report persists"),FFileHelper::LoadFileToString(Report,*R.ReportFilePath));
        Test->TestTrue(TEXT("Report includes failures and memory file"),Report.Contains(TEXT("失败/未处理"))&&Report.Contains(BBCMemory::File()));
        FString CSV;Test->TestTrue(TEXT("Memory CSV persists"),FFileHelper::LoadFileToString(CSV,*BBCMemory::File()));
        for(const TCHAR* Stage:{TEXT("BeforeSaveAll"),TEXT("AfterSaveAll"),TEXT("BeforeAssetLoad"),TEXT("BeforeOptimization"),TEXT("AfterCandidateCompile"),TEXT("AfterPreviewCleanup"),TEXT("BetweenItems_"),TEXT("DeferredMin5Seconds_"),TEXT("DeferredMin15Seconds_QuietObserved")})
            Test->TestTrue(FString(TEXT("Memory contains stage "))+Stage,CSV.Contains(Stage));
        for(const auto& I:R.Items)if(I.Result.bSuccess)Sources.Add(FAssetData(LoadObject<UBlueprint>(nullptr,*I.SourceObjectPath)));
        FBuildingBlueprintOptimizationOptions O;O.bReplaceOriginal=true;
        Test->TestTrue(TEXT("Batch supports replacement mode with same settings"),BBCOptimizationBatch::Start(Sources,TEXT(""),O,{}));Phase=1;
        return false;
    }
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCOptimizationBatchTest,"BuildingBlueprintCreator.OptimizationBatch",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FBBCOptimizationBatchTest::RunTest(const FString&)
{
    auto* W=FAutomationEditorCommonUtils::CreateNewMap();auto* Root=W->SpawnActor<AActor>();auto* R=NewObject<USceneComponent>(Root);Root->AddInstanceComponent(R);Root->SetRootComponent(R);R->SetMobility(EComponentMobility::Static);R->RegisterComponent();
    for(int32 I=0;I<2;++I)
    {
        auto* A=W->SpawnActor<AStaticMeshActor>(FVector(I*200,0,100),FRotator::ZeroRotator);
        A->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
        A->GetStaticMeshComponent()->SetMaterial(0,LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial")));
        A->AttachToActor(Root,FAttachmentTransformRules::KeepWorldTransform);
    }
    const FString Folder=TEXT("/Game/BBCOptBatch_")+FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
    TestTrue(TEXT("Save fixture map before automated Save All"), FEditorFileUtils::SaveMap(W, Folder/TEXT("TestMap")));
    TArray<FAssetData> Assets;
    for(int32 I=0;I<2;++I)
    {
        FBuildingBlueprintOptions O;O.FolderPath=Folder/FString::Printf(TEXT("Source%d"),I);O.AssetName=TEXT("House");O.bIncludeAllAttachedChildren=true;
        auto Made=UBuildingBlueprintLibrary::CreateBuildingBlueprint({Root},O);if(!TestTrue(TEXT("Source blueprint fixture created"),Made.bSuccess))return false;
        Assets.Add(FAssetData(LoadObject<UBlueprint>(nullptr,*Made.AssetObjectPath)));
    }
    W->GetOutermost()->MarkPackageDirty(); // Save All must persist the level too.
    auto* Invalid=FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),CreatePackage(*(Folder/TEXT("Unrelated"))),TEXT("Unrelated"),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass());
    Assets.Insert(FAssetData(Invalid),1);const FAssetData Duplicate=Assets[0];Assets.Add(Duplicate);
    FBuildingBlueprintOptimizationOptions O;
    TestTrue(TEXT("Batch queue accepts shared settings and output folder"),BBCOptimizationBatch::Start(Assets,Folder/TEXT("Output"),O,{}));
    TestFalse(TEXT("Second concurrent optimization queue is refused"),BBCOptimizationBatch::Start(Assets,Folder/TEXT("Other"),O,{}));
    ADD_LATENT_AUTOMATION_COMMAND(FWaitOptimizationBatch(this,Folder/TEXT("Output")));
    return true;
}
#endif
