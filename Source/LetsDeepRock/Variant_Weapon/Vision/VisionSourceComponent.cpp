// 核心视野系统 - 视野来源组件实现

#include "VisionSourceComponent.h"
#include "GameFramework/Actor.h"

UVisionSourceComponent::UVisionSourceComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UVisionSourceComponent::ConfigureCone(float InRange, float InHalfAngle)
{
	Shape = EVisionSourceShape::Cone;
	Range = FMath::Max(0.0f, InRange);
	HalfAngle = FMath::Clamp(InHalfAngle, 0.0f, 180.0f);
}

void UVisionSourceComponent::ConfigureCircle(float InRange)
{
	Shape = EVisionSourceShape::Circle;
	Range = FMath::Max(0.0f, InRange);
}

FVector UVisionSourceComponent::GetVisionLocation() const
{
	return GetOwner() ? GetOwner()->GetActorLocation() : FVector::ZeroVector;
}

FVector UVisionSourceComponent::GetVisionDirection() const
{
	return GetOwner() ? GetOwner()->GetActorForwardVector() : FVector::ForwardVector;
}
