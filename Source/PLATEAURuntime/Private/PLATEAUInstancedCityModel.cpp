// Copyright © 2023 Ministry of Land, Infrastructure and Transport


#include "PLATEAUInstancedCityModel.h"
#include "Misc/DefaultValueHelper.h"
#include <plateau/dataset/i_dataset_accessor.h>
#include <plateau/granularity_convert/granularity_converter.h>
#include <citygml/citygml.h>
#include <citygml/citymodel.h>
#include "CityGML/PLATEAUCityGmlProxy.h"
#include <PLATEAUMeshExporter.h>
#include <PLATEAUMeshLoader.h>
#include <PLATEAUExportSettings.h>
#include "Reconstruct/PLATEAUModelReconstruct.h"
#include <Reconstruct/PLATEAUModelClassificationByType.h>
#include <Reconstruct/PLATEAUModelClassificationByAttribute.h>
#include <Reconstruct/PLATEAUModelLandscape.h>
#include <Reconstruct/PLATEAUMeshLoaderForLandscapeMesh.h>
#include <Reconstruct/PLATEAUModelAlignLand.h>
#include "Tasks/Pipe.h"

using namespace UE::Tasks;
using namespace plateau::granularityConvert;

namespace {
    /**
     * @brief 3D都市モデル内のCityGMLファイルに相当するコンポーネントを入力として、CityGMLファイル名を返します。
     * @return CityGMLファイル名
     */
    FString GetGmlFileName(const USceneComponent* const InGmlComponent) {
        return InGmlComponent->GetName().Append(".gml");
    }

    /**
     * @brief Gmlコンポーネントのパッケージ情報を取得します。
     */
    plateau::dataset::PredefinedCityModelPackage GetCityModelPackage(const USceneComponent* const InGmlComponent) {
        const auto GmlFileName = GetGmlFileName(InGmlComponent);
        // udxのサブフォルダ名は地物種類名に相当するため、UdxSubFolderの関数を使用してgmlのパッケージ種を取得
        return plateau::dataset::UdxSubFolder::getPackage(plateau::dataset::GmlFile(TCHAR_TO_UTF8(*GmlFileName)).getFeatureType());
    }

    /**
     * @brief 指定コンポーネントを基準として子階層のCityObjectを取得
     * @param SceneComponent CityObject取得の基準となるコンポーネント
     * @param RootCityObjects 取得されたCityObject配列
     */
    void GetRootCityObjectsRecursive(USceneComponent* SceneComponent, TArray<FPLATEAUCityObject>& RootCityObjects) {
        if (const auto& CityObjectGroup = Cast<UPLATEAUCityObjectGroup>(SceneComponent)) {
            const auto& AllRootCityObjects = CityObjectGroup->GetAllRootCityObjects();
            for (const auto& CityObject : AllRootCityObjects) {
                RootCityObjects.Add(CityObject);
            }
        }

        for (const auto& AttachedComponent : SceneComponent->GetAttachChildren()) {
            GetRootCityObjectsRecursive(AttachedComponent, RootCityObjects);
        }
    }

    /**
     * @brief 対象コンポーネントとその子コンポーネントのコリジョン設定変更
     * @param ParentComponent コリジョン設定変更対象コンポーネント
     * @param bCollisionResponseBlock コリジョンをブロック設定に変更するか？
     * @param bPropagateToChildren 子コンポーネントのコリジョン設定を変更するか？
     */
    void ApplyCollisionResponseBlockToChannel(USceneComponent* ParentComponent, const bool bCollisionResponseBlock, const bool bPropagateToChildren=false) {
        if (const auto& ParentStaticMeshComponent = Cast<UStaticMeshComponent>(ParentComponent); ParentStaticMeshComponent != nullptr) {
            ParentStaticMeshComponent->SetCollisionResponseToChannel(ECC_Visibility, bCollisionResponseBlock ? ECR_Block : ECR_Ignore);
        }

        if (!bPropagateToChildren)
            return;
        
        if (const TArray<USceneComponent*>& AttachedChildren = ParentComponent->GetAttachChildren(); 0 < AttachedChildren.Num()) {
            TInlineComponentArray<USceneComponent*, NumInlinedActorComponents> ComponentStack;
            ComponentStack.Append(AttachedChildren);
            while (0 < ComponentStack.Num()) {
                if (const auto& CurrentComp = ComponentStack.Pop(/*bAllowShrinking=*/ false); CurrentComp != nullptr) {
                    ComponentStack.Append(CurrentComp->GetAttachChildren());
                    if (const auto& StaticMeshComponent = Cast<UStaticMeshComponent>(CurrentComp); StaticMeshComponent != nullptr) {
                        StaticMeshComponent->SetCollisionResponseToChannel(ECC_Visibility, bCollisionResponseBlock ? ECR_Block : ECR_Ignore);
                    }
                }
            }
        }
    }
}

