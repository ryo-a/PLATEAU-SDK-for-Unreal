// Fill out your copyright notice in the Description page of Project Settings.

#include "PLATEAUExtentEditorVPClient.h"
#include "PLATEAUExtentEditor.h"

#include <plateau/udx/udx_file_collection.h>

#include "PLATEAUExtentGizmo.h"
#include "PLATEAUMeshCodeGizmo.h"

#include "EditorModeManager.h"
#include "CanvasTypes.h"

#include "SEditorViewport.h"
#include "AdvancedPreviewScene.h"
#include "SPLATEAUExtentEditorViewport.h"

#include "AssetViewerSettings.h"
#include "CameraController.h"
#include "EditorViewportClient.h"

#define LOCTEXT_NAMESPACE "FPLATEAUExtentEditorViewportClient"

namespace {
    /**
     * 範囲選択のつまみのためのHitProxyクラス
     */
    struct HPLATEAUExtentHandleProxy : public HHitProxy {
        DECLARE_HIT_PROXY();

        int Index;

        HPLATEAUExtentHandleProxy(int index) :
            HHitProxy(HPP_UI),
            Index(index) {}
    };
    IMPLEMENT_HIT_PROXY(HPLATEAUExtentHandleProxy, HHitProxy);

}


FPLATEAUExtentEditorViewportClient::FPLATEAUExtentEditorViewportClient(
    TWeakPtr<FPLATEAUExtentEditor> InExtentEditor,
    const TSharedRef<SPLATEAUExtentEditorViewport>& InPLATEAUExtentEditorViewport,
    const TSharedRef<FAdvancedPreviewScene>& InPreviewScene)
    : FEditorViewportClient(nullptr, &InPreviewScene.Get(), StaticCastSharedRef<SEditorViewport>(InPLATEAUExtentEditorViewport))
    , ExtentEditorPtr(InExtentEditor) {
    InPreviewScene->SetFloorVisibility(false);
    ExtentGizmo = MakeUnique<FPLATEAUExtentGizmo>();
}

FPLATEAUExtentEditorViewportClient::~FPLATEAUExtentEditorViewportClient() {
    UAssetViewerSettings::Get()->OnAssetViewerSettingsChanged().RemoveAll(this);
}

void FPLATEAUExtentEditorViewportClient::Initialize(plateau::udx::UdxFileCollection& FileCollection) {
    InitCamera();

    const auto ExtentEditor = ExtentEditorPtr.Pin();
    auto GeoReference = ExtentEditor->GetGeoReference();
    if (ExtentEditor->GetExtent().IsSet())
        ExtentGizmo->SetExtent(ExtentEditor->GetExtent().GetValue(), GeoReference);

    MeshCodeGizmos.Reset();
    for (const auto& MeshCode : FileCollection.getMeshCodes()) {
        MeshCodeGizmos.AddDefaulted();
        MeshCodeGizmos.Last().Init(MeshCode, GeoReference.GetData());
    }
}

FPLATEAUExtent FPLATEAUExtentEditorViewportClient::GetExtent() const {
    auto GeoReference = ExtentEditorPtr.Pin()->GetGeoReference();
    return ExtentGizmo->GetExtent(GeoReference);
}

void FPLATEAUExtentEditorViewportClient::InitCamera() {
    ToggleOrbitCamera(false);
    SetCameraSetup(
        FVector::ZeroVector,
        FRotator::ZeroRotator,
        FVector(0.0, 0, 10000.0),
        FVector::Zero(),
        FVector(0, 0, 10000.0),
        FRotator(-90, -90, 0)
    );
    CameraController->AccessConfig().bLockedPitch = true;
    CameraController->AccessConfig().MaximumAllowedPitchRotation = -90;
    CameraController->AccessConfig().MinimumAllowedPitchRotation = -90;
}

