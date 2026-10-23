#if WITH_DEV_AUTOMATION_TESTS
#include "BuildingBlueprintLibrary.h"
#include "BuildingOutlinerVisibility.h"
#include "Components/ChildActorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/PointLight.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMeshActor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "ISceneOutliner.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SceneOutlinerModule.h"
#include "SSceneOutliner.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "Widgets/SWindow.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCOutlinerVisibilityTest,"BuildingBlueprintCreator.OutlinerVisibility",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)

bool FBBCOutlinerVisibilityTest::RunTest(const FString& Parameters)
{
    UWorld* World=FAutomationEditorCommonUtils::CreateNewMap();
    AStaticMeshActor* Source=World->SpawnActor<AStaticMeshActor>(FVector(500,200,100),FRotator::ZeroRotator);
    Source->SetActorLabel(TEXT("原模型"));
    Source->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
    FBuildingBlueprintOptions Options;Options.FolderPath=TEXT("/Game/BBCOutliner_")+FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
    FBuildingBlueprintResult Created=UBuildingBlueprintLibrary::CreateBuildingBlueprint({Source},Options);
    if(!TestTrue(TEXT("Building fixture saves: ")+Created.Message,Created.bSuccess))return false;
    UBlueprint* BP=LoadObject<UBlueprint>(nullptr,*Created.AssetObjectPath);
    UPackage* Package=CreatePackage(*(TEXT("/Temp/BBCOrdinary_")+FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    UBlueprint* Ordinary=FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),Package,TEXT("BP_Ordinary"),
        BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass());
    USCS_Node* Root=Ordinary->SimpleConstructionScript->CreateNode(USceneComponent::StaticClass(),TEXT("OrdinaryRoot"));
    Ordinary->SimpleConstructionScript->AddNode(Root);
    USCS_Node* Part=Ordinary->SimpleConstructionScript->CreateNode(UChildActorComponent::StaticClass(),TEXT("OrdinaryPart"));Root->AddChildNode(Part);
    CastChecked<UChildActorComponent>(Part->ComponentTemplate)->SetChildActorClass(AStaticMeshActor::StaticClass());
    FKismetEditorUtilities::CompileBlueprint(Ordinary);
    AActor* Other=World->SpawnActor<AActor>(Ordinary->GeneratedClass);Other->SetActorLabel(TEXT("其他普通蓝图"));
    AActor* OtherChild=Other->FindComponentByClass<UChildActorComponent>()->GetChildActor();
    // Complete all blueprint compilation before creating the observed instance.
    AActor* Building=World->SpawnActor<AActor>(BP->GeneratedClass,Created.Pivot,FRotator::ZeroRotator);
    Building->SetActorLabel(TEXT("生成建筑（仅显示整体）"));
    UChildActorComponent* Component=Building->FindComponentByClass<UChildActorComponent>();
    if(!TestNotNull(TEXT("Building part component exists"),Component))return false;
    AActor* Child=Component->GetChildActor();
    if(!TestTrue(TEXT("Building child is valid"),IsValid(Child)))return false;
    TestTrue(TEXT("Internal actor has parent-component identity"),Child->IsChildActor());
    TestEqual(TEXT("Internal actor belongs to building"),Child->GetParentActor(),Building);
    AActor* Manual=World->SpawnActor<AStaticMeshActor>();Manual->SetActorLabel(TEXT("未替换的挂接对象"));
    Manual->AttachToActor(Building,FAttachmentTransformRules::KeepWorldTransform);
    TestTrue(TEXT("Generated building recognized"),BBCOutliner::IsGeneratedBuilding(Building));
    TestFalse(TEXT("Generated internal actor filtered"),BBCOutliner::ShouldShowActor(Child));
    TestTrue(TEXT("Building root remains visible"),BBCOutliner::ShouldShowActor(Building));
    TestTrue(TEXT("Manual attached actor is not hidden"),BBCOutliner::ShouldShowActor(Manual));
    TestTrue(TEXT("Source level actor remains visible"),BBCOutliner::ShouldShowActor(Source));
    TestTrue(TEXT("Ordinary BP internal actor remains visible"),BBCOutliner::ShouldShowActor(OtherChild));
    // The strict legacy signature works without marking or saving existing assets.
    BP->GetOutermost()->GetMetaData().RemoveValue(BP,TEXT("BuildingBlueprintCreator.GeneratedBuilding"));
    BP->GetOutermost()->SetDirtyFlag(false);
    TestTrue(TEXT("Existing 1.0--1.2 assets recognized without marker"),BBCOutliner::IsGeneratedBuilding(Building));
    TestFalse(TEXT("Legacy building child filtered"),BBCOutliner::ShouldShowActor(Child));
    TestFalse(TEXT("Reading legacy signature does not dirty the asset"),BP->GetOutermost()->IsDirty());
    UChildActorComponent* Nested=NewObject<UChildActorComponent>(Child,TEXT("NestedInsidePart"));
    Child->AddInstanceComponent(Nested);Nested->SetupAttachment(Child->GetRootComponent());Nested->SetChildActorClass(APointLight::StaticClass());Nested->RegisterComponent();
    AActor* Deep=Nested->GetChildActor();
    if(!TestNotNull(TEXT("Nested internal actor exists"),Deep))return false;
    TestFalse(TEXT("Deep internal actor filtered"),BBCOutliner::ShouldShowActor(Deep));
    const FTransform Before=Child->GetActorTransform();
    UStaticMesh* BeforeMesh=Child->FindComponentByClass<UStaticMeshComponent>()->GetStaticMesh();

    FSceneOutlinerInitializationOptions Init;
    Init.bShowHeaderRow=true;
    TSharedRef<ISceneOutliner> Outliner=FModuleManager::LoadModuleChecked<FSceneOutlinerModule>(TEXT("SceneOutliner")).CreateActorBrowser(Init,World);
    TSharedRef<SWindow> Window=SNew(SWindow).Title(FText::FromString(TEXT("建筑大纲显示验证"))).ClientSize(FVector2D(620,600));
    Window->SetContent(Outliner);FSlateApplication::Get().AddWindow(Window);
    auto Tick=[&]
    {
        Window->SlatePrepass();
        for(int32 I=0;I<4;++I) { Outliner->Tick(Outliner->GetCachedGeometry(),FPlatformTime::Seconds(),0.016f);FSlateApplication::Get().Tick(); }
    };
    Tick();
    TestTrue(TEXT("Unfiltered Outliner contains original internal row"),Outliner->GetTreeItem(FObjectKey(Child)).IsValid());
    BBCOutliner::InstallOnOutliner(Outliner);Tick();
    TestTrue(TEXT("Actual Outliner retains building row"),Outliner->GetTreeItem(FObjectKey(Building)).IsValid());
    TestFalse(TEXT("Actual Outliner omits internal actor row"),Outliner->GetTreeItem(FObjectKey(Child)).IsValid());
    TestFalse(TEXT("Actual Outliner omits deep internal row"),Outliner->GetTreeItem(FObjectKey(Deep)).IsValid());
    TestTrue(TEXT("Actual Outliner retains unrelated internal actor"),Outliner->GetTreeItem(FObjectKey(OtherChild)).IsValid());
    TestTrue(TEXT("Actual Outliner retains unconverted attached actor"),Outliner->GetTreeItem(FObjectKey(Manual)).IsValid());
    TestTrue(TEXT("Filter does not destroy model"),IsValid(Child));
    TestTrue(TEXT("Filter does not change transform"),Child->GetActorTransform().Equals(Before,0.001));
    TestEqual(TEXT("Filter does not change mesh"),Child->FindComponentByClass<UStaticMeshComponent>()->GetStaticMesh().Get(),BeforeMesh);
    TestFalse(TEXT("Filter does not dirty original generated asset"),BP->GetOutermost()->IsDirty());
    GEditor->SelectNone(false,true,false);GEditor->SelectActor(Building,true,true);Tick();
    TestTrue(TEXT("Whole building remains selectable"),Building->IsSelected());
    TArray<FColor> Pixels;FIntVector Size;
    if(TestTrue(TEXT("Outliner screenshot captured"),FSlateApplication::Get().TakeScreenshot(Window,Pixels,Size)))
    {
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        TestTrue(TEXT("Outliner screenshot saved"),FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("BuildingBlueprintCreator_outliner.png"))));
    }
    Window->RequestDestroyWindow();
    return !HasAnyErrors();
}
#endif