FString APLATEAUInstancedCityModel::GetOriginalComponentName(const USceneComponent* const InComponent) {
    auto ComponentName = InComponent->GetName();
    int Index = 0;
    if (ComponentName.FindLastChar('_', Index)) {
        if (ComponentName.RightChop(Index + 1).IsNumeric()) {
            ComponentName = ComponentName.LeftChop(ComponentName.Len() - Index + 1);
        }
    }
    return ComponentName;
}

int APLATEAUInstancedCityModel::ParseLodComponent(const USceneComponent* const InLodComponent) {
    auto LodString = GetOriginalComponentName(InLodComponent);
    // "Lod{数字}"から先頭3文字除外することで数字を抜き出す。
    LodString = LodString.RightChop(3);

    int Lod;
    FDefaultValueHelper::ParseInt(LodString, Lod);

    return Lod;
}

void APLATEAUInstancedCityModel::DestroyOrHideComponents(TArray<UPLATEAUCityObjectGroup*> Components, bool bDestroy) {
    for (auto Comp : Components) {
        if (bDestroy)
            Comp->DestroyComponent();
        else
            Comp->SetVisibility(false);
    }
}

// Sets default values
APLATEAUInstancedCityModel::APLATEAUInstancedCityModel() {
    // Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
    PrimaryActorTick.bCanEverTick = true;
}

double APLATEAUInstancedCityModel::GetLatitude() {
    return GeoReference.GetData().unproject(TVec3d(0, 0, 0)).latitude;
}

double APLATEAUInstancedCityModel::GetLongitude() {
    return GeoReference.GetData().unproject(TVec3d(0, 0, 0)).longitude;
}

FPLATEAUCityObjectInfo APLATEAUInstancedCityModel::GetCityObjectInfo(USceneComponent* Component) {
    FPLATEAUCityObjectInfo Result;
    Result.DatasetName = DatasetName;

    if (Component == nullptr)
        return Result;

    Result.ID = GetOriginalComponentName(Component);

    auto GmlComponent = Component;
    while (GmlComponent->GetAttachParent() != RootComponent) {
        GmlComponent = GmlComponent->GetAttachParent();

        // TODO: エラーハンドリング
        if (GmlComponent == nullptr)
            return Result;
    }

    Result.GmlName = GetGmlFileName(GmlComponent);

    return Result;
}

TArray<FPLATEAUCityObject>& APLATEAUInstancedCityModel::GetAllRootCityObjects() {
    if (0 < RootCityObjects.Num()) {
        return RootCityObjects;
    }

    GetRootCityObjectsRecursive(GetRootComponent(), RootCityObjects);
    return RootCityObjects;
}

