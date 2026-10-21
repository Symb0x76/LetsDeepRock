#include "Swarm/SwarmSpatialHash.h"

FSwarmSpatialHash::FSwarmSpatialHash(float InCellSize)
	: CellSize(FMath::Max(InCellSize, 1.f))
{
}

void FSwarmSpatialHash::Reset()
{
	Cells.Reset();
}

FIntPoint FSwarmSpatialHash::CellOf(const FVector2D& Position) const
{
	return FIntPoint(FMath::FloorToInt(Position.X / CellSize), FMath::FloorToInt(Position.Y / CellSize));
}

void FSwarmSpatialHash::Insert(int32 Id, const FVector2D& Position)
{
	FEntry& Entry = Cells.FindOrAdd(CellOf(Position)).AddDefaulted_GetRef();
	Entry.Id = Id;
	Entry.Position = Position;
}

void FSwarmSpatialHash::Query(const FVector2D& Center, float Radius, TArray<int32>& OutIds) const
{
	const FIntPoint Min = CellOf(Center - FVector2D(Radius, Radius));
	const FIntPoint Max = CellOf(Center + FVector2D(Radius, Radius));
	const float RadiusSq = FMath::Square(Radius);
	for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
	{
		for (int32 X = Min.X; X <= Max.X; ++X)
		{
			const TArray<FEntry>* Bucket = Cells.Find(FIntPoint(X, Y));
			if (!Bucket)
			{
				continue;
			}
			for (const FEntry& Entry : *Bucket)
			{
				if (FVector2D::DistSquared(Entry.Position, Center) <= RadiusSq)
				{
					OutIds.Add(Entry.Id);
				}
			}
		}
	}
}
