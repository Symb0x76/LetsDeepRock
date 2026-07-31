# Host Migration Recovery Contract

Updated: 2026-07-31

Status: Accepted design contract; payload transport, fixed-role/Epoch fencing, NetworkFailure-driven A/B recovery, planned handoff, fenced RecoveryReady acknowledgements, presentation state and split-brain-safe local SaveGame fork are implemented. Packaged runtime proof remains pending.

## 1. Purpose

This document is the canonical contract for the first serverless host-migration implementation. It defines what must remain stable across recovery, which state may reset, how snapshots become committed, and how failures are classified.

The selected topology is fixed:

```text
Active Host -> Backup A -> Backup B
```

Only Backup A and Backup B receive full recovery payloads. Ordinary clients do not participate in host election and do not hold a complete recovery mirror.

## 2. Current serialization baseline

- `UWeaponMatchSaveGame::SaveFormatVersion` is `1`.
- `FWeaponSnapshot::SchemaVersion` is `7`.
- The SaveGame header owns the target map package and session capacity.
- The nested Snapshot owns mutable authoritative match state.
- A format or schema mismatch is rejected; no compatibility migration is currently promised.
- Schema versions change only when serialized layout or field semantics change.

`USaveGame` remains a serialization envelope. It does not select a host, discover actors, create a recovery session, or apply gameplay state.

## 3. Recovery identifiers

Each identifier answers one question and must not be reused for another purpose.

| Identifier | Meaning | Monotonicity / lifetime |
|---|---|---|
| `MatchId` | Which logical match is being recovered? | Stable for one match and every successful migration of that match |
| `AuthorityEpoch` | Which host term is currently allowed to publish authority? | Increases whenever the candidate/authority term changes |
| `SnapshotRevision` | Which immutable checkpoint inside the match is this? | Increases for each captured Snapshot inside a MatchId |
| `RosterRevision` | Which participant-state set was used by this attempt? | Increases whenever a roster entry changes recovery state |
| `RecoveryAttemptId` | Which single recovery execution do events belong to? | Fresh GUID for every clean Recovery World attempt |
| `CandidatePlayerId` | Which stable player is attempting to become Host? | Fixed for one attempt; represented by `FUniqueNetIdRepl` |

Messages, ACKs, sessions and `RecoveryReady` events with a stale MatchId, AuthorityEpoch or RecoveryAttemptId must be ignored.

The initial Listen Host owns Epoch 1. Within one term, roster changes may republish A/B roles but do not change Epoch. If the Active Host fails, the assigned Backup A candidate uses `previous Epoch + 1`; Backup B uses `previous Epoch + 2` only after A fails. This prevents A and B from publishing conflicting candidates under the same Epoch. Existing clients accept a higher term only when its Active PlayerId and exact increment match that deterministic predecessor relation; they reject a different Active Host inside the same Epoch.

## 4. Player identity and roster contract

Player identity uses the valid `FUniqueNetIdRepl` carried by the connection and `APlayerState`. `PlayerName` is presentation only and must never be used as a recovery key or fallback.

`FUniqueNetIdRepl` is considered stable across Recovery Sessions while the same client process and OnlineSubsystem identity remain alive. OnlineSubsystemNull does not guarantee the same ID after a client process restart, so client-process restart during migration is outside this MVP.

Each stable PlayerId owns one original Slot for the lifetime of a MatchId. Reconnect order never reallocates that Slot.

One roster entry has exactly one recovery disposition:

- `Pending`: still belongs to the match and reserves its original Slot, but is not restore-ready.
- `Ready`: the stable identity has reclaimed its Slot and all required connection/runtime dependencies are ready.
- `Departed`: explicitly left, exceeded the grace period, or was removed by policy; it does not block Restore.

Host/Backup/Candidate role and gameplay alive/dead state are orthogonal to this disposition.

The current server derives roles only from connected `Ready` roster entries. Its one local Listen Host controller is `ActiveHost`; every other eligible player is ordered by stable `OriginalSlot`, with the lowest assigned to Backup A and the next to Backup B. Reconnect order, RTT and PlayerName never participate. The complete assignment carries MatchId, AuthorityEpoch and RosterRevision and is sent reliably to every connected client, whose `UWeaponHostMigrationSubsystem` retains it across Travel.