void APLATEAUInstancedCityModel::FilterLowLods(const USceneComponent* const InGmlComponent, const int MinLod, const int MaxLod) {
    const TArray<USceneComponent*>& AttachedLodChildren = InGmlComponent->GetAttachChildren();

    // 各LODに対して形状データ(コンポーネント)が存在するコンポーネント名を検索
    TMap<int, TSet<FString>> NameMap;
    for (const auto& LodComponent : AttachedLodChildren) {
        const auto Lod = ParseLodComponent(LodComponent);
        auto& Value = NameMap.Add(Lod);

        if (Lod < MinLod || Lod > MaxLod)
            continue;

        TArray<USceneComponent*> FeatureComponents;
        LodComponent->GetChildrenComponents(false, FeatureComponents);
        for (const auto FeatureComponent: FeatureComponents) {
            Value.Add(GetOriginalComponentName(FeatureComponent));
        }
    }

    // フィルタリング実行
    for (const auto& LodComponent : AttachedLodChildren) {
        const TArray<USceneComponent*>& AttachedFeatureChildren = LodComponent->GetAttachChildren();
        const auto Lod = ParseLodComponent(LodComponent);

        if (Lod < MinLod || Lod > MaxLod) {
            // LOD範囲外のLOD形状は非表示化
            for (const auto& FeatureComponent : AttachedFeatureChildren) {
                ApplyCollisionResponseBlockToChannel(FeatureComponent, false, true);
                FeatureComponent->SetVisibility(false, true);
            }
            continue;
        }

        for (const auto& FeatureComponent : AttachedFeatureChildren) {
            auto ComponentName = GetOriginalComponentName(FeatureComponent);
            TArray<int> Keys;
            NameMap.GetKeys(Keys);
            auto bIsMaxLod = true;
            for (const auto Key : Keys) {
                if (Key <= Lod)
                    continue;

                // コンポーネントのLODよりも大きいLODが存在する場合非表示
                if (NameMap[Key].Contains(ComponentName))
                    bIsMaxLod = false;
            }

            ApplyCollisionResponseBlockToChannel(FeatureComponent, bIsMaxLod, true);
            FeatureComponent->SetVisibility(bIsMaxLod, true);
        }
    }
}

void APLATEAUInstancedCityModel::BeginPlay() {
    Super::BeginPlay();
}

void APLATEAUInstancedCityModel::Tick(float DeltaTime) {
    Super::Tick(DeltaTime);
}

plateau::dataset::PredefinedCityModelPackage APLATEAUInstancedCityModel::GetCityModelPackages() const {
    auto Packages = plateau::dataset::PredefinedCityModelPackage::None;
    for (const auto& GmlComponent : GetGmlComponents()) {
        Packages = Packages | GetCityModelPackage(GmlComponent);
    }
    return Packages;
}

APLATEAUInstancedCityModel* APLATEAUInstancedCityModel::FilterByLods(const plateau::dataset::PredefinedCityModelPackage InPackage, const TMap<plateau::dataset::PredefinedCityModelPackage, FPLATEAUMinMaxLod>& PackageToLodRangeMap, const bool bOnlyMaxLod) {
    bIsFiltering = true;

    for (const auto& GmlComponent : GetGmlComponents()) {
        const TArray<USceneComponent*>& AttachedLodChildren = GmlComponent->GetAttachChildren();

        // 一度全ての地物メッシュを不可視にする
        for (const auto& LodComponent : AttachedLodChildren) {
            const TArray<USceneComponent*>& AttachedFeatureChildren = LodComponent->GetAttachChildren();
            for (const auto& FeatureComponent : AttachedFeatureChildren) {
                ApplyCollisionResponseBlockToChannel(FeatureComponent, false, true);
                FeatureComponent->SetVisibility(false, true);
            }
        }

        // 選択されていないパッケージを除外
        const auto Package = GetCityModelPackage(GmlComponent);
        if ((Package & InPackage) == plateau::dataset::PredefinedCityModelPackage::None)
            continue;

        const auto MinLod = PackageToLodRangeMap[Package].MinLod;
        const auto MaxLod = PackageToLodRangeMap[Package].MaxLod;

        // 各地物について全てのLODを表示する場合の処理
        if (!bOnlyMaxLod) {
            for (const auto& LodComponent : AttachedLodChildren) {
                const auto Lod = ParseLodComponent(LodComponent);
                if (MinLod <= Lod && Lod <= MaxLod) {
                    for (const auto& FeatureComponent : LodComponent->GetAttachChildren()) {
                        ApplyCollisionResponseBlockToChannel(FeatureComponent, true, true);
                        FeatureComponent->SetVisibility(true, true);
                    }
                } else {
                    for (const auto& FeatureComponent : LodComponent->GetAttachChildren()) {
                        ApplyCollisionResponseBlockToChannel(FeatureComponent, false, true);
                        FeatureComponent->SetVisibility(false, true);
                    }
                }
            }
            continue;
        }

        // 各地物について最大LODのみを表示する場合の処理
        FilterLowLods(GmlComponent, MinLod, MaxLod);
    }

    bIsFiltering = false;
    return this;
}

