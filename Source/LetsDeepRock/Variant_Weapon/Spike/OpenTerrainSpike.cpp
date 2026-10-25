// 一次性原型(Spike):验证开放丘陵地形的手感。不进入正式玩法,随时可删除。

#include "Spike/OpenTerrainSpike.h"

#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "ProceduralMeshComponent.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"
#include "WeaponCharacter.h"

AOpenTerrainSpike::AOpenTerrainSpike()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	bAlwaysRelevant = true;

	Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("TerrainMesh"));
	Mesh->bUseComplexAsSimpleCollision = true;
	Mesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	SetRootComponent(Mesh);

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> GridMaterial(TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
	if (GridMaterial.Succeeded())
	{
		Mesh->SetMaterial(0, GridMaterial.Object);
	}
}

void AOpenTerrainSpike::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AOpenTerrainSpike, Seed);
}

void AOpenTerrainSpike::BeginPlay()
{
	Super::BeginPlay();
	if (Seed != 0)
	{
		BuildTerrain();
	}
	// 角色构造时启用了平面约束(俯视角平地);Spike 期间每秒解除一次,覆盖重生的角色。
	GetWorldTimerManager().SetTimer(PlaneConstraintTimer, this, &AOpenTerrainSpike::ReleasePlaneConstraints, 1.0f, true, 0.0f);
}

void AOpenTerrainSpike::SetSeedAuthority(const int32 InSeed)
{
	Seed = InSeed == 0 ? 1 : InSeed;
	BuildTerrain();
	ForceNetUpdate();
}

void AOpenTerrainSpike::OnRep_Seed()
{
	BuildTerrain();
}

float AOpenTerrainSpike::SampleHeight(const int32 InSeed, const float LocalX, const float LocalY)
{
	// 种子决定噪声采样偏移;4 个八度的 Perlin 叠加,低频大丘 + 高频起伏。
	FRandomStream Stream(InSeed);
	const FVector2D Offset(Stream.FRandRange(-10000.f, 10000.f), Stream.FRandRange(-10000.f, 10000.f));

	float Noise = 0.f;
	float Amplitude = 1.f;
	float Frequency = 1.f / 6000.f;
	float AmplitudeSum = 0.f;
	for (int32 Octave = 0; Octave < 4; ++Octave)
	{
		Noise += Amplitude * FMath::PerlinNoise2D(FVector2D(LocalX * Frequency, LocalY * Frequency) + Offset);
		AmplitudeSum += Amplitude;
		Amplitude *= 0.45f;
		Frequency *= 2.f;
	}
	// [-1,1] → [0,1],再用平方压低谷底,让平地多、山丘少而明显。
	const float Normalized = FMath::Clamp(Noise / AmplitudeSum * 0.5f + 0.5f, 0.f, 1.f);
	const float Height = FMath::Square(Normalized) * MaxHeight;

	// 出生点平地:内圈为 0,外圈平滑过渡。
	const float Distance = FMath::Sqrt(LocalX * LocalX + LocalY * LocalY);
	const float Blend = FMath::SmoothStep(FlatInnerRadius, FlatOuterRadius, Distance);
	return Height * Blend;
}

