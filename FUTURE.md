# mod-chronicle — Future Improvements & Hook Analysis

## Available Hooks We Don't Use Yet

These hooks exist in **current mainline AzerothCore** and require no forking.

### `OnBeforeSetBossState(uint32 id, EncounterState newState, EncounterState oldState, Map* instance)`

**What it gives us:** Fires whenever a boss encounter transitions state (e.g. NOT_STARTED → IN_PROGRESS → DONE / FAIL).

**What we can do:**
- Emit `ENCOUNTER_START` when `newState == IN_PROGRESS`
- Emit `ENCOUNTER_END` when `newState == DONE` or `newState == FAIL`
- The `id` maps to a boss index in the instance's `EncounterState` array — we can cross-reference it with DBC data or hardcode boss names per dungeon/raid
- This is the **most impactful missing feature** — Chronicle currently detects encounters by matching mob GUIDs against a creature database, but explicit encounter boundaries are far more reliable

**Suggested V2 event format:**
```
ts|ENCOUNTER_START|mapId|instanceId|bossIndex|encounterName|difficultyId
ts|ENCOUNTER_END|mapId|instanceId|bossIndex|encounterName|difficultyId|success
```

### `OnAfterUpdateEncounterState(Map* map, EncounterCreditType type, uint32 creditEntry, Unit* source, Difficulty difficulty_fixed, DungeonEncounterList const* encounters, uint32 dungeonCompleted, bool updated)`

**What it gives us:** Fires after an encounter credit is processed (kill credit or cast credit). Provides the `DungeonEncounterList` which contains the DBC encounter entries with names and IDs.

**What we can do:**
- Emit a `ENCOUNTER_CREDIT` event with the official encounter name from DBC
- Detect full dungeon completion (`dungeonCompleted` flag)
- Cross-reference with `OnBeforeSetBossState` for richer encounter tracking
- The `encounters` list gives us access to `DungeonEncounterEntry` which has the display name

### `OnInstanceIdRemoved(uint32 instanceId)`

**What it gives us:** Fires when an instance save is cleaned up from the database.

**What we can do:**
- Final cleanup signal — if we missed `OnDestroyInstance` for any reason, this is a safety net
- Could trigger log upload for any orphaned instance logs
- Less useful than the other two, but good for robustness

---

## Hooks That Do NOT Exist (Require Custom Fork)

The reference module (`mod-encounter-logs` by MellianStudios) used a **custom AzerothCore fork** with these hooks patched into the core. They are **not available** in mainline AzerothCore.

### Combat Detail Hooks (would fix data quality)

| Hook | What it would give us | Impact |
|------|----------------------|--------|
| `OnDealMeleeDamage(CalcDamageInfo*, DamageInfo*, uint32)` | Full melee damage breakdown: absorb, block, resist, hit outcome | Would fix SWING always showing 0 for absorb/block/resist |
| `OnSendSpellNonMeleeDamageLog(SpellNonMeleeDamage*)` | Full spell damage breakdown with all mitigation values | Would fix SPELL_DMG always showing 0 for absorb/block/resist |
| `OnSendAttackStateUpdate(CalcDamageInfo*, int32)` | Hit result enum (MISS, DODGE, PARRY, BLOCK, CRIT, CRUSHING, GLANCING) | Would enable MISS events and crit detection |
| `OnSendHealSpellLog(HealInfo const&, bool critical)` | Heal amount with crit flag | Would fix HEAL always showing isCrit=0 |
| `OnSendSpellMiss(Unit*, Unit*, uint32, SpellMissInfo)` | Spell miss with reason (resist, immune, reflect, etc.) | Would enable spell MISS events |
| `OnSendSpellDamageImmune(Unit*, Unit*, uint32)` | Spell immunity events | Would enable IMMUNE events |
| `OnSendSpellDamageResist(Unit*, Unit*, uint32)` | Full spell resists | Would improve resist tracking |
| `OnSendEnergizeSpellLog(Unit*, Unit*, uint32, uint32, Powers)` | Mana/rage/energy gains | Would enable ENERGIZE events |
| `OnSendPeriodicAuraLog(Unit*, SpellPeriodicAuraLogInfo*)` | Periodic tick data (DoT/HoT) with crit/amount | Would enable isPeriodic flag |
| `OnSendSpellNonMeleeReflectLog(SpellNonMeleeDamage*, Unit*)` | Spell reflects (e.g. Spell Reflect warrior ability) | Would enable REFLECT events |

### Data Tracking Hooks (would enable new features)

| Hook | What it would give us | Impact |
|------|----------------------|--------|
| `OnAuraApplicationClientUpdate(Unit*, Aura*, bool)` | Real-time aura state changes as sent to client | More accurate buff/debuff tracking |
| `OnChangeUpdateData(Object*, uint16, uint64)` | Raw field updates (health, power, max health) | Would enable continuous HP/mana tracking |
| `OnSpellSendSpellGo(Spell*)` | Spell go packet with full target list | More accurate SPELL_GO with hit/miss per target |

### Options for Getting These Hooks

1. **Contribute upstream to AzerothCore** — propose the hooks as a PR. These are read-only observer hooks with no gameplay impact, so they have a reasonable chance of acceptance. The reference fork proves the concept works.

2. **Maintain a lightweight fork** — cherry-pick just the ScriptMgr hook additions (no custom member variables). Smaller maintenance surface than the full MellianStudios fork.

3. **Patch at the module level** — some information can be approximated:
   - Crit detection: check `SPELL_ATTR0_CU_DIRECT_DAMAGE` + compare damage to expected values
   - Miss detection: `OnBeforeRollMeleeOutcomeAgainst` gives us the chance values, but not the actual roll result
   - Health tracking: snapshot on combat events (approximate, not continuous)

4. **Accept the limitations** — the current hooks produce usable logs. Chronicle's parser already handles missing fields gracefully. Focus on the events we *can* emit accurately.

---

## Immediate Next Steps (No Fork Required)

1. **Add boss encounter events** using `OnBeforeSetBossState` — high value, zero risk
2. **Add `OnAfterUpdateEncounterState`** for encounter credit/completion detection
3. **Use `OnInstanceIdRemoved`** as a cleanup safety net
4. **Fix duplicate SWING+SPELL_DMG** using a thread-local suppression flag (pure logic fix, no new hooks needed)
5. **Add ENV_DMG** — `OnEnvironmentalDamage` exists in `PlayerScript` (need to verify availability)

## What the Reference Module Architecture Teaches Us (Even Without Its Hooks)

- **Unit type classification** — distinguish Player/Creature/Pet/Totem/Summon/Vehicle/Object (we can do this with existing APIs)
- **Owner chain resolution** — `getOwnerRecursively()` follows pet→owner chains (we can implement this)
- **SpawnId for creatures** — use `GetSpawnId()` instead of object GUID for stable creature identity across respawns
- **Scope filtering** — `shouldNotBeTracked()` to skip irrelevant units (we should add this)
- **Combat-start stat snapshot** — `OnPlayerEnterCombat` captures gear/talents/stats (we have this hook available via `PlayerScript`)