The default `Pending` grace period is 60 seconds and remains configurable.

The server GameMode is the first runtime owner of this state machine. It keeps a server-only stable PlayerId map and a monotonic `RosterRevision`:

- first admission creates a `Ready` entry and reserves its original Slot;
- Logout changes `Ready -> Pending` and starts the grace timer;
- the same identity reconnecting before expiry changes `Pending -> Ready` and cancels the timer;
- expiry, explicit leave or policy drop changes the entry to `Departed` and releases its Slot;
- importing a complete recovery reservation set rebuilds the roster and advances the revision once.

Every entry creation, Slot change or disposition change advances `RosterRevision`. Host role and gameplay alive/dead changes do not.

## 5. Pending Pawn and projectile attribution

When a connected player becomes Pending, its gameplay Pawn is converted into an inert attribution tombstone instead of being destroyed immediately:

- hide presentation;
- disable collision, movement, input, ability activation, vision and gameplay Tick;
- retain the associated PlayerState and ASC;
- remove the pair from connected-player and active-roster accounting;
- keep it only as the source of already active Projectile/Grenade effects.

Do not delete Mesh or Collision components. The Actor/component layout remains intact until normal destruction.

Roster expiry and attribution lifetime are separate. At 60 seconds the entry becomes Departed and loses Slot-reclaim rights, but its tombstone remains until the last associated Projectile/Grenade ends. Active-projectile ownership is tracked by stable PlayerId through spawn/EndPlay registration, not by a per-Tick World scan.

A reconnecting player may receive a new active Pawn/PlayerState while the tombstone remains source context for pre-disconnect projectiles. The tombstone is never counted as a second roster participant.

In a fresh Recovery World, tombstones for Pending/Departed projectile sources must exist before Projectile/Grenade staging begins.

## 6. Preserved state

The following domains are checkpoint-preserved because schema v7 has concrete Capture, Validate and Restore ownership:

- SaveGame header: map package and maximum players;
- Match: MatchId, SnapshotRevision, phase and relative remaining phase time;
- Recovery roster: RosterRevision, stable identity, original Slot, Pending/Ready/Departed disposition and relative remaining Pending grace;
- Player: stable identity, original Slot, ready/team state, Pawn presence, transform, horizontal rotation, velocity, character config, selected static weapon index and captured numeric GAS attributes;
- Enemy: class, transform, velocity, enemy config and captured numeric GAS attributes;
- GridMap: deterministic baseline, latest tile deltas and `NextTileRevision` continuity;
- normal Projectile: class/config identity, source player, transform, velocity, damage state, terrain penetration and remaining lifespan;
- Grenade: DataAsset identity, source player, transform, velocity and remaining fuse.

Recovery is checkpoint-consistent rather than frame-exact. With the default interval, crash recovery loses between zero and approximately five seconds of authoritative mutations.

## 7. Explicit reset and excluded state

The following runtime state resets during recovery unless a later owner-specific contract promotes it into the Snapshot:

- active GameplayAbility execution state;
- active GameplayEffects, cooldowns and transient loose tags not represented by numeric attributes;
- AI task, blackboard execution, current target, path-following and animation state;
- transient VFX, audio, camera and UI presentation;
- Drone state;
- Wave, Objective and Drop state while no concrete owner-specific Capture/Apply path exists.

An excluded domain cannot silently become required by gameplay. If migration correctness depends on it, migration remains incomplete until that owner exposes and tests a recovery contract.

## 8. Configurable provisional limits

These values are defaults, not frozen protocol constants:

| Setting | Initial value | Purpose |
|---|---:|---|
| `SnapshotIntervalSeconds` | 5 s | Maximum planned crash-recovery RPO |
| `PendingGracePeriodSeconds` | 60 s | Stable identity Slot-reclaim window |
| `CandidateRecoveryTimeoutSeconds` | 60 s | Overall deadline before A fails over to B, or B returns to Lobby |
| `TransferAckTimeoutSeconds` | 10 s | Deadline for one A/B payload acknowledgement |
| `ChunkAssemblyTimeoutSeconds` | 10 s | Deadline for one Pending payload to become complete |
| `MaxSnapshotBytes` | 4 MiB | Reject unexpectedly large serialized payloads |
| `ChunkPayloadBytes` | 32 KiB | Initial application-level chunk payload |