APLATEAUInstancedCityModel* APLATEAUInstancedCityModel::FilterByFeatureTypes(const citygml::CityObject::CityObjectsType InCityObjectType) {
    if (!HasAttributeInfo())
        return FilterByFeatureTypesLegacy(InCityObjectType);
    bIsFiltering = true;
    for (const auto& GmlComponent : GetRootComponent()->GetAttachChildren()) {
        // BillboardComponentを無視
        if (GmlComponent.GetName().Contains("BillboardComponent"))
            continue;

        // 起伏は重いため意図的に除外
        const auto Package = GetCityModelPackage(GmlComponent);
        if (Package == plateau::dataset::PredefinedCityModelPackage::Relief)
            continue;

        for (const auto& LodComponent : GmlComponent->GetAttachChildren()) {
            TArray<USceneComponent*> FeatureComponents;
            LodComponent->GetChildrenComponents(true, FeatureComponents);
            for (const auto& FeatureComponent : FeatureComponents) {
                //この時点で不可視状態ならLodフィルタリングで不可視化されたことになるので無視
                if (!FeatureComponent->IsVisible())
                    continue;

                auto FeatureID = FeatureComponent->GetName();
                // BillboardComponentも混ざってるので無視
                if (FeatureID.Contains("BillboardComponent"))
                    continue;

                if (!FeatureComponent->IsA(UPLATEAUCityObjectGroup::StaticClass()))
                    continue;

                const auto CityObjGrp = StaticCast<UPLATEAUCityObjectGroup*>(FeatureComponent);
                const auto ObjList = CityObjGrp->GetAllRootCityObjects();
                if (ObjList.Num() != 1)
                    continue;

                const int64 CityObjectType = UPLATEAUCityObjectBlueprintLibrary::GetTypeAsInt64(ObjList[0].Type);
                if (static_cast<int64>(InCityObjectType) & CityObjectType) 
                    continue;

                ApplyCollisionResponseBlockToChannel(FeatureComponent, false);
                FeatureComponent->SetVisibility(false);
            }
        }
    }  
    bIsFiltering = false;
    return this;
}

APLATEAUInstancedCityModel* APLATEAUInstancedCityModel::FilterByFeatureTypesLegacy(const citygml::CityObject::CityObjectsType InCityObjectType) {
    bIsFiltering = true;
    Launch(
        TEXT("ParseGmlsTask"),
        [this, InCityObjectType, GmlComponents = GetGmlComponents()] {
            // 処理が重いため先にCityGMLのパースを行って内部的にキャッシュしておく。
            for (const auto& GmlComponent : GmlComponents) {
                // BillboardComponentを無視
                if (GmlComponent.GetName().Contains("BillboardComponent"))
                    continue;

                // 起伏は重いため意図的に除外
                const auto Package = GetCityModelPackage(GmlComponent);
                if (Package == plateau::dataset::PredefinedCityModelPackage::Relief)
                    continue;

                FPLATEAUCityObjectInfo GmlInfo;
                GmlInfo.DatasetName = DatasetName;
                GmlInfo.GmlName = GetGmlFileName(GmlComponent);
                const auto CityModel = UPLATEAUCityGmlProxy::Load(GmlInfo);
            }

            // フィルタリング実行。スレッドセーフでない関数を使用するためメインスレッドで実行する。
            const auto GameThreadTask =
                FFunctionGraphTask::CreateAndDispatchWhenReady(
                    [this, InCityObjectType] {
                        FilterByFeatureTypesInternal(InCityObjectType);
                    }, TStatId(), nullptr, ENamedThreads::GameThread);
            GameThreadTask->Wait();

            bIsFiltering = false;
        },
        ETaskPriority::BackgroundHigh);

    return this;
}