void AOpenTerrainSpike::BuildTerrain()
{
	if (bBuilt || Seed == 0)
	{
		return;
	}
	bBuilt = true;

	const double StartSeconds = FPlatformTime::Seconds();
	const float HalfExtent = (VertsPerSide - 1) * VertexSpacing * 0.5f;

	TArray<FVector> Vertices;
	TArray<FVector> Normals;
	TArray<FVector2D> UV0;
	TArray<int32> Triangles;
	Vertices.Reserve(VertsPerSide * VertsPerSide);
	Normals.Reserve(VertsPerSide * VertsPerSide);
	UV0.Reserve(VertsPerSide * VertsPerSide);
	Triangles.Reserve((VertsPerSide - 1) * (VertsPerSide - 1) * 6);

	for (int32 Y = 0; Y < VertsPerSide; ++Y)
	{
		for (int32 X = 0; X < VertsPerSide; ++X)
		{
			const float LocalX = X * VertexSpacing - HalfExtent;
			const float LocalY = Y * VertexSpacing - HalfExtent;
			Vertices.Add(FVector(LocalX, LocalY, SampleHeight(Seed, LocalX, LocalY)));

			// 中心差分求法线
			const float HL = SampleHeight(Seed, LocalX - VertexSpacing, LocalY);
			const float HR = SampleHeight(Seed, LocalX + VertexSpacing, LocalY);
			const float HD = SampleHeight(Seed, LocalX, LocalY - VertexSpacing);
			const float HU = SampleHeight(Seed, LocalX, LocalY + VertexSpacing);
			Normals.Add(FVector(HL - HR, HD - HU, 2.f * VertexSpacing).GetSafeNormal());
			UV0.Add(FVector2D(LocalX, LocalY) / 400.f);
		}
	}

	for (int32 Y = 0; Y < VertsPerSide - 1; ++Y)
	{
		for (int32 X = 0; X < VertsPerSide - 1; ++X)
		{
			const int32 I0 = Y * VertsPerSide + X;
			const int32 I1 = I0 + 1;
			const int32 I2 = I0 + VertsPerSide;
			const int32 I3 = I2 + 1;
			// 逆时针朝上(UE 左手系下从 +Z 俯视)
			Triangles.Append({ I0, I2, I1, I1, I2, I3 });
		}
	}

	Mesh->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, UV0, TArray<FLinearColor>(),
		TArray<FProcMeshTangent>(), true);

	UE_LOG(LogTemp, Log, TEXT("[TerrainSpike] Built seed=%d verts=%d tris=%d extent=%.0fuu in %.1f ms (%s)"),
		Seed, Vertices.Num(), Triangles.Num() / 3, HalfExtent * 2.f, (FPlatformTime::Seconds() - StartSeconds) * 1000.0,
		HasAuthority() ? TEXT("server") : TEXT("client"));
}

void AOpenTerrainSpike::ReleasePlaneConstraints()
{
	for (TActorIterator<AWeaponCharacter> It(GetWorld()); It; ++It)
	{
		if (UCharacterMovementComponent* Movement = It->GetCharacterMovement(); Movement && Movement->bConstrainToPlane)
		{
			Movement->SetPlaneConstraintEnabled(false);
		}
	}
}

namespace
{
	FAutoConsoleCommandWithWorldAndArgs GSpikeTerrainCommand(
		TEXT("Spike.Terrain"),
		TEXT("[Spike] 在本地玩家脚下生成开放丘陵地形。用法:Spike.Terrain [Seed](仅服务器/单机)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (!World || World->GetNetMode() == NM_Client)
			{
				UE_LOG(LogTemp, Warning, TEXT("[TerrainSpike] server only"));
				return;
			}
			for (TActorIterator<AOpenTerrainSpike> It(World); It; ++It)
			{
				It->Destroy();
			}

			FVector Origin = FVector::ZeroVector;
			if (const APlayerController* PC = World->GetFirstPlayerController(); PC && PC->GetPawn())
			{
				const APawn* Pawn = PC->GetPawn();
				Origin = Pawn->GetActorLocation() - FVector(0.0, 0.0, Pawn->GetSimpleCollisionHalfHeight());
			}
			const int32 InSeed = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : FMath::RandRange(1, MAX_int32);

			AOpenTerrainSpike* Terrain = World->SpawnActor<AOpenTerrainSpike>(Origin, FRotator::ZeroRotator);
			if (Terrain)
			{
				Terrain->SetSeedAuthority(InSeed);
			}
		}));

	FAutoConsoleCommandWithWorld GSpikeTerrainClearCommand(
		TEXT("Spike.TerrainClear"),
		TEXT("[Spike] 移除开放丘陵地形"),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			if (!World || World->GetNetMode() == NM_Client)
			{
				return;
			}
			for (TActorIterator<AOpenTerrainSpike> It(World); It; ++It)
			{
				It->Destroy();
			}
		}));
}
