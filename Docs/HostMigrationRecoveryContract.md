# Host Migration Recovery Contract

Updated: 2026-07-31

Status: Accepted design contract; orchestration is not implemented yet.

## 1. Purpose

This document is the canonical contract for the first serverless host-migration implementation. It defines what must remain stable across recovery, which state may reset, how snapshots become committed, and how failures are classified.

The selected topology is fixed:

```text
Active Host -> Backup A -> Backup B
```

Only Backup A and Backup B receive full recovery payloads. Ordinary clients do not participate in host election and do not hold a complete recovery mirror.

## 2. Current serialization baseline

- `UWeaponMatchSaveGame::SaveFormatVersion` is `1`.
- `FWeaponSnapshot::SchemaVersion` is `6`.
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

## 4. Player identity and roster contract

Player identity uses the valid `FUniqueNetIdRepl` carried by the connection and `APlayerState`. `PlayerName` is presentation only and must never be used as a recovery key or fallback.

`FUniqueNetIdRepl` is considered stable across Recovery Sessions while the same client process and OnlineSubsystem identity remain alive. OnlineSubsystemNull does not guarantee the same ID after a client process restart, so client-process restart during migration is outside this MVP.

Each stable PlayerId owns one original Slot for the lifetime of a MatchId. Reconnect order never reallocates that Slot.

One roster entry has exactly one recovery disposition:

- `Pending`: still belongs to the match and reserves its original Slot, but is not restore-ready.
- `Ready`: the stable identity has reclaimed its Slot and all required connection/runtime dependencies are ready.
- `Departed`: explicitly left, exceeded the grace period, or was removed by policy; it does not block Restore.

Host/Backup/Candidate role and gameplay alive/dead state are orthogonal to this disposition.

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

## 10. Common committed revision

The Active Host tracks the latest identical SnapshotRevision/hash ACKed by both A and B. This is `CommonCommittedRevision` and is the only checkpoint allowed for crash migration.

- A revision ACKed only by A is not common committed.
- A revision ACKed only by B is not common committed.
- The Host local SaveGame may contain a newer revision, but it does not change the A/B crash baseline.
- Backup B's Committed bytes remain immutable while A attempts recovery.

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

If RosterRevision changes during Validate/Stage/Apply, the candidate must not publish `RecoveryReady`. It discards that Recovery World and starts a new attempt using the unchanged CommonCommittedRevision and updated roster.

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

Failures must be observable with MatchId, AuthorityEpoch, SnapshotRevision, RosterRevision, RecoveryAttemptId and CandidatePlayerId.

## 13. Host departure and local SaveGame

For planned departure, the Active Host freezes gameplay, captures a final Snapshot and waits for A/B commit ACKs up to the configured deadline before leaving.

The original Host writes validated payloads to a local SaveGame slot. A later local restart may continue only as:

- a join to the already migrated authority; or
- a new fork with a new MatchId.

It must never advertise the old MatchId as a competing authority when the migrated match may still exist.

## 14. Failover ladder

```text
Active Host fails
-> Backup A creates a gated Recovery Session
-> A restores CommonCommittedRevision
-> if A fails before RecoveryReady, discard A's Recovery World
-> Backup B restores the identical bytes in a clean World
-> if B also fails, survivors return to Lobby
```

There is no third arbitrary-client candidate in this MVP. Peer RTT does not participate in election.

## 15. Acceptance evidence for this contract

Implementation is not considered complete until packaged multi-process tests prove:

- A and B promote only complete, hash-valid payloads;
- only a revision ACKed by both becomes CommonCommitted;
- Host crash loses no more than the latest configured Snapshot interval under normal transfer conditions;
- reversed reconnect order reclaims original Slots by stable identity;
- a Pending player can time out while its prior projectiles retain valid damage attribution;
- RosterRevision changes prevent RecoveryReady publication;
- A failure before/during Restore leaves B's Committed bytes unchanged;
- B retries the identical revision/hash and B failure returns survivors to Lobby;
- stale Epoch/session/messages cannot become authoritative;
- GridMap, Enemy, Projectile and Grenade runtime results match the committed Snapshot;
- corrupt, incomplete, oversized and incompatible payloads fail closed.

Editor compilation proves only source compatibility. It does not prove host migration behavior.