FPLATEAUMinMaxLod APLATEAUInstancedCityModel::GetMinMaxLod(const plateau::dataset::PredefinedCityModelPackage InPackage) const {
    TArray<int> Lods;

    for (const auto& GmlComponent : GetGmlComponents()) {
        if ((GetCityModelPackage(GmlComponent) & InPackage) == plateau::dataset::PredefinedCityModelPackage::None)
            continue;

        for (const auto& LodComponent : GmlComponent->GetAttachChildren()) {
            const auto Lod = ParseLodComponent(LodComponent);
            if (Lods.Contains(Lod))
                continue;

            Lods.Add(Lod);
        }
    }

    return { FMath::Min(Lods), FMath::Max(Lods) };
}

bool APLATEAUInstancedCityModel::IsFiltering() {
    return bIsFiltering;
}

const TArray<TObjectPtr<USceneComponent>>& APLATEAUInstancedCityModel::GetGmlComponents() const {
    return GetRootComponent()->GetAttachChildren();
}

TArray<UActorComponent*> APLATEAUInstancedCityModel::GetComponentsByPackage(EPLATEAUCityModelPackage Pkg) const {
    TArray<UActorComponent*> ResultComponents;
    plateau::dataset::PredefinedCityModelPackage Package = UPLATEAUImportSettings::GetPredefinedCityModelPackageFromPLATEAUCityModelPackage(Pkg);
    for (const auto& GmlComponent : GetGmlComponents()) {
        if (GetCityModelPackage(GmlComponent) == Package) {
            if (!GmlComponent.GetName().Contains("BillboardComponent"))
                ResultComponents.Add(GmlComponent);
        }
    }
    return ResultComponents;
}

bool APLATEAUInstancedCityModel::HasAttributeInfo() {
    TArray<USceneComponent*> Components;
    GetRootComponent()->GetChildrenComponents(true, Components);
    return Components.ContainsByPredicate([](const auto& Comp) {
        return Comp->IsA(UPLATEAUCityObjectGroup::StaticClass());
        });
}

void APLATEAUInstancedCityModel::FilterByFeatureTypesInternal(const citygml::CityObject::CityObjectsType InCityObjectType) {
    for (const auto& GmlComponent : GetRootComponent()->GetAttachChildren()) {
        // BillboardComponentを無視
        if (GmlComponent.GetName().Contains("BillboardComponent"))
            continue;

        // 起伏は重いため意図的に除外
        const auto Package = GetCityModelPackage(GmlComponent);
        if (Package == plateau::dataset::PredefinedCityModelPackage::Relief)
            continue;

        for (const auto& LodComponent : GmlComponent->GetAttachChildren()) {
            TArray<USceneComponent*> FeatureComponents;
            LodComponent->GetChildrenComponents(true, FeatureComponents);
            for (const auto& FeatureComponent : FeatureComponents) {
                //この時点で不可視状態ならLodフィルタリングで不可視化されたことになるので無視
                if (!FeatureComponent->IsVisible())
                    continue;

                auto FeatureID = FeatureComponent->GetName();

                // TODO: 最小地物の場合元の地物IDに_{数値}が入っている場合があるため、最小地物についてのみ処理する。よりロバストな方法検討必要
                if (FeatureComponent->GetAttachParent() != LodComponent) {
                    FeatureID = GetOriginalComponentName(FeatureComponent);
                }

                // BillboardComponentも混ざってるので無視
                if (FeatureID.Contains("BillboardComponent"))
                    continue;

                FPLATEAUCityObjectInfo GmlInfo;
                GmlInfo.DatasetName = DatasetName;
                GmlInfo.GmlName = GetGmlFileName(GmlComponent);
                const auto CityModel = UPLATEAUCityGmlProxy::Load(GmlInfo);

                if (CityModel == nullptr) {
                    UE_LOG(LogTemp, Error, TEXT("Invalid Dataset or Gml : %s, %s"), *GmlInfo.DatasetName, *GmlInfo.GmlName);
                    continue;
                }

                const auto CityObject = CityModel->getCityObjectById(TCHAR_TO_UTF8(*FeatureID));
                if (CityObject == nullptr) {
                    UE_LOG(LogTemp, Error, TEXT("Invalid ID : %s"), *FeatureID);
                    continue;
                }

                const auto CityObjectType = CityObject->getType();
                if (static_cast<uint64_t>(InCityObjectType & CityObjectType))
                    continue;

                ApplyCollisionResponseBlockToChannel(FeatureComponent, false);
                FeatureComponent->SetVisibility(false);
            }
        }
    }
}

