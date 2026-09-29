#include "ClickNetSubsystem.h"
#include "ClickNetLocalBox3DWorld.h"
#include "box3d/box3d.h"
#include "box3d/collision.h"
#include "BaseBox3DComponent.h"
#include "ClickNetRemoteMovementReceiver.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "GameNetworkingSockets/steam/steamnetworkingsockets.h"
#include "GameNetworkingSockets/steam/isteamnetworkingutils.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#if WITH_EDITOR
#include "DrawDebugHelpers.h"
#endif

namespace
{
TMap<HSteamNetConnection, UClickNetSubsystem*> ConnectionOwners;
int32 NetworkingUsers = 0;
uint64 HashLevelFile(const FString& FilePath)
{
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *FilePath)) return 0;
    uint64 Hash = 14695981039346656037ull;
    for (uint8 Byte : Bytes)
    {
        Hash ^= Byte;
        Hash *= 1099511628211ull;
    }
    return Hash;
}

bool OnMoverPlaneFound(b3ShapeId ShapeId, const b3PlaneResult* Results, int Count, void* Context)
{
    // Dynamic bodies are pushed by the player's kinematic proxy in the world step.
    if (b3Body_GetType(b3Shape_GetBody(ShapeId)) == b3_dynamicBody)
    {
        return true;
    }
    TArray<b3CollisionPlane>& Planes = *static_cast<TArray<b3CollisionPlane>*>(Context);
    for (int Index = 0; Index < Count; ++Index)
    {
        b3CollisionPlane& Plane = Planes.AddDefaulted_GetRef();
        Plane.plane = Results[Index].plane;
        Plane.pushLimit = FLT_MAX;
        Plane.push = 0.0f;
        Plane.clipVelocity = true;
    }
    return true;
}

bool ShouldCastMoverHit(b3ShapeId ShapeId, void* Context)
{
    return b3Body_GetType(b3Shape_GetBody(ShapeId)) != b3_dynamicBody;
}

bool ShouldCastRemoteVisualHit(b3ShapeId ShapeId, void*)
{
    return b3Body_GetType(b3Shape_GetBody(ShapeId)) == b3_staticBody;
}

bool OnRemoteVisualPlaneFound(b3ShapeId ShapeId, const b3PlaneResult* Results, int Count, void* Context)
{
    if (!ShouldCastRemoteVisualHit(ShapeId, nullptr)) return true;
    return OnMoverPlaneFound(ShapeId, Results, Count, Context);
}
}

bool UClickNetSubsystem::SimulateLocalMover(b3Pos& Position, const b3Capsule& Capsule,
    b3Vec3& Velocity, float DeltaTime, bool& bGrounded, bool bJump, float JumpSpeed)
{
    if (!b3World_IsValid(m_localWorldId) || Capsule.radius <= 0.0f) return false;
    if (!m_localMover)
    {
        m_localMover = std::make_unique<clicknet::Mover>(m_localWorldId, Capsule, Position,
            static_cast<uint64_t>(EBox3DCollisionProfile::Pawn));
        if (!b3Body_IsValid(m_localMover->ProxyBodyId()))
        {
            m_localMover.reset();
            return false;
        }
        m_localPlayerBodyId = m_localMover->ProxyBodyId();
    }
    if (!m_localMover->Simulate(b3Vec3(Velocity.x, Velocity.y, 0.0f), bJump, DeltaTime, JumpSpeed))
    {
        return false;
    }
    const clicknet::MoverState& State = m_localMover->State();
    Position = State.position;
    Velocity = State.velocity;
    bGrounded = State.grounded;
    m_localPlayerTargetPosition = Position;
    return true;
}

void UClickNetSubsystem::ResetLocalMoverState(const clicknet::MoverState& State)
{
    if (m_localMover)
    {
        m_localMover->ResetState(State);
        m_localPlayerTargetPosition = State.position;
    }
}

void UClickNetSubsystem::PlaceLocalMoverProxyAtPredictedPosition()
{
    if (!b3Body_IsValid(m_localPlayerBodyId)) return;
    // Input replay updates the mover state without stepping the physics world.
    // Do not turn that replay distance into a one-tick kinematic impact.
    b3Body_SetTransform(m_localPlayerBodyId, m_localPlayerTargetPosition, b3Quat_identity);
    b3Body_SetLinearVelocity(m_localPlayerBodyId, b3Vec3(0.0f, 0.0f, 0.0f));
}

void UClickNetSubsystem::TrackLocalBodyContacts()
{
    if (!b3Body_IsValid(m_localPlayerBodyId)) return;
    const int32 Capacity = b3Body_GetContactCapacity(m_localPlayerBodyId);
    if (Capacity <= 0) return;
    TArray<b3ContactData, TInlineAllocator<16>> Contacts;
    Contacts.SetNumUninitialized(Capacity);
    const int32 Count = b3Body_GetContactData(m_localPlayerBodyId, Contacts.GetData(), Capacity);
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const b3BodyId BodyA = b3Shape_GetBody(Contacts[Index].shapeIdA);
        const b3BodyId BodyB = b3Shape_GetBody(Contacts[Index].shapeIdB);
        const b3BodyId OtherBody = B3_ID_EQUALS(BodyA, m_localPlayerBodyId) ? BodyB : BodyA;
        if (!b3Body_IsValid(OtherBody) || b3Body_GetType(OtherBody) != b3_dynamicBody) continue;
        const b3Vec3 Velocity = b3Body_GetLinearVelocity(OtherBody);
        if (Velocity.x * Velocity.x + Velocity.y * Velocity.y + Velocity.z * Velocity.z < 0.01f) continue;
        for (const TPair<FGuid, b3BodyId>& Entry : m_dynamicBodyById)
        {
            if (B3_ID_EQUALS(Entry.Value, OtherBody))
            {
                m_recentLocalContactUntilStep.Add(Entry.Key, m_localSimulationStep + 3);
                break;
            }
        }
    }
}


void UClickNetSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    if (!AppearanceCatalog && AppearanceCatalogPath.IsValid())
        AppearanceCatalog = Cast<UPlayerAppearanceCatalog>(AppearanceCatalogPath.TryLoad());
    if (!AppearanceCatalog) UE_LOG(LogTemp, Warning, TEXT("ClickNet appearance catalog is not assigned or could not load: %s"), *AppearanceCatalogPath.ToString());
    if (!m_timingTrace.Open(std::filesystem::path(*FPaths::ProjectSavedDir()) / "Timing", "client"))
        UE_LOG(LogTemp, Warning, TEXT("Could not open ClickNet timing trace"));

    m_localWorldId = FClientClickNetLocalBox3DWorld::CreateLocalWorld();
    if (!b3World_IsValid(m_localWorldId))
    {
        UE_LOG(LogTemp, Error, TEXT("Could not create the client Box3D world."));
    }

    if (NetworkingUsers == 0)
    {
        SteamDatagramErrMsg errMsg;
        if (!GameNetworkingSockets_Init(nullptr, errMsg))
        {
            UE_LOG(LogTemp, Error, TEXT("GameNetworkingSockets_Init failed: %s"), ANSI_TO_TCHAR(errMsg));
            return;
        }
    }
    ++NetworkingUsers;
    m_pInterface = SteamNetworkingSockets();
    m_bGNSInitialized = true;
}

