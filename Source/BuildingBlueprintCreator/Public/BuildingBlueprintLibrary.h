#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "BuildingBlueprintLibrary.generated.h"

class UBlueprint;

USTRUCT(BlueprintType)
struct BUILDINGBLUEPRINTCREATOR_API FBuildingBlueprintOptions
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building")
    FString FolderPath = TEXT("/Game/Buildings");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building")
    FString AssetName = TEXT("BP_Building");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building")
    bool bReplaceActors = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building")
    bool bIncludeAllAttachedChildren = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building")
    bool bUseISM = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building")
    bool bEnableNanite = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building", meta=(DeprecatedProperty, DeprecationMessage="Use bUseISM. New output uses ISM."))
    bool bUseHISM = false;
};

USTRUCT(BlueprintType)
struct BUILDINGBLUEPRINTCREATOR_API FBuildingBlueprintResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Building") bool bSuccess = false;
    UPROPERTY(BlueprintReadOnly, Category="Building") FString AssetObjectPath;
    UPROPERTY(BlueprintReadOnly, Category="Building") FString Message;
    UPROPERTY(BlueprintReadOnly, Category="Building") TObjectPtr<AActor> ReplacementActor = nullptr;
    UPROPERTY(BlueprintReadOnly, Category="Building") int32 ActorCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Building") FVector Pivot = FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly, Category="Building") int32 ISMGroupCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Building") int32 ISMInstanceCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Building", meta=(DeprecatedProperty, DeprecationMessage="Use ISMGroupCount.")) int32 HISMGroupCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Building", meta=(DeprecatedProperty, DeprecationMessage="Use ISMInstanceCount.")) int32 HISMInstanceCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Building") TArray<FString> Warnings;
    UPROPERTY(BlueprintReadOnly, Category="Building") bool bNaniteProcessed = false;
    UPROPERTY(BlueprintReadOnly, Category="Building") int32 NaniteEnabledCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Building") int32 NaniteAlreadyEnabledCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Building") FString BackupDirectory;
};

// Read-only selection plan; source actors stay in the editor world.
struct BUILDINGBLUEPRINTCREATOR_API FBuildingBlueprintBatchPlan
{
    TArray<AActor*> Roots;
    TArray<AActor*> IncludedDescendants;
    FString Reason;
    bool IsValid() const { return Reason.IsEmpty() && !Roots.IsEmpty(); }
};

USTRUCT(BlueprintType)
struct BUILDINGBLUEPRINTCREATOR_API FBuildingBlueprintBatchItem
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="Building") FString SourceLabel;
    UPROPERTY(BlueprintReadOnly, Category="Building") FString SourceObjectPath;
    UPROPERTY(BlueprintReadOnly, Category="Building") FBuildingBlueprintResult Result;
};

USTRUCT(BlueprintType)
struct BUILDINGBLUEPRINTCREATOR_API FBuildingBlueprintBatchResult
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="Building") bool bAccepted = false;
    UPROPERTY(BlueprintReadOnly, Category="Building") int32 CreatedCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Building") int32 FailedCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Building") TArray<FString> IncludedDescendantLabels;
    UPROPERTY(BlueprintReadOnly, Category="Building") TArray<FBuildingBlueprintBatchItem> Items;
    UPROPERTY(BlueprintReadOnly, Category="Building") FString Message;
    UPROPERTY(BlueprintReadOnly, Category="Building") FString ReportFilePath;
};

UCLASS()
class BUILDINGBLUEPRINTCREATOR_API UBuildingBlueprintLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    static bool IsBatchParentActor(const AActor* Actor);
    static FBuildingBlueprintBatchPlan PlanParentBatch(const TArray<AActor*>& Actors);

    UFUNCTION(BlueprintCallable, Category="Editor|Building Blueprint")
    static FBuildingBlueprintBatchResult CreateParentBlueprintBatch(const TArray<AActor*>& Actors, const FBuildingBlueprintOptions& Options);

    UFUNCTION(BlueprintCallable, Category="Editor|Building Blueprint")
    static FBuildingBlueprintResult CreateBuildingBlueprint(const TArray<AActor*>& Actors, const FBuildingBlueprintOptions& Options);

    UFUNCTION(BlueprintCallable, Category="Editor|Building Blueprint")
    static TArray<AActor*> CollectActors(const TArray<AActor*>& Actors, bool bIncludeAllAttachedChildren);

    UFUNCTION(BlueprintCallable, Category="Editor|Building Blueprint")
    static FString FindAvailableAssetName(const FString& FolderPath, const FString& DesiredName);

    UFUNCTION(BlueprintCallable, Category="Editor|Building Blueprint")
    static void OpenCreationDialog();
};
