#pragma once

#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "GameNetworkingSockets/steam/isteamnetworkingsockets.h"
#include "Box3D/Box3D.h"
#include "CoreMinimal.h"
#include "ClickNetLocalBox3DWorld.h"
#include "ClickNetTypes.h"
#include "ClickNetWire.h"
#include "ClickNetShared.h"
#include "ClickNetTrace.h"
#include "ClickNetInputChannel.h"
#include "PlayerAppearanceCatalog.h"
#include <memory>
#include "ClickNetSubsystem.generated.h"


class UStaticMeshComponent;
class USkeletalMeshComponent;
class UBaseBox3DComponent;
class AActor;
class UPlayerAppearanceCatalog;

struct FClientBox3DVisualBinding
{
    b3BodyId BodyId = b3_nullBodyId;
    FGuid ExportId;
    TWeakObjectPtr<AActor> Actor;
    TWeakObjectPtr<UBaseBox3DComponent> ShapeComponent;
    TWeakObjectPtr<UStaticMeshComponent> Mesh;
    FTransform ActorRelativeToBody;
    FTransform ShapeRelativeToBody;
    FTransform RelativeToBody;
};

struct FClientPawnBinding
{
    uint32 PlayerId = 0;                        // matches clicknet::wire::PlayerState.playerId
    TWeakObjectPtr<APawn> Actor;
    uint8 AppearanceId = 0;
    TWeakObjectPtr<USkeletalMeshComponent> Mesh;
    FTransform MeshRelativeToBody;              // feet offset + yaw fix (e.g. -90)
    // optional, if you smooth/interpolate remote players:
    /*FVector SmoothedVelocity = FVector::ZeroVector;
    float SmoothedYaw = 0.f;*/
};

struct FClientBox3DBodyVisualState
{
    FVector PositionOffset = FVector::ZeroVector;
    FQuat RotationOffset = FQuat::Identity;
};

UCLASS(Config=Game)
class CLICKNETSUBSYSTEMPLUGIN_API UClickNetSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    // FTickableGameObject
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override { return TStatId(); }
    virtual bool IsTickable() const override { return !IsTemplate() && (m_bConnected || b3World_IsValid(m_localWorldId)); }

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance")
    TObjectPtr<UPlayerAppearanceCatalog> AppearanceCatalog;
    UPROPERTY(Config, EditAnywhere, Category = "Appearance")
    FSoftObjectPath AppearanceCatalogPath;

    UFUNCTION(BlueprintCallable, Category = "ClickNet|Networking")
    void ConnectToServer(const FString& Address, int32 Port);
    UFUNCTION(BlueprintCallable, Category = "ClickNet|Networking")
    void SetPlayerName(const FString& PlayerName);
    UFUNCTION(BlueprintCallable, Category = "ClickNet|Appearance")
    void SetPlayerAppearance(uint8 AppearanceId);
    uint8 m_localAppearanceId = 0;
    void SendInput(const FClicknetPlayerInput& Input);
    uint32 GetLastSentInputTick() const { return m_lastInputSendTick; }
    bool HasPendingInputThrough(uint32 Tick) const { return m_inputSender.PendingThrough(Tick); }
    bool IsConnectedToServer() const { return m_bConnected; }
    bool ConsumePlayerState(clicknet::wire::PlayerState& OutState);
    const TMap<uint32, clicknet::wire::PlayerState>& GetRemotePlayers() const { return m_remotePlayers; }

    UPROPERTY(EditAnywhere, Category = "ClickNet|Networking")
    bool bAutoConnectInPIE = true;

    // Replaces the local simulation world only when the entire file imports successfully.
    UFUNCTION(BlueprintCallable, Category = "ClickNet|Box3D")
    bool LoadLocalBox3DFile(const FString& FilePath);

    b3WorldId GetLocalBox3DWorldId() const { return m_localWorldId; }
    int32 GetImportedBodyCount() const { return m_importedBodyCount; }

    // Keep mover queries beside the world: Box3D is statically linked into this module.
    bool SimulateLocalMover(b3Pos& Position, const b3Capsule& Capsule,
        b3Vec3& Velocity, float DeltaTime, bool& bGrounded, bool bJump, float JumpSpeed);
    void ResetLocalMoverState(const clicknet::MoverState& State);
    void PlaceLocalMoverProxyAtPredictedPosition();