void UClickNetSubsystem::Deinitialize()
{
    if (m_pInterface && m_hConnection != k_HSteamNetConnection_Invalid)
    {
        ConnectionOwners.Remove(m_hConnection);
        m_pInterface->CloseConnection(m_hConnection, 0, nullptr, false);
        m_hConnection = k_HSteamNetConnection_Invalid;
    }

    if (m_bGNSInitialized)
    {
        if (--NetworkingUsers == 0) GameNetworkingSockets_Kill();
        m_bGNSInitialized = false;
    }
    m_localMover.reset();
    if (b3World_IsValid(m_localWorldId))
    {
        FClientClickNetLocalBox3DWorld::DestroyLocalWorld(m_localWorldId);
        m_localWorldId = b3_nullWorldId;
    }
    m_localPlayerBodyId = b3_nullBodyId;
    m_importedBodyCount = 0;
    m_importedShapes.Reset();
    m_visualBindings.Reset();
    m_localTimeAccumulator = 0.0f;
    m_checkedMapName.Empty();
    m_bConnected = false;
    m_bAutoConnectAttempted = false;
    m_bPendingPlayerState = false;
    ClearRemotePawns();
    m_remotePlayers.Reset();
    m_remoteVisualCheckedAt.Reset();
    m_remoteClockTick = 0;
    m_remoteMaxPredictionError = 0.0f;
    m_remoteBlockedChecks = 0;
    m_remoteLargestCorrection.Reset();
    m_remoteClockAt = m_remoteStatsAt = 0.0;
    m_remoteSnapshotCount = m_remoteLargeCorrections = m_remoteBudgetHits = 0;
    m_remoteMaxSnapshotAge = m_remoteMaxCorrection = 0.0f;
    m_remoteMaxCheckGap = 0.0;
    m_remotePlayerVisualOffsets.Reset();
    m_remotePlayerVisiblePositions.Reset();
    m_remoteVisualCursor = 0;
    m_remotePlayerDespawnTick.Reset();
    m_estimatedServerTicksPerSecond = 60.0f;
    m_lastTickRateSampleAt = 0.0;
    m_lastTickRateSampleTick = 0;
    m_dynamicBodyById.Reset();
    m_bodyServerTicks.Reset();
    m_bodyVisualStates.Reset();
    m_recentLocalContactUntilStep.Reset();
    m_localSimulationStep = 0;
    m_oneWayDelaySeconds = 0.0f;
    m_hasSentInput = false;
    m_inputSender = clicknet::InputSender{};
    m_lastInputQueuedTick = m_lastInputSendTick = 0;
    m_localLevelHash = 0;

    Super::Deinitialize();
}

void UClickNetSubsystem::Tick(float DeltaTime)
{
    m_timingTrace.Flush();
    const float RemoteCorrectionRemaining = FMath::Exp(-FMath::Max(0.0f, DeltaTime) / 0.25f);
    for (TPair<uint32, FVector>& Entry : m_remotePlayerVisualOffsets)
        Entry.Value *= RemoteCorrectionRemaining;
    if (UWorld* World = GetWorld())
    {
        const FString MapName = UWorld::RemovePIEPrefix(World->GetMapName());
        if (!MapName.IsEmpty() && MapName != m_checkedMapName)
        {
            m_checkedMapName = MapName;
            const b3WorldId EmptyWorld = FClientClickNetLocalBox3DWorld::CreateLocalWorld();
            if (b3World_IsValid(EmptyWorld))
            {
                m_localMover.reset();
                if (b3World_IsValid(m_localWorldId))
                {
                    FClientClickNetLocalBox3DWorld::DestroyLocalWorld(m_localWorldId);
                }
                m_localWorldId = EmptyWorld;
                m_localPlayerBodyId = b3_nullBodyId;
                m_importedBodyCount = 0;
                m_importedShapes.Reset();
                m_visualBindings.Reset();
                ClearRemotePawns();
                m_localTimeAccumulator = 0.0f;
                m_dynamicBodyById.Reset();
                m_bodyServerTicks.Reset();
                m_bodyVisualStates.Reset();
                m_recentLocalContactUntilStep.Reset();
                m_localSimulationStep = 0;
                m_localLevelHash = 0;
            }
            const FString FilePath = FPaths::Combine(FPaths::ProjectSavedDir(), MapName + TEXT(".box3d"));
            if (IFileManager::Get().FileExists(*FilePath))
            {
                LoadLocalBox3DFile(FilePath);
            }
        }
        if (World->WorldType == EWorldType::PIE && bAutoConnectInPIE && !m_bAutoConnectAttempted)
        {
            m_bAutoConnectAttempted = true;
            FString ServerAddress(TEXT("127.0.0.1"));
            int32 ServerPort = 27015;
            FString Override;
            if (FParse::Value(FCommandLine::Get(), TEXT("ClickNetServer="), Override))
            {
                FString PortText;
                if (Override.Split(TEXT(":"), &ServerAddress, &PortText, ESearchCase::CaseSensitive,
                    ESearchDir::FromEnd))
                {
                    ServerPort = FCString::Atoi(*PortText);
                }
                else
                {
                    ServerAddress = Override;
                }
            }
            ConnectToServer(ServerAddress, ServerPort);
        }
    }
    if (b3World_IsValid(m_localWorldId))
    {
        // Limit catch-up work after a hitch while retaining a stable simulation step.
        m_localTimeAccumulator = FMath::Min(m_localTimeAccumulator + FMath::Clamp(DeltaTime, 0.0f, 0.25f), 0.25f);
        constexpr float Step = 1.0f / 60.0f;
        const int32 StepsToRun = FMath::Min(4, FMath::FloorToInt(m_localTimeAccumulator / Step));
        for (int32 Steps = 0; Steps < StepsToRun; ++Steps)
        {
            if (b3Body_IsValid(m_localPlayerBodyId))
            {
                const b3Pos Current = b3Body_GetPosition(m_localPlayerBodyId);
                const b3Vec3 ToTarget(
                    m_localPlayerTargetPosition.x - Current.x,
                    m_localPlayerTargetPosition.y - Current.y,
                    m_localPlayerTargetPosition.z - Current.z);
                b3Vec3 ProxyVelocity = ToTarget * (1.0f / (Step * (StepsToRun - Steps)));
                const float HorizontalSpeed = FMath::Sqrt(ProxyVelocity.x * ProxyVelocity.x
                    + ProxyVelocity.y * ProxyVelocity.y);
                if (HorizontalSpeed > 6.0f)
                {
                    const float Scale = 6.0f / HorizontalSpeed;
                    ProxyVelocity.x *= Scale;
                    ProxyVelocity.y *= Scale;
                }
                ProxyVelocity.z = FMath::Clamp(ProxyVelocity.z, -8.0f, 8.0f);
                b3Body_SetLinearVelocity(m_localPlayerBodyId, ProxyVelocity);
            }
            b3World_Step(m_localWorldId, Step, 4);
            ++m_localSimulationStep;
            TrackLocalBodyContacts();
            m_localTimeAccumulator -= Step;
        }
        UpdateVisualMeshes();
#if WITH_EDITOR
        // The Box3D world is separate from Unreal's scene. Show its current
        // shapes in PIE so movement is visible while building the client sim.
        if (UWorld* World = GetWorld(); World && World->WorldType == EWorldType::PIE)
        {
            for (const FClientBox3DImportedShape& Shape : m_importedShapes)
            {
                if (!b3Body_IsValid(Shape.BodyId)) continue;
                const FColor Color = Shape.bDynamic ? FColor::Orange : FColor::Cyan;
                if (Shape.Type == 0)
                {
                    const FTransform Visual = Shape.bDynamic
                        ? GetBodyVisualTransform(Shape.ExportId, Shape.BodyId)
                        : GetBodyVisualTransform(FGuid(), Shape.BodyId);
                    DrawDebugBox(World, Visual.GetLocation(),
                        FVector(Shape.BoxHalfExtents.x, Shape.BoxHalfExtents.y, Shape.BoxHalfExtents.z) * 100.0,
                        Visual.GetRotation(), Color, false, 0.0f, 0, 2.0f);
                }
                else
                {
                    const FTransform Visual = Shape.bDynamic
                        ? GetBodyVisualTransform(Shape.ExportId, Shape.BodyId)
                        : GetBodyVisualTransform(FGuid(), Shape.BodyId);
                    const FVector A = Visual.TransformPosition(FVector(Shape.CapsuleCenter1.x,
                        Shape.CapsuleCenter1.y, Shape.CapsuleCenter1.z) * 100.0);
                    const FVector B = Visual.TransformPosition(FVector(Shape.CapsuleCenter2.x,
                        Shape.CapsuleCenter2.y, Shape.CapsuleCenter2.z) * 100.0);
                    const FVector Axis = B - A;
                    const float Radius = Shape.CapsuleRadius * 100.0f;
                    const FQuat Rotation = Axis.IsNearlyZero()
                        ? FQuat::Identity : FQuat::FindBetweenNormals(FVector::UpVector, Axis.GetSafeNormal());
                    DrawDebugCapsule(World, (A + B) * 0.5, Axis.Size() * 0.5 + Radius,
                        Radius, Rotation, Color, false, 0.0f, 0, 2.0f);
                }
            }
            for (const TPair<uint32, clicknet::wire::PlayerState>& Entry : m_remotePlayers)
            {
                const clicknet::wire::PlayerState& State = Entry.Value;
                const FVector* Visible = m_remotePlayerVisiblePositions.Find(Entry.Key);
                DrawDebugCapsule(World, (Visible ? *Visible : FVector(State.px, State.py, State.pz)) * 100.0,
                    90.0f, 35.0f, FQuat::Identity, FColor::Green, false, 0.0f, 0, 2.0f);
            }
        }
#endif
        UpdateRemotePlayerVisuals();
        UpdateRemotePawns();
        AdvanceBodyVisuals(DeltaTime);
    }
    if (m_pInterface)
    {
        m_pInterface->RunCallbacks();
        if (m_bConnected && m_hConnection != k_HSteamNetConnection_Invalid)
        {
            SteamNetConnectionRealTimeStatus_t Status{};
            if (m_pInterface->GetConnectionRealTimeStatus(m_hConnection, &Status, 0, nullptr) == k_EResultOK
                && Status.m_nPing > 0)
            {
                m_oneWayDelaySeconds = FMath::Clamp(Status.m_nPing * 0.0005f, 0.0f, 0.1f);
                const double Now = FPlatformTime::Seconds();
                if (Now - m_lastTimingQueueAt >= 1.0)
                {
                    m_lastTimingQueueAt = Now;
                    m_timingTrace.Row("send_queue", m_playerId, 0, 0, 0, 0, 0, Status.m_usecQueueTime);
                    m_timingTrace.Row("pending_reliable", m_playerId, 0, 0, 0, 0, 0, Status.m_cbPendingReliable);
                }
            }
        }
        PollIncomingMessages();
    }
}