Byte limits and time limits are independent. A 60-second player grace period must not be reused as a payload size or per-transfer ACK timeout.

Only one revision per backup may be Pending. A newer revision replaces an incomplete older Pending revision; it never replaces Committed state until validation succeeds.

Packaged multi-process measurements must record payload distribution, capture/serialization time, chunk transfer time and ACK latency before these defaults are tuned or frozen.

## 9. Transfer envelope and integrity

Every transmitted payload carries at least:

- MatchId;
- AuthorityEpoch;
- SnapshotRevision;
- RosterRevision at capture time;
- total payload byte count;
- chunk index and total chunk count;
- full-payload cryptographic hash;
- SaveGame/Schema versions available after deserialization.

Chunks are accepted only for the current MatchId/Epoch and one internally consistent envelope. Duplicate chunks may be ignored. Conflicting duplicate chunks invalidate Pending state.

The full payload is promoted from Pending to Committed only after:

1. all chunks are present;
2. byte count and hash match;
3. SaveGame deserialization succeeds;
4. pure SaveGame/Snapshot validation succeeds;
5. the backup emits an ACK containing MatchId, Epoch, Revision and hash.

Invalid or incomplete Pending data is discarded without modifying the previous Committed payload.

The concrete cross-travel owner is `UWeaponHostMigrationSubsystem`, one `UGameInstanceSubsystem` per process. Its receive side has exactly two buffers: replaceable `Pending` chunk assembly and immutable `Committed` bytes. It validates envelope limits before allocation, accepts identical duplicate chunks, rejects or invalidates conflicting/stale chunks within the buffered authority term, verifies full-payload SHA-256, then deserializes and validates the SaveGame before promotion. SHA-256 uses UE's runtime `PlatformCrypto` facade rather than `FPlatformMisc::GetSHA256Signature`, whose generic UE 5.8 implementation asserts on platforms without an override. Before invoking `LoadGameFromMemory`, the coordinator also requires the modern `SAVG` file tag; legacy untagged data is not a migration payload. Transport and timeout code may discard Pending, but cannot mutate Committed.

The GameMode supplies the two deterministically selected remote `AWeaponPlayerController` instances and the current non-zero Epoch to the subsystem. The subsystem verifies their stable PlayerIds against the current A/B assignment, performs `CaptureSaveGame -> SerializeSaveGame -> envelope`, completes the local asynchronous SaveGame write, then sends one reliable application chunk per Tick through each owning controller. A receiver accepts chunks only when its own stable identity is the current A or B and MatchId/Epoch/RosterRevision exactly match the published assignment. A Backup promotes locally before its controller emits the Server ACK. Assembly timeout discards only Pending; ACK timeout discards only the Host's outgoing transfer record.

## 10. Common committed revision

The Active Host tracks the latest identical SnapshotRevision/hash ACKed by both A and B. This is `CommonCommittedRevision` and is the only checkpoint allowed for crash migration.

- A revision ACKed only by A is not common committed.
- A revision ACKed only by B is not common committed.
- The Host local SaveGame may contain a newer revision, but it does not change the A/B crash baseline.
- Backup B's Committed bytes remain immutable while A attempts recovery.

The concrete Host aggregator records ACKs only from its configured A/B controllers and only when MatchId, Epoch, SnapshotRevision, RosterRevision and hash identify the current outgoing payload. Duplicate ACKs are idempotent. It publishes `CommonCommittedRevision` only after both role-specific flags are true; an ACK timeout preserves the previous common value.

After both ACKs, the Host reliably announces the complete CommonCommitted ACK metadata to every connected client. An assigned Backup accepts that announcement only if its own immutable Committed bytes identify the same payload. Ordinary clients retain the metadata for later recovery-session routing but do not become candidates and do not receive full payload bytes.

Each Backup pins a separate `CommonCommittedPayload` byte buffer when it accepts that announcement, then confirms acceptance back to the Host. Its latest validated `Committed` buffer may continue advancing while a later revision waits for the other Backup, but it cannot overwrite the bytes selected for crash recovery. The role assignment that produced the common ACK is pinned with the bytes, so a later same-term roster/role change cannot silently change the candidate ladder for the older baseline. A planned departure requires both confirmations for its final revision; reliable announcement delivery alone is not treated as convergence proof.

