#include "BuildingBlueprintLibrary.h"

#include "Components/SceneComponent.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "ObjectTools.h"

bool UBuildingBlueprintLibrary::IsBatchParentActor(const AActor* Actor)
{
    if (!IsValid(Actor) || Actor->GetClass() != AActor::StaticClass() || Actor->IsChildActor()
        || !Actor->GetRootComponent() || !GEditor || GEditor->PlayWorld
        || Actor->GetWorld() != GEditor->GetEditorWorldContext().World()) return false;
    TInlineComponentArray<UActorComponent*> Components(Actor);
    for (const UActorComponent* Component : Components)
        if (Component && !Component->IsVisualizationComponent() && Component->GetClass() != USceneComponent::StaticClass()) return false;
    TArray<AActor*> Children;
    Actor->GetAttachedActors(Children);
    return !Children.IsEmpty();
}

FBuildingBlueprintBatchPlan UBuildingBlueprintLibrary::PlanParentBatch(const TArray<AActor*>& Input)
{
    FBuildingBlueprintBatchPlan Plan;
    if (Input.IsEmpty()) { Plan.Reason = TEXT("请先选择有挂接子级的普通空父级 Actor。"); return Plan; }
    TSet<AActor*> Selected;
    TArray<AActor*> Unique;
    for (AActor* Actor : Input)
    {
        if (!IsBatchParentActor(Actor))
        {
            Plan.Reason = (IsValid(Actor) ? Actor->GetActorLabel() : TEXT("已失效对象"))
                + FString(TEXT("：批量模式要求所选对象全部为当前关卡中有挂接子级、仅含普通场景组件的空 Actor。"));
            return Plan;
        }
        if (!Selected.Contains(Actor)) { Selected.Add(Actor); Unique.Add(Actor); }
    }
    for (AActor* Actor : Unique)
    {
        bool bIncluded = false;
        TSet<AActor*> Seen { Actor };
        for (AActor* Parent = Actor->GetAttachParentActor(); Parent; Parent = Parent->GetAttachParentActor())
        {
            if (Seen.Contains(Parent)) { Plan.Reason = TEXT("挂接层级存在循环，请先修复层级。"); return Plan; }
            Seen.Add(Parent);
            bIncluded |= Selected.Contains(Parent);
        }
        (bIncluded ? Plan.IncludedDescendants : Plan.Roots).Add(Actor);
    }
    // Native editor groups can collect members outside an attachment subtree.
    // Refuse intersecting collection ranges before creating or deleting anything.
    TMap<AActor*, AActor*> Owners;
    for (AActor* Root : Plan.Roots)
        for (AActor* Member : CollectActors({Root}, true))
        {
            if (AActor** Previous = Owners.Find(Member); Previous && *Previous != Root)
            {
                Plan.Reason = FString::Printf(TEXT("%s 与 %s 的收集范围交叉（%s）。请先整理跨父级分组，避免重复转换。"),
                    *(*Previous)->GetActorLabel(), *Root->GetActorLabel(), *Member->GetActorLabel());
                return Plan;
            }
            Owners.Add(Member, Root);
        }
    return Plan;
}

