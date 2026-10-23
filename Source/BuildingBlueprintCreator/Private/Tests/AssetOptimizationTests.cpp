#if WITH_DEV_AUTOMATION_TESTS
#include "BuildingBlueprintLibrary.h"
#include "BuildingBackupManager.h"
#include "BuildingAssetReplacement.h"
#include "Components/ChildActorComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "ContentBrowserMenuContexts.h"
#include "DatasmithAssetUserData.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "FileHelpers.h"
#include "StaticMeshCompiler.h"
#include "Engine/StaticMesh.h"
#include "Misc/ScopeExit.h"
#include "ImageUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ObjectThumbnail.h"
#include "ObjectTools.h"
#include "Tests/AutomationEditorCommon.h"
#include "ToolMenus.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace BBCAssetTest
{
static FString Manifest(){return FPaths::ProjectSavedDir()/TEXT("BuildingBlueprintCreator/asset_optimizer_roundtrip.txt");}
static AActor* Parent(UWorld* World,const TCHAR* Label,AActor* Parent=nullptr)
{
    auto* A=World->SpawnActor<AActor>();auto* R=NewObject<USceneComponent>(A);
    A->AddInstanceComponent(R);A->SetRootComponent(R);R->SetMobility(EComponentMobility::Static);R->RegisterComponent();A->SetActorLabel(Label);
    if(Parent)A->AttachToActor(Parent,FAttachmentTransformRules::KeepRelativeTransform);return A;
}
static AStaticMeshActor* Mesh(UWorld* World,AActor* Parent,const TCHAR* Label,FVector Local)
{
    auto* A=World->SpawnActor<AStaticMeshActor>();A->SetActorLabel(Label);
    A->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
    A->GetStaticMeshComponent()->SetMaterial(0,LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial")));
    A->AttachToActor(Parent,FAttachmentTransformRules::KeepRelativeTransform);A->SetActorRelativeLocation(Local);return A;
}
static bool Save(UBlueprint* BP)
{
    FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;
    return UPackage::SavePackage(BP->GetOutermost(),BP,*FPackageName::LongPackageNameToFilename(BP->GetOutermost()->GetName(),FPackageName::GetAssetPackageExtension()),Args);
}
struct FGeometry{UStaticMesh* Mesh;TArray<UMaterialInterface*> Materials;FTransform Transform;};
static void Gather(AActor* A,TArray<FGeometry>& Out)
{
    TInlineComponentArray<UStaticMeshComponent*> Meshes(A);
    for(auto* M:Meshes)if(M->GetStaticMesh())
    {
        FGeometry G{M->GetStaticMesh(),{},M->GetComponentTransform()};for(int32 I=0;I<M->GetNumMaterials();++I)G.Materials.Add(M->GetMaterial(I));
        if(auto* ISM=Cast<UInstancedStaticMeshComponent>(M))for(int32 I=0;I<ISM->GetInstanceCount();++I){ISM->GetInstanceTransform(I,G.Transform,true);Out.Add(G);}
        else Out.Add(G);
    }
    TInlineComponentArray<UChildActorComponent*> Children(A);for(auto* C:Children)if(C->GetChildActor())Gather(C->GetChildActor(),Out);
}
static void CompareGeometry(FAutomationTestBase& T,AActor* Source,AActor* Output)
{
    TArray<FGeometry> A,B;Gather(Source,A);Gather(Output,B);T.TestEqual(TEXT("All geometry placements remain"),B.Num(),A.Num());
    TSet<int32> Found;
    for(const auto& G:A)
    {
        int32 Match=INDEX_NONE;
        for(int32 I=0;I<B.Num();++I)if(!Found.Contains(I)&&B[I].Mesh==G.Mesh&&B[I].Materials==G.Materials&&B[I].Transform.Equals(G.Transform,.001)){Match=I;break;}
        T.TestTrue(TEXT("Mesh, materials and world transform uniquely preserved"),Match!=INDEX_NONE);if(Match!=INDEX_NONE)Found.Add(Match);
    }
}
static int32 LevelCount(UWorld* W){int32 Count=0;for(TActorIterator<AActor> It(W);It;++It)++Count;return Count;}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCAssetOptimizationTest,"BuildingBlueprintCreator.AssetOptimization",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FBBCAssetOptimizationTest::RunTest(const FString& Parameters)
{
    using namespace BBCAssetTest;auto* World=FAutomationEditorCommonUtils::CreateNewMap();
    auto* Root=Parent(World,TEXT("LegacyBuilding"));Root->SetActorLocation(FVector(1500,300,100));Root->SetActorRotation(FRotator(0,20,0));
    auto* Facade=Parent(World,TEXT("Facade"),Root);Facade->SetActorRelativeScale3D(FVector(1.5,2,1));
    auto* A=Mesh(World,Facade,TEXT("WindowA"),FVector(100,0,100));auto* B=Mesh(World,Facade,TEXT("WindowB"),FVector(250,0,100));
    B->SetActorRelativeScale3D(FVector(1,1,2));
    for(auto* M:{A,B}){auto* Data=NewObject<UDatasmithAssetUserData>(M->GetStaticMeshComponent());Data->MetaData.Add(TEXT("Part"),M->GetActorLabel());M->GetStaticMeshComponent()->AddAssetUserData(Data);}
    auto* P2=Parent(World,TEXT("OtherFacade"),Root);Mesh(World,P2,TEXT("SameMeshOtherParent"),FVector(400,0,100));
    auto* Different=Mesh(World,Facade,TEXT("OtherMaterial"),FVector(600,0,100));Different->GetStaticMeshComponent()->SetMaterial(0,LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial")));
    auto* Empty=World->SpawnActor<AStaticMeshActor>();Empty->SetActorLabel(TEXT("Empty"));Empty->AttachToActor(Root,FAttachmentTransformRules::KeepRelativeTransform);Mesh(World,Empty,TEXT("UnderEmpty"),FVector(800,0,100));
    FBuildingBlueprintOptions O;O.FolderPath=TEXT("/Game/BBCAssetOptimizer_")+FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
    O.AssetName=TEXT("Nested");auto Nested=UBuildingBlueprintLibrary::CreateBuildingBlueprint({Different},O);
    if(!TestTrue(TEXT("Nested source fixture saves"),Nested.bSuccess))return false;
    auto* NBP=LoadObject<UBlueprint>(nullptr,*Nested.AssetObjectPath);auto* N=World->SpawnActor<AActor>(NBP->GeneratedClass);N->AttachToActor(Root,FAttachmentTransformRules::KeepRelativeTransform);
    O.AssetName=TEXT("Original");O.bIncludeAllAttachedChildren=true;
    auto R=UBuildingBlueprintLibrary::CreateBuildingBlueprint({Root},O);
    if(!TestTrue(TEXT("Legacy uninstanced building created"),R.bSuccess))return false;
    auto* BP=LoadObject<UBlueprint>(nullptr,*R.AssetObjectPath);
    // Existing HISM must remain; only eligible legacy ChildActors are converted.
    auto* HNode=BP->SimpleConstructionScript->CreateNode(UHierarchicalInstancedStaticMeshComponent::StaticClass(),TEXT("ExistingHISM"));
    auto* H=CastChecked<UHierarchicalInstancedStaticMeshComponent>(HNode->ComponentTemplate);H->SetMobility(EComponentMobility::Static);
    H->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Sphere.Sphere")));H->SetMaterial(0,LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial")));
    H->AddInstance(FTransform(FVector(0,500,100)));H->AddInstance(FTransform(FVector(200,500,100)));BP->SimpleConstructionScript->GetRootNodes()[0]->AddChildNode(HNode);
    FKismetEditorUtilities::CompileBlueprint(BP);if(!TestTrue(TEXT("Original with existing HISM saves"),Save(BP)))return false;
    const FString File=FPackageName::LongPackageNameToFilename(BP->GetOutermost()->GetName(),FPackageName::GetAssetPackageExtension());
    TArray<uint8> Before;FFileHelper::LoadFileToArray(Before,*File);UClass* OriginalClass=BP->GeneratedClass;
    const FTransform Placement(FRotator(0,35,0),FVector(3000,900,200),FVector(1.5));
    auto* Placed=World->SpawnActor<AActor>(BP->GeneratedClass,Placement);GEditor->SelectNone(false,true,false);GEditor->SelectActor(Placed,true,false);
    const int32 CountBefore=LevelCount(World);
    auto Optimized=UBuildingBlueprintLibrary::OptimizeBuildingBlueprint(BP,O.FolderPath,TEXT("Original_ISM"));
    if(!TestTrue(TEXT("Existing blueprint optimizes: ")+Optimized.Message,Optimized.bSuccess))return false;
    TestEqual(TEXT("Only two siblings newly instance"),Optimized.ISMInstanceCount,2);TestEqual(TEXT("One new HISM reported"),Optimized.ISMGroupCount,1);
    TestEqual(TEXT("No level actors added or deleted by optimizer"),LevelCount(World),CountBefore);TestTrue(TEXT("Placed source remains selected"),Placed->IsSelected());
    TestEqual(TEXT("Original class unchanged"),BP->GeneratedClass.Get(),OriginalClass);TestFalse(TEXT("Original package stays clean"),BP->GetOutermost()->IsDirty());
    TestEqual(TEXT("Original generated class still belongs to original asset"),BP->GeneratedClass->ClassGeneratedBy.Get(),static_cast<UObject*>(BP));
    TArray<uint8> After;FFileHelper::LoadFileToArray(After,*File);TestTrue(TEXT("Original disk asset byte-identical"),Before==After);
    auto* Copy=LoadObject<UBlueprint>(nullptr,*Optimized.AssetObjectPath);auto* CopyActor=World->SpawnActor<AActor>(Copy->GeneratedClass,Placement);
    CompareGeometry(*this,Placed,CopyActor);
    TInlineComponentArray<UInstancedStaticMeshComponent*> HS(CopyActor);TestEqual(TEXT("Existing HISM plus new HISM"),HS.Num(),2);
    TInlineComponentArray<UHierarchicalInstancedStaticMeshComponent*> OldHISMs(CopyActor);
    TestEqual(TEXT("Only original HISM remains hierarchical"),OldHISMs.Num(),1);
    int32 NewISMCount=0;for(auto* Component:HS)if(Component->GetClass()==UInstancedStaticMeshComponent::StaticClass())++NewISMCount;
    TestEqual(TEXT("Optimization generates exact ISM"),NewISMCount,1);
    FThumbnailMap Thumbnails;const FName ThumbnailKey(*UClass::ConvertFullNameToShortTypeFullName(Copy->GetFullName()));
    const FString CopyFile=FPackageName::LongPackageNameToFilename(Copy->GetOutermost()->GetName(),FPackageName::GetAssetPackageExtension());
    if(TestTrue(TEXT("Optimized copy thumbnail persists"),ThumbnailTools::LoadThumbnailsFromPackage(CopyFile,{ThumbnailKey},Thumbnails)))
    {
        auto* Thumbnail=Thumbnails.Find(ThumbnailKey);
        if(TestNotNull(TEXT("Optimized asset has saved preview"),Thumbnail))
        {
            const auto& Bytes=Thumbnail->GetUncompressedImageData();TArray<FColor> Pixels;
            if(TestTrue(TEXT("Saved optimized preview has pixels"),Bytes.Num()>0&&Bytes.Num()==Thumbnail->GetImageWidth()*Thumbnail->GetImageHeight()*4))
            {
                Pixels.Append(reinterpret_cast<const FColor*>(Bytes.GetData()),Bytes.Num()/4);TArray64<uint8> PNG;
                FImageUtils::PNGCompressImageArray(Thumbnail->GetImageWidth(),Thumbnail->GetImageHeight(),Pixels,PNG);
                FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("BuildingBlueprintCreator_optimized_asset_thumbnail.png")));
            }
        }
    }
    TInlineComponentArray<UChildActorComponent*> OriginalParts(Placed),CopyParts(CopyActor);TestEqual(TEXT("Only converted leaf actors removed"),CopyParts.Num(),OriginalParts.Num()-2);
    auto Second=UBuildingBlueprintLibrary::OptimizeBuildingBlueprint(BP,O.FolderPath,TEXT("Original_ISM"));TestTrue(TEXT("Repeated optimization creates uniquely named copy"),Second.bSuccess&&Second.AssetObjectPath.Contains(TEXT("Original_ISM_1")));
    auto NoMore=UBuildingBlueprintLibrary::OptimizeBuildingBlueprint(Copy,O.FolderPath,TEXT("NoChange"));TestFalse(TEXT("No-op optimization creates no redundant copy"),NoMore.bSuccess);
    TestFalse(TEXT("No-op destination absent"),FPackageName::DoesPackageExist(O.FolderPath/TEXT("NoChange")));
    BP->GetOutermost()->SetDirtyFlag(true);TestTrue(TEXT("Unsaved original is refused"),UBuildingBlueprintLibrary::GetBuildingOptimizationReason(BP).Contains(TEXT("保存")));BP->GetOutermost()->SetDirtyFlag(false);
    BP->NewVariables.AddDefaulted();TestTrue(TEXT("Custom variables require preserving original behavior"),UBuildingBlueprintLibrary::GetBuildingOptimizationReason(BP).Contains(TEXT("自定义变量")));BP->NewVariables.Pop();
    auto* Ordinary=FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),CreatePackage(*(O.FolderPath/TEXT("Unrelated"))),TEXT("Unrelated"),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass());
    TestFalse(TEXT("Unrelated BP cannot be optimized"),UBuildingBlueprintLibrary::GetBuildingOptimizationReason(Ordinary).IsEmpty());
    TArray<FString> Lines{R.AssetObjectPath,Optimized.AssetObjectPath,Placement.ToString()};IFileManager::Get().MakeDirectory(*FPaths::GetPath(Manifest()),true);FFileHelper::SaveStringArrayToFile(Lines,*Manifest());
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCAssetOptimizationReloadTest,"BuildingBlueprintCreator.AssetOptimizationReload",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FBBCAssetOptimizationReloadTest::RunTest(const FString& Parameters)
{
    using namespace BBCAssetTest;TArray<FString> Lines;FFileHelper::LoadFileToStringArray(Lines,*Manifest());
    if(!TestEqual(TEXT("Optimizer manifest saved"),Lines.Num(),3))return false;
    auto* W=FAutomationEditorCommonUtils::CreateNewMap();auto* A=LoadObject<UBlueprint>(nullptr,*Lines[0]);auto* B=LoadObject<UBlueprint>(nullptr,*Lines[1]);
    if(!TestNotNull(TEXT("Original reloads independently"),A)||!TestNotNull(TEXT("Optimized copy reloads independently"),B))return false;
    FTransform Placement;Placement.InitFromString(Lines[2]);auto* Original=W->SpawnActor<AActor>(A->GeneratedClass,Placement);auto* Optimized=W->SpawnActor<AActor>(B->GeneratedClass,Placement);
    CompareGeometry(*this,Original,Optimized);TInlineComponentArray<UInstancedStaticMeshComponent*> HS(Optimized);TestEqual(TEXT("Both saved HISM components reload"),HS.Num(),2);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCAssetOptimizationMenuTest,"BuildingBlueprintCreator.AssetOptimizationMenu",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FBBCAssetOptimizationMenuTest::RunTest(const FString& Parameters)
{
    using namespace BBCAssetTest;TArray<FString> Lines;FFileHelper::LoadFileToStringArray(Lines,*Manifest());if(Lines.IsEmpty())return false;
    auto* BP=LoadObject<UBlueprint>(nullptr,*Lines[0]);auto* Context=NewObject<UContentBrowserAssetContextMenuContext>();Context->SelectedAssets.Add(FAssetData(BP));Context->CommonClass=UBlueprint::StaticClass();Context->bCanBeModified=true;Context->bCanView=true;
    FToolMenuContext MenuContext(Context);auto* Menu=UToolMenus::Get()->GenerateMenu(TEXT("ContentBrowser.AssetContextMenu.Blueprint"),MenuContext);
    auto* Section=Menu->FindSection(TEXT("BuildingBlueprintOptimization"));
    auto* Entry=Section?Section->FindEntry(TEXT("OptimizeBuildingISM")):nullptr;TestNotNull(TEXT("Real blueprint asset menu contains optimizer"),Entry);
    if(Entry)TestEqual(TEXT("Exact requested menu label"),Entry->Label.Get().ToString(),FString(TEXT("优化建筑蓝图…")));
    TSharedRef<SWindow> Window=SNew(SWindow).Title(NSLOCTEXT("BBC","AssetMenuTest","内容浏览器蓝图右键菜单验证")).ClientSize(FVector2D(640,760));
    Window->SetContent(UToolMenus::Get()->GenerateWidget(Menu));FSlateApplication::Get().AddWindow(Window);Window->SlatePrepass();FSlateApplication::Get().Tick();
    TArray<FColor> Pixels;FIntVector Size;if(TestTrue(TEXT("Actual asset menu screenshot captured"),FSlateApplication::Get().TakeScreenshot(Window,Pixels,Size)))
    {TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("BuildingBlueprintCreator_asset_menu.png")));}
    Window->RequestDestroyWindow();return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCOptimizationModesTest,"BuildingBlueprintCreator.OptimizationModes",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FBBCOptimizationModesTest::RunTest(const FString& Parameters)
{
    using namespace BBCAssetTest; auto* W=FAutomationEditorCommonUtils::CreateNewMap();
    const FString Folder=TEXT("/Game/BBCModes_")+FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
    auto* Model=DuplicateObject<UStaticMesh>(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")),CreatePackage(*(Folder/TEXT("Model"))),TEXT("Model"));
    Model->SetFlags(RF_Public|RF_Standalone|RF_Transactional);
    Model->GetStaticMaterials().Reset();Model->GetStaticMaterials().Add(FStaticMaterial(LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"))));
    Model->NaniteSettings.bEnabled=false;Model->PostEditChange();TArray<UStaticMesh*> Meshes{Model};FStaticMeshCompilingManager::Get().FinishCompilation(Meshes);
    FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;
    TestTrue(TEXT("Saved original project mesh"),UPackage::SavePackage(Model->GetOutermost(),Model,*FPackageName::LongPackageNameToFilename(Model->GetOutermost()->GetName(),FPackageName::GetAssetPackageExtension()),Args));
    auto* Root=Parent(W,TEXT("Building"));auto* A=Mesh(W,Root,TEXT("A"),FVector(100,0,50));auto* B=Mesh(W,Root,TEXT("B"),FVector(300,0,50));
    A->GetStaticMeshComponent()->SetStaticMesh(Model);B->GetStaticMeshComponent()->SetStaticMesh(Model);
    FBuildingBlueprintOptions Create;Create.FolderPath=Folder;Create.AssetName=TEXT("Original");Create.bIncludeAllAttachedChildren=true;
    auto Made=UBuildingBlueprintLibrary::CreateBuildingBlueprint({Root},Create);if(!TestTrue(TEXT("Mode fixture created"),Made.bSuccess))return false;
    const FString OriginalPath=Made.AssetObjectPath;auto* Source=LoadObject<UBlueprint>(nullptr,*OriginalPath);
    const FString File=FPackageName::LongPackageNameToFilename(Source->GetOutermost()->GetName(),FPackageName::GetAssetPackageExtension());
    TArray<uint8> Before;FFileHelper::LoadFileToArray(Before,*File);
    FBuildingBlueprintOptimizationOptions Options;Options.bUseISM=false;
    auto Empty=UBuildingBlueprintLibrary::OptimizeBuildingBlueprintWithOptions(Source,Folder,TEXT("Invalid"),Options);
    TestFalse(TEXT("No selected optimization is refused"),Empty.bSuccess);TestFalse(TEXT("No unwanted copy"),FPackageName::DoesPackageExist(Folder/TEXT("Invalid")));
    Options.bEnableNanite=true;
    auto Nanite=UBuildingBlueprintLibrary::OptimizeBuildingBlueprintWithOptions(Source,Folder,TEXT("NaniteOnly"),Options);
    if(!TestTrue(TEXT("Nanite only can create copy: ")+Nanite.Message,Nanite.bSuccess))return false;
    TestEqual(TEXT("Nanite only does not generate ISM"),Nanite.ISMGroupCount,0);TestEqual(TEXT("Shared original mesh enabled once"),Nanite.NaniteEnabledCount,1);
    TestTrue(TEXT("Original model has real Nanite data"),Model->NaniteSettings.bEnabled&&Model->HasValidNaniteData());
    AddInfo(FString::Printf(TEXT("After Nanite: source dirty=%d status=%d"),Source->GetOutermost()->IsDirty(),int32(Source->Status)));
    TArray<uint8> Bytes;FFileHelper::LoadFileToArray(Bytes,*File);TestTrue(TEXT("Nanite copy leaves original blueprint bytes"),Before==Bytes);
    // The native static mesh rebuild dirties dependent blueprints. Respect the
    // saved-source precondition between separate user operations; don't clear it.
    TestTrue(TEXT("Save native dependent-blueprint update before next operation"),Save(Source));FFileHelper::LoadFileToArray(Before,*File);
    auto* Baseline=LoadObject<UBlueprint>(nullptr,*Nanite.AssetObjectPath);
    const FTransform Placement(FRotator(0,37,0),FVector(2300,800,100),FVector(1.3));
    auto* Placed=W->SpawnActor<AActor>(Source->GeneratedClass,Placement);const FSoftObjectPath PlacedPath(Placed);
    auto* BaselineActor=W->SpawnActor<AActor>(Baseline->GeneratedClass,Placement);
    auto* Attached=Parent(W,TEXT("ExternalAttachment"));Attached->AttachToActor(Placed,FAttachmentTransformRules::KeepWorldTransform);
    const FTransform AttachedTransform=Attached->GetActorTransform();
    GEditor->SelectNone(false,true,false);GEditor->SelectActor(Placed,true,false);
    AddInfo(FString::Printf(TEXT("After placing: source dirty=%d status=%d"),Source->GetOutermost()->IsDirty(),int32(Source->Status)));
    Options.bUseISM=true;
    auto Both=UBuildingBlueprintLibrary::OptimizeBuildingBlueprintWithOptions(Source,Folder,TEXT("Both"),Options);
    TestTrue(TEXT("ISM and Nanite together create copy: ")+Both.Message,Both.bSuccess&&Both.ISMInstanceCount==2&&Both.NaniteAlreadyEnabledCount==1);
    FBuildingBlueprintOptions Wrap;Wrap.FolderPath=Folder;Wrap.AssetName=TEXT("Referencer");
    auto Wrapped=UBuildingBlueprintLibrary::CreateBuildingBlueprint({Placed},Wrap);
    if(!TestTrue(TEXT("Saved nested reference fixture"),Wrapped.bSuccess))return false;
    auto* Referencer=LoadObject<UBlueprint>(nullptr,*Wrapped.AssetObjectPath);
    auto* WrappedActor=W->SpawnActor<AActor>(Referencer->GeneratedClass,FTransform(FVector(7000,0,0)));
    {TInlineComponentArray<UChildActorComponent*> Check(WrappedActor);AddInfo(FString::Printf(TEXT("Before replace wrapper components=%d target=%s"),Check.Num(),*Source->GeneratedClass->GetPathName()));for(auto* C:Check)AddInfo(FString::Printf(TEXT("Wrapper before %s class=%s actor=%s"),*C->GetName(),*GetPathNameSafe(C->GetChildActorClass()),*GetPathNameSafe(C->GetChildActor()?C->GetChildActor()->GetClass():nullptr)));}
    Options.bReplaceOriginal=true;Options.bEnableNanite=false;
    auto& Platform=FPlatformFileManager::Get().GetPlatformFile();Platform.SetReadOnly(*File,true);
    ON_SCOPE_EXIT{Platform.SetReadOnly(*File,false);};
    auto Blocked=UBuildingBlueprintLibrary::OptimizeBuildingBlueprintWithOptions(Source,TEXT(""),TEXT(""),Options);
    TestFalse(TEXT("Read-only original is not replaced"),Blocked.bSuccess);TestTrue(TEXT("Read-only refusal makes no backup"),Blocked.BackupDirectory.IsEmpty());Platform.SetReadOnly(*File,false);
    // Preserve per-instance edits by refusing replacement before touching Source.
    TInlineComponentArray<UChildActorComponent*> Parts(Placed);
    UStaticMeshComponent* Overridden=nullptr;
    for(auto* P:Parts)if(auto* M=Cast<AStaticMeshActor>(P->GetChildActor())){Overridden=M->GetStaticMeshComponent();break;}
    if(TestNotNull(TEXT("Instance has a mesh part for override protection"),Overridden))
    {
        auto* Old=Overridden->GetMaterial(0);Overridden->SetMaterial(0,LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial")));
        auto Protected=UBuildingBlueprintLibrary::OptimizeBuildingBlueprintWithOptions(Source,TEXT(""),TEXT(""),Options);
        TestFalse(TEXT("Instance-specific material prevents unsafe replacement"),Protected.bSuccess);
        TestEqual(TEXT("Refused replacement keeps source class"),Placed->GetClass(),Source->GeneratedClass.Get());Overridden->SetMaterial(0,Old);
    }
    BBCAssetReplacement::FailNextSaveForTest();
    auto FailedSave=UBuildingBlueprintLibrary::OptimizeBuildingBlueprintWithOptions(Source,TEXT(""),TEXT(""),Options);
    TestFalse(TEXT("Simulated save failure returns failure"),FailedSave.bSuccess);TestTrue(TEXT("Save failure restored original asset"),FailedSave.Message.Contains(TEXT("已恢复原蓝图")));
    Source=LoadObject<UBlueprint>(nullptr,*OriginalPath);Placed=Cast<AActor>(PlacedPath.ResolveObject());
    if(!TestNotNull(TEXT("Placed original resolves after rollback"),Placed))return false;
    CompareGeometry(*this,BaselineActor,Placed);FFileHelper::LoadFileToArray(Bytes,*File);TestTrue(TEXT("Rollback restores original disk bytes"),Before==Bytes);
    for(auto* Node:Source->SimpleConstructionScript->GetAllNodes())
        if(auto* Property=FindFProperty<FObjectPropertyBase>(Source->GeneratedClass,Node->GetVariableName()))
        {UObject* Value=Property->GetObjectPropertyValue_InContainer(Placed);TestTrue(TEXT("Rollback restores valid generated component variables"),Value&&Value->IsA(Node->ComponentClass));}
    auto Replaced=UBuildingBlueprintLibrary::OptimizeBuildingBlueprintWithOptions(Source,TEXT(""),TEXT(""),Options);
    if(!TestTrue(TEXT("Replace original completes: ")+Replaced.Message,Replaced.bSuccess))return false;
    TestEqual(TEXT("Original asset object path kept"),Replaced.AssetObjectPath,OriginalPath);
    TArray<uint8> Backup;FFileHelper::LoadFileToArray(Backup,*(Replaced.BackupDirectory/FPaths::GetCleanFilename(File)));
    TestTrue(TEXT("Backup is byte identical to original"),Backup==Before);
    const auto Records=BBCBackups::List();TestTrue(TEXT("Backup path and original asset recorded"),Records.ContainsByPredicate([&](const BBCBackups::FRecord& R){return R.Directory==Replaced.BackupDirectory&&R.AssetPath==OriginalPath&&R.Bytes>=Before.Num();}));
    auto* Current=LoadObject<UBlueprint>(nullptr,*OriginalPath);auto* CurrentActor=Cast<AActor>(PlacedPath.ResolveObject());
    if(!TestNotNull(TEXT("Placed actor resolves after replacement"),CurrentActor))return false;
    TestTrue(TEXT("Placed actor now uses updated original class"),CurrentActor->GetClass()==Current->GeneratedClass);
    TestTrue(TEXT("Selection preserved"),CurrentActor->IsSelected());CompareGeometry(*this,BaselineActor,CurrentActor);
    TInlineComponentArray<UChildActorComponent*> WrappedParts(WrappedActor);
    AddInfo(FString::Printf(TEXT("After replace wrapper components=%d target=%s"),WrappedParts.Num(),*Current->GeneratedClass->GetPathName()));for(auto* C:WrappedParts)AddInfo(FString::Printf(TEXT("Wrapper after %s class=%s actor=%s"),*C->GetName(),*GetPathNameSafe(C->GetChildActorClass()),*GetPathNameSafe(C->GetChildActor()?C->GetChildActor()->GetClass():nullptr)));
    TestTrue(TEXT("Another blueprint's loaded child reference updated"),WrappedParts.Num()==1&&WrappedParts[0]->GetChildActor()&&WrappedParts[0]->GetChildActor()->GetClass()==Current->GeneratedClass);
    TestEqual(TEXT("External actor attachment remapped"),Attached->GetAttachParentActor(),CurrentActor);TestTrue(TEXT("External actor transform preserved"),Attached->GetActorTransform().Equals(AttachedTransform,.001));
    TestFalse(TEXT("Updated original saved clean"),Current->GetOutermost()->IsDirty());
    TestTrue(TEXT("Updated original remains recognized as plugin building"),UBuildingBlueprintLibrary::GetBuildingOptimizationReason(Current).IsEmpty());
    TInlineComponentArray<UInstancedStaticMeshComponent*> ISMs(CurrentActor);TestEqual(TEXT("Original now uses ISM"),ISMs.Num(),1);
    Options.bUseISM=false;Options.bEnableNanite=true;
    auto NaniteReplace=UBuildingBlueprintLibrary::OptimizeBuildingBlueprintWithOptions(Current,TEXT(""),TEXT(""),Options);
    if(!TestTrue(TEXT("Nanite-only replacement works without remaining ISM candidates: ")+NaniteReplace.Message,NaniteReplace.bSuccess&&NaniteReplace.NaniteEnabledCount==0&&NaniteReplace.NaniteAlreadyEnabledCount==1))return false;
    // Safe cleanup refuses paths outside its root and backups containing unknown content.
    FString Error;TestFalse(TEXT("Cleanup refuses project content path"),BBCBackups::Remove(FPaths::GetPath(File),Error));
    const FString Extra=NaniteReplace.BackupDirectory/TEXT("DoNotDelete.txt");FFileHelper::SaveStringToFile(TEXT("unknown"),*Extra);
    TestFalse(TEXT("Unknown backup content prevents deletion"),BBCBackups::Remove(NaniteReplace.BackupDirectory,Error));
    TestTrue(TEXT("Unknown file preserved"),IFileManager::Get().FileExists(*Extra));IFileManager::Get().Delete(*Extra);
    TestTrue(TEXT("Explicit selected backup can be deleted"),BBCBackups::Remove(NaniteReplace.BackupDirectory,Error));
    TestFalse(TEXT("Selected backup removed"),IFileManager::Get().DirectoryExists(*NaniteReplace.BackupDirectory));
    TestTrue(TEXT("Unselected prior backup remains"),IFileManager::Get().FileExists(*(Replaced.BackupDirectory/TEXT("BackupRecord.txt"))));
    TestTrue(TEXT("Current asset is never deleted by backup cleanup"),IFileManager::Get().FileExists(*File));
    // Save the actual referenced actor level, then validate it in a new UE process.
    TestTrue(TEXT("Save refreshed dependent blueprint reference"),Save(Referencer));
    const FString MapFile=FPackageName::LongPackageNameToFilename(Folder/TEXT("ReferencedLevel"),FPackageName::GetMapPackageExtension());
    TestTrue(TEXT("Referenced level saves after replacement"),FEditorFileUtils::SaveLevel(W->PersistentLevel,MapFile));
    TArray<FString> Manifest{OriginalPath,Nanite.AssetObjectPath,MapFile,CurrentActor->GetName(),Placement.ToString(),Replaced.BackupDirectory,Wrapped.AssetObjectPath};
    FFileHelper::SaveStringArrayToFile(Manifest,*(FPaths::ProjectSavedDir()/TEXT("BuildingBlueprintCreator/optimization_modes.txt")));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCOptimizationReplaceReloadTest,"BuildingBlueprintCreator.OptimizationReplaceReload",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FBBCOptimizationReplaceReloadTest::RunTest(const FString& Parameters)
{
    using namespace BBCAssetTest;TArray<FString> Lines;
    FFileHelper::LoadFileToStringArray(Lines,*(FPaths::ProjectSavedDir()/TEXT("BuildingBlueprintCreator/optimization_modes.txt")));
    if(!TestEqual(TEXT("Replacement roundtrip recorded"),Lines.Num(),7))return false;
    if(!TestTrue(TEXT("Saved referenced level reopens"),FEditorFileUtils::LoadMap(Lines[2],false,false)))return false;
    auto* W=GEditor->GetEditorWorldContext().World();auto* BP=LoadObject<UBlueprint>(nullptr,*Lines[0]);auto* Baseline=LoadObject<UBlueprint>(nullptr,*Lines[1]);
    if(!TestNotNull(TEXT("Original path reloads optimized blueprint"),BP)||!TestNotNull(TEXT("Baseline reference loads"),Baseline))return false;
    auto* Placed=FindObject<AActor>(W->PersistentLevel,*Lines[3]);if(!TestNotNull(TEXT("Level actor still exists by same name"),Placed))return false;
    TestEqual(TEXT("Saved actor class references original asset"),Placed->GetClass(),BP->GeneratedClass.Get());
    FTransform Placement;Placement.InitFromString(Lines[4]);auto* Expected=W->SpawnActor<AActor>(Baseline->GeneratedClass,Placement);CompareGeometry(*this,Expected,Placed);
    TInlineComponentArray<UInstancedStaticMeshComponent*> ISMs(Placed);TestEqual(TEXT("Replaced asset ISM loads from disk"),ISMs.Num(),1);
    if(!ISMs.IsEmpty()){UStaticMesh* Mesh=ISMs[0]->GetStaticMesh();TArray<UStaticMesh*> One{Mesh};FStaticMeshCompilingManager::Get().FinishCompilation(One);TestTrue(TEXT("Shared mesh Nanite setting persists"),Mesh->NaniteSettings.bEnabled&&Mesh->HasValidNaniteData());}
    TestTrue(TEXT("Backup registry survives new process"),BBCBackups::List().ContainsByPredicate([&](const BBCBackups::FRecord& R){return R.Directory==Lines[5]&&R.AssetPath==Lines[0];}));
    auto* Referencer=LoadObject<UBlueprint>(nullptr,*Lines[6]);if(!TestNotNull(TEXT("Dependent blueprint reloads"),Referencer))return false;
    auto* Nested=W->SpawnActor<AActor>(Referencer->GeneratedClass);TInlineComponentArray<UChildActorComponent*> Parts(Nested);
    TestTrue(TEXT("Saved blueprint reference still targets original optimized class"),Parts.Num()==1&&Parts[0]->GetChildActor()&&Parts[0]->GetChildActor()->GetClass()==BP->GeneratedClass);
    return !HasAnyErrors();
}
#endif