void UClickNetSubsystem::ConnectToServer(const FString& Address, int32 Port)
{
    if (!m_pInterface || Port <= 0 || Port > 65535)
    {
        UE_LOG(LogTemp, Warning, TEXT("ClickNet cannot connect: networking unavailable or invalid port %d"), Port);
        return;
    }
    SteamNetworkingIPAddr Remote;
    Remote.Clear();
    if (!Remote.ParseString(TCHAR_TO_UTF8(*Address)))
    {
        UE_LOG(LogTemp, Warning, TEXT("ClickNet expects a numeric IPv4/IPv6 address: %s"), *Address);
        return;
    }
    Remote.m_port = static_cast<uint16>(Port);
    if (m_hConnection != k_HSteamNetConnection_Invalid)
    {
        ConnectionOwners.Remove(m_hConnection);
        m_pInterface->CloseConnection(m_hConnection, 0, nullptr, false);
    }
    m_bConnected = false;
    m_playerId = 0;
    FString OverrideName;
    if (FParse::Value(FCommandLine::Get(), TEXT("ClickNetPlayerName="), OverrideName))
    {
        m_playerName = OverrideName;
    }
    m_lastServerTick = 0;
    m_bPendingPlayerState = false;
    ClearRemotePawns();
    m_remotePlayers.Reset();
    m_remoteVisualCheckedAt.Reset();
    m_remoteClockTick = 0;
    m_remoteMaxPredictionError = 0.0f;
    m_remoteBlockedChecks = 0;
    m_remoteLargestCorrection.Reset();
    m_remoteClockAt = m_remoteStatsAt = 0.0;
    m_remoteSnapshotCount = m_remoteLargeCorrections = m_remoteBudgetHits = 0;
    m_remoteMaxSnapshotAge = m_remoteMaxCorrection = 0.0f;
    m_remoteMaxCheckGap = 0.0;
    m_remotePlayerVisualOffsets.Reset();
    m_remotePlayerVisiblePositions.Reset();
    m_remoteVisualCursor = 0;
    m_remotePlayerDespawnTick.Reset();
    m_estimatedServerTicksPerSecond = 60.0f;
    m_lastTickRateSampleAt = 0.0;
    m_lastTickRateSampleTick = 0;
    m_bodyServerTicks.Reset();
    m_bodyVisualStates.Reset();
    m_recentLocalContactUntilStep.Reset();
    m_oneWayDelaySeconds = 0.0f;
    m_hasSentInput = false;
    m_inputSender = clicknet::InputSender{};
    m_lastInputQueuedTick = m_lastInputSendTick = 0;

    SteamNetworkingConfigValue_t Callback;
    Callback.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
        reinterpret_cast<void*>(SteamNetConnectionStatusChangedCallback));
    m_hConnection = m_pInterface->ConnectByIPAddress(Remote, 1, &Callback);
    if (m_hConnection == k_HSteamNetConnection_Invalid)
    {
        UE_LOG(LogTemp, Warning, TEXT("ClickNet connection could not start: %s:%d"), *Address, Port);
    }
    else
    {
        ConnectionOwners.Add(m_hConnection, this);
        UE_LOG(LogTemp, Display, TEXT("ClickNet connecting to %s:%d"), *Address, Port);
    }
}

void UClickNetSubsystem::SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* Info)
{
    if (UClickNetSubsystem** Owner = ConnectionOwners.Find(Info->m_hConn))
    {
        (*Owner)->OnConnectionStatusChanged(Info);
    }
}

void UClickNetSubsystem::OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* Info)
{
    if (!m_pInterface || Info->m_hConn != m_hConnection) return;
    switch (Info->m_info.m_eState)
    {
    case k_ESteamNetworkingConnectionState_Connected:
        m_bConnected = true;
        UE_LOG(LogTemp, Display, TEXT("ClickNet connected to server"));
        break;
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        UE_LOG(LogTemp, Warning, TEXT("ClickNet disconnected: %s"), ANSI_TO_TCHAR(Info->m_info.m_szEndDebug));
        ConnectionOwners.Remove(m_hConnection);
        m_pInterface->CloseConnection(m_hConnection, 0, nullptr, false);
        m_hConnection = k_HSteamNetConnection_Invalid;
        m_bConnected = false;
        m_playerId = 0;
        m_bPendingPlayerState = false;
        ClearRemotePawns();
    m_remotePlayers.Reset();
        m_remoteVisualCheckedAt.Reset();
        m_remoteClockTick = 0;
        m_remoteMaxPredictionError = 0.0f;
        m_remoteBlockedChecks = 0;
        m_remoteLargestCorrection.Reset();
        m_remoteClockAt = m_remoteStatsAt = 0.0;
        m_remoteSnapshotCount = m_remoteLargeCorrections = m_remoteBudgetHits = 0;
        m_remoteMaxSnapshotAge = m_remoteMaxCorrection = 0.0f;
        m_remoteMaxCheckGap = 0.0;
        m_remotePlayerVisualOffsets.Reset();
        m_remotePlayerVisiblePositions.Reset();
        m_remoteVisualCursor = 0;
        m_remotePlayerDespawnTick.Reset();
        m_estimatedServerTicksPerSecond = 60.0f;
        m_lastTickRateSampleAt = 0.0;
        m_lastTickRateSampleTick = 0;
        m_bodyServerTicks.Reset();
        m_bodyVisualStates.Reset();
        m_recentLocalContactUntilStep.Reset();
        m_oneWayDelaySeconds = 0.0f;
        m_hasSentInput = false;
        m_inputSender = clicknet::InputSender{};
        m_lastInputQueuedTick = m_lastInputSendTick = 0;
        break;
    default:
        break;
    }
}