FBuildingBlueprintBatchResult UBuildingBlueprintLibrary::CreateParentBlueprintBatch(
    const TArray<AActor*>& Input, const FBuildingBlueprintOptions& Options)
{
    FBuildingBlueprintBatchResult Batch;
    const auto Plan = PlanParentBatch(Input);
    if (!Plan.IsValid()) { Batch.Message = Plan.Reason; return Batch; }
    FString Folder = Options.FolderPath;
    Folder.RemoveFromEnd(TEXT("/"));
    if (!(Folder == TEXT("/Game") || Folder.StartsWith(TEXT("/Game/")))
        || !FPackageName::IsValidLongPackageName(Folder / TEXT("BP_Parent")))
    { Batch.Message = TEXT("请使用 /Game/ 下的有效 UE 保存目录。"); return Batch; }

    struct FSource
    {
        TWeakObjectPtr<AActor> Root;
        FString Label, Path, Name;
        TArray<TWeakObjectPtr<AActor>> Members;
    };
    TArray<FSource> Sources;
    for (AActor* Root : Plan.Roots)
    {
        FSource Source; Source.Root = Root; Source.Label = Root->GetActorLabel(); Source.Path = Root->GetPathName();
        Source.Name = TEXT("BP_") + ObjectTools::SanitizeObjectName(Source.Label);
        for (AActor* Member : CollectActors({Root}, true)) Source.Members.Add(Member);
        Sources.Add(MoveTemp(Source));
    }
    for (AActor* Included : Plan.IncludedDescendants) Batch.IncludedDescendantLabels.Add(Included->GetActorLabel());
    Batch.bAccepted = true;
    FScopedSlowTask Progress(Sources.Num(), NSLOCTEXT("BuildingBlueprintCreator", "ParentBatchProgress", "正在逐父级创建建筑蓝图…"));
    Progress.MakeDialog();
    for (const FSource& Source : Sources)
    {
        Progress.EnterProgressFrame(1, FText::FromString(Source.Label));
        FBuildingBlueprintBatchItem Item; Item.SourceLabel = Source.Label; Item.SourceObjectPath = Source.Path;
        AActor* Root = Source.Root.Get();
        const TArray<AActor*> Current = IsBatchParentActor(Root) ? CollectActors({Root}, true) : TArray<AActor*>();
        const bool bUnchanged = Current.Num() == Source.Members.Num()
            && !Source.Members.ContainsByPredicate([&](const TWeakObjectPtr<AActor>& Member)
                { return !Member.IsValid() || !Current.Contains(Member.Get()); });
        if (!bUnchanged)
            Item.Result.Message = TEXT("处理前父级或收集范围已变化，本组未执行转换，请重新选择后重试。");
        else
        {
            FBuildingBlueprintOptions PerRoot = Options;
            PerRoot.FolderPath = Folder;
            PerRoot.AssetName = Source.Name;
            PerRoot.bIncludeAllAttachedChildren = true;
            // Each call owns its existing save/validation/replacement transaction.
            // A failed group cannot roll back an earlier successful group.
            Item.Result = CreateBuildingBlueprint({Root}, PerRoot);
        }
        if (Item.Result.bSuccess) ++Batch.CreatedCount; else ++Batch.FailedCount;
        Batch.Items.Add(MoveTemp(Item));
    }
    Batch.Message = FString::Printf(TEXT("批量完成：成功 %d 组，失败 %d 组；另有 %d 个父级已包含在所选祖先中，未重复创建。\n输出目录：%s\n"),
        Batch.CreatedCount, Batch.FailedCount, Batch.IncludedDescendantLabels.Num(), *Folder);
    if (Options.bReplaceActors && Batch.CreatedCount)
        Batch.Message += TEXT("替换按组支持撤销；请检查后按 Ctrl+S 保存关卡。\n");
    for (const FString& Name : Batch.IncludedDescendantLabels) Batch.Message += TEXT("\n已包含，不单独创建：") + Name;
    for (const auto& Item : Batch.Items)
        Batch.Message += FString::Printf(TEXT("\n\n[%s] %s\n%s"), Item.Result.bSuccess ? TEXT("成功") : TEXT("失败"),
            *Item.SourceLabel, *Item.Result.Message);
    const FString Dir = FPaths::ProjectSavedDir() / TEXT("BuildingBlueprintCreator/BatchReports");
    IFileManager::Get().MakeDirectory(*Dir, true);
    const FDateTime Time = FDateTime::UtcNow() + FTimespan(8,0,0);
    const FString File = Dir / (TEXT("Batch_") + Time.ToString(TEXT("%Y%m%d_%H%M%S_"))
        + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8) + TEXT(".txt"));
    if (FFileHelper::SaveStringToFile(Batch.Message, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        Batch.ReportFilePath = FPaths::ConvertRelativePathToFull(File);
    else Batch.Message += TEXT("\n\n批次报告文件写入失败，请复制本窗口结果；已创建的蓝图不受影响。");
    return Batch;
}
