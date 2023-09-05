// Copyright © 2023 Ministry of Land, Infrastructure and Transport

#include "PLATEAUMeshLoader.h"
#include "PLATEAUTextureLoader.h"

#include "plateau/polygon_mesh/mesh_extractor.h"
#include "citygml/citygml.h"

#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshResources.h"
#include "PhysicsEngine/BodySetup.h"
#include "ImageUtils.h"
#include "MeshElementRemappings.h"
#include "PLATEAUCityModelLoader.h"
#include "PLATEAUCityObjectGroup.h"
#include "PLATEAUInstancedCityModel.h"
#include "StaticMeshAttributes.h"
#include "Misc/DefaultValueHelper.h"


#if WITH_EDITOR

DECLARE_STATS_GROUP(TEXT("PLATEAUMeshLoader"), STATGROUP_PLATEAUMeshLoader, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("Mesh.Build"), STAT_Mesh_Build, STATGROUP_PLATEAUMeshLoader);

namespace {
    void ComputeNormals(FStaticMeshAttributes& Attributes) {
        const auto Normals = Attributes.GetVertexInstanceNormals();
        const auto Indices = Attributes.GetVertexInstanceVertexIndices();
        const auto Vertices = Attributes.GetVertexPositions();

        const uint32 NumFaces = Indices.GetNumElements() / 3;
        for (uint32 FaceIndex = 0; FaceIndex < NumFaces; ++FaceIndex) {
            const int32 FaceOffset = FaceIndex * 3;

            FVector3f VertexPositions[3];
            int32 VertexIndices[3];

            // Retrieve vertex indices and positions
            VertexIndices[0] = Indices[FaceOffset];
            VertexPositions[0] = Vertices[VertexIndices[0]];

            VertexIndices[1] = Indices[FaceOffset + 1];
            VertexPositions[1] = Vertices[VertexIndices[1]];

            VertexIndices[2] = Indices[FaceOffset + 2];
            VertexPositions[2] = Vertices[VertexIndices[2]];


            // Calculate normal for triangle face			
            FVector3f N = FVector3f::CrossProduct((VertexPositions[0] - VertexPositions[1]), (VertexPositions[0] - VertexPositions[2]));
            N.Normalize();

            // Unrolled loop
            Normals[FaceOffset + 0] += N;
            Normals[FaceOffset + 1] += N;
            Normals[FaceOffset + 2] += N;
        }

        for (int i = 0; i < Normals.GetNumElements(); ++i) {
            Normals[i].Normalize();
        }
    }