void UClickNetSubsystem::SetPlayerName(const FString& PlayerName)
{
    m_playerName = PlayerName.TrimStartAndEnd();
    if (m_bConnected && m_playerId != 0) SendPlayerName();
}

void UClickNetSubsystem::SendPlayerName()
{
    if (!m_pInterface || m_hConnection == k_HSteamNetConnection_Invalid || m_playerName.IsEmpty()) return;
    FString Clean = m_playerName.Replace(TEXT("\r"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" "));
    Clean = Clean.Replace(TEXT("\t"), TEXT(" ")).TrimStartAndEnd();
    std::string Utf8 = TCHAR_TO_UTF8(*Clean);
    while (!Utf8.empty() && !clicknet::wire::ValidPlayerName(Utf8)) Utf8.pop_back();
    if (Utf8.empty()) return;
    const std::vector<uint8_t> Bytes = clicknet::wire::Encode(clicknet::wire::PlayerName{Utf8});
    if (!Bytes.empty())
    {
        m_pInterface->SendMessageToConnection(m_hConnection, Bytes.data(), static_cast<uint32>(Bytes.size()),
            k_nSteamNetworkingSend_ReliableNoNagle, nullptr);
    }
}

void UClickNetSubsystem::SetPlayerAppearance(uint8 AppearanceId)
{
    m_localAppearanceId = AppearanceId;
    if (!m_bConnected || !m_pInterface || m_playerId == 0) return;
    clicknet::wire::PlayerAppearance Appearance;
    Appearance.playerId = m_playerId;
    Appearance.value = AppearanceId;
    const auto Bytes = clicknet::wire::Encode(Appearance);
    m_pInterface->SendMessageToConnection(m_hConnection, Bytes.data(), static_cast<uint32>(Bytes.size()),
        k_nSteamNetworkingSend_Reliable, nullptr);
}

void UClickNetSubsystem::SendInput(const FClicknetPlayerInput& Input)
{
    if (!m_bConnected || !m_pInterface || m_hConnection == k_HSteamNetConnection_Invalid) return;
    clicknet::wire::Input Packet;
    Packet.tick = Input.SequenceNumber;
    Packet.moveX = static_cast<float>(Input.MoveAxis.X);
    Packet.moveY = static_cast<float>(Input.MoveAxis.Y);
    Packet.yaw = Input.YawRadians;
    Packet.jump = Input.bJumpPressed;
    // Send changes immediately and retry unacknowledged history without transport ordering.
    // A one-second refresh keeps the server timeout meaningful.
    const bool Changed = !m_hasSentInput
        || FMath::Abs(Packet.moveX - m_lastSentInput.moveX) > 0.005f
        || FMath::Abs(Packet.moveY - m_lastSentInput.moveY) > 0.005f
        || FMath::Abs(FMath::FindDeltaAngleRadians(Packet.yaw, m_lastSentInput.yaw)) > 0.02f
        || Packet.jump;
    const bool NewCommand = Changed || Packet.tick - m_lastInputQueuedTick >= 60;
    const auto NowUs = clicknet::TraceMonoUs();
    if (NewCommand)
    {
        Packet = m_inputSender.Queue(Packet);
        m_lastSentInput = Packet;
        m_lastInputQueuedTick = Packet.tick;
        m_hasSentInput = true;
    }
    if (!NewCommand && !m_inputSender.RetryDue(NowUs)) return;
    const auto History = m_inputSender.Packet();
    if (History.commands.empty()) return;
    const std::vector<uint8_t> Bytes = clicknet::wire::Encode(History);
    m_inputSender.Attempted(NowUs);
    const EResult Result = m_pInterface->SendMessageToConnection(m_hConnection,
        Bytes.data(), static_cast<uint32>(Bytes.size()), k_nSteamNetworkingSend_UnreliableNoNagle, nullptr);
    if (Result != k_EResultOK)
    {
        UE_LOG(LogTemp, Warning, TEXT("ClickNet input send failed: %d"), static_cast<int32>(Result));
    }
    else
    {
        if (NewCommand) m_timingTrace.Row("command_send", m_playerId, 0, Packet.tick, m_lastServerTick, 0, 0);
        m_lastInputSendTick = FMath::Max(m_lastInputSendTick, History.commands.back().tick);
    }
}

bool UClickNetSubsystem::ConsumePlayerState(clicknet::wire::PlayerState& OutState)
{
    if (!m_bPendingPlayerState) return false;
    OutState = m_pendingPlayerState;
    m_bPendingPlayerState = false;
    return true;
}



