#if WITH_DEV_AUTOMATION_TESTS
#include "BuildingBlueprintLibrary.h"
#include "BuildingOutlinerVisibility.h"
#include "BuildingThumbnailRenderer.h"
#include "Components/StaticMeshComponent.h"
#include "AssetThumbnail.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Crc.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/ObjectThumbnail.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "ObjectTools.h"
#include "RenderingThread.h"
#include "Tests/AutomationEditorCommon.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Widgets/SWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"

namespace BBCThumbnailTest
{
static FString Manifest() { return FPaths::ProjectSavedDir()/TEXT("BuildingBlueprintCreator/thumbnail_roundtrip.txt"); }
static TArray<FThumbnailRenderingInfo> Registrations()
{
    auto& Manager=UThumbnailManager::Get();
    auto* Property=FindFProperty<FArrayProperty>(Manager.GetClass(),TEXT("RenderableThumbnailTypes"));
    return *Property->ContainerPtrToValuePtr<TArray<FThumbnailRenderingInfo>>(&Manager);
}
static bool ReadDisk(UBlueprint* BP, FObjectThumbnail& Out)
{
    const FName Key(*UClass::ConvertFullNameToShortTypeFullName(BP->GetFullName()));
    FThumbnailMap Map;
    const FString File=FPackageName::LongPackageNameToFilename(BP->GetOutermost()->GetName(),FPackageName::GetAssetPackageExtension());
    if(!ThumbnailTools::LoadThumbnailsFromPackage(File,{Key},Map))return false;
    const FObjectThumbnail* Found=Map.Find(Key);
    if(!Found)return false;
    Out=*Found;return !Out.IsEmpty()&&Out.HasValidImageData();
}
static uint32 Digest(FObjectThumbnail& Thumb)
{
    auto& Bytes=Thumb.AccessCompressedImageData();return FCrc::MemCrc32(Bytes.GetData(),Bytes.Num());
}
static bool SavePNG(FObjectThumbnail& Thumb,const FString& Name)
{
    const auto& Bytes=Thumb.GetUncompressedImageData();
    if(Bytes.Num()!=Thumb.GetImageWidth()*Thumb.GetImageHeight()*4)return false;
    TArray<FColor> Pixels;Pixels.SetNumUninitialized(Bytes.Num()/4);
    FMemory::Memcpy(Pixels.GetData(),Bytes.GetData(),Bytes.Num());
    TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Thumb.GetImageWidth(),Thumb.GetImageHeight(),Pixels,PNG);
    return FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/Name));
}
}

class FBBCCaptureThumbnailWidgets final : public IAutomationLatentCommand
{
public:
    FBBCCaptureThumbnailWidgets(TSharedRef<SWindow> InWindow,FAutomationTestBase* InTest,
        TSharedRef<FAssetThumbnailPool> InPool,TArray<TSharedPtr<FAssetThumbnail>> InThumbnails,TSharedRef<TSet<FName>> InRendered)
        :Window(InWindow),Test(InTest),Pool(InPool),Thumbnails(MoveTemp(InThumbnails)),Rendered(InRendered),Start(FPlatformTime::Seconds()){}
    bool Update() override
    {
        Window->SlatePrepass();
        for(const auto& Thumbnail:Thumbnails)Thumbnail->GetViewportRenderTargetTexture();
        Pool->Tick(.016f);FSlateApplication::Get().Tick();FlushRenderingCommands();
        const double Elapsed=FPlatformTime::Seconds()-Start;
        if(Elapsed<2||(Rendered->Num()<2&&Elapsed<15))return false;
        Test->TestTrue(TEXT("Content Browser pool renders the new building tile"),Rendered->Contains(TEXT("BP_ThumbnailBuilding")));
        Test->TestTrue(TEXT("Content Browser pool renders old building without saved thumbnail"),Rendered->Contains(TEXT("BP_LegacyBuilding")));
        TArray<FColor> Pixels;FIntVector Size;
        if(Test->TestTrue(TEXT("Native Content Browser thumbnail widgets screenshot"),FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(),Pixels,Size)))
        {
            TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
            Test->TestTrue(TEXT("Thumbnail widgets screenshot saved"),FFileHelper::SaveArrayToFile(PNG,
                *(FPaths::ProjectSavedDir()/TEXT("BuildingBlueprintCreator_thumbnail_widgets.png"))));
        }
        Window->RequestDestroyWindow();Window.Reset();return true;
    }
