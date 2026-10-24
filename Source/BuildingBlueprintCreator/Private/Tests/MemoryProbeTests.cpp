#if WITH_DEV_AUTOMATION_TESTS
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshEditorSubsystem.h"
#include "StaticMeshCompiler.h"
#include "ScopedTransaction.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformMemory.h"
#include "HAL/UnrealMemory.h"
#include "UObject/SavePackage.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "Materials/MaterialInterface.h"
#include "Tests/AutomationEditorCommon.h"
#include "MeshDescription.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/PackageName.h"
#include "BuildingBlueprintLibrary.h"
#include "BuildingThumbnailRenderer.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "PhysicsEngine/BodySetup.h"
#include "FileHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCMemoryProbe,"BuildingBlueprintCreator.MemoryProbe",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FBBCMemoryProbe::RunTest(const FString&)
{
    // Explicit experiment only. Never reset undo or modify assets in the user's project.
    if (!FPaths::GetProjectFilePath().Contains(TEXT("BBCBuild_20261021"))) return false;
    FAutomationEditorCommonUtils::CreateNewMap();
    FString CSV=TEXT("Stage,CommitBytes,UndoBytes,LoadedMeshResourceBytes,LoadedMeshes\n");
    auto Sample=[&](const FString& Stage)
    {
        uint64 Bytes=0; int32 Count=0;
        for(TObjectIterator<UStaticMesh> It;It;++It) {Bytes+=It->GetResourceSizeBytes(EResourceSizeMode::Exclusive);++Count;}
        CSV+=FString::Printf(TEXT("%s,%llu,%llu,%llu,%d\n"),*Stage,uint64(FPlatformMemory::GetStats().UsedVirtual),uint64(GEditor->Trans->GetUndoSize()),Bytes,Count);
        FFileHelper::SaveStringToFile(CSV,*(FPaths::ProjectSavedDir()/TEXT("memory_probe.csv")));
    };
    Sample(TEXT("Baseline"));
    for(const TCHAR* Name:{TEXT("KB3D_NSF_BldgMdIndustrialComplex_A_GreenWall"),TEXT("KB3D_NSF_BldgLgMultimediaTower_A_BuildingD")})
    {
        const FString Path=FString(TEXT("/Game/MemoryProbe_20261023/"))+Name;
        auto* Mesh=LoadObject<UStaticMesh>(nullptr,*(Path+TEXT(".")+Name));
        if(!Mesh) {Sample(TEXT("LoadFailed"));return false;}
        FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
        Sample(FString(Name)+TEXT("_Loaded"));
        if(Mesh->NaniteSettings.bEnabled)
        {
            Mesh->NaniteSettings.bEnabled=false;Mesh->PostEditChange();
            FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
        }
        Sample(FString(Name)+TEXT("_PreparedWithoutNanite"));
        // Copied model only. Materials are irrelevant to the geometry experiment.
        Mesh->GetStaticMaterials().Reset();
        Mesh->GetStaticMaterials().Add(FStaticMaterial(LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"))));
        {
            FScopedTransaction Transaction(NSLOCTEXT("BBC","MemoryProbe","Memory probe copied mesh"));
            auto Settings=Mesh->NaniteSettings;Settings.bEnabled=true;
            GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>()->SetNaniteSettings(Mesh,Settings,true);
            FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
        }
        Sample(FString(Name)+TEXT("_Built"));
        FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;
        UPackage::SavePackage(Mesh->GetOutermost(),Mesh,*(FPaths::ProjectContentDir()/TEXT("MemoryProbe_20261023")/(FString(Name)+TEXT(".uasset"))),Args);
        Sample(FString(Name)+TEXT("_Saved"));
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS,true);Sample(FString(Name)+TEXT("_GC"));
    }
    GEditor->Trans->Reset(NSLOCTEXT("BBC","ProbeUndoReset","Independent memory experiment"));
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS,true);Sample(TEXT("AfterUndoResetGC"));
    for(TObjectIterator<UStaticMesh> It;It;++It)
        if(It->GetPathName().StartsWith(TEXT("/Game/MemoryProbe_20261023/"))) It->ClearMeshDescriptions();
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS,true);Sample(TEXT("AfterMeshDescriptionClearGC"));
    FMemory::Trim(true);Sample(TEXT("AfterAllocatorTrim"));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCMemoryCacheCompatibility,"BuildingBlueprintCreator.MemoryCacheCompatibility",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FBBCMemoryCacheCompatibility::RunTest(const FString&)
{
    if(!FPaths::GetProjectFilePath().Contains(TEXT("BBCBuild_20261021"))) return false;
    const bool Fresh=FParse::Param(FCommandLine::Get(),TEXT("BBCCacheReload"));
    const bool NoClear=FParse::Param(FCommandLine::Get(),TEXT("BBCCacheNoClear"));
    const FString Manifest=FPaths::ProjectSavedDir()/TEXT("memory_cache_expected.txt");
    TArray<FString> Expected,Output;
    if(Fresh&&!FFileHelper::LoadFileToStringArray(Expected,*Manifest))return false;
    int32 Index=0;
    for(const TCHAR* Name:{TEXT("KB3D_NSF_BldgMdIndustrialComplex_A_GreenWall"),TEXT("KB3D_NSF_BldgLgMultimediaTower_A_BuildingD")})
    {
        const FString Path=FString(TEXT("/Game/MemoryProbe_20261023/"))+Name;
        UStaticMesh* Mesh=LoadObject<UStaticMesh>(nullptr,*(Path+TEXT(".")+Name));
        if(!TestNotNull(TEXT("Copied model loads"),Mesh))return false;
        FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
        auto Signature=[&]() -> FString
        {
            const FMeshDescription* D=Mesh->GetMeshDescription(0);
            return D?FString::Printf(TEXT("%d,%d"),D->Vertices().Num(),D->Triangles().Num()):TEXT("Missing");
        };
        const FString Before=Signature();
        TestTrue(TEXT("Source geometry exists"),Before!=TEXT("Missing"));
        if(Fresh) {if(!Expected.IsValidIndex(Index))return false;TestEqual(TEXT("New process source geometry counts persist"),Before,Expected[Index]);}
        if(!NoClear)Mesh->ClearMeshDescriptions();
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS,true);
        TestEqual(TEXT("Geometry can be reconstructed from bulk data after cache clear"),Signature(),Before);
        TestTrue(TEXT("Saved Nanite data remains valid"),Mesh->HasValidNaniteData());
        if(!Fresh)
        {
            {
                FScopedTransaction T(NSLOCTEXT("BBC","CacheCheck","Copied model compatibility check"));
                auto S=Mesh->NaniteSettings;S.bEnabled=false;
                GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>()->SetNaniteSettings(Mesh,S,true);
                FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
            }
            if(!NoClear)Mesh->ClearMeshDescriptions();
            TestTrue(TEXT("Undo survives cache clear"),GEditor->UndoTransaction());
            FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
            TestTrue(TEXT("Undo restores Nanite"),Mesh->NaniteSettings.bEnabled&&Mesh->HasValidNaniteData());
            if(!NoClear)Mesh->ClearMeshDescriptions();
            TestTrue(TEXT("Redo survives cache clear"),GEditor->RedoTransaction());
            FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
            TestFalse(TEXT("Redo restores disabled state"),Mesh->NaniteSettings.bEnabled);
            auto S=Mesh->NaniteSettings;S.bEnabled=true;
            GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>()->SetNaniteSettings(Mesh,S,true);
            FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
            TestTrue(TEXT("Nanite rebuild after clear succeeds"),Mesh->HasValidNaniteData());
            TestEqual(TEXT("Source geometry counts survive rebuild and undo"),Signature(),Before);
            FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;
            TestTrue(TEXT("Rebuilt model saves"),UPackage::SavePackage(Mesh->GetOutermost(),Mesh,*(FPaths::ProjectContentDir()/TEXT("MemoryProbe_20261023")/(FString(Name)+TEXT(".uasset"))),Args));
            if(!NoClear)Mesh->ClearMeshDescriptions();Output.Add(Before);
        }
        ++Index;
    }
    if(!Fresh)TestTrue(TEXT("Save expected geometry for new process"),FFileHelper::SaveStringArrayToFile(Output,*Manifest));
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCCacheScene,"BuildingBlueprintCreator.MemoryCacheScene",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FBBCCacheScene::RunTest(const FString&)
{
    if(!FPaths::GetProjectFilePath().Contains(TEXT("BBCBuild_20261021")))return false;
    const bool Fresh=FParse::Param(FCommandLine::Get(),TEXT("BBCCacheReload"));
    const FString Folder=TEXT("/Game/MemoryProbe_20261023");
    const FString BPPath=Folder/TEXT("BP_CacheScene.BP_CacheScene");
    UBlueprint* BP=nullptr;
    if(!Fresh)
    {
        auto* W=FAutomationEditorCommonUtils::CreateNewMap();
        TArray<AActor*> Parts;
        int32 I=0;
        for(const TCHAR* Name:{TEXT("KB3D_NSF_BldgMdIndustrialComplex_A_GreenWall"),TEXT("KB3D_NSF_BldgLgMultimediaTower_A_BuildingD")})
        {
            auto* Mesh=LoadObject<UStaticMesh>(nullptr,*((Folder/Name)+TEXT(".")+Name));
            if(!Mesh)return false;
            FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
            AddInfo(FString::Printf(TEXT("%s SourceLODs=%d CollisionTrace=%d SimpleShapes=%d"),Name,Mesh->GetNumSourceModels(),Mesh->GetBodySetup()?int32(Mesh->GetBodySetup()->CollisionTraceFlag):-1,Mesh->GetBodySetup()?Mesh->GetBodySetup()->AggGeom.GetElementCount():0));
            auto* A=W->SpawnActor<AStaticMeshActor>(FVector(1000*I++,0,200),FRotator(0,30,0));
            A->GetStaticMeshComponent()->SetStaticMesh(Mesh);Parts.Add(A);
        }
        FBuildingBlueprintOptions O;O.FolderPath=Folder;O.AssetName=TEXT("BP_CacheScene");
        auto R=UBuildingBlueprintLibrary::CreateBuildingBlueprint(Parts,O);
        if(!TestTrue(TEXT("Create real-model building blueprint"),R.bSuccess))return false;
        BP=LoadObject<UBlueprint>(nullptr,*R.AssetObjectPath);
        // A unique suffix from a prior test run is recorded explicitly.
        FFileHelper::SaveStringToFile(R.AssetObjectPath,*(FPaths::ProjectSavedDir()/TEXT("cache_scene_bp.txt")));
    }
    else
    {
        FString P;if(!FFileHelper::LoadFileToString(P,*(FPaths::ProjectSavedDir()/TEXT("cache_scene_bp.txt"))))return false;
        BP=LoadObject<UBlueprint>(nullptr,*P);
    }
    if(!TestNotNull(TEXT("Building blueprint loads"),BP))return false;
    auto* W=GEditor->GetEditorWorldContext().World();
    auto* Building=W->SpawnActor<AActor>(BP->GeneratedClass,FTransform(FRotator(0,45,0),FVector(100,200,300)));
    if(!TestNotNull(TEXT("Building instance spawns"),Building))return false;
    TArray<AActor*> Family;Building->GetAllChildActors(Family,true);
    TestEqual(TEXT("Both real model parts instantiated"),Family.Num(),2);
    struct FPart { UStaticMeshComponent* C; FTransform T; int32 Collision; UMaterialInterface* M; UStaticMesh* Mesh; };
    TArray<FPart> Before;
    for(auto* A:Family)
    {
        TInlineComponentArray<UStaticMeshComponent*> Comps(A);
        for(auto* C:Comps)if(UStaticMesh* Mesh=C->GetStaticMesh())
        {
            Before.Add({C,C->GetComponentTransform(),int32(C->GetCollisionEnabled()),C->GetMaterial(0),Mesh});
            Mesh->ClearMeshDescriptions();
        }
    }
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS,true);
    for(const auto& P:Before)
    {
        TestTrue(TEXT("World transform retained"),P.C->GetComponentTransform().Equals(P.T));
        TestTrue(TEXT("Model reference retained"),P.C->GetStaticMesh()==P.Mesh);
        TestEqual(TEXT("Material reference retained"),P.C->GetMaterial(0),P.M);
        TestEqual(TEXT("Collision enabled retained"),int32(P.C->GetCollisionEnabled()),P.Collision);
        TestTrue(TEXT("Nanite render data remains valid"),P.Mesh->HasValidNaniteData());
    }
    FKismetEditorUtilities::CompileBlueprint(BP);
    TestTrue(TEXT("Dependent blueprint compiles"),BP->Status!=BS_Error);
    BBCThumbnails::CacheForSave(BP);
    FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;
    TestTrue(TEXT("Blueprint with thumbnail saves"),UPackage::SavePackage(BP->GetOutermost(),BP,*FPackageName::LongPackageNameToFilename(BP->GetOutermost()->GetName(),FPackageName::GetAssetPackageExtension()),Args));
    const FString MapFile=FPackageName::LongPackageNameToFilename(Folder/(Fresh?TEXT("CacheSceneReload"):TEXT("CacheScene")),FPackageName::GetMapPackageExtension());
    TestTrue(TEXT("Scene saves"),FEditorFileUtils::SaveMap(W,MapFile));
    return !HasAnyErrors();
}
#endif