// A real function body:
void UClickNetSubsystem::PollIncomingMessages()
{
    if (!m_pInterface || m_hConnection == k_HSteamNetConnection_Invalid)
        return;

    constexpr int32 BatchSize = 64;
    constexpr int32 MaxBatchesPerFrame = 4;
    ISteamNetworkingMessage* incoming[BatchSize]{};
    for (int32 batch = 0; batch < MaxBatchesPerFrame; ++batch)
    {
        const int numMsgs = m_pInterface->ReceiveMessagesOnConnection(m_hConnection, incoming, BatchSize);
        if (numMsgs <= 0) break;
        for (int i = 0; i < numMsgs; ++i)
        {
        ISteamNetworkingMessage* pIncomingMsg = incoming[i];
        if (m_hConnection == k_HSteamNetConnection_Invalid)
        {
            pIncomingMsg->Release();
            continue;
        }
        switch (clicknet::wire::MessageType(pIncomingMsg->m_pData, pIncomingMsg->m_cbSize))
        {
        case clicknet::wire::Type::Welcome:
        {
            clicknet::wire::Welcome Welcome;
            if (clicknet::wire::Decode(pIncomingMsg->m_pData, pIncomingMsg->m_cbSize, Welcome))
            {
                if (Welcome.levelHash == 0 || Welcome.levelHash != m_localLevelHash)
                {
                    UE_LOG(LogTemp, Error, TEXT("ClickNet level mismatch: local %llu, server %llu. Re-export and load the same .box3d level on both processes."),
                        static_cast<unsigned long long>(m_localLevelHash),
                        static_cast<unsigned long long>(Welcome.levelHash));
                    ConnectionOwners.Remove(m_hConnection);
                    m_pInterface->CloseConnection(m_hConnection, 0, "Level mismatch", false);
                    m_hConnection = k_HSteamNetConnection_Invalid;
                    m_bConnected = false;
                    break;
                }
                m_playerId = Welcome.playerId;
                UE_LOG(LogTemp, Display, TEXT("ClickNet assigned player %u"), m_playerId);
                SendPlayerName();
                SetPlayerAppearance(m_localAppearanceId);
            }
            break;
        }
        case clicknet::wire::Type::PlayerState:
        case clicknet::wire::Type::PlayerStateBatch:
        {
            clicknet::wire::PlayerStateBatch States;
            if (clicknet::wire::MessageType(pIncomingMsg->m_pData, pIncomingMsg->m_cbSize)
                == clicknet::wire::Type::PlayerState)
            {
                clicknet::wire::PlayerState State;
                if (clicknet::wire::Decode(pIncomingMsg->m_pData, pIncomingMsg->m_cbSize, State))
                    States.states.push_back(State);
            }
            else clicknet::wire::Decode(pIncomingMsg->m_pData, pIncomingMsg->m_cbSize, States);
            for (const clicknet::wire::PlayerState& State : States.states)
            {
                if (State.playerId == m_playerId || clicknet::TraceSample(State.playerId))
                    m_timingTrace.Row("snapshot_receive", State.playerId, m_playerId, 0,
                        State.serverTick, State.acknowledgedInputTick, State.receivedInputTick,
                        SteamNetworkingUtils()->GetLocalTimestamp() - pIncomingMsg->m_usecTimeReceived);
                const double PacketNow = FPlatformTime::Seconds();
                if (m_remoteClockAt == 0.0 || State.serverTick > m_remoteClockTick)
                {
                    m_remoteClockTick = State.serverTick;
                    m_remoteClockAt = PacketNow;
                }
                if (State.playerId == m_playerId && State.serverTick > m_lastServerTick)
                {
                    m_inputSender.Acknowledge(State.inputAckSequence, State.inputAckBits);
                    const double Now = FPlatformTime::Seconds();
                    if (m_lastTickRateSampleAt > 0.0)
                    {
                        const double Elapsed = Now - m_lastTickRateSampleAt;
                        if (Elapsed >= 0.5 && State.serverTick > m_lastTickRateSampleTick)
                        {
                            const float ObservedRate = FMath::Clamp(
                                static_cast<float>((State.serverTick - m_lastTickRateSampleTick) / Elapsed),
                                1.0f, 60.0f);
                            m_estimatedServerTicksPerSecond = FMath::Lerp(
                                m_estimatedServerTicksPerSecond, ObservedRate, 0.25f);
                            m_lastTickRateSampleAt = Now;
                            m_lastTickRateSampleTick = State.serverTick;
                        }
                    }
                    else
                    {
                        m_lastTickRateSampleAt = Now;
                        m_lastTickRateSampleTick = State.serverTick;
                    }
                    m_lastServerTick = State.serverTick;
                    m_pendingPlayerState = State;
                    m_bPendingPlayerState = true;
                }
                else if (State.playerId != m_playerId)
                {
                    if (const uint32* RemovedTick = m_remotePlayerDespawnTick.Find(State.playerId);
                        RemovedTick && State.serverTick <= *RemovedTick) continue;
                    clicknet::wire::PlayerState* Previous = m_remotePlayers.Find(State.playerId);
                    if (!Previous || State.serverTick > Previous->serverTick)
                    {
                        const double Now = FPlatformTime::Seconds();
                        FVector Correction = FVector::ZeroVector;
                        const float PredictionSeconds = GetRemotePredictionSeconds(State.serverTick, Now);
                        ++m_remoteSnapshotCount;
                        m_remoteMaxSnapshotAge = FMath::Max(m_remoteMaxSnapshotAge, PredictionSeconds);
                        if (Previous)
                        {
                            const auto Before = clicknet::wire::PredictPlayerState(*Previous,
                                GetRemotePredictionSeconds(Previous->serverTick, Now));
                            const auto After = clicknet::wire::PredictPlayerState(State,
                                PredictionSeconds);
                            const FVector* CheckedPosition = m_remotePlayerVisiblePositions.Find(State.playerId);
                            const FVector VisibleBefore = CheckedPosition ? *CheckedPosition
                                : FVector(Before.px, Before.py, Before.pz)
                                + m_remotePlayerVisualOffsets.FindRef(State.playerId);
                            const FVector NewPredicted(After.px, After.py, After.pz);
                            Correction = VisibleBefore - NewPredicted;
                            const FVector PredictionError = FVector(Before.px, Before.py, Before.pz) - NewPredicted;
                            m_remoteMaxPredictionError = FMath::Max(m_remoteMaxPredictionError, float(PredictionError.Size()));
                            if (Correction.Size() > m_remoteMaxCorrection)
                            {
                                m_remoteLargestCorrection = FString::Printf(
                                    TEXT("id=%u tickGap=%u previousAge=%.3fs grounded=%d->%d rawError=(%.2f,%.2f,%.2f)m displayError=(%.2f,%.2f,%.2f)m previousVelocity=(%.2f,%.2f,%.2f) newVelocity=(%.2f,%.2f,%.2f)"),
                                    State.playerId, State.serverTick - Previous->serverTick,
                                    GetRemotePredictionSeconds(Previous->serverTick, Now), int(Previous->grounded), int(State.grounded),
                                    PredictionError.X, PredictionError.Y, PredictionError.Z,
                                    Correction.X, Correction.Y, Correction.Z,
                                    Previous->vx, Previous->vy, Previous->vz, State.vx, State.vy, State.vz);
                            }
                            m_remoteMaxCorrection = FMath::Max(m_remoteMaxCorrection, float(Correction.Size()));
                            if (Correction.SizeSquared() > FMath::Square(0.3f)) ++m_remoteLargeCorrections;
                            if (Correction.SizeSquared() > FMath::Square(4.0f))
                            {
                                Correction = FVector::ZeroVector;
                                m_remotePlayerVisiblePositions.Remove(State.playerId);
                            }
                        }
                        m_remotePlayers.Add(State.playerId, State);
                        m_remotePlayerVisualOffsets.Add(State.playerId, Correction);
                        m_remotePlayerDespawnTick.Remove(State.playerId);
                    }
                }
            }
            break;
        }
        case clicknet::wire::Type::DespawnPlayer:
        {
            clicknet::wire::DespawnPlayer Despawn;
            if (clicknet::wire::Decode(pIncomingMsg->m_pData, pIncomingMsg->m_cbSize, Despawn))
            {
                const clicknet::wire::PlayerState* Current = m_remotePlayers.Find(Despawn.playerId);
                if (!Current || Despawn.serverTick >= Current->serverTick)
                {
                    RemoveRemotePawn(Despawn.playerId);
                    m_remoteAppearances.Remove(Despawn.playerId);
                    m_remotePlayers.Remove(Despawn.playerId);
                    m_remoteVisualCheckedAt.Remove(Despawn.playerId);
                    m_remotePlayerVisualOffsets.Remove(Despawn.playerId);
                    m_remotePlayerVisiblePositions.Remove(Despawn.playerId);
                    m_remotePlayerDespawnTick.Add(Despawn.playerId, Despawn.serverTick);
                }
            }
            break;
        }
        case clicknet::wire::Type::BodyState:
        {
            clicknet::wire::BodyState State;
            if (clicknet::wire::Decode(pIncomingMsg->m_pData, pIncomingMsg->m_cbSize, State))
            {
                const FGuid Id(State.exportId[0], State.exportId[1], State.exportId[2], State.exportId[3]);
                if (b3BodyId* Body = m_dynamicBodyById.Find(Id))
                {
                    const uint32 PreviousTick = m_bodyServerTicks.FindRef(Id);
                    if (State.serverTick > PreviousTick && b3Body_IsValid(*Body))
                    {
                        // Project the server state to approximately when this packet arrived.
                        const float TickAge = m_lastServerTick > State.serverTick
                            ? static_cast<float>(m_lastServerTick - State.serverTick) / 60.0f : 0.0f;
                        const float Age = FMath::Clamp(m_oneWayDelaySeconds + TickAge, 0.0f, 0.1f);
                        const FVector PredictedPosition = FVector(State.px, State.py, State.pz)
                            + FVector(State.vx, State.vy, State.vz) * Age;
                        FQuat PredictedRotation(State.qx, State.qy, State.qz, State.qw);
                        PredictedRotation.Normalize();
                        const FVector AngularAxis(State.wx, State.wy, State.wz);
                        const float AngularSpeed = AngularAxis.Size();
                        if (AngularSpeed > KINDA_SMALL_NUMBER)
                        {
                            PredictedRotation = FQuat(AngularAxis / AngularSpeed, AngularSpeed * Age) * PredictedRotation;
                        }

                        const b3Pos OldP = b3Body_GetPosition(*Body);
                        const b3Quat OldQ = b3Body_GetRotation(*Body);
                        const FVector ErrorMeters = PredictedPosition - FVector(OldP.x, OldP.y, OldP.z);
                        const FQuat OldRotation(OldQ.v.x, OldQ.v.y, OldQ.v.z, OldQ.s);
                        const float AngleError = OldRotation.AngularDistance(PredictedRotation);
                        const uint64* ContactUntil = m_recentLocalContactUntilStep.Find(Id);
                        const bool bRecentLocalContact = ContactUntil && m_localSimulationStep <= *ContactUntil;
                        const bool bLargeError = ErrorMeters.SizeSquared() > FMath::Square(0.3f)
                            || AngleError > FMath::DegreesToRadians(20.0f);
                        const bool bNeedsCorrection = PreviousTick == 0 || bLargeError
                            || (!bRecentLocalContact &&
                                (ErrorMeters.SizeSquared() > FMath::Square(0.05f)
                                    || AngleError > FMath::DegreesToRadians(6.0f)));
                        if (bNeedsCorrection)
                        {
                            FClientBox3DBodyVisualState& Visual = m_bodyVisualStates.FindOrAdd(Id);
                            const FVector Correction = -ErrorMeters * 100.0f;
                            if (PreviousTick != 0 && Correction.SizeSquared() <= FMath::Square(200.0f))
                            {
                                Visual.PositionOffset = (Visual.PositionOffset + Correction).GetClampedToMaxSize(200.0f);
                                Visual.RotationOffset = (Visual.RotationOffset * OldRotation
                                    * PredictedRotation.Inverse()).GetNormalized();
                            }
                            else
                            {
                                Visual = FClientBox3DBodyVisualState();
                            }
                            b3Body_SetTransform(*Body,
                                b3Pos{ static_cast<float>(PredictedPosition.X), static_cast<float>(PredictedPosition.Y),
                                    static_cast<float>(PredictedPosition.Z) },
                                b3Quat{ b3Vec3(static_cast<float>(PredictedRotation.X),
                                    static_cast<float>(PredictedRotation.Y), static_cast<float>(PredictedRotation.Z)),
                                    static_cast<float>(PredictedRotation.W) });
                        }
                        if (!bRecentLocalContact || bLargeError || PreviousTick == 0)
                        {
                            b3Body_SetLinearVelocity(*Body, b3Vec3(State.vx, State.vy, State.vz));
                            b3Body_SetAngularVelocity(*Body, b3Vec3(State.wx, State.wy, State.wz));
                        }
                        m_bodyServerTicks.Add(Id, State.serverTick);
                    }
                }
            }
            break;
        }
        case clicknet::wire::Type::PlayerAppearance:
        {
            clicknet::wire::PlayerAppearance Appearance;
            if (clicknet::wire::Decode(pIncomingMsg->m_pData, pIncomingMsg->m_cbSize, Appearance))
            {
                if (Appearance.playerId == 0 || Appearance.playerId == m_playerId) break;
                if (const uint32* Removed = m_remotePlayerDespawnTick.Find(Appearance.playerId);
                    Removed && Appearance.serverTick <= *Removed) break;
                const auto* Previous = m_remoteAppearances.Find(Appearance.playerId);
                if (Previous && Appearance.serverTick < Previous->serverTick) break;
                m_remoteAppearances.Add(Appearance.playerId, Appearance);
            }
            break;
        }
        default:
            break;
        }
        pIncomingMsg->Release();
        }
        if (numMsgs < BatchSize || m_hConnection == k_HSteamNetConnection_Invalid) break;
    }
}