TTask<TArray<USceneComponent*>> APLATEAUInstancedCityModel::ReconstructModel(const TArray<USceneComponent*> TargetComponents, const EPLATEAUMeshGranularity ReconstructType, bool bDestroyOriginal)  {

    UE_LOG(LogTemp, Log, TEXT("ReconstructModel: %d %d %s"), TargetComponents.Num(), static_cast<int>(ReconstructType), bDestroyOriginal ? TEXT("True") : TEXT("False"));
    TTask<TArray<USceneComponent*>> ReconstructModelTask = Launch(TEXT("ReconstructModelTask"), [this, TargetComponents, ReconstructType, bDestroyOriginal] {       
        FPLATEAUModelReconstruct ModelReconstruct(this, FPLATEAUModelReconstruct::GetConvertGranularityFromReconstructType(ReconstructType));
        const auto& TargetCityObjects = ModelReconstruct.GetUPLATEAUCityObjectGroupsFromSceneComponents(TargetComponents);
        auto Task = ReconstructTask(ModelReconstruct, TargetCityObjects, bDestroyOriginal);
        AddNested(Task);
        Task.Wait();
        FFunctionGraphTask::CreateAndDispatchWhenReady([&]() {
            //終了イベント通知
            OnReconstructFinished.Broadcast();
            }, TStatId(), NULL, ENamedThreads::GameThread);
            
        return Task.GetResult();
    });
    return ReconstructModelTask;
}

TTask<TArray<USceneComponent*>> APLATEAUInstancedCityModel::ClassifyModel(const TArray<USceneComponent*> TargetComponents, TMap<EPLATEAUCityObjectsType, UMaterialInterface*> Materials, const EPLATEAUMeshGranularity ReconstructType, bool bDestroyOriginal) {
    
    UE_LOG(LogTemp, Log, TEXT("ClassifyModelByType: %d %d %s"), TargetComponents.Num(), static_cast<int>(ReconstructType), bDestroyOriginal ? TEXT("True") : TEXT("False"));
    TTask<TArray<USceneComponent*>> ClassifyModelByTypeTask = Launch(TEXT("ClassifyModelByTypeTask"), [&, this, TargetComponents, bDestroyOriginal, Materials, ReconstructType] {

        FPLATEAUModelClassificationByType ModelClassification(this, Materials);
        const auto& TargetCityObjects = ModelClassification.GetUPLATEAUCityObjectGroupsFromSceneComponents(TargetComponents);
        auto Task = ClassifyTask(ModelClassification, TargetCityObjects, ReconstructType, bDestroyOriginal);
        AddNested(Task);
        Task.Wait();
        
        FFunctionGraphTask::CreateAndDispatchWhenReady([&]() {
            //終了イベント通知
            OnClassifyFinished.Broadcast();
            }, TStatId(), NULL, ENamedThreads::GameThread);
        
        return Task.GetResult();
    });
    return ClassifyModelByTypeTask;
}

UE::Tasks::TTask<TArray<USceneComponent*>> APLATEAUInstancedCityModel::ClassifyModel(const TArray<USceneComponent*> TargetComponents, const FString AttributeKey, TMap<FString, UMaterialInterface*> Materials, const EPLATEAUMeshGranularity ReconstructType, bool bDestroyOriginal) {
    
    UE_LOG(LogTemp, Log, TEXT("ClassifyModelByAttr: %d %d %s"), TargetComponents.Num(), static_cast<int>(ReconstructType), bDestroyOriginal ? TEXT("True") : TEXT("False"));
    TTask<TArray<USceneComponent*>> ClassifyModelByAttrTask = Launch(TEXT("ClassifyModelByAttrTask"), [&, this, TargetComponents, AttributeKey, bDestroyOriginal, Materials, ReconstructType] {

        FPLATEAUModelClassificationByAttribute ModelClassification(this, AttributeKey, Materials);
        const auto& TargetCityObjects = ModelClassification.GetUPLATEAUCityObjectGroupsFromSceneComponents(TargetComponents);
        auto Task = ClassifyTask(ModelClassification, TargetCityObjects, ReconstructType, bDestroyOriginal);
        AddNested(Task);
        Task.Wait();

        FFunctionGraphTask::CreateAndDispatchWhenReady([&]() {
            //終了イベント通知
            OnClassifyFinished.Broadcast();
            }, TStatId(), NULL, ENamedThreads::GameThread);

        return Task.GetResult();
        });
    return ClassifyModelByAttrTask;
}

