# Current State

Updated: 2026-07-15

## Multiplayer

- `feature/online` contains the four-player GameState/PlayerState baseline from `9b85d15`.
- `WeaponEquipmentComponent` now keeps inventory and current selection in a server-authoritative replicated state, with client-to-server switching through `ServerSwitchWeapon`.
- `WeaponCharacter::OnRep_PlayerState` no longer creates client-local inventory.
- Match flow now cycles through `WaitingForPlayers -> Starting -> InGame -> Ending -> WaitingForPlayers` under server-only GameMode control.
- Player ready intent uses an owning PlayerState Server RPC; match phase and phase-end server time replicate through GameState.
- Starting is cancelled if readiness becomes invalid, while InGame mid-join does not rewind the match.
- UE 5.8 Editor build passed after the equipment and match-state changes.

## Remaining validation

- Run a two-client PIE check for remote switching, replicated inventory, ability selection, and `OnRep_EquipmentState` delivery.
- Run a two-client PIE state-machine check for ready RPC, countdown cancellation, start, InGame mid-join, ending, and ready reset.
- Add phase consumers: input/AI/task gating plus round-world reset or travel after Ending.
- Add LAN session discovery and lobby flow only after multiplayer combat behaviour is validated.
- Remote weapon visuals remain a separate presentation issue because `AWeaponBase` is not replicated.