After a successful migration, the new Active Host replenishes Backup A/B roles by stable original Slot where population permits. Reduced redundancy must be visible in logs/UI.

## 11. Restore publication contract

Every candidate restores in a fresh gated Recovery World/Session:

```text
ValidateRestore
-> Stage fallible replacement actors
-> Apply/Commit with no expected business failure
-> verify MatchId/Epoch/RosterRevision/AttemptId again
-> publish RecoveryReady
-> receive required client readiness acknowledgements
-> release gameplay barrier
```

Before `RecoveryReady`, player input, gameplay RPCs, AI, damage, match timers and new Snapshot capture remain blocked.

The concrete gate is the replicated `AWeaponGameState::bGameplayRecoveryBlocked` fact. Authority enters and releases it only through `AWeaponGameMode::EnterGameplayRecoveryBarrier` and `ReleaseGameplayRecoveryBarrier`; those edge transitions freeze or resume player/NPC movement, weapon-tagged GAS abilities, AI Behavior Trees, drones, projectiles, grenade fuses, respawn timers and the current match-phase deadline. Authoritative mutation owners also read the same gate at their RPC/damage/GridMap/capture boundaries. `RestoreSaveGame` requires the target World to be gated before Validate and leaves it gated after Apply; a later migration orchestrator alone may publish `RecoveryReady` and call Release.

Recovery listen travel carries `WeaponRecovery=1`; `AWeaponGameMode::InitGameState` closes the barrier before local PostLogin or gameplay BeginPlay can publish a normal role term. The candidate restores the pinned CommonCommitted bytes in that fresh World and creates a Ready-only LAN Recovery Session while the barrier remains closed. Other survivors search and join only after every advertised MatchId/Epoch/base Revision/RosterRevision/Candidate/attempt/hash/map/build field matches.

Successful Session creation no longer releases gameplay. The candidate records the post-Restore runtime `RosterRevision`, verifies that it did not change while RecoveryReady publication was in flight, then enters `WaitingForReadyAcks`. Every connected remote `Ready` roster member receives a reliable request fenced by MatchId, successor Epoch, current runtime RosterRevision and RecoveryAttemptId. Its owning `AWeaponPlayerController` accepts that request only for the Recovery Session it actually joined, lets the local presentation state advance, and emits the Server ACK on the next game-thread tick. A reconnect, disconnect or grace expiry changes RosterRevision, invalidates the complete ACK set and issues a new fence. The barrier opens only when no roster entry remains `Pending` and every connected `Ready` identity has ACKed the current fence. Pending grace timers continue while gameplay is blocked.

The protocol structs and orchestration enums remain internal C++ reflection types. Blueprint receives only the intentional presentation surface on `UWeaponHostMigrationSubsystem`: overlay visibility, message text, normalized progress and one change delegate. No MatchId, Epoch, Candidate, payload or ACK struct is exposed as a Blueprint data model.

If RosterRevision changes during Validate/Stage/Apply or before the Ready Session publication callback completes, the candidate must not begin readiness ACK collection. It destroys the stale Session/World and restarts the same candidate with a new RecoveryAttemptId and the unchanged CommonCommitted bytes. Roster changes after publication are normal reconnect/grace events and advance the ACK fence instead of mutating an older ACK set.

There is no generic in-place rollback. An unpublished Recovery World is disposable.

The local synchronous restorer implements this boundary with one private, call-scoped context. `ValidateRestore` fully resolves target Players, classes, assets, Equipment and ASC compatibility without mutation. `StageRestoreActors` creates replacement Characters, Enemies, Projectiles and Grenades with collision/tick disabled; any expected failure destroys only those staged Actors. GridMap applies only after its data, authority and generator have been validated, then `ApplySnapshot` performs the no-expected-failure owner swap and publishes capacity, Slot reservations, Match phase and Snapshot revision last. The context is not a reusable transaction or undo log.

## 12. Failure taxonomy and required action

