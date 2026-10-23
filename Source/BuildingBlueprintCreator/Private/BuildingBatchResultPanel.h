#pragma once
#include "CoreMinimal.h"
class SWidget;
struct FBuildingBlueprintBatchResult;
TSharedRef<SWidget> MakeBatchResultPanel(const FBuildingBlueprintBatchResult& Result);
