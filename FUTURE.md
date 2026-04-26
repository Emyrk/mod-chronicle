# mod-chronicle — Future Improvements

## Mainline Hooks Not Yet Used

These hooks exist in **current mainline AzerothCore** and require no core patches.

### Boss Encounter Events (high priority)

**`OnBeforeSetBossState(uint32 id, EncounterState newState, EncounterState oldState, Map* instance)`**

Fires whenever a boss encounter transitions state (NOT_STARTED → IN_PROGRESS → DONE/FAIL).
Would emit `ENCOUNTER_START` / `ENCOUNTER_END` events — more reliable than
Chronicle's current approach of matching mob GUIDs against a creature database.

**`OnAfterUpdateEncounterState(Map* map, EncounterCreditType type, uint32 creditEntry, ...)`**

Fires after encounter credit is processed. Provides `DungeonEncounterList` with
official encounter names from DBC. Would enable `ENCOUNTER_CREDIT` events and
dungeon completion detection.

**`OnInstanceIdRemoved(uint32 instanceId)`**

Safety net for log cleanup — fires when an instance save is removed from the database.

## Event Improvements

- **`DISPEL` events** — not yet tracked
- **Per-target hit/miss in `SPELL_CAST_SUCCESS`** — `Spell::m_UniqueTargetInfo` has
  per-target miss results, but `OnSpellSendSpellGo` only gives us the `Spell*`; need
  to extract and format the target list
- **`SpellPeriodicEnergize` power type** — `SpellPeriodicAuraLogInfo` doesn't carry
  the power type, so we emit `0`. Could be derived from the aura effect.

## Known Issues

- **Failed uploads are lost.** If the Chronicle server is unreachable or returns
  an error when a log is uploaded, the gzipped file is deleted anyway. There is
  no retry queue or dead-letter storage — the log is simply gone.
- **Idle instance logs grow forever.** Logs are written for the entire lifetime
  of an instance, including long stretches of zero combat (e.g. a player AFKing
  in a raid). This wastes disk space and produces bloated files full of
  no-op time gaps. Need to either stop writing when no combat is active, or
  close/rotate the log after a configurable idle timeout.

## Architectural Improvements

- **Owner chain resolution** — follow pet→owner chains via `getOwnerRecursively()`
  for more accurate source attribution
- **Scope filtering** — skip irrelevant units (e.g. non-instance, training dummies)
  to reduce log noise
- **SpawnId for creatures** — use `GetSpawnId()` for stable creature identity
  across respawns

## Upstream Contribution

The 15 custom ScriptMgr hooks should be submitted as an AzerothCore PR.
They are read-only observer hooks with zero gameplay impact, inserted at
the server's existing packet-send points. The reference implementation
(MellianStudios fork) proves the concept works.