| Failure | Required action |
|---|---|
| `StaleAuthority` | Ignore stale Epoch/Candidate/Attempt messages without changing buffers |
| `UnsupportedVersion` | Reject payload; retain previous Committed revision |
| `PayloadTooLarge` | Reject before allocation/assembly; retain previous Committed revision |
| `IncompletePayload` | Expire Pending state after assembly timeout |
| `IntegrityMismatch` | Discard Pending state and log Match/Epoch/Revision/hash metadata |
| `DeserializeFailed` | Discard Pending state; retain previous Committed revision |
| `SnapshotInvalid` | Discard Pending state; retain previous Committed revision |
| `TargetIncompatible` | Abort the candidate Recovery World; try the next candidate or Lobby |
| `RosterChanged` | Abort and restart a clean attempt with a new AttemptId/RosterRevision |
| `StageFailed` | Destroy staged actors and abort the Recovery World |
| `CandidateTimeout` | A fails over to B; B failure returns survivors to Lobby |
| `CandidateDisconnected` | The remaining backup retries the identical CommonCommitted bytes |

Failures must be observable with MatchId, AuthorityEpoch, SnapshotRevision, RosterRevision, RecoveryAttemptId and CandidatePlayerId. Every `UWeaponHostMigrationSubsystem` state transition emits those control facts plus its previous/new state and presentation message; payload, planned-handoff, readiness and local-fork boundaries add their operation-specific revision or player identity.

## 13. Host departure and local SaveGame

For planned departure, `RequestPlannedHostHandoff` requires the live fixed A/B transport and a current-role CommonCommitted baseline already confirmed by both backups. On the game thread it captures the final live state and immediately closes the gameplay barrier in the same frame, preserving projectile/fuse values without allowing another gameplay tick. The Host waits up to `TransferAckTimeoutSeconds` for both payload ACKs, broadcasts the final CommonCommitted announcement, and then waits for A and B to confirm that they accepted and pinned it. Only that converged final revision permits departure. A timeout, local SaveGame failure or incomplete announcement convergence cancels the handoff; gameplay and periodic transport resume instead of leaving different processes on different baselines. A successful Host leaves to a standalone lobby and participates as a normal survivor searching for the deterministic successor.

The original Host writes each validated payload asynchronously to the configured `WeaponHostMigrationLatest` local SaveGame slot before distributing that revision to A/B. The local file may therefore be newer than `CommonCommittedRevision`, but a failed local write prevents that revision from entering transport. A later local restart may continue only as:

- a join to the already migrated authority; or
- a new fork with a new MatchId.

It must never advertise the old MatchId as a competing authority when the migrated match may still exist. `RestartLatestLocalSaveAsNewMatch` therefore works only from a standalone Idle/Failed state, requires the current OSS identity to own a non-Departed saved Slot, assigns a fresh MatchId before Restore, restores in a clean gated listen World, advertises a normal LAN Session, and only then releases gameplay. There is deliberately no API that loads the local file and advertises its old MatchId. Joining the migrated authority uses the normal fenced recovery search path instead.

## 14. Failover ladder

```text
Active Host fails
-> Backup A begins Epoch N+1 and creates a gated Recovery Session
-> A restores CommonCommittedRevision
-> if A fails before RecoveryReady, discard A's Recovery World
-> Backup B begins Epoch N+2 and restores the identical bytes in a clean World
-> if B also fails, survivors return to Lobby
```

There is no third arbitrary-client candidate in this MVP. Peer RTT does not participate in election.

`UWeaponHostMigrationSubsystem` is the single state-machine writer. Its Engine NetworkFailure handler snapshots only the committed control facts and never calls OpenLevel; it waits for the Engine's default `?closed` travel, then starts or searches the appropriate candidate. A cross-World core ticker enforces the per-candidate deadline and retries exact LAN discovery. A failure/timeout returns the process to the same map as a standalone Waiting/Lobby World before advancing A to B; B failure leaves the process in Lobby. Session delegates report events but do not choose candidates or mutate Epochs.

## 15. Acceptance evidence for this contract

### 15.1 Deterministic protocol evidence

The deterministic Editor Automation layer currently proves, through the production subsystem/coordinator paths:

- complete out-of-order assembly, incomplete rejection, newer-Pending replacement, identical/conflicting duplicate behavior, corrupt hash rejection, byte/chunk limits, protocol-version rejection and preservation of the previous Committed payload;
- a standard SHA-256 known vector and rejection of bytes without a modern SaveGame header;
- SaveGame byte round-trip coverage for GridMap deltas, Enemy, Projectile and Grenade records;
- deterministic A=`old+1`, B=`old+2`, stale Epoch/RosterRevision rejection and same-revision role-conflict rejection;
- CommonCommitted acceptance only when the receiver already holds byte-identical local Committed data.