private:
    TSharedPtr<SWindow> Window;
    FAutomationTestBase* Test;
    TSharedPtr<FAssetThumbnailPool> Pool;
    TArray<TSharedPtr<FAssetThumbnail>> Thumbnails;
    TSharedRef<TSet<FName>> Rendered;
    double Start;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCThumbnailTest,"BuildingBlueprintCreator.ThumbnailPreview",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)

bool FBBCThumbnailTest::RunTest(const FString& Parameters)
{
    using namespace BBCThumbnailTest;
    UWorld* World=FAutomationEditorCommonUtils::CreateNewMap();
    BBCThumbnails::Stop();
    const auto OriginalEntries=Registrations();
    BBCThumbnails::Start();
    const auto WrappedEntries=Registrations();
    TestEqual(TEXT("Renderer registration count unchanged"),OriginalEntries.Num(),WrappedEntries.Num());
    for(int32 I=0;I<FMath::Min(OriginalEntries.Num(),WrappedEntries.Num());++I)
    {
        TestEqual(TEXT("Specialized thumbnail priority unchanged"),OriginalEntries[I].ClassNeedingThumbnailName,WrappedEntries[I].ClassNeedingThumbnailName);
        TestEqual(TEXT("Renderer config unchanged"),OriginalEntries[I].RendererClassName,WrappedEntries[I].RendererClassName);
        if(OriginalEntries[I].ClassNeedingThumbnail!=UBlueprint::StaticClass())
            TestEqual(TEXT("Other asset renderer preserved"),OriginalEntries[I].Renderer.Get(),WrappedEntries[I].Renderer.Get());
    }
    const FString Folder=TEXT("/Game/BBCThumbnail_")+FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
    UBlueprint* Door=FKismetEditorUtilities::CreateBlueprint(AStaticMeshActor::StaticClass(),CreatePackage(*(Folder/TEXT("BP_Door"))),
        TEXT("BP_Door"),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass());
    FKismetEditorUtilities::CompileBlueprint(Door);
    auto* Mesh=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
    auto* Material=LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
    auto* DoorDefaults=CastChecked<AStaticMeshActor>(Door->GeneratedClass->GetDefaultObject())->GetStaticMeshComponent();
    DoorDefaults->SetStaticMesh(Mesh);DoorDefaults->SetMaterial(0,Material);
    FSavePackageArgs Save;Save.TopLevelFlags=RF_Public|RF_Standalone;
    TestTrue(TEXT("Nested source blueprint saves"),UPackage::SavePackage(Door->GetOutermost(),Door,
        *FPackageName::LongPackageNameToFilename(Folder/TEXT("BP_Door"),FPackageName::GetAssetPackageExtension()),Save));
    TArray<AActor*> Sources;
    auto Add=[&](FVector Location,FVector Scale)
    {
        auto* A=World->SpawnActor<AStaticMeshActor>(Location+FVector(15000,-8000,400),FRotator::ZeroRotator);
        A->GetStaticMeshComponent()->SetStaticMesh(Mesh);A->GetStaticMeshComponent()->SetMaterial(0,Material);
        A->SetActorScale3D(Scale);Sources.Add(A);
    };
    Add(FVector(0,0,0),FVector(6,4,.25));
    Add(FVector(-250,0,150),FVector(.25,4,3));
    Add(FVector(250,0,150),FVector(.25,4,3));
    Add(FVector(0,150,150),FVector(5,.25,3));
    Add(FVector(0,0,325),FVector(6,4,.25));
    AActor* DoorActor=World->SpawnActor<AActor>(Door->GeneratedClass,FVector(15000,-8175,475),FRotator(0,20,0));
    DoorActor->SetActorScale3D(FVector(.8,.25,1.5));Sources.Add(DoorActor);
    FBuildingBlueprintOptions Options;Options.FolderPath=Folder;Options.AssetName=TEXT("BP_ThumbnailBuilding");
    const FBuildingBlueprintResult Result=UBuildingBlueprintLibrary::CreateBuildingBlueprint(Sources,Options);
    if(!TestTrue(TEXT("Building fixture creates: ")+Result.Message,Result.bSuccess))return false;
    UBlueprint* BP=LoadObject<UBlueprint>(nullptr,*Result.AssetObjectPath);
    UBlueprintThumbnailRenderer* Native=NewObject<UBlueprintThumbnailRenderer>();
    TestFalse(TEXT("Native renderer rejects this child-actor-only blueprint"),Native->CanVisualizeAsset(BP));
    auto* Info=UThumbnailManager::Get().GetRenderingInfo(BP);
    if(!TestTrue(TEXT("Content Browser selects our renderer"),Info&&Info->Renderer&&Info->Renderer->IsA<UBuildingThumbnailRenderer>()))return false;
    TestTrue(TEXT("Ordinary mesh blueprint retains native visualization"),Info->Renderer->CanVisualizeAsset(Door)==Native->CanVisualizeAsset(Door));
    {
        struct FTestScene : FBlueprintThumbnailScene { FBoxSphereBounds Bounds() const {return GetPreviewActorBounds();} } Scene;
        Scene.SetBlueprint(BP);
        const FVector Extent=Scene.Bounds().BoxExtent;
        TestTrue(TEXT("Camera bounds cover complete building, including roof"),Extent.X>=299&&Extent.Y>=199&&Extent.Z>=170);
        int32 Parts=0;
        for(TActorIterator<AStaticMeshActor> It(Scene.GetWorld());It;++It)if(It->IsChildActor())++Parts;
        TestEqual(TEXT("Preview spawns all five meshes and nested source blueprint"),Parts,6);
    }
    FObjectThumbnail Disk;
    if(!TestTrue(TEXT("New building thumbnail is already saved inside uasset"),ReadDisk(BP,Disk)))return false;
    TestTrue(TEXT("Thumbnail has usable dimensions"),Disk.GetImageWidth()>=128&&Disk.GetImageHeight()>=128);
    TestTrue(TEXT("Saved preview PNG"),SavePNG(Disk,TEXT("BuildingBlueprintCreator_thumbnail.png")));
    TestEqual(TEXT("Thumbnail rendering does not remove original actors"),Sources.Num(),6);
    for(AActor* A:Sources)TestTrue(TEXT("Original actor is unchanged and valid"),IsValid(A));
    TestFalse(TEXT("Rendering does not dirty saved building"),BP->GetOutermost()->IsDirty());

    // Legacy buildings must render without converting their structure or saving.
    BP->GetOutermost()->GetMetaData().RemoveValue(BP,TEXT("BuildingBlueprintCreator.GeneratedBuilding"));
    BP->GetOutermost()->SetDirtyFlag(false);
    TestTrue(TEXT("Legacy unmarked building gets renderer"),Info->Renderer->CanVisualizeAsset(BP));
    FObjectThumbnail Legacy;ThumbnailTools::RenderThumbnail(BP,256,256,ThumbnailTools::EThumbnailTextureFlushMode::NeverFlush,nullptr,&Legacy);
    TestTrue(TEXT("Legacy building produces pixels"),Legacy.HasValidImageData());
    TestFalse(TEXT("Legacy preview is read-only"),BP->GetOutermost()->IsDirty());

    // Save a truly old-style fixture: no marker and no cached thumbnail.
    Options.AssetName=TEXT("BP_LegacyBuilding");
    const auto OldResult=UBuildingBlueprintLibrary::CreateBuildingBlueprint(Sources,Options);
    if(!TestTrue(TEXT("Legacy fixture creates"),OldResult.bSuccess))return false;
    UBlueprint* Old=LoadObject<UBlueprint>(nullptr,*OldResult.AssetObjectPath);
    Old->GetOutermost()->GetMetaData().RemoveValue(Old,TEXT("BuildingBlueprintCreator.GeneratedBuilding"));
    Old->GetOutermost()->AccessThumbnailMap().Empty();
    TestTrue(TEXT("Unmarked legacy fixture saves without thumbnail"),UPackage::SavePackage(Old->GetOutermost(),Old,
        *FPackageName::LongPackageNameToFilename(Old->GetOutermost()->GetName(),FPackageName::GetAssetPackageExtension()),Save));
    FObjectThumbnail Missing;TestFalse(TEXT("Legacy file has no saved preview"),ReadDisk(Old,Missing));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Manifest()),true);
    FFileHelper::SaveStringToFile(Result.AssetObjectPath+TEXT("\n")+FString::Printf(TEXT("%u"),Digest(Disk))
        +TEXT("\n")+OldResult.AssetObjectPath,*Manifest());
    // Exercise the exact thumbnail pool and Slate widget used by Content Browser,
    // independently of its unrelated virtual-folder discovery in RenderOffscreen.
    auto Pool=MakeShared<FAssetThumbnailPool>(4);
    auto Rendered=MakeShared<TSet<FName>>();
    Pool->OnThumbnailRendered().AddLambda([Rendered](const FAssetData& Asset){Rendered->Add(Asset.AssetName);});
    TArray<TSharedPtr<FAssetThumbnail>> Thumbnails;
    auto NewThumb=MakeShared<FAssetThumbnail>(BP,256,256,Pool);
    auto OldThumb=MakeShared<FAssetThumbnail>(Old,256,256,Pool);
    Thumbnails.Add(NewThumb);Thumbnails.Add(OldThumb);
    TSharedRef<SWindow> Window=SNew(SWindow).Title(FText::FromString(TEXT("内容浏览器原生缩略图控件验证"))).ClientSize(FVector2D(600,320));
    Window->SetContent(SNew(SHorizontalBox)
        +SHorizontalBox::Slot().Padding(12)[SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[SNew(SBox).WidthOverride(256).HeightOverride(256)[NewThumb->MakeThumbnailWidget()]]
            +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(TEXT("BP_ThumbnailBuilding")))]]
        +SHorizontalBox::Slot().Padding(12)[SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[SNew(SBox).WidthOverride(256).HeightOverride(256)[OldThumb->MakeThumbnailWidget()]]
            +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(TEXT("BP_LegacyBuilding")))]]);
    FSlateApplication::Get().AddWindow(Window);
    ADD_LATENT_AUTOMATION_COMMAND(FBBCCaptureThumbnailWidgets(Window,this,Pool,Thumbnails,Rendered));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCThumbnailReloadTest,"BuildingBlueprintCreator.ThumbnailReload",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)