UE::Tasks::TTask<TArray<USceneComponent*>> APLATEAUInstancedCityModel::ClassifyTask(FPLATEAUModelClassification& ModelClassification, const TArray<UPLATEAUCityObjectGroup*> TargetCityObjects, const EPLATEAUMeshGranularity ReconstructType, bool bDestroyOriginal) {

    TTask<TArray<USceneComponent*>> ClassifyTask = Launch(TEXT("ClassifyTask"), [&, TargetCityObjects, ReconstructType, bDestroyOriginal] {

        if (ReconstructType == EPLATEAUMeshGranularity::DoNotChange) {

            //粒度ごとにターゲットを取得して実行
            TArray<USceneComponent*> JoinedResults;
            TArray<ConvertGranularity> GranularityList{ 
                ConvertGranularity::PerAtomicFeatureObject,
                ConvertGranularity::PerPrimaryFeatureObject,
                ConvertGranularity::PerCityModelArea,
                ConvertGranularity::MaterialInPrimary
            };

            for (const auto& Granularity : GranularityList) {
                const auto& Targets = ModelClassification.FilterComponentsByConvertGranularity(TargetCityObjects, Granularity);
                if (Targets.Num() > 0) {
                    ModelClassification.SetConvertGranularity(Granularity);
                    auto GranularityTask = ReconstructTask(ModelClassification, Targets, bDestroyOriginal);
                    AddNested(GranularityTask);
                    GranularityTask.Wait();
                    JoinedResults.Append(GranularityTask.GetResult());
                }
            }
            return JoinedResults;
        }
        else {
            const auto& ConvertGranularity = FPLATEAUModelReconstruct::GetConvertGranularityFromReconstructType(ReconstructType);
            ModelClassification.SetConvertGranularity(ConvertGranularity);
            auto Task = ReconstructTask(ModelClassification, TargetCityObjects, bDestroyOriginal);
            return Task.GetResult();
        }

        });
    return ClassifyTask;
}


UE::Tasks::TTask<TArray<USceneComponent*>> APLATEAUInstancedCityModel::ReconstructTask(FPLATEAUModelReconstruct& ModelReconstruct, const TArray<UPLATEAUCityObjectGroup*> TargetCityObjects, bool bDestroyOriginal) {

    TTask<TArray<USceneComponent*>> ConvertTask = Launch(TEXT("ReconstructTask"), [&, TargetCityObjects, bDestroyOriginal] {
        std::shared_ptr<plateau::polygonMesh::Model> converted = ModelReconstruct.ConvertModelForReconstruct(TargetCityObjects);
        FFunctionGraphTask::CreateAndDispatchWhenReady([&]() {
            //コンポーネント削除
            DestroyOrHideComponents(TargetCityObjects, bDestroyOriginal);
            }, TStatId(), NULL, ENamedThreads::GameThread)
            ->Wait();

        const auto ResultComponents = ModelReconstruct.ReconstructFromConvertedModel(converted);
        return ResultComponents;
    });
    return ConvertTask;
}

