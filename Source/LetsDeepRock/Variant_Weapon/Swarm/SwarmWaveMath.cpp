#include "Swarm/SwarmWaveMath.h"

int32 SwarmComputeBurst(const int32 RemainingQuota, const int32 Alive, const int32 Cap, const int32 BurstSize)
{
	return FMath::Max(0, FMath::Min3(RemainingQuota, BurstSize, Cap - Alive));
}

TArray<FWeaponSwarmRoomCount> SwarmBucketByRoom(const TConstArrayView<FSwarmBugView> Bugs, const TConstArrayView<FBox2D> Rooms)
{
	TArray<FWeaponSwarmRoomCount> Result;
	if (Rooms.IsEmpty())
	{
		return Result;
	}

	// (房间, 行名) -> 数量;之后统一排序,保证输出确定。
	TMap<TPair<int32, FName>, int32> Counts;
	for (const FSwarmBugView& Bug : Bugs)
	{
		const FVector2D Pos(Bug.Location.X, Bug.Location.Y);
		int32 Room = INDEX_NONE;
		for (int32 Index = 0; Index < Rooms.Num(); ++Index)
		{
			if (Rooms[Index].IsInside(Pos))
			{
				Room = Index;
				break;
			}
		}
		if (Room == INDEX_NONE)
		{
			double BestDistSq = TNumericLimits<double>::Max();
			for (int32 Index = 0; Index < Rooms.Num(); ++Index)
			{
				const double DistSq = FVector2D::DistSquared(Rooms[Index].GetCenter(), Pos);
				if (DistSq < BestDistSq)
				{
					BestDistSq = DistSq;
					Room = Index;
				}
			}
		}
		++Counts.FindOrAdd(TPair<int32, FName>(Room, Bug.ArchetypeRow));
	}

	for (const TPair<TPair<int32, FName>, int32>& Pair : Counts)
	{
		FWeaponSwarmRoomCount& Entry = Result.AddDefaulted_GetRef();
		Entry.RoomIndex = Pair.Key.Key;
		Entry.ArchetypeRow = Pair.Key.Value;
		Entry.Count = Pair.Value;
	}
	Result.Sort([](const FWeaponSwarmRoomCount& A, const FWeaponSwarmRoomCount& B)
	{
		return A.RoomIndex != B.RoomIndex ? A.RoomIndex < B.RoomIndex : A.ArchetypeRow.LexicalLess(B.ArchetypeRow);
	});
	return Result;
}