bool FBBCThumbnailReloadTest::RunTest(const FString& Parameters)
{
    using namespace BBCThumbnailTest;
    TArray<FString> Lines;FFileHelper::LoadFileToStringArray(Lines,*Manifest());
    if(!TestEqual(TEXT("Thumbnail manifest exists"),Lines.Num(),3))return false;
    UBlueprint* BP=LoadObject<UBlueprint>(nullptr,*Lines[0]);
    if(!TestNotNull(TEXT("Building reloads independently"),BP))return false;
    FObjectThumbnail Disk;
    if(!TestTrue(TEXT("Thumbnail reloads from disk without rendering"),ReadDisk(BP,Disk)))return false;
    TestEqual(TEXT("Saved thumbnail survives process restart"),Digest(Disk),static_cast<uint32>(FCString::Strtoui64(*Lines[1],nullptr,10)));
    TestFalse(TEXT("Reading stored preview does not dirty asset"),BP->GetOutermost()->IsDirty());
    if(FParse::Param(FCommandLine::Get(),TEXT("BBCFreshReload")))
        TestNull(TEXT("Legacy building starts unloaded in fresh process"),FindObject<UBlueprint>(nullptr,*Lines[2]));
    UBlueprint* Legacy=LoadObject<UBlueprint>(nullptr,*Lines[2]);
    if(!TestNotNull(TEXT("Unmarked old building loads"),Legacy))return false;
    auto* Info=UThumbnailManager::Get().GetRenderingInfo(Legacy);
    if(!TestTrue(TEXT("Previously saved unmarked building gets renderer after reload"),Info&&Info->Renderer&&Info->Renderer->IsA<UBuildingThumbnailRenderer>()))return false;
    FObjectThumbnail OldPreview;ThumbnailTools::RenderThumbnail(Legacy,256,256,ThumbnailTools::EThumbnailTextureFlushMode::NeverFlush,nullptr,&OldPreview);
    TestTrue(TEXT("Old building preview renders on demand after reload"),SavePNG(OldPreview,TEXT("BuildingBlueprintCreator_legacy_thumbnail_reloaded.png")));
    TestFalse(TEXT("Old building preview does not modify old asset"),Legacy->GetOutermost()->IsDirty());
    return !HasAnyErrors();
}
#endif
