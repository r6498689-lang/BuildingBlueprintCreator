#if WITH_DEV_AUTOMATION_TESTS
#include "BuildingBlueprintLibrary.h"
#include "Components/AudioComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DatasmithAssetUserData.h"
#include "Editor.h"
#include "Editor/GroupActor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/PointLight.h"
#include "Engine/StaticMeshActor.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ObjectTemplates/DatasmithSceneComponentTemplate.h"
#include "Tests/AutomationEditorCommon.h"

namespace BBCHierarchyTest
{
static AActor* Empty(UWorld* World, const TCHAR* Label, FVector Location)
{
    AActor* Actor = World->SpawnActor<AActor>();
    USceneComponent* Root = NewObject<USceneComponent>(Actor,TEXT("ImportedRoot"),RF_Transactional);
    Actor->AddInstanceComponent(Root); Actor->SetRootComponent(Root);
    Root->SetMobility(EComponentMobility::Static); Root->RegisterComponent();
    Actor->SetActorLabel(Label); Actor->SetActorLocation(Location);
    return Actor;
}
static AStaticMeshActor* Mesh(UWorld* World, const TCHAR* Label, FVector Location)
{
    AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location,FRotator(0,15,0));
    Actor->SetActorLabel(Label);
    Actor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
    Actor->GetStaticMeshComponent()->SetMaterial(0,LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial")));
    return Actor;
}
static UChildActorComponent* Part(AActor* Building, const TCHAR* Label)
{
    TInlineComponentArray<UChildActorComponent*> Components(Building);
    const FString Suffix=TEXT("_")+FString(Label);
    for (UChildActorComponent* Component:Components) if (Component->GetName().EndsWith(Suffix)) return Component;
    return nullptr;
}
static void CheckHierarchy(FAutomationTestBase& Test,AActor* Instance,const TArray<FString>& Lines,FVector Offset)
{
    const TCHAR* Names[]={TEXT("TownRoot_grp"),TEXT("Inner_grp"),TEXT("Wall_A"),TEXT("Wall_B")};
    for (int32 I=0;I<4;++I)
    {
        UChildActorComponent* Node=Part(Instance,Names[I]);
        if (!Test.TestNotNull(TEXT("Hierarchy part exists: ")+FString(Names[I]),Node)) continue;
        if (!Test.TestNotNull(TEXT("Hierarchy child exists"),Node->GetChildActor())) continue;
        FTransform Expected;Expected.InitFromString(Lines[I]);Expected.AddToTranslation(Offset);
        Test.TestTrue(TEXT("Hierarchy world transform preserved: ")+FString(Names[I]),Node->GetChildActor()->GetActorTransform().Equals(Expected,0.001));
        Test.TestNull(TEXT("No editor group references in runtime child"),Node->GetChildActor()->GroupActor.Get());
    }
    UChildActorComponent* Root=Part(Instance,TEXT("TownRoot_grp"));
    UChildActorComponent* Inner=Part(Instance,TEXT("Inner_grp"));
    UChildActorComponent* A=Part(Instance,TEXT("Wall_A"));
    UChildActorComponent* B=Part(Instance,TEXT("Wall_B"));
    if (Root && Inner && A && B)
    {
        Test.TestEqual(TEXT("Inner empty node stays under root"),Inner->GetAttachParent(),static_cast<USceneComponent*>(Root));
        Test.TestEqual(TEXT("Wall A stays under inner node"),A->GetAttachParent(),static_cast<USceneComponent*>(Inner));
        Test.TestEqual(TEXT("Wall B stays under root"),B->GetAttachParent(),static_cast<USceneComponent*>(Root));
        UDatasmithAssetUserData* Data=UDatasmithAssetUserData::GetDatasmithUserData(Inner->GetChildActor());
        if (Test.TestNotNull(TEXT("Empty-node metadata retained"),Data))
        {
            Test.TestEqual(TEXT("Container metadata preserved"),Data->MetaData.FindRef(TEXT("用途")),FString(TEXT("建筑层级")));
            Test.TestEqual(TEXT("Container importer templates removed"),Data->ObjectTemplates.Num(),0);
        }
    }
}
static void CheckGroup(FAutomationTestBase& Test,AActor* Instance)
{
    const TCHAR* Names[]={TEXT("EditorOuter"),TEXT("EditorInner"),TEXT("Group_C"),TEXT("Group_D"),TEXT("Group_E")};
    for (const TCHAR* Name:Names)
    {
        UChildActorComponent* Node=Part(Instance,Name);
        if (!Test.TestNotNull(TEXT("Group part exists: ")+FString(Name),Node))continue;
        AActor* Child=Node->GetChildActor();
        if (!Test.TestNotNull(TEXT("Runtime group child exists"),Child))continue;
        Test.TestFalse(TEXT("Editor GroupActor is not serialized into runtime building"),Child->IsA<AGroupActor>());
        Test.TestNull(TEXT("Runtime group child has no editor group reference"),Child->GroupActor.Get());
    }
    UChildActorComponent* Outer=Part(Instance,TEXT("EditorOuter"));
    UChildActorComponent* Inner=Part(Instance,TEXT("EditorInner"));
    UChildActorComponent* D=Part(Instance,TEXT("Group_D"));
    if (Outer&&Inner&&D)
    {
        Test.TestEqual(TEXT("Nested group hierarchy represented"),Inner->GetAttachParent(),static_cast<USceneComponent*>(Outer));
        Test.TestEqual(TEXT("Nested group member hierarchy represented"),D->GetAttachParent(),static_cast<USceneComponent*>(Inner));
    }
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCHierarchyCompatibilityTest,"BuildingBlueprintCreator.HierarchyCompatibility",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FBBCHierarchyCompatibilityTest::RunTest(const FString& Parameters)
{
    using namespace BBCHierarchyTest;
    UWorld* World=FAutomationEditorCommonUtils::CreateNewMap();
    const FString Folder=TEXT("/Game/BBCHierarchy_")+FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
    AActor* Root=Empty(World,TEXT("TownRoot_grp"),FVector(400,100,200));
    Root->SetActorScale3D(FVector(-2,1.5,1));Root->SetActorRotation(FRotator(0,25,0));Root->SetIsSpatiallyLoaded(false);
    AActor* Inner=Empty(World,TEXT("Inner_grp"),FVector(500,200,260));
    Inner->AttachToActor(Root,FAttachmentTransformRules::KeepWorldTransform);
    AStaticMeshActor* A=Mesh(World,TEXT("Wall_A"),FVector(650,230,300));
    AStaticMeshActor* B=Mesh(World,TEXT("Wall_B"),FVector(420,420,310));
    A->AttachToActor(Inner,FAttachmentTransformRules::KeepWorldTransform);
    B->AttachToActor(Root,FAttachmentTransformRules::KeepWorldTransform);
    UDatasmithAssetUserData* Data=NewObject<UDatasmithAssetUserData>(Inner->GetRootComponent(),NAME_None,RF_Public);
    Data->MetaData.Add(TEXT("用途"),TEXT("建筑层级"));Inner->GetRootComponent()->AddAssetUserData(Data);
    UDatasmithSceneComponentTemplate* ImportTemplate=NewObject<UDatasmithSceneComponentTemplate>();
    ImportTemplate->Load(Inner->GetRootComponent());FDatasmithObjectTemplateUtils::SetObjectTemplate(Inner,ImportTemplate);
    TestEqual(TEXT("Checkbox off collects only selected empty node"),UBuildingBlueprintLibrary::CollectActors({Root},false).Num(),1);
    TestEqual(TEXT("Checkbox on collects nested containers and models"),UBuildingBlueprintLibrary::CollectActors({Root,A},true).Num(),4);
    FBuildingBlueprintOptions Options;Options.FolderPath=Folder;Options.AssetName=TEXT("BP_EmptyOnly");
    FBuildingBlueprintResult EmptyOnly=UBuildingBlueprintLibrary::CreateBuildingBlueprint({Root},Options);
    TestFalse(TEXT("Empty-only selection explains missing contents"),EmptyOnly.bSuccess);
    TestTrue(TEXT("Empty-only message asks to include descendants"),EmptyOnly.Message.Contains(TEXT("没有模型")));
    UAudioComponent* Audio=NewObject<UAudioComponent>(Inner,TEXT("ImportantAudio"));Inner->AddInstanceComponent(Audio);
    Options.bIncludeAllAttachedChildren=true;
    FBuildingBlueprintResult Unsupported=UBuildingBlueprintLibrary::CreateBuildingBlueprint({Root},Options);
    TestFalse(TEXT("Container with behavior is not silently skipped"),Unsupported.bSuccess);
    TestTrue(TEXT("Unsupported component error identifies audio"),Unsupported.Message.Contains(TEXT("AudioComponent")));
    Audio->DestroyComponent();

    AGroupActor* OuterGroup=World->SpawnActor<AGroupActor>();OuterGroup->SetActorLabel(TEXT("EditorOuter"));OuterGroup->SetActorLocation(FVector(900,100,50));
    AGroupActor* InnerGroup=World->SpawnActor<AGroupActor>();InnerGroup->SetActorLabel(TEXT("EditorInner"));InnerGroup->SetActorLocation(FVector(1000,300,50));
    AStaticMeshActor* C=Mesh(World,TEXT("Group_C"),FVector(1000,200,150));
    AStaticMeshActor* D=Mesh(World,TEXT("Group_D"),FVector(1300,300,150));
    AStaticMeshActor* E=Mesh(World,TEXT("Group_E"),FVector(1400,450,150));
    OuterGroup->Add(*C);OuterGroup->Add(*InnerGroup);InnerGroup->Add(*D);InnerGroup->Add(*E);
    TestEqual(TEXT("Native group expansion includes all nested members without checkbox"),UBuildingBlueprintLibrary::CollectActors({OuterGroup,D},false).Num(),5);
    const FString SourceMap=Folder/TEXT("SourceMap");
    const FString MapFile=FPackageName::LongPackageNameToFilename(SourceMap,FPackageName::GetMapPackageExtension());
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(MapFile),true);
    if(!TestTrue(TEXT("Hierarchy source map saves"),FEditorFileUtils::SaveLevel(World->PersistentLevel,MapFile)))return false;
    TArray<FString> Transforms={Root->GetTransform().ToString(),Inner->GetTransform().ToString(),A->GetTransform().ToString(),B->GetTransform().ToString()};
    Options.AssetName=TEXT("BP_Hierarchy");
    FBuildingBlueprintResult Created=UBuildingBlueprintLibrary::CreateBuildingBlueprint({Root},Options);
    if(!TestTrue(TEXT("Datasmith-style empty containers save: ")+Created.Message,Created.bSuccess))return false;
    TestEqual(TEXT("Source container import templates unchanged"),Data->ObjectTemplates.Num(),1);
    TestEqual(TEXT("Original mesh attachment unchanged"),A->GetAttachParentActor(),Inner);
    TestFalse(TEXT("Create-only leaves source map clean"),World->GetOutermost()->IsDirty());
    UBlueprint* BP=LoadObject<UBlueprint>(nullptr,*Created.AssetObjectPath);
    AActor* Instance=World->SpawnActor<AActor>(BP->GeneratedClass,Created.Pivot,FRotator::ZeroRotator);
    CheckHierarchy(*this,Instance,Transforms,FVector::ZeroVector);World->EditorDestroyActor(Instance,false);
    Options.bIncludeAllAttachedChildren=false;Options.AssetName=TEXT("BP_NativeGroup");
    FBuildingBlueprintResult GroupCreated=UBuildingBlueprintLibrary::CreateBuildingBlueprint({OuterGroup},Options);
    if(!TestTrue(TEXT("Native editor groups save as runtime hierarchy: ")+GroupCreated.Message,GroupCreated.bSuccess))return false;
    TestEqual(TEXT("Source group direct membership unchanged"),OuterGroup->GroupActors.Num(),1);
    TestEqual(TEXT("Source nested group membership unchanged"),InnerGroup->GroupActors.Num(),2);
    UBlueprint* GroupBP=LoadObject<UBlueprint>(nullptr,*GroupCreated.AssetObjectPath);
    AActor* GroupInstance=World->SpawnActor<AActor>(GroupBP->GeneratedClass,GroupCreated.Pivot,FRotator::ZeroRotator);
    CheckGroup(*this,GroupInstance);World->EditorDestroyActor(GroupInstance,false);
    APointLight* Light=World->SpawnActor<APointLight>();InnerGroup->Add(*Light);
    Options.AssetName=TEXT("BP_UnsupportedMember");
    TestFalse(TEXT("Unsupported native-group member rejects whole conversion"),UBuildingBlueprintLibrary::CreateBuildingBlueprint({OuterGroup},Options).bSuccess);
    TestFalse(TEXT("No partial group asset saved"),FPackageName::DoesPackageExist(Folder/Options.AssetName));
    InnerGroup->Remove(*Light);World->EditorDestroyActor(Light,false);

    Options.bReplaceActors=true;Options.AssetName=TEXT("BP_PartialGroup");
    FBuildingBlueprintResult Partial=UBuildingBlueprintLibrary::CreateBuildingBlueprint({D},Options);
    if(!TestTrue(TEXT("Partial group replacement succeeds: ")+Partial.Message,Partial.bSuccess))return false;
    TestTrue(TEXT("Unselected group member remains"),IsValid(E));
    TestTrue(TEXT("Unselected group keeps unrelated member"),InnerGroup->Contains(*E));
    TestTrue(TEXT("Replacement stays in common unselected group"),InnerGroup->Contains(*Partial.ReplacementActor));
    TestTrue(TEXT("Group membership Undo succeeds"),GEditor->UndoTransaction());
    TestTrue(TEXT("Undo restores original member"),InnerGroup->Contains(*D));
    TestEqual(TEXT("Undo restores actor group back-reference"),D->GroupActor.Get(),static_cast<AActor*>(InnerGroup));
    TestTrue(TEXT("Group membership Redo succeeds"),GEditor->RedoTransaction());
    TestFalse(TEXT("Redo removes old member"),InnerGroup->Contains(*D));
    TestTrue(TEXT("Undo partial replacement for next test"),GEditor->UndoTransaction());

    Options.AssetName=TEXT("BP_AllGroupsReplaced");
    FBuildingBlueprintResult Replaced=UBuildingBlueprintLibrary::CreateBuildingBlueprint({OuterGroup},Options);
    if(!TestTrue(TEXT("Whole nested group replacement succeeds: ")+Replaced.Message,Replaced.bSuccess))return false;
    TestFalse(TEXT("Native group wrapper removed from level"),IsValid(OuterGroup));
    TestTrue(TEXT("Whole-group Undo succeeds"),GEditor->UndoTransaction());
    TestTrue(TEXT("Undo restores group wrapper"),IsValid(OuterGroup));
    TestTrue(TEXT("Undo restores nested group membership"),InnerGroup->Contains(*D)&&InnerGroup->Contains(*E));
    TestTrue(TEXT("Whole-group Redo succeeds"),GEditor->RedoTransaction());
    TestFalse(TEXT("Redo removes group wrapper"),IsValid(OuterGroup));

    Options.bIncludeAllAttachedChildren=true;Options.AssetName=TEXT("BP_EmptyHierarchyReplaced");
    FBuildingBlueprintResult RootReplaced=UBuildingBlueprintLibrary::CreateBuildingBlueprint({Root},Options);
    if(!TestTrue(TEXT("Whole empty-container hierarchy replaces: ")+RootReplaced.Message,RootReplaced.bSuccess))return false;
    CheckHierarchy(*this,RootReplaced.ReplacementActor,Transforms,FVector::ZeroVector);
    TestFalse(TEXT("Selected empty container removed"),IsValid(Root));
    TestTrue(TEXT("Empty-container replacement Undo succeeds"),GEditor->UndoTransaction());
    TestTrue(TEXT("Undo restores source empty container"),IsValid(Root));
    TestEqual(TEXT("Undo restores original nested attachment"),A->GetAttachParentActor(),Inner);
    TestEqual(TEXT("Undo retains original Datasmith template"),Data->ObjectTemplates.Num(),1);
    TestTrue(TEXT("Empty-container replacement Redo succeeds"),GEditor->RedoTransaction());

    const FString Manifest=Created.AssetObjectPath+TEXT("\n")+SourceMap+TEXT("\n")+Created.Pivot.ToString()+TEXT("\n")
        +FString::Join(Transforms,TEXT("\n"))+TEXT("\n")+GroupCreated.AssetObjectPath;
    const FString Dir=FPaths::ProjectSavedDir()/TEXT("BuildingBlueprintCreator");IFileManager::Get().MakeDirectory(*Dir,true);
    FFileHelper::SaveStringToFile(Manifest,*(Dir/TEXT("hierarchy_roundtrip.txt")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCHierarchyReloadTest,"BuildingBlueprintCreator.HierarchyReload",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FBBCHierarchyReloadTest::RunTest(const FString& Parameters)
{
    using namespace BBCHierarchyTest;
    TArray<FString> Lines;FFileHelper::LoadFileToStringArray(Lines,*(FPaths::ProjectSavedDir()/TEXT("BuildingBlueprintCreator/hierarchy_roundtrip.txt")));
    if(!TestEqual(TEXT("Hierarchy manifest available"),Lines.Num(),8))return false;
    UWorld* World=FAutomationEditorCommonUtils::CreateNewMap();
    const bool bSourceLoaded=FindPackage(nullptr,*Lines[1])!=nullptr;
    if(FParse::Param(FCommandLine::Get(),TEXT("BBCFreshReload")))TestFalse(TEXT("Source map not loaded in new process"),bSourceLoaded);
    UBlueprint* BP=LoadObject<UBlueprint>(nullptr,*Lines[0]);if(!TestNotNull(TEXT("Hierarchy building loads"),BP))return false;
    FVector Pivot;Pivot.InitFromString(Lines[2]);TArray<FString> Transforms={Lines[3],Lines[4],Lines[5],Lines[6]};
    for(int32 I=0;I<2;++I)
    {
        FVector Offset(2000*I,0,0);AActor* Instance=World->SpawnActor<AActor>(BP->GeneratedClass,Pivot+Offset,FRotator::ZeroRotator);
        if(!TestNotNull(TEXT("Repeated hierarchy use succeeds"),Instance))return false;
        CheckHierarchy(*this,Instance,Transforms,Offset);
    }
    UBlueprint* GroupBP=LoadObject<UBlueprint>(nullptr,*Lines[7]);if(!TestNotNull(TEXT("Group building loads"),GroupBP))return false;
    AActor* GroupInstance=World->SpawnActor<AActor>(GroupBP->GeneratedClass);CheckGroup(*this,GroupInstance);
    if(!bSourceLoaded)TestNull(TEXT("Loading buildings does not load original source map"),FindPackage(nullptr,*Lines[1]));
    return !HasAnyErrors();
}
#endif