//Landscape
UE::Tasks::FTask APLATEAUInstancedCityModel::CreateLandscape(const TArray<USceneComponent*> TargetComponents, FPLATEAULandscapeParam Param, bool bDestroyOriginal) {

    UE_LOG(LogTemp, Log, TEXT("CreateLandscape: %d %s"), TargetComponents.Num(), bDestroyOriginal ? TEXT("True") : TEXT("False"));
    FTask CreateLandscapeTask = Launch(TEXT("CreateLandscapeTask"), [&, TargetComponents, Param, bDestroyOriginal] {

        FPLATEAUModelLandscape Landscape(this);
        const auto& TargetCityObjects = Landscape.GetUPLATEAUCityObjectGroupsFromSceneComponents(TargetComponents);

        FPLATEAUMeshExportOptions ExtOptions;
        ExtOptions.bExportHiddenObjects = false;
        ExtOptions.bExportTexture = true;
        ExtOptions.TransformType = EMeshTransformType::Local;
        ExtOptions.CoordinateSystem = ECoordinateSystem::ESU;
        FPLATEAUMeshExporter MeshExporter;
        std::shared_ptr<plateau::polygonMesh::Model> smodel = MeshExporter.CreateModelFromComponents(this, TargetCityObjects, ExtOptions);

        auto Results = Landscape.CreateHeightMap(smodel, Param);

        // 高さを地形に揃える (LOD3Roadの場合は、ResultのHeightmap書き換え)
        if (Param.AlignLand || Param.InvertRoadLod3) {
            const auto& AlignedComponents = AlignLand(Results, Param, bDestroyOriginal);
            FFunctionGraphTask::CreateAndDispatchWhenReady([&, AlignedComponents, bDestroyOriginal]() {
                // Align コンポーネント削除
                DestroyOrHideComponents(AlignedComponents, bDestroyOriginal);
                }, TStatId(), NULL, ENamedThreads::GameThread)->Wait();
        }

        for (const auto Result : Results) {
            //　平滑化Mesh / Landscape生成
            if (Param.ConvertTerrain) {
                if (!Param.ConvertToLandscape) {
                    //平滑化Mesh生成
                    FPLATEAUMeshLoaderForLandscapeMesh MeshLoader;
                    MeshLoader.CreateMeshFromHeightMap(*this, Param.TextureWidth, Param.TextureHeight, Result.Min, Result.Max, Result.MinUV, Result.MaxUV, Result.Data->data(), Result.NodeName);
                }
                else {
                    //Landscape生成
                    TArray<uint16> HeightData(Result.Data->data(), Result.Data->size());
                    //LandScape  
                    FFunctionGraphTask::CreateAndDispatchWhenReady(
                        [&, HeightData, Result, Param ] {
                            auto LandActor = Landscape.CreateLandScape(GetWorld(), Param.NumSubsections, Param.SubsectionSizeQuads,
                            Param.ComponentCountX, Param.ComponentCountY,
                            Param.TextureWidth, Param.TextureHeight,
                            Result.Min, Result.Max, Result.MinUV, Result.MaxUV, Result.TexturePath, HeightData, Result.NodeName);
                            Landscape.CreateLandScapeReference(LandActor, this, Result.NodeName);
                        }, TStatId(), nullptr, ENamedThreads::GameThread)->Wait();
                }
            }
        }

        FFunctionGraphTask::CreateAndDispatchWhenReady([&, TargetCityObjects, bDestroyOriginal, Results]() {

            // Landscape コンポーネント削除
            if (Param.ConvertTerrain)
                DestroyOrHideComponents(TargetCityObjects, bDestroyOriginal);

            //終了イベント通知
            EPLATEAULandscapeCreationResult Res = Results.Num() > 0 ? EPLATEAULandscapeCreationResult::Success : EPLATEAULandscapeCreationResult::Fail;
            OnLandscapeCreationFinished.Broadcast(Res);
        }, TStatId(), NULL, ENamedThreads::GameThread)->Wait();

    });
    return CreateLandscapeTask;
}

TArray<UPLATEAUCityObjectGroup*> APLATEAUInstancedCityModel::AlignLand(TArray<HeightmapCreationResult>& Results, FPLATEAULandscapeParam Param, bool bDestroyOriginal) {

    FPLATEAUModelAlignLand ModelAlign(this);
    ModelAlign.SetResults(Results, Param);
    TArray<UPLATEAUCityObjectGroup*> TargetCityObjects = ModelAlign.GetTargetCityObjectsForAlignLand();
    //Lod3Roadの場合はLandscape生成前にResultのHeightmap情報書き換え&TargetCityObjectsからLod3Road除外
    if (Param.InvertRoadLod3) 
        Results = ModelAlign.UpdateHeightMapForLod3Road(TargetCityObjects);
    if (Param.AlignLand) 
        ModelAlign.Align(TargetCityObjects);
    return TargetCityObjects;
}