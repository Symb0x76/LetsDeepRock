// Swarm 池化虫群：客户端"滑行"检测（纯逻辑，网络冒烟 harness 与单元测试共用）

#pragma once

#include "CoreMinimal.h"

/**
 * 单个虫的滑行跟踪：激活中的虫连续 FrameThreshold 帧、每帧 2D 位移 > FrameDistance 计一次滑行，
 * 同一段连续快速帧只计一次。刚激活的首帧不计位移（槽位回收时客户端吸附属预期行为），
 * 失活期间的位移一律忽略。
 */
struct FSwarmSlideTracker
{
	static constexpr float DefaultFrameDistance = 150.f;
	static constexpr int32 DefaultFrameThreshold = 3;

	float FrameDistance = DefaultFrameDistance;
	int32 FrameThreshold = DefaultFrameThreshold;

	/** 观测一帧；本帧新触发一次滑行时返回 true */
	bool Observe(const bool bActive, const FVector2D& Location)
	{
		bool bNewSlide = false;
		if (bActive && bWasActive)
		{
			if (FVector2D::Distance(Location, LastLocation) > FrameDistance)
			{
				bNewSlide = ++ConsecutiveFastFrames == FrameThreshold;
				SlideCount += bNewSlide ? 1 : 0;
			}
			else
			{
				ConsecutiveFastFrames = 0;
			}
		}
		else
		{
			ConsecutiveFastFrames = 0;
		}
		bWasActive = bActive;
		LastLocation = Location;
		return bNewSlide;
	}

	int32 GetSlideCount() const { return SlideCount; }

private:
	FVector2D LastLocation = FVector2D::ZeroVector;
	int32 ConsecutiveFastFrames = 0;
	int32 SlideCount = 0;
	bool bWasActive = false;
};