    bool ConvertMesh(const plateau::polygonMesh::Mesh& InMesh, FMeshDescription& OutMeshDescription) {
        FStaticMeshAttributes Attributes(OutMeshDescription);

        // UVチャンネル数を3に設定
        const auto VertexInstanceUVs = Attributes.GetVertexInstanceUVs();
        if (VertexInstanceUVs.GetNumChannels() < 4) {
            VertexInstanceUVs.SetNumChannels(4);
        }

        const auto& InVertices = InMesh.getVertices();
        const auto& InIndices = InMesh.getIndices();

        const auto FaceCount = InIndices.size() / 3;
        // 同じ頂点は複数の面に利用されないように複製されるため、頂点数はインデックス数と同じサイズになる。
        const auto VertexCount = InIndices.size();

        OutMeshDescription.ReserveNewVertices(VertexCount);
        OutMeshDescription.ReserveNewPolygons(FaceCount);
        OutMeshDescription.ReserveNewVertexInstances(VertexCount);
        OutMeshDescription.ReserveNewEdges(VertexCount);

        const auto VertexPositions = Attributes.GetVertexPositions();
        for (const auto& Vertex : InVertices) {
            const auto VertexID = OutMeshDescription.CreateVertex();
            VertexPositions[VertexID] = FVector3f(Vertex.x, Vertex.y, Vertex.z);
        }

        // 頂点の再利用を防ぐため使用済みの頂点を保持
        TSet<unsigned> UsedVertexIDs;

        const auto PolygonGroupImportedMaterialSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

        for (const auto& SubMesh : InMesh.getSubMeshes()) {
            const auto PolygonGroupID = OutMeshDescription.CreatePolygonGroup();

            // マテリアル設定
            FString MaterialName = "DefaultMaterial";
            const auto& TexturePath = SubMesh.getTexturePath();
            if (TexturePath != "") {
                MaterialName = FPaths::GetBaseFilename(UTF8_TO_TCHAR(TexturePath.c_str()));
            }
            PolygonGroupImportedMaterialSlotNames[PolygonGroupID] = FName(MaterialName);

            // インデックス、UV設定
            const auto& StartIndex = SubMesh.getStartIndex();
            const auto& EndIndex = SubMesh.getEndIndex();
            TArray<FVertexInstanceID> VertexInstanceIDs;
            for (int InIndexIndex = StartIndex; InIndexIndex <= EndIndex; ++InIndexIndex) {
                auto VertexID = InIndices[InIndexIndex];

                // 頂点が使用済みの場合は複製
                if (UsedVertexIDs.Contains(VertexID)) {
                    const auto NewVertexID = OutMeshDescription.CreateVertex();
                    VertexPositions[NewVertexID] = VertexPositions[VertexID];
                    VertexID = NewVertexID;
                }

                const auto NewVertexInstanceID = OutMeshDescription.CreateVertexInstance(VertexID);
                VertexInstanceIDs.Add(NewVertexInstanceID);

                const auto InUV1 = InMesh.getUV1()[InIndices[InIndexIndex]];
                const auto UV1 = FVector2f(InUV1.x, 1.0f - InUV1.y);
                VertexInstanceUVs.Set(NewVertexInstanceID, 0, UV1);

                const auto InUV4 = InMesh.getUV4()[InIndices[InIndexIndex]];
                const auto UV4 = FVector2f(InUV4.x, InUV4.y);
                VertexInstanceUVs.Set(NewVertexInstanceID, 3, UV4);

                UsedVertexIDs.Add(VertexID);
            }

            // 3頂点毎にPolygonを生成
            TArray<FVertexInstanceID> VertexInstanceIDsCache;
            VertexInstanceIDsCache.SetNumUninitialized(3);
            TArray<FVector3f> TriangleVerticesCache;
            TriangleVerticesCache.SetNumUninitialized(3);
            for (int32 TriangleIndex = 0; TriangleIndex < (EndIndex - StartIndex + 1) / 3; ++TriangleIndex) {
                FMemory::Memcpy(VertexInstanceIDsCache.GetData(), VertexInstanceIDs.GetData() + TriangleIndex * 3, sizeof(FVertexInstanceID) * 3);

                // Invert winding order for triangles
                VertexInstanceIDsCache.Swap(0, 2);

                const FPolygonID NewPolygonID = OutMeshDescription.CreatePolygon(PolygonGroupID, VertexInstanceIDsCache);
                // Fill in the polygon's Triangles - this won't actually do any polygon triangulation as we always give it triangles
                OutMeshDescription.ComputePolygonTriangulation(NewPolygonID);
            }
        }

        ComputeNormals(Attributes);

        //Compact the MeshDescription, if there was visibility mask or some bounding box clip, it need to be compacted so the sparse array are from 0 to n with no invalid data in between. 
        FElementIDRemappings ElementIDRemappings;
        OutMeshDescription.Compact(ElementIDRemappings);
        return OutMeshDescription.Polygons().Num() > 0;
    }

    UStaticMesh* CreateStaticMesh(const plateau::polygonMesh::Mesh& InMesh, UObject* InOuter, FName Name) {
        const auto StaticMesh = NewObject<UStaticMesh>(InOuter, Name);

        StaticMesh->InitResources();
        // make sure it has a new lighting guid
        StaticMesh->SetLightingGuid();

        // Set it to use textured lightmaps. Note that Build Lighting will do the error-checking (texcoordindex exists for all LODs, etc).
        StaticMesh->SetLightMapResolution(64);
        StaticMesh->SetLightMapCoordinateIndex(1);

        FStaticMeshSourceModel& SrcModel = StaticMesh->AddSourceModel();
        /*Don't allow the engine to recalculate normals*/
        SrcModel.BuildSettings.bRecomputeNormals = false;
        SrcModel.BuildSettings.bRecomputeTangents = false;
        SrcModel.BuildSettings.bRemoveDegenerates = false;
        SrcModel.BuildSettings.bUseHighPrecisionTangentBasis = false;
        SrcModel.BuildSettings.bUseFullPrecisionUVs = false;
        SrcModel.BuildSettings.bBuildReversedIndexBuffer = false;

        return StaticMesh;
    }
}

void FPLATEAUMeshLoader::LoadModel(AActor* ModelActor, USceneComponent* ParentComponent, const std::shared_ptr<plateau::polygonMesh::Model> Model,
                                   const FLoadInputData& LoadInputData, const std::shared_ptr<const citygml::CityModel> CityModel, TAtomic<bool>* bCanceled) {
    UE_LOG(LogTemp, Log, TEXT("Model->getRootNodeCount(): %d"), Model->getRootNodeCount());
    for (int i = 0; i < Model->getRootNodeCount(); i++) {

        if (bCanceled->Load(EMemoryOrder::Relaxed))
            break;

        LoadNodeRecursive(ParentComponent, Model->getRootNodeAt(i), LoadInputData, CityModel, *ModelActor);

        // メッシュをワールド内にビルド
        const auto CopiedStaticMeshes = StaticMeshes;
        FFunctionGraphTask::CreateAndDispatchWhenReady(
            [CopiedStaticMeshes, &bCanceled]() {
                UStaticMesh::BatchBuild(CopiedStaticMeshes, true, [&bCanceled](UStaticMesh* mesh) {
                    return bCanceled->Load(EMemoryOrder::Relaxed);
                    });

            }, TStatId(), nullptr, ENamedThreads::GameThread);
        StaticMeshes.Reset();
    }

    // 最大LOD以外の形状を非表示化
    FFunctionGraphTask::CreateAndDispatchWhenReady(
    [ParentComponent]() {
        APLATEAUInstancedCityModel::FilterLowLods(ParentComponent);
    }, TStatId(), nullptr, ENamedThreads::GameThread)->Wait();
}