void FPLATEAUExtentEditorViewportClient::Tick(float DeltaSeconds) {
    const auto ExtentMin = ExtentGizmo->GetMin();
    const auto ExtentMax = ExtentGizmo->GetMax();
    for (auto& Gizmo : MeshCodeGizmos) {
        Gizmo.SetSelected(Gizmo.IntersectsWith(ExtentMin, ExtentMax));
    }

    // ベースマップ
    const auto ExtentEditor = ExtentEditorPtr.Pin();
    auto GeoReference = ExtentEditor->GetGeoReference();
    if (Basemap == nullptr) {
        Basemap = MakeUnique<FPLATEAUBasemap>(GeoReference, SharedThis(this));
    }

    TArray<FVector> CornerWorldPositions;
    CornerWorldPositions.Add(GetWorldPosition(0, 0));
    CornerWorldPositions.Add(GetWorldPosition(0, Viewport->GetSizeXY().Y));
    CornerWorldPositions.Add(GetWorldPosition(Viewport->GetSizeXY().X, 0));
    CornerWorldPositions.Add(GetWorldPosition(Viewport->GetSizeXY().X, Viewport->GetSizeXY().Y));

    FVector MinPosition = CornerWorldPositions[0];
    FVector MaxPosition = CornerWorldPositions[1];
    for (int i = 1; i < 4; ++i) {
        MinPosition.X = FMath::Min(MinPosition.X, CornerWorldPositions[i].X);
        MinPosition.Y = FMath::Min(MinPosition.Y, CornerWorldPositions[i].Y);
        MaxPosition.X = FMath::Max(MaxPosition.X, CornerWorldPositions[i].X);
        MaxPosition.Y = FMath::Max(MaxPosition.Y, CornerWorldPositions[i].Y);
    }

    const TVec3d RawMinPosition(MinPosition.X, MinPosition.Y, MinPosition.Z);
    const TVec3d RawMaxPosition(MaxPosition.X, MaxPosition.Y, MaxPosition.Z);

    auto MinCoordinate = GeoReference.GetData().unproject(RawMinPosition);
    auto MaxCoordinate = GeoReference.GetData().unproject(RawMaxPosition);

    // Unproject後の最小最大を再計算
    const auto TempMinCoordinate = MinCoordinate;
    MinCoordinate.latitude = FMath::Min(TempMinCoordinate.latitude, MaxCoordinate.latitude);
    MinCoordinate.longitude = FMath::Min(TempMinCoordinate.longitude, MaxCoordinate.longitude);
    MaxCoordinate.latitude = FMath::Max(TempMinCoordinate.latitude, MaxCoordinate.latitude);
    MaxCoordinate.longitude = FMath::Max(TempMinCoordinate.longitude, MaxCoordinate.longitude);

    FPLATEAUExtent Extent(plateau::geometry::Extent(MinCoordinate, MaxCoordinate));

    Basemap->UpdateAsync(Extent);

    // 何も選択されていない場合は既定の動作(視点移動等)
    if (SelectedHandleIndex == -1) {
        FEditorViewportClient::Tick(DeltaSeconds);
        return;
    }

    FVector CurrentCursorPosition;
    if (!TryGetWorldPositionOfCursor(CurrentCursorPosition))
        return;

    const auto Offset = CurrentCursorPosition - TrackingStartedPosition;

    ExtentGizmo->SetHandlePosition(SelectedHandleIndex, TrackingStartedGizmoPosition + Offset);
}

void FPLATEAUExtentEditorViewportClient::Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI) {
    FEditorViewportClient::Draw(View, PDI);

    constexpr FColor SelectedColor(225, 225, 110);
    constexpr FColor UnselectedColor(20, 20, 220);

    for (int i = 0; i < 4; ++i) {
        const auto HitProxy = new HPLATEAUExtentHandleProxy(i);
        PDI->SetHitProxy(HitProxy);
        const FColor Color = i == SelectedHandleIndex ? SelectedColor : UnselectedColor;
        ExtentGizmo->DrawHandle(i, Color, View, PDI);
        PDI->SetHitProxy(nullptr);
    }
    ExtentGizmo->DrawExtent(View, PDI);

    for (const auto& Gizmo : MeshCodeGizmos) {
        Gizmo.DrawExtent(View, PDI);
    }
}

void FPLATEAUExtentEditorViewportClient::TrackingStarted(const FInputEventState& InInputState, bool bIsDragging,
    bool bNudge) {
    const auto HitProxy = static_cast<HPLATEAUExtentHandleProxy*>(Viewport->GetHitProxy(CachedMouseX, CachedMouseY));
    if (!HitProxy)
        return;

    if (!TryGetWorldPositionOfCursor(TrackingStartedPosition))
        return;

    TrackingStartedGizmoPosition = ExtentGizmo->GetHandlePosition(HitProxy->Index);
    SelectedHandleIndex = HitProxy->Index;
}

void FPLATEAUExtentEditorViewportClient::TrackingStopped() {
    SelectedHandleIndex = -1;
}

bool FPLATEAUExtentEditorViewportClient::ShouldScaleCameraSpeedByDistance() const {
    return true;
}

FVector FPLATEAUExtentEditorViewportClient::GetWorldPosition(uint32 X, uint32 Y) {
    FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
        Viewport,
        GetScene(),
        EngineShowFlags)
        .SetRealtimeUpdate(IsRealtime()));

    const FSceneView* View = CalcSceneView(&ViewFamily);

    const auto Location = FViewportCursorLocation(View,
        this,
        X,
        Y
    );

    const FPlane Plane(FVector::ZeroVector, FVector::UpVector);
    const auto StartPoint = Location.GetOrigin();
    const auto EndPoint = Location.GetOrigin() + Location.GetDirection() * 100000.0;
    FVector Position;
    FMath::SegmentPlaneIntersection(
        StartPoint,
        EndPoint,
        Plane, Position);
    return Position;
}

bool FPLATEAUExtentEditorViewportClient::TryGetWorldPositionOfCursor(FVector& Position) {
    const auto CursorLocation = GetCursorWorldLocationFromMousePos();
    const FPlane Plane(FVector::ZeroVector, FVector::UpVector);
    const auto StartPoint = CursorLocation.GetOrigin();
    const auto EndPoint = CursorLocation.GetOrigin() + CursorLocation.GetDirection() * 100000.0;
    return FMath::SegmentPlaneIntersection(
        StartPoint,
        EndPoint,
        Plane, Position);
}

#undef LOCTEXT_NAMESPACE