private:
    void BindVisualMeshes();
    void UpdateVisualMeshes();
    void AdvanceBodyVisuals(float DeltaTime);
    void BindPlayerVisuals();
    void UpdateRemotePlayerVisuals();
    void UpdateRemotePawns();
    void RemoveRemotePawn(uint32 PlayerId);
    void ClearRemotePawns();
    TMap<uint32, clicknet::wire::PlayerAppearance> m_remoteAppearances;
    float GetRemotePredictionSeconds(uint32 ServerTick, double Now) const;
    void TrackLocalBodyContacts();
    FTransform GetBodyVisualTransform(const FGuid& ExportId, b3BodyId BodyId) const;
    void PollIncomingMessages();
    void SendPlayerName();
    void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* Info);
    static void SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* pInfo);

    ISteamNetworkingSockets* m_pInterface = nullptr;
    clicknet::TimingTrace m_timingTrace;
    double m_lastTimingQueueAt = 0.0;
    HSteamNetConnection      m_hConnection = k_HSteamNetConnection_Invalid;
    bool                     m_bConnected = false;
    bool m_bAutoConnectAttempted = false;
    uint32 m_playerId = 0;
    FString m_playerName;
    uint32 m_lastServerTick = 0;
    bool m_bPendingPlayerState = false;
    clicknet::wire::PlayerState m_pendingPlayerState;
    TMap<uint32, clicknet::wire::PlayerState> m_remotePlayers;
    uint32 m_remoteClockTick = 0;
    double m_remoteClockAt = 0.0;
    TMap<uint32, double> m_remoteVisualCheckedAt;
    double m_remoteStatsAt = 0.0;
    uint32 m_remoteSnapshotCount = 0;
    uint32 m_remoteLargeCorrections = 0;
    uint32 m_remoteBudgetHits = 0;
    float m_remoteMaxSnapshotAge = 0.0f;
    float m_remoteMaxCorrection = 0.0f;
    float m_remoteMaxPredictionError = 0.0f;
    uint32 m_remoteBlockedChecks = 0;
    FString m_remoteLargestCorrection;
    double m_remoteMaxCheckGap = 0.0;
    TMap<uint32, FVector> m_remotePlayerVisualOffsets;
    TMap<uint32, FVector> m_remotePlayerVisiblePositions;
    int32 m_remoteVisualCursor = 0;
    TMap<uint32, uint32> m_remotePlayerDespawnTick;
    float m_estimatedServerTicksPerSecond = 60.0f;
    double m_lastTickRateSampleAt = 0.0;
    uint32 m_lastTickRateSampleTick = 0;
    TMap<FGuid, b3BodyId> m_dynamicBodyById;
    TMap<FGuid, uint32> m_bodyServerTicks;
    TMap<FGuid, FClientBox3DBodyVisualState> m_bodyVisualStates;
    TMap<FGuid, uint64> m_recentLocalContactUntilStep;
    uint64 m_localSimulationStep = 0;
    float m_oneWayDelaySeconds = 0.0f;
    bool m_hasSentInput = false;
    clicknet::wire::Input m_lastSentInput{};
    uint32 m_lastInputSendTick = 0;
    uint32 m_lastInputQueuedTick = 0;
    clicknet::InputSender m_inputSender;
    uint64 m_localLevelHash = 0;

    TMap<uint32, FClientPawnBinding> m_pawnBindings;


    // Local prediction state
    b3WorldId m_localWorldId = b3_nullWorldId;
    b3BodyId  m_localPlayerBodyId = b3_nullBodyId;
    std::unique_ptr<clicknet::Mover> m_localMover;
    b3Pos m_localPlayerTargetPosition = b3Pos{0.0f, 0.0f, 0.0f};
    float m_localTimeAccumulator = 0.0f;
    int32 m_importedBodyCount = 0;
    TArray<FClientBox3DImportedShape> m_importedShapes;
    TArray<FClientBox3DVisualBinding> m_visualBindings;
    FString m_checkedMapName;
    bool m_bGNSInitialized = false;

};