bool UClickNetSubsystem::LoadLocalBox3DFile(const FString& FilePath)
{
    b3WorldId ImportedWorld = b3_nullWorldId;
    int32 BodyCount = 0;
    FString Error;
    TArray<FClientBox3DImportedShape> ImportedShapes;
    if (!FClientClickNetLocalBox3DWorld::ImportFile(FilePath, ImportedWorld, BodyCount, Error, &ImportedShapes))
    {
        UE_LOG(LogTemp, Error, TEXT("Box3D import failed for '%s': %s"), *FilePath, *Error);
        return false;
    }
    if (b3World_IsValid(m_localWorldId))
    {
        m_localMover.reset();
        m_visualBindings.Reset();
        ClearRemotePawns();
        FClientClickNetLocalBox3DWorld::DestroyLocalWorld(m_localWorldId);
    }
    m_localWorldId = ImportedWorld;
    m_localPlayerBodyId = b3_nullBodyId;
    m_importedBodyCount = BodyCount;
    m_importedShapes = MoveTemp(ImportedShapes);
    m_dynamicBodyById.Reset();
    m_bodyServerTicks.Reset();
    m_bodyVisualStates.Reset();
    m_recentLocalContactUntilStep.Reset();
    m_localSimulationStep = 0;
    for (const FClientBox3DImportedShape& Shape : m_importedShapes)
    {
        if (Shape.bDynamic && Shape.ExportId.IsValid() && b3Body_IsValid(Shape.BodyId))
        {
            m_dynamicBodyById.Add(Shape.ExportId, Shape.BodyId);
        }
    }
    m_localLevelHash = HashLevelFile(FilePath);
    BindVisualMeshes();
    m_localTimeAccumulator = 0.0f;
    if (const UWorld* World = GetWorld())
    {
        m_checkedMapName = UWorld::RemovePIEPrefix(World->GetMapName());
    }
    UE_LOG(LogTemp, Display, TEXT("Imported %d Box3D bodies into the client simulation from %s"), BodyCount, *FilePath);
    return true;
}

void UClickNetSubsystem::BindVisualMeshes()
{
    m_visualBindings.Reset();
    UWorld* World = GetWorld();
    if (!World) return;

    TMap<FGuid, UBaseBox3DComponent*> Components;
    TSet<FGuid> DuplicateIds;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        TInlineComponentArray<UBaseBox3DComponent*> ActorComponents(*It);
        for (UBaseBox3DComponent* Component : ActorComponents)
        {
            if (!IsValid(Component) || !Component->ExportId.IsValid()) continue;
            if (Components.Contains(Component->ExportId))
            {
                DuplicateIds.Add(Component->ExportId);
                Components.Remove(Component->ExportId);
            }
            else if (!DuplicateIds.Contains(Component->ExportId))
            {
                Components.Add(Component->ExportId, Component);
            }
        }
    }

    TMap<AActor*, int32> ShapesPerActor;
    for (const FClientBox3DImportedShape& Shape : m_importedShapes)
    {
        if (UBaseBox3DComponent* const* Found = Components.Find(Shape.ExportId))
        {
            if (AActor* Owner = (*Found)->GetOwner())
            {
                ++ShapesPerActor.FindOrAdd(Owner);
            }
        }
    }

    TSet<UStaticMeshComponent*> UsedMeshes;
    int32 Unbound = 0;
    int32 LinkedMeshes = 0;
    int32 LinkedActors = 0;
    for (const FClientBox3DImportedShape& Shape : m_importedShapes)
    {
        if (!Shape.bDynamic) continue;
        UBaseBox3DComponent* const* Found = Components.Find(Shape.ExportId);
        UBaseBox3DComponent* ShapeComponent = Found ? *Found : nullptr;
        if (!IsValid(ShapeComponent) || !b3Body_IsValid(Shape.BodyId))
        {
            ++Unbound;
            continue;
        }

        const b3Pos P = b3Body_GetPosition(Shape.BodyId);
        const b3Quat Q = b3Body_GetRotation(Shape.BodyId);
        const FTransform BodyTransform(FQuat(Q.v.x, Q.v.y, Q.v.z, Q.s),
            FVector(P.x, P.y, P.z) * 100.0);
        ShapeComponent->SetMobility(EComponentMobility::Movable);
        FClientBox3DVisualBinding& Binding = m_visualBindings.AddDefaulted_GetRef();
        Binding.BodyId = Shape.BodyId;
        Binding.ExportId = Shape.ExportId;
        Binding.ShapeComponent = ShapeComponent;
        Binding.ShapeRelativeToBody = ShapeComponent->GetComponentTransform().GetRelativeTransform(BodyTransform);

        UStaticMeshComponent* Mesh = ShapeComponent->ResolveVisualMesh();
        if (IsValid(Mesh) && Mesh->GetWorld() == World && !UsedMeshes.Contains(Mesh))
        {
            Mesh->SetSimulatePhysics(false);
            Mesh->SetMobility(EComponentMobility::Movable);
            Binding.Mesh = Mesh;
            Binding.RelativeToBody = Mesh->GetComponentTransform().GetRelativeTransform(BodyTransform);
            UsedMeshes.Add(Mesh);
            ++LinkedMeshes;
        }
        else
        {
            ++Unbound;
        }
        AActor* Owner = ShapeComponent->GetOwner();
        if (Owner && Owner->GetRootComponent() && ShapesPerActor.FindRef(Owner) == 1)
        {
            Owner->GetRootComponent()->SetMobility(EComponentMobility::Movable);
            Binding.Actor = Owner;
            Binding.ActorRelativeToBody = Owner->GetActorTransform().GetRelativeTransform(BodyTransform);
            ++LinkedActors;
        }
    }
    UE_LOG(LogTemp, Display, TEXT("Box3D linked %d dynamic bodies to actors and %d to Static Mesh components (%d mesh links unbound)."),
        LinkedActors, LinkedMeshes, Unbound);
    if (LinkedActors < m_visualBindings.Num())
    {
        UE_LOG(LogTemp, Warning, TEXT("Box3D left %d actor pivots unchanged because those actors contain multiple exported shapes or have no root component."),
            m_visualBindings.Num() - LinkedActors);
    }
    if (Unbound > 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("Box3D visual binding needs a version 2 export and a Static Mesh on each dynamic component's actor; select Visual Mesh when an actor has multiple meshes."));
    }
}

