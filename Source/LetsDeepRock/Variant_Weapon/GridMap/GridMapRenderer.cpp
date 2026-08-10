// 网格地图系统 - 64x64 区块渲染层实现

#include "GridMapRenderer.h"
#include "GridMapData.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

UGridMapRenderer::UGridMapRenderer()
{
	PrimaryComponentTick.bCanEverTick = false;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded()) { TileMesh = CubeMesh.Object; }
}

void UGridMapRenderer::BuildFromMapData(UGridMapData* MapData)
{
	ClearAllChunks();
	CachedMapData = MapData;
	if (!MapData) { return; }

	for (const FGridTileData& Tile : MapData->GetAllTiles())
	{
		if (Tile.TileType != EGridTileType::Tree && Tile.TileType != EGridTileType::Rock && Tile.TileType != EGridTileType::LargeTree) { continue; }
		FChunkRenderData& Chunk = Chunks.FindOrAdd({Tile.GridX / ChunkSize, Tile.GridY / ChunkSize});
		TArray<FTransform>* Transforms = Tile.TileType == EGridTileType::Tree ? &Chunk.TreeTransforms
			: Tile.TileType == EGridTileType::Rock ? &Chunk.RockTransforms : &Chunk.LargeTreeTransforms;
		Transforms->Add(MakeTileTransform(Tile));
	}

	for (TPair<FIntPoint, FChunkRenderData>& Pair : Chunks)
	{
		FChunkRenderData& Chunk = Pair.Value;
		Chunk.TreeHISM = CreateChunkHISM(Pair.Key, EGridTileType::Tree, Chunk.TreeTransforms);
		Chunk.RockHISM = CreateChunkHISM(Pair.Key, EGridTileType::Rock, Chunk.RockTransforms);
		Chunk.LargeTreeHISM = CreateChunkHISM(Pair.Key, EGridTileType::LargeTree, Chunk.LargeTreeTransforms);
	}
	LastFlushChunkCount = 0;
}

void UGridMapRenderer::MarkDirtyChunk(const int32 GridX, const int32 GridY)
{
	if (CachedMapData && CachedMapData->IsValidCoord(GridX, GridY)) { DirtyChunks.Add({GridX / ChunkSize, GridY / ChunkSize}); }
}

void UGridMapRenderer::FlushDirtyChunks()
{
	LastFlushChunkCount = DirtyChunks.Num();
	TArray<FIntPoint> Pending = DirtyChunks.Array();
	DirtyChunks.Reset();
	for (const FIntPoint ChunkCoord : Pending) { RebuildChunk(ChunkCoord); }
}

void UGridMapRenderer::RemoveTileInstance(const int32 GridX, const int32 GridY)
{
	MarkDirtyChunk(GridX, GridY);
}

void UGridMapRenderer::RefreshTileInstance(UGridMapData* MapData, const int32 GridX, const int32 GridY)
{
	CachedMapData = MapData;
	MarkDirtyChunk(GridX, GridY);
}

int32 UGridMapRenderer::GetTotalInstanceCount() const
{
	return GetInstanceCountForType(EGridTileType::Tree) + GetInstanceCountForType(EGridTileType::Rock) + GetInstanceCountForType(EGridTileType::LargeTree);
}

int32 UGridMapRenderer::GetInstanceCountForType(const EGridTileType Type) const
{
	int32 Count = 0;
	for (const TPair<FIntPoint, FChunkRenderData>& Pair : Chunks)
	{
		Count += Type == EGridTileType::Tree ? Pair.Value.TreeTransforms.Num()
			: Type == EGridTileType::Rock ? Pair.Value.RockTransforms.Num()
			: Type == EGridTileType::LargeTree ? Pair.Value.LargeTreeTransforms.Num() : 0;
	}
	return Count;
}

void UGridMapRenderer::ClearAllChunks()
{
	for (TPair<FIntPoint, FChunkRenderData>& Pair : Chunks) { DestroyChunkComponents(Pair.Value); }
	Chunks.Reset();
	DirtyChunks.Reset();
	DynamicHISMs.Reset();
}

void UGridMapRenderer::CollectChunk(const FIntPoint& ChunkCoord, FChunkRenderData& OutData) const
{
	if (!CachedMapData) { return; }
	const FGridMapConfig& Config = CachedMapData->GetConfig();
	const int32 MinX = ChunkCoord.X * ChunkSize, MinY = ChunkCoord.Y * ChunkSize;
	const int32 MaxX = FMath::Min(MinX + ChunkSize, Config.MapWidth), MaxY = FMath::Min(MinY + ChunkSize, Config.MapHeight);
	for (int32 Y = MinY; Y < MaxY; ++Y)
	{
		for (int32 X = MinX; X < MaxX; ++X)
		{
			const FGridTileData* Tile = CachedMapData->GetTile(X, Y);
			if (!Tile) { continue; }
			TArray<FTransform>* Transforms = Tile->TileType == EGridTileType::Tree ? &OutData.TreeTransforms
				: Tile->TileType == EGridTileType::Rock ? &OutData.RockTransforms
				: Tile->TileType == EGridTileType::LargeTree ? &OutData.LargeTreeTransforms : nullptr;
			if (Transforms) { Transforms->Add(MakeTileTransform(*Tile)); }
		}
	}
}

