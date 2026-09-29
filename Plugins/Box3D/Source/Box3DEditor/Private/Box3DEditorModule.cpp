#include "Box3DBoxComponent.h"
#include "Box3DCapsuleComponent.h"
#include "Box3DComponentVisualizer.h"
#include "Box3DFileCreator.h"
#include "Editor/UnrealEdEngine.h"
#include "Modules/ModuleManager.h"
#include "ToolMenus.h"
#include "ToolMenu.h"
#include "ToolMenuEntry.h"
#include "ToolMenuSection.h"
#include "ToolMenuDelegates.h"
#include "Framework/Commands/UIAction.h"
#include "Textures/SlateIcon.h"
#include "UnrealEdGlobals.h"

class FBox3DEditorModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        if (GUnrealEd)
        {
            const TSharedPtr<FBox3DComponentVisualizer> Visualizer =
                MakeShared<FBox3DComponentVisualizer>();
            GUnrealEd->RegisterComponentVisualizer(
                UBox3DBoxComponent::StaticClass()->GetFName(), Visualizer);
            GUnrealEd->RegisterComponentVisualizer(
                UBox3DCapsuleComponent::StaticClass()->GetFName(), Visualizer);
            Visualizer->OnRegister();
            UE_LOG(LogTemp, Display, TEXT("Box3D editor: registered box and capsule visualizers"));
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("Box3D editor: GUnrealEd was unavailable during visualizer registration"));
        }

        UToolMenus::RegisterStartupCallback(
            FSimpleMulticastDelegate::FDelegate::CreateRaw(
                this, &FBox3DEditorModule::RegisterMenus));


    }

    void RegisterMenus()
    {
        FToolMenuOwnerScoped Owner(this);

        UToolMenu* Toolbar = UToolMenus::Get()->ExtendMenu(
            "LevelEditor.LevelEditorToolBar.User");
        FToolMenuSection& Section = Toolbar->FindOrAddSection("Box3D");

        Section.AddEntry(FToolMenuEntry::InitComboButton(
            "Box3DDropdown",                              // internal ID
            FToolUIActionChoice(),
            FNewToolMenuDelegate::CreateRaw(
                this, &FBox3DEditorModule::BuildBox3DMenu),
            FText::FromString("Box3D"),                   // toolbar label
            FText::FromString("Box3D tools"),             // hover tooltip
            FSlateIcon(),
            false,
            NAME_None));

        UE_LOG(LogTemp, Display, TEXT("Box3D editor: registered toolbar dropdown"));
    }

    void BuildBox3DMenu(UToolMenu* Menu)
    {
        FToolMenuSection& Section = Menu->FindOrAddSection("Actions");

        Section.AddMenuEntry(
            "FirstAction",                                // internal ID
            FText::FromString("Generate .box3d file"),           // dropdown item text
            FText::FromString("create file for use with client and server box3d world"),   // hover tooltip
            FSlateIcon(),
            FExecuteAction::CreateRaw(
                this, &FBox3DEditorModule::OnFirstAction),
            EUserInterfaceActionType::Button,
            NAME_None,
            TOptional<FText>());
    }

    void OnFirstAction()
    {
        FBox3DFileCreator::ExportBox3DFile();
    }

    virtual void ShutdownModule() override
    {
        UToolMenus::UnRegisterStartupCallback(this);
        UToolMenus::UnregisterOwner(this);

        if (GUnrealEd)
        {
            GUnrealEd->UnregisterComponentVisualizer(
                UBox3DBoxComponent::StaticClass()->GetFName());
            GUnrealEd->UnregisterComponentVisualizer(
                UBox3DCapsuleComponent::StaticClass()->GetFName());
        }
    }
};

IMPLEMENT_MODULE(FBox3DEditorModule, Box3DEditor)