float UClickNetSubsystem::GetRemotePredictionSeconds(uint32 ServerTick, double Now) const
{
    if (m_remoteClockAt == 0.0) return 0.0f;
    const double CurrentTick = double(m_remoteClockTick)
        + FMath::Max(0.0, Now - m_remoteClockAt) * m_estimatedServerTicksPerSecond;
    return FMath::Clamp(float((CurrentTick - double(ServerTick)) / 60.0
        + m_oneWayDelaySeconds * m_estimatedServerTicksPerSecond / 60.0), 0.0f, 6.0f);
}

void UClickNetSubsystem::UpdateRemotePlayerVisuals()
{
    if (!b3World_IsValid(m_localWorldId) || m_remotePlayers.IsEmpty()) return;
    TArray<uint32> Players;
    m_remotePlayers.GenerateKeyArray(Players);
    if (m_remoteVisualCursor >= Players.Num()) m_remoteVisualCursor = 0;
    const int32 StartCursor = m_remoteVisualCursor;
    const double StartedAt = FPlatformTime::Seconds();
    if (m_remoteStatsAt == 0.0) m_remoteStatsAt = StartedAt;
    if (StartedAt - m_remoteStatsAt >= 2.0)
    {
        UE_LOG(LogTemp, Display, TEXT("ClickNet remote: visible=%d snapshots=%u maxSnapshotAge=%.1fms maxCorrection=%.3fm correctionsOver30cm=%u maxCheckGap=%.1fms budgetHits=%u"),
            Players.Num(), m_remoteSnapshotCount, m_remoteMaxSnapshotAge * 1000.0f,
            m_remoteMaxCorrection, m_remoteLargeCorrections, m_remoteMaxCheckGap * 1000.0, m_remoteBudgetHits);
        UE_LOG(LogTemp, Display, TEXT("ClickNet remote detail: maxPredictionError=%.3fm blockedChecks=%u largest={%s}"),
            m_remoteMaxPredictionError, m_remoteBlockedChecks, *m_remoteLargestCorrection);
        m_remoteMaxPredictionError = 0.0f;
        m_remoteBlockedChecks = 0;
        m_remoteLargestCorrection.Reset();
        m_remoteStatsAt = StartedAt;
        m_remoteSnapshotCount = m_remoteLargeCorrections = m_remoteBudgetHits = 0;
        m_remoteMaxSnapshotAge = m_remoteMaxCorrection = 0.0f;
        m_remoteMaxCheckGap = 0.0;
    }
    constexpr double BudgetSeconds = 0.003;
    b3Capsule Capsule{};
    Capsule.center1 = b3Vec3(0.0f, 0.0f, -0.55f);
    Capsule.center2 = b3Vec3(0.0f, 0.0f, 0.55f);
    Capsule.radius = 0.35f;
    b3QueryFilter Filter = b3DefaultQueryFilter();
    Filter.categoryBits = static_cast<uint64_t>(EBox3DCollisionProfile::Pawn);
    Filter.maskBits &= ~Filter.categoryBits;
    TArray<b3CollisionPlane> Planes;
    Planes.Reserve(16);
    for (int32 Offset = 0; Offset < Players.Num(); ++Offset)
    {
        const int32 Cursor = (StartCursor + Offset) % Players.Num();
        if (Offset > 0 && FPlatformTime::Seconds() - StartedAt >= BudgetSeconds)
        {
            ++m_remoteBudgetHits;
            m_remoteVisualCursor = Cursor;
            return;
        }
        const uint32 Id = Players[Cursor];
        
        if (const double* CheckedAt = m_remoteVisualCheckedAt.Find(Id))
            m_remoteMaxCheckGap = FMath::Max(m_remoteMaxCheckGap, StartedAt - *CheckedAt);
        
        m_remoteVisualCheckedAt.Add(Id, StartedAt);

        const auto& State = m_remotePlayers.FindChecked(Id);
        const auto Predicted = clicknet::wire::PredictPlayerState(State,
            GetRemotePredictionSeconds(State.serverTick, StartedAt));

        const FVector Goal = FVector(Predicted.px, Predicted.py, Predicted.pz)
            + m_remotePlayerVisualOffsets.FindRef(Id);

        FVector* Previous = m_remotePlayerVisiblePositions.Find(Id);
        const FVector Start = Previous ? *Previous : FVector(State.px, State.py, State.pz);
        const FVector Delta = Goal - Start;

        if (Delta.SizeSquared() < FMath::Square(0.001f)) continue;

        b3Pos Position{float(Start.X), float(Start.Y), float(Start.Z)};
        const b3Vec3 Translation(float(Delta.X), float(Delta.Y), float(Delta.Z));
        const float Fraction = b3World_CastMover(m_localWorldId, Position, &Capsule,
            Translation, Filter, ShouldCastRemoteVisualHit, nullptr);

        Position = Position + Translation * Fraction;

        if (Fraction < 1.0f)
        {
            ++m_remoteBlockedChecks;
            // Slide along contacted walls/ground rather than letting the next
            // extrapolated goal pull the display through them.
            Planes.Reset();
            b3World_CollideMover(m_localWorldId, Position, &Capsule, Filter,
                OnRemoteVisualPlaneFound, &Planes);
            const b3Vec3 Remaining(float(Goal.X) - Position.x,
                float(Goal.Y) - Position.y, float(Goal.Z) - Position.z);
            const auto Slide = b3SolvePlanes(Remaining, Planes.GetData(), Planes.Num());
            const float SlideFraction = b3World_CastMover(m_localWorldId, Position, &Capsule,
                Slide.delta, Filter, ShouldCastRemoteVisualHit, nullptr);
            Position = Position + Slide.delta * SlideFraction;
            Planes.Reset();
            b3World_CollideMover(m_localWorldId, Position, &Capsule, Filter,
                OnRemoteVisualPlaneFound, &Planes);
            Position = Position + b3SolvePlanes(b3Vec3(0.0f, 0.0f, 0.0f),
                Planes.GetData(), Planes.Num()).delta;
        }
        m_remotePlayerVisiblePositions.Add(Id, FVector(Position.x, Position.y, Position.z));
    }
    m_remoteVisualCursor = (StartCursor + 1) % Players.Num();
}

void UClickNetSubsystem::RemoveRemotePawn(uint32 PlayerId)
{
    if (FClientPawnBinding* Binding = m_pawnBindings.Find(PlayerId))
        if (APawn* Pawn = Binding->Actor.Get()) Pawn->Destroy();
    m_pawnBindings.Remove(PlayerId);
}

void UClickNetSubsystem::ClearRemotePawns()
{
    for (auto& Entry : m_pawnBindings)
        if (APawn* Pawn = Entry.Value.Actor.Get()) Pawn->Destroy();
    m_pawnBindings.Reset();
    m_remoteAppearances.Reset();
}

