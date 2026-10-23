#pragma once
#include "CoreMinimal.h"
#include "BuildingBlueprintLibrary.h"
#include "AssetRegistry/AssetData.h"
namespace BBCOptimizationBatch
{
bool Start(const TArray<FAssetData>& Assets,const FString& Folder,const FBuildingBlueprintOptimizationOptions& Options,TFunction<void(const FBuildingBlueprintBatchResult&)> Finished);
bool IsRunning();
FString Status();
void Cancel();
void Stop();
const FBuildingBlueprintBatchResult& LastResult();
}