These tests run one Editor process with `NewMap` loaded. They prove local protocol invariants, not RPC delivery, disconnect Travel, runtime Actor parity or failover timing.

### 15.2 Packaged three-process failover evidence

`UWeaponHostMigrationHarnessSubsystem` is an opt-in, non-Shipping `UGameInstanceSubsystem`. It is inert unless `-WeaponMigrationHarnessRole=Host|Client1|Client2` is present. The driver uses the public LAN Session APIs and observes the production migration subsystem; it does not assign roles, inject payloads or mutate the recovery state machine directly. Machine-readable `WMH|...` log markers expose only observation points such as `BaselineReady`, `MigrationState` and `RecoveryLive`.

`Tools/RunHostMigrationHarness.ps1` starts three packaged processes, verifies that all three observe one Match/Epoch/Roster/Snapshot/hash baseline with unique OSS identities, kills the requested authority process, and compares the recovered authority against that pinned baseline. When passed the outer packaged bootstrap executable, the runner resolves and owns the inner `Binaries/Win64/LetsDeepRock.exe` process so that crash injection and final cleanup target the real game process.

With an existing Development package, run both scenarios from the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File Tools/RunHostMigrationHarness.ps1 `
  -Executable Saved/HostMigrationHarnessPackage/<build>/Windows/LetsDeepRock.exe `
  -Scenario All `
  -OutputRoot Saved/HostMigrationHarnessRuns/<run>
```

The scenarios can also be isolated with `-Scenario HostCrashToA` or `-Scenario HostThenBackupACrashToB`. The checked 2026-07-31 package was `Saved/HostMigrationHarnessPackage/20260731-160000/Windows/LetsDeepRock.exe`.

The following packaged runs passed on one Windows machine and exercised the real 60-second connection timeout:

- `HostCrashToA`, RunId `74ff6b188a7d464d91cbb8f1395155c9`: Epoch 1 / Snapshot 2 recovered on Backup A as an Epoch 2 Listen Server. MatchId, Snapshot revision and SHA-256 stayed identical, and the surviving client converged on the same recovery fence.
- `HostThenBackupACrashToB`, RunId `c734058d16f24d53852101d79e48804b`: after the original Host failed, A reached `WaitingForReadyAcks` and was killed; Backup B then handled a second `ConnectionTimeout`, created a clean Epoch 3 attempt and reached `RecoveryLive` as a Listen Server with the original MatchId, Snapshot revision and SHA-256.

An early revision-1 chunk can be rejected by a client before its Backup role assignment arrives. The passing baseline is the later revision 2 that all three processes report as CommonCommitted; the rejection is fail-closed and does not replace the previous committed bytes.

This packaged evidence proves the local-machine happy path for LAN discovery/join, paced Host-to-A/B transport, common-baseline convergence, Engine disconnect handling, gated Recovery Session travel, A takeover, A-crash-to-B retry, byte-identical baseline reuse, Epoch advancement, survivor reconnect and RecoveryReady release. It is stronger than Editor compilation and single-process Automation, but it is not real multi-machine or gameplay-fidelity evidence.

### 15.3 Remaining acceptance work

The following behavior still requires targeted packaged failure injection, gameplay assertions or real multi-machine tests:

- Host crash loses no more than the latest configured Snapshot interval under normal transfer conditions;
- reversed reconnect order reclaims original Slots by stable identity;
- a Pending player can time out while its prior projectiles retain valid damage attribution;
- RosterRevision changes prevent RecoveryReady publication;
- incomplete, corrupt, oversized and incompatible payloads fail closed over real RPC transport;
- one-sided chunk/ACK timeout and duplicate delivery preserve the previous CommonCommitted bytes;
- failures injected inside Restore abandon the candidate World without changing B's pinned bytes;
- B failure returns survivors to Lobby;
- stale Epoch/session/messages cannot become authoritative;
- GridMap, Enemy, Projectile and Grenade runtime results match the committed Snapshot;
- planned Host handoff and local SaveGame restart-as-new-Match follow their separate contracts;
- the same scenarios converge across distinct machines and realistic LAN loss/latency.

Editor compilation proves only source compatibility. It does not prove host migration behavior.