void FPLATEAUMeshLoader::LoadNodeRecursive(
    USceneComponent* InParentComponent,
    const plateau::polygonMesh::Node& InNode,
    const FLoadInputData& InLoadInputData,
    const std::shared_ptr<const citygml::CityModel> InCityModel,
    AActor& InActor) {
    UStaticMeshComponent* Component = LoadNode(InParentComponent, InNode, InLoadInputData, InCityModel, InActor);

    for (int i = 0; i < InNode.getChildCount(); i++) {
        const auto& TargetNode = InNode.getChildAt(i);

        LoadNodeRecursive(Component, TargetNode, InLoadInputData, InCityModel, InActor);
    }
}

UStaticMeshComponent* FPLATEAUMeshLoader::CreateStaticMeshComponent(AActor& Actor, USceneComponent& ParentComponent, const plateau::polygonMesh::Mesh& InMesh,
                                                                    const FLoadInputData& LoadInputData,
                                                                    const std::shared_ptr<const citygml::CityModel> CityModel, const std::string& InNodeName) {
    // コンポーネント作成
    const FString NodeName = UTF8_TO_TCHAR(InNodeName.c_str());
    UStaticMesh* StaticMesh;
    UStaticMeshComponent* Component = nullptr;
    UStaticMeshComponent* ComponentRef = nullptr;
    FMeshDescription* MeshDescription;
    {
        FFunctionGraphTask::CreateAndDispatchWhenReady([&]() {
            if (LoadInputData.bIncludeAttrInfo) {
                const auto& PLATEAUCityObjectGroup = NewObject<UPLATEAUCityObjectGroup>(&Actor, NAME_None);
                PLATEAUCityObjectGroup->SerializeCityObject(InNodeName, InMesh, LoadInputData, CityModel);
                Component = PLATEAUCityObjectGroup;
            } else {
                Component = NewObject<UStaticMeshComponent>(&Actor, NAME_None);
            }

            if (bAutomationTest) {
                Component->Mobility = EComponentMobility::Movable;
            } else {
                Component->Mobility = EComponentMobility::Static;
            }
            Component->bVisualizeComponent = true;

            // StaticMesh作成
            StaticMesh = CreateStaticMesh(InMesh, Component, FName(NodeName));
            MeshDescription = StaticMesh->CreateMeshDescription(0);
        }, TStatId(), nullptr, ENamedThreads::GameThread)->Wait();
    }

    ConvertMesh(InMesh, *MeshDescription);

    FFunctionGraphTask::CreateAndDispatchWhenReady(
        [&]() {
            StaticMesh->CommitMeshDescription(0);
        }, TStatId(), nullptr, ENamedThreads::GameThread)->Wait();

        StaticMeshes.Add(StaticMesh);
        StaticMesh->OnPostMeshBuild().AddLambda(
            [Component](UStaticMesh* Mesh) {
                if (Component == nullptr)
                    return;
                FFunctionGraphTask::CreateAndDispatchWhenReady(
                    [Component, Mesh] {
                        Component->SetStaticMesh(Mesh);

                        // Collision情報設定
                        Mesh->CreateBodySetup();
                        Mesh->GetBodySetup()->CollisionTraceFlag = ECollisionTraceFlag::CTF_UseComplexAsSimple;
                    }, TStatId(), nullptr, ENamedThreads::GameThread)->Wait();
            });

        FGraphEventRef Task = FFunctionGraphTask::CreateAndDispatchWhenReady([&] {
            // ビルド前にImportVersionを設定する必要がある。
            StaticMesh->ImportVersion = EImportStaticMeshVersion::LastVersion;

            // TODO: 適切なフラグの設定
            // https://docs.unrealengine.com/4.26/ja/ProgrammingAndScripting/ProgrammingWithCPP/UnrealArchitecture/Objects/Creation/
            //StaticMesh->SetFlags();
            }, TStatId(), nullptr, ENamedThreads::GameThread);
        Task->Wait();


        // テクスチャ読み込み(無ければnullptrを入れる)
        TArray<UTexture2D*> SubMeshTextures;
        for (const auto& SubMesh : InMesh.getSubMeshes()) {
            FString TexturePath = UTF8_TO_TCHAR(SubMesh.getTexturePath().c_str());
            const auto Texture = FPLATEAUTextureLoader::Load(TexturePath);
            SubMeshTextures.Add(Texture);
        }

        const auto ComponentSetupTask = FFunctionGraphTask::CreateAndDispatchWhenReady(
            [&, SubMeshTextures] {
                // マテリアル作成
                for (const auto& Texture : SubMeshTextures) {
                    const auto SourceMaterialPath =
                        Texture != nullptr
                        ? TEXT("/PLATEAU-SDK-for-Unreal/DefaultMaterial")
                        : TEXT("/PLATEAU-SDK-for-Unreal/DefaultMaterial_No_Texture");
                    UMaterial* Mat = Cast<UMaterial>(StaticLoadObject(UMaterial::StaticClass(), nullptr, SourceMaterialPath));
                    UMaterialInstanceDynamic* DynMaterial = UMaterialInstanceDynamic::Create(Mat, Component);
                    if (Texture != nullptr) {
                        DynMaterial->SetTextureParameterValue("Texture", Texture);
                    }
                    DynMaterial->TwoSided = false;
                    StaticMesh->AddMaterial(DynMaterial);
                }

                // 名前設定、ヒエラルキー設定など
                Component->DepthPriorityGroup = SDPG_World;
                FString NewUniqueName = StaticMesh->GetName();
                if (!Component->Rename(*NewUniqueName, nullptr, REN_Test)) {
                    NewUniqueName = MakeUniqueObjectName(&Actor, UPLATEAUCityObjectGroup::StaticClass(), FName(StaticMesh->GetName())).ToString();
                }
                Component->Rename(*NewUniqueName, nullptr, REN_DontCreateRedirectors);
                Actor.AddInstanceComponent(Component);
                Component->RegisterComponent();
                Component->AttachToComponent(&ParentComponent, FAttachmentTransformRules::KeepWorldTransform);
                Component->PostEditChange();
                ComponentRef = Component;
            }, TStatId(), nullptr, ENamedThreads::GameThread);
        ComponentSetupTask->Wait();

        return ComponentRef;
}

