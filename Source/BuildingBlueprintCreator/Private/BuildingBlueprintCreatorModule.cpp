#include "BuildingBlueprintLibrary.h"
#include "BuildingOutlinerVisibility.h"
#include "BuildingThumbnailRenderer.h"
#include "BuildingBlueprintAssetMenu.h"
#include "BuildingBatchResultPanel.h"

#include "ContentBrowserModule.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "IContentBrowserSingleton.h"
#include "LevelEditor.h"
#include "LevelEditorContextMenu.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/MessageDialog.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/PlatformApplicationMisc.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "BuildingBlueprintCreator"

class SBuildingBlueprintDialog : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SBuildingBlueprintDialog) {} SLATE_END_ARGS()

    void Construct(const FArguments&, const TArray<AActor*>& Actors, const TSharedRef<SWindow>& Window)
    {
        ParentWindow = Window;
        for (AActor* Actor : Actors) Selection.Add(Actor);
        GConfig->GetString(TEXT("BuildingBlueprintCreator"), TEXT("LastFolder"), Options.FolderPath, GEditorPerProjectIni);
        GConfig->GetBool(TEXT("BuildingBlueprintCreator"), TEXT("bIncludeAllAttachedChildren"), Options.bIncludeAllAttachedChildren, GEditorPerProjectIni);
        GConfig->GetBool(TEXT("BuildingBlueprintCreator"), TEXT("bReplaceActors"), Options.bReplaceActors, GEditorPerProjectIni);
        if (!GConfig->GetBool(TEXT("BuildingBlueprintCreator"), TEXT("bUseISM"), Options.bUseISM, GEditorPerProjectIni))
            GConfig->GetBool(TEXT("BuildingBlueprintCreator"), TEXT("bUseHISM"), Options.bUseISM, GEditorPerProjectIni);
        GConfig->GetBool(TEXT("BuildingBlueprintCreator"), TEXT("bEnableNanite"), Options.bEnableNanite, GEditorPerProjectIni);
        if (!Options.FolderPath.StartsWith(TEXT("/Game/"))) Options.FolderPath = TEXT("/Game/Buildings");
        RefreshBatchPlan();
        FPathPickerConfig Picker;
        Picker.DefaultPath = Options.FolderPath; Picker.bAddDefaultPath = true;
        Picker.bAllowReadOnlyFolders = false; Picker.bFocusSearchBoxWhenOpened = false;
        Picker.OnPathSelected = FOnPathSelected::CreateLambda([this](const FString& Path)
        {
            Options.FolderPath = Path; Error = FText::GetEmpty();
        });
        ChildSlot
        [
            SNew(SBorder).BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder")).Padding(18)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
                [ SNew(STextBlock).Text(LOCTEXT("Hint", "将所选建筑保存为可复用蓝图，保留模型、材质和已有蓝图。\n基准点：建筑整体底部中心。")) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
                [ SNew(STextBlock).Text_Lambda([this] { return bBatchMode
                    ? LOCTEXT("BatchNameLabel", "蓝图名称：自动使用 BP_父级显示名称，同名追加编号")
                    : LOCTEXT("NameLabel", "蓝图名称"); }) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
                [ SAssignNew(NameBox, SEditableTextBox).Text(FText::FromString(Options.AssetName))
                    .IsEnabled_Lambda([this] { return !bBatchMode; })
                    .OnTextChanged_Lambda([this](const FText& Text) { Options.AssetName = Text.ToString(); Error = FText::GetEmpty(); }) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
                [ SNew(STextBlock).Text(LOCTEXT("FolderLabel", "保存文件夹（右键文件夹可新建文件夹）")) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 5)
                [ SNew(STextBlock).Text_Lambda([this] { return FText::FromString(Options.FolderPath); }).AutoWrapText(true) ]
                + SVerticalBox::Slot().FillHeight(1).Padding(0, 0, 0, 12)
                [ FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().CreatePathPicker(Picker) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
                [ SAssignNew(BatchCheck, SCheckBox)
                    .IsEnabled_Lambda([this] { return bBatchMode || CanUseBatch(); })
                    .IsChecked_Lambda([this] { return bBatchMode ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                    .ToolTipText_Lambda([this] { return FText::FromString(BatchSummary); })
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { SetBatchMode(State == ECheckBoxState::Checked); })
                    [ SNew(STextBlock).Text(LOCTEXT("ParentBatch", "每个父级分别创建蓝图")) ] ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [ SNew(STextBlock).Text_Lambda([this] { return FText::FromString(BatchSummary); }).AutoWrapText(true) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [ SAssignNew(IncludeCheck, SCheckBox).IsEnabled_Lambda([this] { return !bBatchMode; })
                    .IsChecked_Lambda([this] { return bBatchMode || Options.bIncludeAllAttachedChildren ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                    .ToolTipText(LOCTEXT("IncludeBatchHint", "批量模式自动包含所有层级子级，不改变普通模式记住的勾选状态。"))
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
                    {
                        if (bBatchMode) return;
                        Options.bIncludeAllAttachedChildren = State == ECheckBoxState::Checked;
                        SaveCheckboxPreferences();
                    })
                    [ SNew(STextBlock).Text(LOCTEXT("IncludeChildren", "包含全部子 Actor（所有层级）")) ] ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [ SAssignNew(ReplaceCheck, SCheckBox).IsChecked(Options.bReplaceActors ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
                    {
                        Options.bReplaceActors = State == ECheckBoxState::Checked;
                        SaveCheckboxPreferences();
                    })
                    [ SNew(STextBlock).Text(LOCTEXT("Replace", "用新蓝图替换关卡中的原 Actor")) ] ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [ SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth()
                    [ SAssignNew(ISMCheck, SCheckBox).IsChecked(Options.bUseISM ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
                    {
                        Options.bUseISM = State == ECheckBoxState::Checked;
                        SaveCheckboxPreferences();
                    })
                    [ SNew(STextBlock).Text(LOCTEXT("UseISM", "重复模型转为 ISM")) ] ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(18, 0, 0, 0)
                    [ SAssignNew(NaniteCheck, SCheckBox).IsChecked(Options.bEnableNanite ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
                        .ToolTipText(LOCTEXT("NaniteTooltip", "为本次收集范围（含嵌套蓝图和实例组件）的模型开启并保存 Nanite。同一模型仅处理一次，影响所有引用；不开启引擎或插件模型。"))
                        .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
                        { Options.bEnableNanite = State == ECheckBoxState::Checked; SaveCheckboxPreferences(); })
                        [ SNew(STextBlock).Text(LOCTEXT("EnableNanite", "全部子 Actor 开启 Nanite")) ] ] ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [ SNew(STextBlock).Text(LOCTEXT("NaniteHint", "Nanite 将修改并保存原模型，影响所有引用；模型设置有独立撤销步骤。"))
                    .AutoWrapText(true).Visibility_Lambda([this] { return Options.bEnableNanite ? EVisibility::Visible : EVisibility::Collapsed; }) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [ SNew(STextBlock).Text(LOCTEXT("ISMHint", "同一直接父级下至少两个兼容零件才归组；已有蓝图、单独模型及不兼容零件保持原方式。"))
                    .AutoWrapText(true) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [ SNew(STextBlock).Text_Lambda([this]
                    {
                        if (bBatchMode) return LOCTEXT("BatchCollectHint", "批量模式自动包含全部子级，按组校验、保存和替换；失败组保留原对象并继续下一组。");
                        return FText::Format(LOCTEXT("Count", "当前将收集 {0} 个 Actor（含层级节点）；原生组包含全部成员。未勾选时不递归挂接子 Actor。"),
                            FText::AsNumber(UBuildingBlueprintLibrary::CollectActors(GetActors(), Options.bIncludeAllAttachedChildren).Num()));
                    }).AutoWrapText(true) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [ SNew(STextBlock).Text(LOCTEXT("SaveHint", "新蓝图会立即保存。同名自动追加 _1、_2。\n替换支持撤销；替换后请检查结果并自行按 Ctrl+S 保存关卡。\nDatasmith 对象会转为独立蓝图，保留普通元数据，不再按原对象同步更新。"))
                    .AutoWrapText(true) ]
                + SVerticalBox::Slot().AutoHeight().MaxHeight(110).Padding(0, 0, 0, 8)
                [ SNew(SScrollBox) + SScrollBox::Slot()
                    [ SNew(STextBlock).Text_Lambda([this] { return Error; }).ColorAndOpacity(FStyleColors::Warning).AutoWrapText(true) ] ]
                + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
                [ SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                    [ SNew(SButton).Text(LOCTEXT("Cancel", "取消")).OnClicked_Lambda([this] { ParentWindow.Pin()->RequestDestroyWindow(); return FReply::Handled(); }) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ SNew(SButton).ButtonStyle(FAppStyle::Get(), "PrimaryButton").Text(LOCTEXT("Create", "创建蓝图 Actor"))
                        .IsEnabled_Lambda([this] { return !bBatchMode || CanUseBatch(); })
                        .OnClicked(this, &SBuildingBlueprintDialog::Create) ] ]
            ]
        ];
    }

private:
#if WITH_DEV_AUTOMATION_TESTS
    friend class FBBCBatchDialogTest;
#endif
    void RefreshBatchPlan()
    {
        BatchPlan = UBuildingBlueprintLibrary::PlanParentBatch(GetActors());
        if (!BatchPlan.IsValid()) { BatchSummary = BatchPlan.Reason; return; }
        BatchSummary = FString::Printf(TEXT("可分别创建 %d 组；批量时自动包含全部子级。"), BatchPlan.Roots.Num());
        if (!BatchPlan.IncludedDescendants.IsEmpty())
        {
            TArray<FString> Names;
            for (AActor* Actor : BatchPlan.IncludedDescendants) if (Names.Num() < 3) Names.Add(Actor->GetActorLabel());
            BatchSummary += FString::Printf(TEXT("已包含在外层、不重复创建的父级：%s。"), *FString::Join(Names, TEXT("、")));
            if (BatchPlan.IncludedDescendants.Num() > 3) BatchSummary += FString::Printf(TEXT("共 %d 个，完整清单见批次结果。"), BatchPlan.IncludedDescendants.Num());
        }
    }
    bool CanUseBatch() const
    {
        return BatchPlan.IsValid() && !Selection.ContainsByPredicate([](const TWeakObjectPtr<AActor>& Actor) { return !Actor.IsValid(); });
    }
    void SetBatchMode(bool bEnabled)
    {
        RefreshBatchPlan();
        bBatchMode = bEnabled && CanUseBatch();
        Error = bEnabled && !bBatchMode ? FText::FromString(BatchSummary) : FText::GetEmpty();
    }
    void ShowBatchResult(const FBuildingBlueprintBatchResult& Result, bool bBatch = true)
    {
        TSharedRef<SWindow> Window = SNew(SWindow).Title(bBatch ? LOCTEXT("BatchResultTitle", "批量创建蓝图结果") : LOCTEXT("SingleResultTitle", "创建蓝图结果"))
            .ClientSize(FVector2D(720, 520)).SupportsMinimize(false);
        Window->SetContent(SNew(SBorder).Padding(12)
            [ SNew(SVerticalBox)
                + SVerticalBox::Slot().FillHeight(1)
                [ bBatch ? MakeBatchResultPanel(Result) : StaticCastSharedRef<SWidget>(SNew(SMultiLineEditableTextBox).IsReadOnly(true).AutoWrapText(true)
                    .Text(FText::FromString(Result.Message))) ]
                + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0, 10, 0, 0)
                [ SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 10, 0)
                    [ SNew(SButton).Text(LOCTEXT("CopyBatchResult", "复制完整报告")).OnClicked_Lambda([Text = Result.Message + (Result.ReportFilePath.IsEmpty() ? FString() : TEXT("\n\n报告：") + Result.ReportFilePath)]
                        { FPlatformApplicationMisc::ClipboardCopy(*Text); return FReply::Handled(); }) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ SNew(SButton).Text(LOCTEXT("CloseBatchResult", "关闭")).OnClicked_Lambda([Weak = TWeakPtr<SWindow>(Window)]
                        { if (auto Pinned = Weak.Pin()) Pinned->RequestDestroyWindow(); return FReply::Handled(); }) ] ]
            ]);
        FSlateApplication::Get().AddModalWindow(Window, FGlobalTabmanager::Get()->GetRootWindow());
    }
    void SaveCheckboxPreferences() const
    {
        GConfig->SetBool(TEXT("BuildingBlueprintCreator"), TEXT("bIncludeAllAttachedChildren"), Options.bIncludeAllAttachedChildren, GEditorPerProjectIni);
        GConfig->SetBool(TEXT("BuildingBlueprintCreator"), TEXT("bReplaceActors"), Options.bReplaceActors, GEditorPerProjectIni);
        GConfig->SetBool(TEXT("BuildingBlueprintCreator"), TEXT("bUseISM"), Options.bUseISM, GEditorPerProjectIni);
        GConfig->SetBool(TEXT("BuildingBlueprintCreator"), TEXT("bEnableNanite"), Options.bEnableNanite, GEditorPerProjectIni);
        GConfig->Flush(false, GEditorPerProjectIni);
    }

    TArray<AActor*> GetActors() const
    {
        TArray<AActor*> Result;
        // Keep nulls: stale selections must fail, not silently lose a part.
        for (const TWeakObjectPtr<AActor>& Actor : Selection) Result.Add(Actor.Get());
        return Result;
    }
    FReply Create()
    {
        if (bBatchMode)
        {
            const auto Batch = UBuildingBlueprintLibrary::CreateParentBlueprintBatch(GetActors(), Options);
            if (!Batch.bAccepted) { RefreshBatchPlan(); Error = FText::FromString(Batch.Message); return FReply::Handled(); }
            if (Batch.CreatedCount)
            {
                GConfig->SetString(TEXT("BuildingBlueprintCreator"), TEXT("LastFolder"), *Options.FolderPath, GEditorPerProjectIni);
                GConfig->Flush(false, GEditorPerProjectIni);
                TArray<UObject*> Assets;
                for (const auto& Item : Batch.Items) if (Item.Result.bSuccess)
                    if (UObject* Asset = LoadObject<UObject>(nullptr, *Item.Result.AssetObjectPath)) Assets.Add(Asset);
                if (!Assets.IsEmpty()) FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().SyncBrowserToAssets(Assets);
            }
            ParentWindow.Pin()->RequestDestroyWindow();
            ShowBatchResult(Batch);
            return FReply::Handled();
        }
        const FBuildingBlueprintResult Result = UBuildingBlueprintLibrary::CreateBuildingBlueprint(GetActors(), Options);
        if (!Result.bSuccess) { Error = FText::FromString(Result.Message); return FReply::Handled(); }
        GConfig->SetString(TEXT("BuildingBlueprintCreator"), TEXT("LastFolder"), *Options.FolderPath, GEditorPerProjectIni);
        GConfig->Flush(false, GEditorPerProjectIni);
        if (UObject* Asset = LoadObject<UObject>(nullptr, *Result.AssetObjectPath))
        {
            TArray<UObject*> Assets { Asset };
            FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().SyncBrowserToAssets(Assets);
        }
        ParentWindow.Pin()->RequestDestroyWindow();
        if (Options.bUseISM || Options.bEnableNanite || !Result.Warnings.IsEmpty())
        {
            FBuildingBlueprintBatchResult Summary; Summary.Message = Result.Message;
            ShowBatchResult(Summary, false);
        }
        else FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Result.Message), LOCTEXT("Complete", "创建完成"));
        return FReply::Handled();
    }
    FBuildingBlueprintOptions Options;
    FBuildingBlueprintBatchPlan BatchPlan;
    FString BatchSummary;
    bool bBatchMode = false;
    TSharedPtr<SCheckBox> BatchCheck, IncludeCheck, ReplaceCheck, ISMCheck, NaniteCheck;
    TArray<TWeakObjectPtr<AActor>> Selection;
    TWeakPtr<SWindow> ParentWindow;
    TSharedPtr<SEditableTextBox> NameBox;
    FText Error;
};

void UBuildingBlueprintLibrary::OpenCreationDialog()
{
    if (!GEditor || GEditor->PlayWorld) return;
    TArray<AActor*> Actors;
    GEditor->GetSelectedActors()->GetSelectedObjects<AActor>(Actors);
    if (Actors.IsEmpty()) return;
    TSharedRef<SWindow> Window = SNew(SWindow).Title(LOCTEXT("WindowTitle", "创建蓝图 Actor"))
        .ClientSize(FVector2D(660, 680)).SupportsMaximize(false).SupportsMinimize(false);
    Window->SetContent(SNew(SBuildingBlueprintDialog, Actors, Window));
    FSlateApplication::Get().AddModalWindow(Window, FGlobalTabmanager::Get()->GetRootWindow());
}

static TSharedRef<FExtender> MakeBuildingExtender(const TSharedRef<FUICommandList>, TArray<AActor*> Actors)
{
    TSharedRef<FExtender> Extender = MakeShared<FExtender>();
    if (!Actors.IsEmpty()) Extender->AddMenuExtension("WorldPartition", EExtensionHook::After, nullptr,
        FMenuExtensionDelegate::CreateLambda([](FMenuBuilder& Menu)
        {
            Menu.AddMenuEntry(LOCTEXT("MenuLabel", "创建蓝图 Actor…"),
                LOCTEXT("MenuTooltip", "将选中的模型和蓝图保存为可复用建筑蓝图。"),
                FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.Blueprint"),
                FUIAction(FExecuteAction::CreateStatic(&UBuildingBlueprintLibrary::OpenCreationDialog),
                    FCanExecuteAction::CreateLambda([] { return GEditor && !GEditor->PlayWorld; })));
        }));
    return Extender;
}

class FBuildingBlueprintCreatorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
#if WITH_DEV_AUTOMATION_TESTS
        if (!FParse::Param(FCommandLine::Get(), TEXT("BBCMemoryExperiment")))
        {
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCMemoryProbe"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCMemoryCacheCompatibility"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCCacheScene"));
        }
        // Validation creates fixture assets and switches maps. Never expose
        // these tests to an ordinary user's global Run All operation.
        if (!FParse::Param(FCommandLine::Get(), TEXT("BBCValidation")))
        {
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCBuildingCreateTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCBuildingReloadTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCDialogVisualTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCDatasmithCompatibilityTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCDatasmithReloadTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCHierarchyCompatibilityTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCHierarchyReloadTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCOutlinerVisibilityTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCThumbnailTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCThumbnailReloadTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCParentBatchTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCParentBatchReloadTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCBatchDialogTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCBatchResultPresentationTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCOptimizationBatchTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCISMTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCISMReloadTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCAssetOptimizationTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCAssetOptimizationReloadTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCAssetOptimizationMenuTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCNaniteTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCNaniteReloadTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCOptimizationDialogTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCOptimizationModesTest"));
            FAutomationTestFramework::Get().UnregisterAutomationTest(TEXT("FBBCOptimizationReplaceReloadTest"));
        }
#endif
        if (IsRunningCommandlet()) return;
        FLevelEditorModule& LevelEditor = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
        auto Delegate = FLevelEditorModule::FLevelViewportMenuExtender_SelectedActors::CreateStatic(&MakeBuildingExtender);
        Handle = Delegate.GetHandle();
        LevelEditor.GetAllLevelViewportContextMenuExtenders().Add(Delegate);
        BBCOutliner::Start();
        BBCThumbnails::Start();
        BBCAssetMenu::Start();
        // Module unload happens after UObject shutdown during a normal editor
        // exit. Release editor registrations while their owners still exist.
        ExitHandle = FCoreDelegates::OnEnginePreExit.AddRaw(this, &FBuildingBlueprintCreatorModule::ReleaseEditorResources);
    }
    virtual void ShutdownModule() override
    {
        FCoreDelegates::OnEnginePreExit.Remove(ExitHandle);
        ExitHandle.Reset();
        ReleaseEditorResources();
    }
private:
    void ReleaseEditorResources()
    {
        if (bResourcesReleased) return;
        bResourcesReleased = true;
        BBCAssetMenu::Stop();
        BBCThumbnails::Stop();
        BBCOutliner::Stop();
        if (FModuleManager::Get().IsModuleLoaded("LevelEditor"))
            FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor").GetAllLevelViewportContextMenuExtenders().RemoveAll(
                [this](const auto& Delegate) { return Delegate.GetHandle() == Handle; });
        UE_LOG(LogTemp, Display, TEXT("Building Blueprint Creator: editor resources released before module unload."));
    }
    bool bResourcesReleased = false;
    FDelegateHandle ExitHandle;
    FDelegateHandle Handle;
};

IMPLEMENT_MODULE(FBuildingBlueprintCreatorModule, BuildingBlueprintCreator)

#if WITH_DEV_AUTOMATION_TESTS
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMeshActor.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/AutomationEditorCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCDialogVisualTest, "BuildingBlueprintCreator.DialogAndMenu",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FBBCDialogVisualTest::RunTest(const FString& Parameters)
{
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    AActor* Source = World->SpawnActor<AStaticMeshActor>();
    Source->SetActorLabel(TEXT("建筑模型"));
    CastChecked<AStaticMeshActor>(Source)->GetStaticMeshComponent()->SetStaticMesh(
        LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
    TSharedRef<SWindow> Window = SNew(SWindow).Title(LOCTEXT("WindowTitle", "创建蓝图 Actor"))
        .ClientSize(FVector2D(660, 680)).SupportsMaximize(false).SupportsMinimize(false);
    Window->SetContent(SNew(SBuildingBlueprintDialog, TArray<AActor*> { Source }, Window));
    FSlateApplication::Get().AddWindow(Window);
    Window->SlatePrepass();
    FSlateApplication::Get().Tick();
    auto Capture = [&](const TSharedRef<SWindow>& Target, const FString& Name)
    {
        TArray<FColor> Pixels; FIntVector Size;
        if (!TestTrue(TEXT("Slate screenshot ") + Name, FSlateApplication::Get().TakeScreenshot(Target, Pixels, Size))) return;
        TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, PNG);
        TestTrue(TEXT("Screenshot saved ") + Name, FFileHelper::SaveArrayToFile(PNG, *(FPaths::ProjectSavedDir() / Name)));
    };
    Capture(Window, TEXT("BuildingBlueprintCreator_dialog.png"));
    Window->RequestDestroyWindow();
    GEditor->SelectNone(false, true, false);
    GEditor->SelectActor(Source, true, true);
    TSharedPtr<ILevelEditor> LevelEditor = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor").GetFirstLevelEditor();
    if (!TestTrue(TEXT("Actual level editor available"), LevelEditor.IsValid())) return false;
    TSharedRef<SWidget> ActualMenu = FLevelEditorContextMenu::BuildMenuWidget(LevelEditor, ELevelEditorMenuContext::SceneOutliner);
    TSharedRef<SWindow> MenuWindow = SNew(SWindow).Title(LOCTEXT("MenuTest", "右键菜单入口验证"))
        .ClientSize(FVector2D(520, 1000));
    MenuWindow->SetContent(ActualMenu);
    FSlateApplication::Get().AddWindow(MenuWindow); MenuWindow->SlatePrepass(); FSlateApplication::Get().Tick();
    TArray<FString> MenuTexts;
    TFunction<void(const TSharedRef<SWidget>&)> Gather = [&](const TSharedRef<SWidget>& Widget)
    {
        if (Widget->GetType() == FName(TEXT("STextBlock"))) MenuTexts.Add(StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString());
        FChildren* Children = Widget->GetChildren();
        for (int32 I = 0; I < Children->Num(); ++I) Gather(Children->GetChildAt(I));
    };
    Gather(ActualMenu);
    const int32 WorldPartitionIndex = MenuTexts.IndexOfByPredicate([](const FString& Text) { return Text == TEXT("世界分区") || Text == TEXT("World Partition"); });
    const int32 CreateIndex = MenuTexts.IndexOfByKey(TEXT("创建蓝图 Actor…"));
    TestTrue(TEXT("Actual Outliner menu contains World Partition header"), WorldPartitionIndex != INDEX_NONE);
    TestTrue(TEXT("Actual Outliner menu contains building entry after World Partition"), CreateIndex > WorldPartitionIndex && WorldPartitionIndex != INDEX_NONE);
    TestEqual(TEXT("Exactly one building entry"), MenuTexts.FilterByPredicate([](const FString& T) { return T == TEXT("创建蓝图 Actor…"); }).Num(), 1);
    FFileHelper::SaveStringArrayToFile(MenuTexts, *(FPaths::ProjectSavedDir() / TEXT("BuildingBlueprintCreator_actual_menu.txt")));
    Capture(MenuWindow, TEXT("BuildingBlueprintCreator_menu.png"));
    MenuWindow->RequestDestroyWindow();
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCBatchDialogTest,"BuildingBlueprintCreator.BatchDialog",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FBBCBatchDialogTest::RunTest(const FString& Parameters)
{
    bool OldInclude=false,OldReplace=false,OldISM=false,OldLegacy=false,OldNanite=false;
    const bool HadInclude=GConfig->GetBool(TEXT("BuildingBlueprintCreator"),TEXT("bIncludeAllAttachedChildren"),OldInclude,GEditorPerProjectIni);
    const bool HadReplace=GConfig->GetBool(TEXT("BuildingBlueprintCreator"),TEXT("bReplaceActors"),OldReplace,GEditorPerProjectIni);
    const bool HadISM=GConfig->GetBool(TEXT("BuildingBlueprintCreator"),TEXT("bUseISM"),OldISM,GEditorPerProjectIni);
    const bool HadLegacy=GConfig->GetBool(TEXT("BuildingBlueprintCreator"),TEXT("bUseHISM"),OldLegacy,GEditorPerProjectIni);
    const bool HadNanite=GConfig->GetBool(TEXT("BuildingBlueprintCreator"),TEXT("bEnableNanite"),OldNanite,GEditorPerProjectIni);
    ON_SCOPE_EXIT
    {
        if(HadInclude)GConfig->SetBool(TEXT("BuildingBlueprintCreator"),TEXT("bIncludeAllAttachedChildren"),OldInclude,GEditorPerProjectIni);
        else GConfig->RemoveKey(TEXT("BuildingBlueprintCreator"),TEXT("bIncludeAllAttachedChildren"),GEditorPerProjectIni);
        if(HadReplace)GConfig->SetBool(TEXT("BuildingBlueprintCreator"),TEXT("bReplaceActors"),OldReplace,GEditorPerProjectIni);
        else GConfig->RemoveKey(TEXT("BuildingBlueprintCreator"),TEXT("bReplaceActors"),GEditorPerProjectIni);
        if(HadISM)GConfig->SetBool(TEXT("BuildingBlueprintCreator"),TEXT("bUseISM"),OldISM,GEditorPerProjectIni);
        else GConfig->RemoveKey(TEXT("BuildingBlueprintCreator"),TEXT("bUseISM"),GEditorPerProjectIni);
        if(HadLegacy)GConfig->SetBool(TEXT("BuildingBlueprintCreator"),TEXT("bUseHISM"),OldLegacy,GEditorPerProjectIni);
        else GConfig->RemoveKey(TEXT("BuildingBlueprintCreator"),TEXT("bUseHISM"),GEditorPerProjectIni);
        if(HadNanite)GConfig->SetBool(TEXT("BuildingBlueprintCreator"),TEXT("bEnableNanite"),OldNanite,GEditorPerProjectIni);
        else GConfig->RemoveKey(TEXT("BuildingBlueprintCreator"),TEXT("bEnableNanite"),GEditorPerProjectIni);
        GConfig->Flush(false,GEditorPerProjectIni);
    };
    GConfig->SetBool(TEXT("BuildingBlueprintCreator"),TEXT("bIncludeAllAttachedChildren"),false,GEditorPerProjectIni);
    GConfig->SetBool(TEXT("BuildingBlueprintCreator"),TEXT("bReplaceActors"),false,GEditorPerProjectIni);
    GConfig->SetBool(TEXT("BuildingBlueprintCreator"),TEXT("bUseISM"),false,GEditorPerProjectIni);
    GConfig->SetBool(TEXT("BuildingBlueprintCreator"),TEXT("bEnableNanite"),false,GEditorPerProjectIni);
    UWorld* World=FAutomationEditorCommonUtils::CreateNewMap();
    auto Parent=[&](const TCHAR* Label)
    {
        AActor* A=World->SpawnActor<AActor>();
        auto* Root=NewObject<USceneComponent>(A);A->AddInstanceComponent(Root);A->SetRootComponent(Root);
        Root->SetMobility(EComponentMobility::Static);Root->RegisterComponent();A->SetActorLabel(Label);
        AStaticMeshActor* M=World->SpawnActor<AStaticMeshActor>();
        M->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
        M->AttachToActor(A,FAttachmentTransformRules::KeepWorldTransform);return A;
    };
    AActor* A=Parent(TEXT("Building_A_grp"));AActor* B=Parent(TEXT("Building_B_grp"));
    TSharedRef<SWindow> Window=SNew(SWindow).Title(LOCTEXT("BatchUITest","批量父级创建窗口验证")).ClientSize(FVector2D(660,680));
    TSharedPtr<SBuildingBlueprintDialog> Dialog;
    Window->SetContent(SAssignNew(Dialog,SBuildingBlueprintDialog,TArray<AActor*>({A,B}),Window));
    FSlateApplication::Get().AddWindow(Window);
    auto Tick=[&]{Window->SlatePrepass();FSlateApplication::Get().Tick();};Tick();
    TestTrue(TEXT("Parent-only selection enables batch checkbox"),Dialog->BatchCheck->IsEnabled());
    Dialog->BatchCheck->ToggleCheckedState();Tick();
    TestTrue(TEXT("User checkbox enables actual batch mode"),Dialog->bBatchMode);
    TestTrue(TEXT("Batch forces include checkbox checked"),Dialog->IncludeCheck->IsChecked());
    TestFalse(TEXT("Batch locks include checkbox"),Dialog->IncludeCheck->IsEnabled());
    TestFalse(TEXT("Batch locks common asset name"),Dialog->NameBox->IsEnabled());
    TestTrue(TEXT("Batch leaves ISM selectable"),Dialog->ISMCheck->IsEnabled());
    TestFalse(TEXT("ISM has safe unchecked default"),Dialog->ISMCheck->IsChecked());
    Dialog->ISMCheck->ToggleCheckedState();Tick();
    bool ISMRemembered=false;GConfig->GetBool(TEXT("BuildingBlueprintCreator"),TEXT("bUseISM"),ISMRemembered,GEditorPerProjectIni);
    TestTrue(TEXT("ISM click persists independently"),ISMRemembered);
    TestTrue(TEXT("Nanite is independently selectable"),Dialog->NaniteCheck->IsEnabled());
    TestFalse(TEXT("Nanite safe unchecked default"),Dialog->NaniteCheck->IsChecked());
    Dialog->NaniteCheck->ToggleCheckedState();Tick();
    bool NaniteRemembered=false;GConfig->GetBool(TEXT("BuildingBlueprintCreator"),TEXT("bEnableNanite"),NaniteRemembered,GEditorPerProjectIni);
    TestTrue(TEXT("Nanite click persists"),NaniteRemembered);
    bool Remembered=true;GConfig->GetBool(TEXT("BuildingBlueprintCreator"),TEXT("bIncludeAllAttachedChildren"),Remembered,GEditorPerProjectIni);
    TestFalse(TEXT("Batch does not overwrite ordinary include preference"),Remembered);
    TArray<FColor> Pixels;FIntVector Size;
    if(TestTrue(TEXT("Batch dialog screenshot"),FSlateApplication::Get().TakeScreenshot(Window,Pixels,Size)))
    {
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("BuildingBlueprintCreator_batch_dialog.png")));
    }
    Dialog->BatchCheck->ToggleCheckedState();Tick();
    TestFalse(TEXT("Leaving batch restores unchecked include"),Dialog->IncludeCheck->IsChecked());
    TestTrue(TEXT("Leaving batch re-enables include"),Dialog->IncludeCheck->IsEnabled());
    Dialog->IncludeCheck->ToggleCheckedState();Dialog->ReplaceCheck->ToggleCheckedState();Tick();
    GConfig->GetBool(TEXT("BuildingBlueprintCreator"),TEXT("bIncludeAllAttachedChildren"),Remembered,GEditorPerProjectIni);
    TestTrue(TEXT("Ordinary include click persists"),Remembered);
    GConfig->GetBool(TEXT("BuildingBlueprintCreator"),TEXT("bReplaceActors"),Remembered,GEditorPerProjectIni);
    TestTrue(TEXT("Ordinary replace click persists"),Remembered);
    TSharedRef<SWindow> Reopen=SNew(SWindow);
    TSharedRef<SBuildingBlueprintDialog> Next=SNew(SBuildingBlueprintDialog,TArray<AActor*>({A,B}),Reopen);
    TestTrue(TEXT("Reopened dialog restores both choices"),Next->Options.bIncludeAllAttachedChildren&&Next->Options.bReplaceActors);
    TestTrue(TEXT("Reopened dialog restores ISM choice"),Next->Options.bUseISM);
    TestTrue(TEXT("Reopened dialog restores Nanite choice"),Next->Options.bEnableNanite);
    Next->NaniteCheck->ToggleCheckedState();
    GConfig->GetBool(TEXT("BuildingBlueprintCreator"),TEXT("bEnableNanite"),NaniteRemembered,GEditorPerProjectIni);
    TestFalse(TEXT("Nanite uncheck persists"),NaniteRemembered);
    Next->ISMCheck->ToggleCheckedState();
    GConfig->GetBool(TEXT("BuildingBlueprintCreator"),TEXT("bUseISM"),ISMRemembered,GEditorPerProjectIni);
    TestFalse(TEXT("Unchecking ISM persists too"),ISMRemembered);
    TestFalse(TEXT("Batch mode itself does not silently persist"),Next->bBatchMode);
    TArray<AActor*> Meshes;A->GetAttachedActors(Meshes);
    TSharedRef<SWindow> MixedWindow=SNew(SWindow);
    TSharedRef<SBuildingBlueprintDialog> Mixed=SNew(SBuildingBlueprintDialog,TArray<AActor*>({A,Meshes[0]}),MixedWindow);
    TestFalse(TEXT("Mixed selection disables batch checkbox"),Mixed->CanUseBatch());
    Mixed->SetBatchMode(true);TestFalse(TEXT("Programmatic toggle cannot bypass mixed-selection guard"),Mixed->bBatchMode);
    GConfig->RemoveKey(TEXT("BuildingBlueprintCreator"),TEXT("bUseISM"),GEditorPerProjectIni);
    GConfig->SetBool(TEXT("BuildingBlueprintCreator"),TEXT("bUseHISM"),true,GEditorPerProjectIni);
    TSharedRef<SWindow> LegacyWindow=SNew(SWindow);
    TSharedRef<SBuildingBlueprintDialog> Legacy=SNew(SBuildingBlueprintDialog,TArray<AActor*>({A,B}),LegacyWindow);
    TestTrue(TEXT("Legacy HISM preference restores the ISM option"),Legacy->Options.bUseISM);
    Window->RequestDestroyWindow();return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBBCBatchResultPresentationTest,"BuildingBlueprintCreator.BatchResultPresentation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FBBCBatchResultPresentationTest::RunTest(const FString& Parameters)
{
    for (int32 Mode=0; Mode<3; ++Mode)
    {
        FBuildingBlueprintBatchResult R; R.bAccepted=true;
        R.ReportFilePath=FPaths::ProjectSavedDir()/TEXT("BuildingBlueprintCreator/BatchReports/示例报告.txt");
        for (int32 I=0; I<4; ++I)
        {
            FBuildingBlueprintBatchItem Item; Item.SourceLabel=FString::Printf(TEXT("建筑_%d"),I);
            Item.Result.bSuccess=Mode==1||(Mode==0&&I%2==0);
            Item.Result.Message=Item.Result.bSuccess?TEXT("已保存 /Game/Buildings/BP_Building。原建筑已保留。")
                :TEXT("示例失败原因：该建筑引用的模型尚未保存，请保存资源后重试。\n原对象已保留，其他组继续处理。");
            if(Item.Result.bSuccess)++R.CreatedCount;else ++R.FailedCount;R.Items.Add(Item);
        }
        auto Content=MakeBatchResultPanel(R);TArray<FString> Names,Statuses;
        TFunction<void(const TSharedRef<SWidget>&)> Inspect=[&](const TSharedRef<SWidget>& W)
        {
            if(W->GetTypeAsString()==TEXT("STextBlock"))
            {
                auto Text=StaticCastSharedRef<STextBlock>(W);const FString Value=Text->GetText().ToString();
                if(Value.StartsWith(TEXT("建筑_")))Names.Add(Value);
                if(Value==TEXT("成功")||Value==TEXT("失败"))
                {
                    Statuses.Add(Value);const FLinearColor Color=Text->GetColorAndOpacity().GetSpecifiedColor();
                    TestTrue(TEXT("Visible status has correct red/green color"),Value==TEXT("成功")?Color.G>Color.R*2:Color.R>Color.G*2);
                }
            }
            auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)Inspect(Children->GetChildAt(I));
        };
        Inspect(Content);TestEqual(TEXT("All result rows remain visible in scroll content"),Names.Num(),4);
        TestEqual(TEXT("Each row has a colored status"),Statuses.Num(),4);
        if(Mode==0)TestTrue(TEXT("Failed groups are first, stable within each status"),Names==TArray<FString>{TEXT("建筑_1"),TEXT("建筑_3"),TEXT("建筑_0"),TEXT("建筑_2")});
        TestTrue(TEXT("Source processing/result order is unchanged"),R.Items[0].SourceLabel==TEXT("建筑_0"));
        auto Window=SNew(SWindow).Title(LOCTEXT("ResultTest","批量创建蓝图结果")).ClientSize(FVector2D(720,520));Window->SetContent(Content);
        FSlateApplication::Get().AddWindow(Window);Window->SlatePrepass();FSlateApplication::Get().Tick();
        TArray<FColor> Pixels;FIntVector Size;
        if(TestTrue(TEXT("Batch result screenshot captured"),FSlateApplication::Get().TakeScreenshot(Window,Pixels,Size)))
        {
            TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
            FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/FString::Printf(TEXT("BuildingBlueprintCreator_batch_result_%d.png"),Mode)));
        }
        Window->RequestDestroyWindow();
    }
    return !HasAnyErrors();
}
#endif
#undef LOCTEXT_NAMESPACE
