# mod-chronicle — Future Improvements

## Mainline Hooks Not Yet Used

These hooks exist in **current mainline AzerothCore** and require no core patches.

**`OnInstanceIdRemoved(uint32 instanceId)`**

Safety net for log cleanup — fires when an instance save is removed from the database.

## Event Improvements

- **`SPELL_DISPEL` / `SPELL_STOLEN` events** — not yet tracked. Core has
  `Spell::EffectDispel` and `Spell::EffectStealBeneficialBuff` which know the
  removed aura (spell ID, charges, etc.) and the dispeller. Need a new hook
  (or use `OnSpellSendSpellGo` + effect type filtering) to emit
  `SPELL_DISPEL` (hostile dispel), `SPELL_DISPEL_FAILED` (resist), and
  `SPELL_STOLEN` (Spellsteal). Data: caster, target, dispel spell, removed
  aura spell ID, removed aura school.
- **Silence-based interrupts** (e.g. Silencing Shot) — these don't go through
  `EffectInterruptCast`, they use `SPELL_AURA_MOD_SILENCE`. Not captured by
  `OnSpellInterrupt`. Would need a separate hook.
- **`LOOT` events** — not yet tracked. Core has `Player::SendLoot` and
  `LootItem` / `Loot` structures with item ID, count, and recipient.
  Would enable tracking boss loot drops per kill for historical loot tables
  and GP/DKP integration.
- **Per-target hit/miss in `SPELL_CAST_SUCCESS`** — `Spell::m_UniqueTargetInfo` has
  per-target miss results, but `OnSpellSendSpellGo` only gives us the `Spell*`; need
  to extract and format the target list
- **`SPELL_AURA_PERIODIC_LEECH`** (Drain Life, Siphon Life) — unreachable via
  `OnSendPeriodicAuraLog`; the core routes leech ticks through
  `SendSpellNonMeleeDamageLog` + `HealBySpell` instead. Verify these are
  captured correctly by existing hooks (`OnSendSpellNonMeleeDamageLog`,
  `OnSendHealSpellLog`).
- **`SPELL_AURA_PERIODIC_MANA_LEECH`** (Drain Mana, Viper Sting) — reaches
  `SendPeriodicAuraLog` but not yet handled. Needs a `SPELL_PERIODIC_DRAIN`
  formatter. Data: `pInfo->damage` = amount drained, `pInfo->multiplier` =
  gain ratio, `auraEff->GetMiscValue()` = power type.

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

## Account Linking

- **Realm character → Chronicle account linking** — allow players to claim
  their in-game characters on Chronicle. The module could emit a
  `CHRONICLE_ACCOUNT_INFO` event containing the player's game account ID
  (or a hash). Chronicle would then let users prove ownership (e.g. via an
  in-game chat command that generates a one-time code) and link the realm
  character to their Chronicle account. This enables per-player dashboards,
  personal log history, and privacy controls.

## Upstream Contribution

The 16 custom ScriptMgr hooks should be submitted as an AzerothCore PR.
They are read-only observer hooks with zero gameplay impact, inserted at
the server's existing packet-send points. The reference implementation
(MellianStudios fork) proves the concept works.