UStaticMeshComponent* FPLATEAUMeshLoader::LoadNode(USceneComponent* ParentComponent, const plateau::polygonMesh::Node& Node,
                                                   const FLoadInputData& LoadInputData, const std::shared_ptr<const citygml::CityModel> CityModel,
                                                   AActor& Actor) {
    if (Node.getMesh() == nullptr) {
        const auto& CityObject = CityModel->getCityObjectById(Node.getName());
        UStaticMeshComponent* Comp = nullptr;
        UClass* StaticClass;
        FString DesiredName = UTF8_TO_TCHAR(Node.getName().c_str());
        const FGraphEventRef Task = FFunctionGraphTask::CreateAndDispatchWhenReady([&, DesiredName] {
            // CityObjectがある場合はUPLATEAUCityObjectGroupとする
            if (CityObject != nullptr && LoadInputData.bIncludeAttrInfo) {
                StaticClass = UPLATEAUCityObjectGroup::StaticClass();
                const auto& PLATEAUCityObjectGroup = NewObject<UPLATEAUCityObjectGroup>(&Actor, NAME_None);
                PLATEAUCityObjectGroup->SerializeCityObject(Node, CityObject);
                Comp = PLATEAUCityObjectGroup;
            } else {
                StaticClass = UStaticMeshComponent::StaticClass();
                Comp = NewObject<UStaticMeshComponent>(&Actor, NAME_None);
            }
            FString NewUniqueName = FString(DesiredName);
            if (!Comp->Rename(*NewUniqueName, nullptr, REN_Test)) {
                NewUniqueName = MakeUniqueObjectName(&Actor, StaticClass, FName(DesiredName)).ToString();
            }
            Comp->Rename(*NewUniqueName, nullptr, REN_DontCreateRedirectors);

            check(Comp != nullptr);
            if (bAutomationTest) {
                Comp->Mobility = EComponentMobility::Movable;
            } else {
                Comp->Mobility = EComponentMobility::Static;
            }

            Actor.AddInstanceComponent(Comp);
            Comp->RegisterComponent();
            Comp->AttachToComponent(ParentComponent, FAttachmentTransformRules::KeepWorldTransform);
        }, TStatId(), nullptr, ENamedThreads::GameThread);
        Task->Wait();
        return Comp;
    }

    // TODO: 空のMeshが入っている問題
    if (Node.getMesh()->getVertices().size() == 0)
        return nullptr;

    return CreateStaticMeshComponent(Actor, *ParentComponent, *Node.getMesh(), LoadInputData, CityModel, Node.getName());
}

#endif
