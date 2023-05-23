// Copyright © 2023 Ministry of Land, Infrastructure and Transport

#include "ExtentEditor/PLATEAUExtentEditor.h"
#include "ExtentEditor/SPLATEAUExtentEditorViewport.h"

#include "Misc/ScopedSlowTask.h"

#include "Widgets/Docking/SDockTab.h"
#include "EditorViewportTabContent.h"
#include "Engine/Selection.h"

#define LOCTEXT_NAMESPACE "FPLATEUExtentEditor"

const FName FPLATEAUExtentEditor::TabId(TEXT("PLATEAUExtentEditor"));

void FPLATEAUExtentEditor::RegisterTabSpawner(const TSharedRef<class FTabManager>& InTabManager) {
    InTabManager->RegisterTabSpawner(TabId, FOnSpawnTab::CreateSP(this, &FPLATEAUExtentEditor::SpawnTab))
        .SetDisplayName(LOCTEXT("ViewportTab", "Viewport"))
        .SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"));
}

void FPLATEAUExtentEditor::UnregisterTabSpawner(const TSharedRef<class FTabManager>& InTabManager) {
    InTabManager->UnregisterTabSpawner(TabId);
}

FPLATEAUExtentEditor::FPLATEAUExtentEditor() {}

FPLATEAUExtentEditor::~FPLATEAUExtentEditor() {}

TSharedRef<SDockTab> FPLATEAUExtentEditor::SpawnTab(const FSpawnTabArgs& Args) {
    TWeakPtr<FPLATEAUExtentEditor> WeakSharedThis(SharedThis(this));

    const auto Viewport = SNew(SPLATEAUExtentEditorViewport)
        .ExtentEditor(WeakSharedThis);

    TSharedRef< SDockTab > DockableTab =
        SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        [Viewport];

    Viewport->SetOwnerTab(DockableTab);

    return DockableTab;
}

const FString& FPLATEAUExtentEditor::GetSourcePath() const {
    return SourcePath;
}

void FPLATEAUExtentEditor::SetSourcePath(const FString& Path) {
    SourcePath = Path;
}

FPLATEAUGeoReference FPLATEAUExtentEditor::GetGeoReference() const {
    return GeoReference;
}

void FPLATEAUExtentEditor::SetGeoReference(const FPLATEAUGeoReference& InGeoReference) {
    GeoReference = InGeoReference;
}

const TOptional<FPLATEAUExtent>& FPLATEAUExtentEditor::GetExtent() const {
    return Extent;
}

void FPLATEAUExtentEditor::SetExtent(const FPLATEAUExtent& InExtent) {
    Extent = InExtent;
}

void FPLATEAUExtentEditor::ResetExtent() {
    Extent.Reset();
}

const bool FPLATEAUExtentEditor::IsImportFromServer() const {
    return bImportFromServer;
}

void FPLATEAUExtentEditor::SetImportFromServer(bool InBool) {
    bImportFromServer = InBool;
}

std::shared_ptr<plateau::network::Client> FPLATEAUExtentEditor::GetClientPtr() const {
    return ClientPtr;
}

void FPLATEAUExtentEditor::SetClientPtr(const std::shared_ptr<plateau::network::Client>& InClientPtr) {
    ClientPtr = InClientPtr;
}

const std::string& FPLATEAUExtentEditor::GetServerDatasetID() const {
    return ServerDatasetID;
}

void FPLATEAUExtentEditor::SetServerDatasetID(const std::string& InID) {
    ServerDatasetID = InID;
}

const plateau::dataset::PredefinedCityModelPackage& FPLATEAUExtentEditor::GetLocalPackageMask() const {
    return LocalPackageMask;
}

void FPLATEAUExtentEditor::SetLocalPackageMask(const plateau::dataset::PredefinedCityModelPackage& InPackageMask) {
    LocalPackageMask = InPackageMask;
}

const plateau::dataset::PredefinedCityModelPackage& FPLATEAUExtentEditor::GetServerPackageMask() const {
    return ServerPackageMask;
}

void FPLATEAUExtentEditor::SetServerPackageMask(const plateau::dataset::PredefinedCityModelPackage& InPackageMask) {
    ServerPackageMask = InPackageMask;
}

#undef LOCTEXT_NAMESPACE