void UClickNetSubsystem::UpdateRemotePawns()
{
    UWorld* World = GetWorld();
    if (!World || !AppearanceCatalog) return;
    const double Started = FPlatformTime::Seconds();
    int32 Spawned = 0;
    for (const auto& Entry : m_remoteAppearances)
    {
        const uint32 Id = Entry.Key;
        if (Id == m_playerId) continue;
        const auto* State = m_remotePlayers.Find(Id);
        if (!State) continue; // Appearance and movement packets may arrive in either order.
        const auto* Class = AppearanceCatalog->AppearanceClasses.Find(Entry.Value.value);
        if (!Class || !*Class) continue;
        FClientPawnBinding* Binding = m_pawnBindings.Find(Id);
        if (Binding && (!Binding->Actor.IsValid() || Binding->Actor->GetClass() != Class->Get()))
        {
            RemoveRemotePawn(Id);
            Binding = nullptr;
        }
        const FVector* CheckedPosition = m_remotePlayerVisiblePositions.Find(Id);
        const FVector Position = (CheckedPosition ? *CheckedPosition : FVector(State->px, State->py, State->pz)) * 100.0;
        const FRotator Rotation(0.0, FMath::RadiansToDegrees(State->yaw), 0.0);
        if (!Binding)
        {
            if (Spawned >= 2 || FPlatformTime::Seconds() - Started > 0.002) continue;
            ++Spawned;
            const FTransform Transform(Rotation, Position);
            APawn* Pawn = World->SpawnActorDeferred<APawn>(*Class, Transform, nullptr, nullptr,
                ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
            if (!Pawn) continue;
            Pawn->Tags.AddUnique(TEXT("ClickNetRemoteVisual"));
            Pawn->AutoPossessPlayer = EAutoReceiveInput::Disabled;
            Pawn->AutoPossessAI = EAutoPossessAI::Disabled;
            Pawn->SetActorEnableCollision(false);
            UGameplayStatics::FinishSpawningActor(Pawn, Transform);
            if (!IsValid(Pawn)) continue;
            Pawn->SetActorEnableCollision(false);
            TArray<UBaseBox3DComponent*> BoxComponents;
            Pawn->GetComponents(BoxComponents);
            for (UBaseBox3DComponent* Component : BoxComponents)
            {
                Component->bCollisionEnabled = false;
                Component->bEnablePhysics = false;
            }
            TArray<UPrimitiveComponent*> Primitives;
            Pawn->GetComponents(Primitives);
            for (UPrimitiveComponent* Primitive : Primitives)
            {
                Primitive->SetSimulatePhysics(false);
                Primitive->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            }
            FClientPawnBinding NewBinding;
            NewBinding.PlayerId = Id;
            NewBinding.AppearanceId = Entry.Value.value;
            NewBinding.Actor = Pawn;
            NewBinding.Mesh = Pawn->FindComponentByClass<USkeletalMeshComponent>();
            Binding = &m_pawnBindings.Add(Id, NewBinding);
        }
        if (APawn* Pawn = Binding->Actor.Get())
        {
            if (IClickNetRemoteMovementReceiver* Receiver = Cast<IClickNetRemoteMovementReceiver>(Pawn))
            {
                const auto Predicted = clicknet::wire::PredictPlayerState(*State,
                    GetRemotePredictionSeconds(State->serverTick, FPlatformTime::Seconds()));
                Receiver->ApplyRemoteMovementState(FVector(Predicted.vx, Predicted.vy, Predicted.vz) * 100.0,
                    State->grounded);
            }
            Pawn->SetActorLocationAndRotation(Position, Rotation, false, nullptr, ETeleportType::TeleportPhysics);
        }
    }
}

void UClickNetSubsystem::AdvanceBodyVisuals(float DeltaTime)
{
    for (TPair<FGuid, FClientBox3DBodyVisualState>& Entry : m_bodyVisualStates)
    {
        FClientBox3DBodyVisualState& Visual = Entry.Value;
        const float ErrorScale = FMath::Clamp((Visual.PositionOffset.Size() - 5.0f) / 95.0f, 0.0f, 1.0f);
        const float SettleSeconds = FMath::Lerp(0.12f, 0.04f, ErrorScale);
        const float Remaining = FMath::Exp(-FMath::Max(DeltaTime, 0.0f) / SettleSeconds);
        Visual.PositionOffset *= Remaining;
        Visual.RotationOffset = FQuat::Slerp(FQuat::Identity, Visual.RotationOffset, Remaining).GetNormalized();
    }
}

void UClickNetSubsystem::BindPlayerVisuals()
{


}

FTransform UClickNetSubsystem::GetBodyVisualTransform(const FGuid& ExportId, b3BodyId BodyId) const
{
    const b3Pos P = b3Body_GetPosition(BodyId);
    const b3Quat Q = b3Body_GetRotation(BodyId);
    FVector Position(P.x, P.y, P.z);
    FQuat Rotation(Q.v.x, Q.v.y, Q.v.z, Q.s);
    if (ExportId.IsValid())
    {
        // Show locally simulated pushes immediately, including the partial fixed step.
        const float Ahead = FMath::Clamp(m_localTimeAccumulator, 0.0f, 1.0f / 60.0f);
        const b3Vec3 LinearVelocity = b3Body_GetLinearVelocity(BodyId);
        Position += FVector(LinearVelocity.x, LinearVelocity.y, LinearVelocity.z) * Ahead;
        const b3Vec3 AngularVelocity = b3Body_GetAngularVelocity(BodyId);
        const FVector AngularAxis(AngularVelocity.x, AngularVelocity.y, AngularVelocity.z);
        const float AngularSpeed = AngularAxis.Size();
        if (AngularSpeed > KINDA_SMALL_NUMBER)
        {
            Rotation = FQuat(AngularAxis / AngularSpeed, AngularSpeed * Ahead) * Rotation;
        }
        if (const FClientBox3DBodyVisualState* Visual = m_bodyVisualStates.Find(ExportId))
        {
            Position += Visual->PositionOffset / 100.0f;
            Rotation = Visual->RotationOffset * Rotation;
        }
    }
    return FTransform(Rotation, Position * 100.0f);
}

void UClickNetSubsystem::UpdateVisualMeshes()
{
    for (const FClientBox3DVisualBinding& Binding : m_visualBindings)
    {
        UBaseBox3DComponent* ShapeComponent = Binding.ShapeComponent.Get();
        UStaticMeshComponent* Mesh = Binding.Mesh.Get();
        if (!IsValid(ShapeComponent) || !b3Body_IsValid(Binding.BodyId)) continue;
        const FTransform BodyTransform = GetBodyVisualTransform(Binding.ExportId, Binding.BodyId);
        if (AActor* Actor = Binding.Actor.Get(); IsValid(Actor))
        {
            Actor->SetActorTransform(Binding.ActorRelativeToBody * BodyTransform,
                false, nullptr, ETeleportType::TeleportPhysics);
        }
        const FTransform ShapeWorld = Binding.ShapeRelativeToBody * BodyTransform;
        if (!IsValid(Mesh))
        {
            ShapeComponent->SetWorldTransform(ShapeWorld, false, nullptr, ETeleportType::TeleportPhysics);
            continue;
        }

        auto IsDescendantOf = [](const USceneComponent* Child, const USceneComponent* Parent)
        {
            for (const USceneComponent* Current = Child->GetAttachParent(); Current; Current = Current->GetAttachParent())
            {
                if (Current == Parent) return true;
            }
            return false;
        };
        const FTransform MeshWorld = Binding.RelativeToBody * BodyTransform;
        if (IsDescendantOf(ShapeComponent, Mesh))
        {
            // Move the parent first, then place its child exactly at the body transform.
            Mesh->SetWorldTransform(MeshWorld, false, nullptr, ETeleportType::TeleportPhysics);
            ShapeComponent->SetWorldTransform(ShapeWorld, false, nullptr, ETeleportType::TeleportPhysics);
        }
        else
        {
            ShapeComponent->SetWorldTransform(ShapeWorld, false, nullptr, ETeleportType::TeleportPhysics);
            Mesh->SetWorldTransform(MeshWorld, false, nullptr, ETeleportType::TeleportPhysics);
        }
    }
}