void UGridMapRenderer::RebuildChunk(const FIntPoint& ChunkCoord)
{
	if (FChunkRenderData* Existing = Chunks.Find(ChunkCoord)) { DestroyChunkComponents(*Existing); Chunks.Remove(ChunkCoord); }
	FChunkRenderData NewChunk;
	CollectChunk(ChunkCoord, NewChunk);
	if (NewChunk.TreeTransforms.IsEmpty() && NewChunk.RockTransforms.IsEmpty() && NewChunk.LargeTreeTransforms.IsEmpty()) { return; }
	NewChunk.TreeHISM = CreateChunkHISM(ChunkCoord, EGridTileType::Tree, NewChunk.TreeTransforms);
	NewChunk.RockHISM = CreateChunkHISM(ChunkCoord, EGridTileType::Rock, NewChunk.RockTransforms);
	NewChunk.LargeTreeHISM = CreateChunkHISM(ChunkCoord, EGridTileType::LargeTree, NewChunk.LargeTreeTransforms);
	Chunks.Add(ChunkCoord, MoveTemp(NewChunk));
}

void UGridMapRenderer::DestroyChunkComponents(FChunkRenderData& Chunk)
{
	UHierarchicalInstancedStaticMeshComponent* Components[] = {Chunk.TreeHISM, Chunk.RockHISM, Chunk.LargeTreeHISM};
	for (UHierarchicalInstancedStaticMeshComponent* Component : Components)
	{
		if (Component) { DynamicHISMs.Remove(Component); Component->ClearInstances(); Component->DestroyComponent(); }
	}
}

UHierarchicalInstancedStaticMeshComponent* UGridMapRenderer::CreateChunkHISM(const FIntPoint& ChunkCoord, const EGridTileType Type, const TArray<FTransform>& Transforms)
{
	AActor* Owner = GetOwner();
	if (Transforms.IsEmpty() || !Owner || !TileMesh) { return nullptr; }
	const TCHAR* TypeName = Type == EGridTileType::Tree ? TEXT("Tree") : Type == EGridTileType::Rock ? TEXT("Rock") : TEXT("LargeTree");
	const FName Name = MakeUniqueObjectName(Owner, UHierarchicalInstancedStaticMeshComponent::StaticClass(),
		FName(*FString::Printf(TEXT("GridChunk_%d_%d_%s"), ChunkCoord.X, ChunkCoord.Y, TypeName)));
	UHierarchicalInstancedStaticMeshComponent* HISM = NewObject<UHierarchicalInstancedStaticMeshComponent>(Owner, Name);
	Owner->AddInstanceComponent(HISM);
	HISM->SetStaticMesh(TileMesh);
	HISM->SetMobility(EComponentMobility::Movable);
	HISM->SetCullDistances(0, 50000);
	HISM->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	HISM->SetCollisionObjectType(ECollisionChannel::ECC_WorldStatic);
	HISM->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
	HISM->SetGenerateOverlapEvents(false);
	HISM->SetCanEverAffectNavigation(true);
	if (UMaterialInterface* Material = GetMaterialForType(Type)) { HISM->SetMaterial(0, Material); }
	else if (UMaterialInterface* BaseMaterial = HISM->GetMaterial(0))
	{
		UMaterialInstanceDynamic* DynamicMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, Owner);
		if (DynamicMaterial)
		{
			const FLinearColor Color = Type == EGridTileType::Tree ? FLinearColor(0.1f, 0.7f, 0.1f)
				: Type == EGridTileType::Rock ? FLinearColor(0.5f, 0.5f, 0.5f) : FLinearColor(0.05f, 0.4f, 0.05f);
			DynamicMaterial->SetVectorParameterValue(TEXT("BaseColor"), Color); DynamicMaterial->SetVectorParameterValue(TEXT("Color"), Color); HISM->SetMaterial(0, DynamicMaterial);
		}
	}
	HISM->AttachToComponent(Owner->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
	HISM->RegisterComponent();
	// UE 5.8 三参数签名为 (Transforms, bShouldReturnIndices, bWorldSpace)。
	HISM->AddInstances(Transforms, false, true);
	DynamicHISMs.Add(HISM);
	return HISM;
}

FTransform UGridMapRenderer::MakeTileTransform(const FGridTileData& Tile) const
{
	const FGridMapConfig& Config = CachedMapData->GetConfig();
	const FVector WorldPosition = CachedMapData->GridToWorld(Tile.GridX, Tile.GridY);
	const float Height = GetHeightForType(Tile.TileType);
	return FTransform(FRotator::ZeroRotator, FVector(WorldPosition.X, WorldPosition.Y, Height * 0.5f), FVector(Config.TileSize / 100.0f, Config.TileSize / 100.0f, Height / 100.0f));
}

float UGridMapRenderer::GetHeightForType(const EGridTileType Type) const
{
	return Type == EGridTileType::Tree ? TreeHeight : Type == EGridTileType::Rock ? RockHeight : Type == EGridTileType::LargeTree ? LargeTreeHeight : 0.0f;
}

UMaterialInterface* UGridMapRenderer::GetMaterialForType(const EGridTileType Type) const
{
	return Type == EGridTileType::Tree ? TreeMaterial : Type == EGridTileType::Rock ? RockMaterial : Type == EGridTileType::LargeTree ? LargeTreeMaterial : nullptr;
}
