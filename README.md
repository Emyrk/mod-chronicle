# mod-chronicle

A server-side [AzerothCore](https://github.com/azerothcore/azerothcore-wotlk)
module that generates combat log files for
[Chronicle](https://github.com/Emyrk/chronicle).

Every dungeon/raid instance gets its own log file. Events are written in
real-time as combat happens, producing files that can be uploaded directly to
Chronicle for analysis.

> **Requires custom ScriptMgr hooks.** This module depends on 14 hooks added to
> the AzerothCore core that are not in mainline. See
> [Custom Hooks](#custom-scriptmgr-hooks) below.

## Events Captured

### WotLK Combat Log Events

These follow the standard WotLK combat log format: `<unix_millis>  EVENT_TYPE,params...`

| Event | Hook | Description |
|-------|------|-------------|
| `SWING_DAMAGE` | `OnSendAttackStateUpdate` | Melee hit with full breakdown (damage, overkill, school, resist, block, absorb, crit/glancing/crushing) |
| `SWING_MISSED` | `OnSendAttackStateUpdate` | Melee miss with type (MISS, DODGE, PARRY, BLOCK, etc.) |
| `SPELL_DAMAGE` | `OnSendSpellNonMeleeDamageLog` | Spell direct damage with full breakdown |
| `SPELL_MISSED` | `OnSendSpellMiss` | Spell miss with type (RESIST, IMMUNE, REFLECT, etc.) |
| `SPELL_HEAL` | `OnSendHealSpellLog` | Heal with amount, overheal, absorb, crit flag |
| `SPELL_ENERGIZE` | `OnSendEnergizeSpellLog` | Mana/rage/energy gains |
| `SPELL_PERIODIC_DAMAGE` | `OnSendPeriodicAuraLog` | DoT tick damage |
| `SPELL_PERIODIC_HEAL` | `OnSendPeriodicAuraLog` | HoT tick healing |
| `SPELL_PERIODIC_ENERGIZE` | `OnSendPeriodicAuraLog` | Periodic resource gain |
| `DAMAGE_SHIELD` | `OnDealMeleeDamage` | Thorns/retribution aura damage |
| `SPELL_AURA_APPLIED` | `OnAuraApplicationClientUpdate` | Buff/debuff applied |
| `SPELL_AURA_REMOVED` | `OnAuraApplicationClientUpdate` | Buff/debuff removed |
| `SPELL_CAST_SUCCESS` | `OnSpellSendSpellGo` | Spell cast completed (fires at `SPELL_GO` packet) |
| `UNIT_DIED` | `OnUnitDeath` | Unit death |
| `ENVIRONMENTAL_DAMAGE` | `OnEnvironmentalDamage` | Lava, drowning, falling, fatigue damage |

### Chronicle Extension Events

Custom events prefixed with `CHRONICLE_` that provide metadata not present in
the standard combat log.

| Event | Description |
|-------|-------------|
| `CHRONICLE_HEADER` | Written once when the log file is created (realm name, build info) |
| `CHRONICLE_ZONE_INFO` | Instance zone name, map ID, instance ID |
| `CHRONICLE_COMBATANT_INFO` | Player gear, guild, class, race (emitted when a player enters) |
| `CHRONICLE_UNIT_INFO` | Unit metadata, emitted first time a GUID appears in combat |
| `CHRONICLE_UNIT_EVADE` | Creature entered evade mode |
| `CHRONICLE_UNIT_COMBAT` | Unit entered combat with a target |

### Data Fidelity

The hooks tap into the server's `Send*Log` functions — the exact point where
the server builds network packets for the client. This means we capture the
same data the client combat log would show, with full mitigation breakdowns:

**Damage suffix:** `amount,overkill,school,resisted,blocked,absorbed,critical,glancing,crushing`

**Spell prefix:** `spellId,"spellName",0xSchoolMask`

**Heal suffix:** `amount,overheal,absorbed,critical`

## Setup

### 1. Place the Module

The module must live inside the `modules/` directory of your AzerothCore source
tree. AC's CMake auto-discovers any subdirectory containing `.cpp` files.

### 2. Apply Custom Hooks

This module requires 14 custom ScriptMgr hooks patched into the AzerothCore
core. See [Custom ScriptMgr Hooks](#custom-scriptmgr-hooks) for details.

### 3. Build

From the `research/azerothcore/` directory:

```bash
# Full rebuild (core + module) — ~3-4 min with ccache
make rebuild

# Fast incremental rebuild (module changes only) — ~5-15s
make rebuild-module
```

See [Development Workflow](#development-workflow) for details on `rebuild-module`.

### 4. Configuration

Configure via environment variables or `mod_chronicle.conf`:

| Setting | Env Variable | Default | Description |
|---------|-------------|---------|-------------|
| `Chronicle.Enable` | `AC_CHRONICLE_ENABLE` | `1` | Enable/disable the module |
| `Chronicle.LogDir` | `AC_CHRONICLE_LOG_DIR` | `chronicle_logs` | Log subdirectory (relative to LogsDir) |
| `Chronicle.UploadURL` | `AC_CHRONICLE_UPLOAD_URL` | `""` | Chronicle server upload endpoint |
| `Chronicle.UploadSecret` | `AC_CHRONICLE_UPLOAD_SECRET` | `""` | Bearer token for upload auth |

When both `UploadURL` and `UploadSecret` are set, log files are gzipped,
uploaded to Chronicle when an instance closes, and deleted on success.

On startup the module pings the upload endpoint to verify connectivity:
```
Chronicle: enabled=true, logDir=chronicle_logs, upload=http://...
Chronicle: ping OK (HTTP 200) — connected to http://...
```

### 5. Getting Combat Logs

Logs appear in the configured directory:
```bash
ls ./env/dist/logs/chronicle_logs/
# instance_409_1_1714000000.log   (Molten Core, instance 1)
# instance_249_2_1714000100.log   (Onyxia's Lair, instance 2)

# Watch in real-time:
make logs
# or: tail -f ./env/dist/logs/chronicle_logs/instance_*.log
```

### 6. Connecting a WoW 3.3.5a Client

| Service | Address |
|---------|---------|
| Auth server | `127.0.0.1:3724` |
| World server | `127.0.0.1:8085` |
| MySQL | `127.0.0.1:3306` (root / password) |

1. Set your client's `realmlist.wtf` to `set realmlist 127.0.0.1`
2. Create an account via the worldserver console (`docker attach ac-worldserver`):
   ```
   account create testuser testpass
   account set gmlevel testuser 3 -1
   ```
3. Quick test — teleport into a dungeon and fight:
   ```
   .tele deadmines
   ```
4. Check `./env/dist/logs/chronicle_logs/` for the generated log.

## Development Workflow

### `make rebuild` — Full Rebuild (~3-4 min)

Rebuilds the Docker image with core + module, recreates the worldserver
container. Use when you've changed AzerothCore core files (e.g. hook patches).

### `make rebuild-module` — Fast Incremental (~5-15s)

Only recompiles changed module `.cpp`/`.h` files and relinks. Uses a persistent
Docker volume for the cmake build tree — first run is slow (cmake configure),
subsequent runs only touch changed files.

```bash
# Edit module source...
vim modules/mod-chronicle/src/Chronicle.cpp

# Rebuild and restart (~5-15s)
make rebuild-module
```

How it works:
1. Runs cmake inside a builder container (build stage with clang/cmake/ccache)
2. Module source is bind-mounted — picks up local edits without image rebuild
3. Build cache persists in a Docker volume across runs
4. Hot-swaps the worldserver binary into the running container via `docker cp`
5. Restarts worldserver to pick up the new binary

### Other Targets

```bash
make up          # Start all services
make down        # Stop all services
make restart     # Restart worldserver (no rebuild)
make logs        # Tail chronicle combat log files
make server-logs # Tail worldserver stdout/stderr
```

## Custom ScriptMgr Hooks

This module requires 14 hooks added to the AzerothCore core. These are
**read-only observer hooks** inserted at the server's packet-send points — they
have zero gameplay impact.

The hooks live as an uncommitted diff in the local AzerothCore working tree.
The goal is to submit them upstream as a PR.

### UnitScript (10 hooks)

| Hook | Inserted At | Data |
|------|------------|------|
| `OnSendAttackStateUpdate(CalcDamageInfo*, int32)` | `Unit::SendAttackStateUpdate()` | Full melee hit/miss with all mitigation |
| `OnSendSpellNonMeleeDamageLog(SpellNonMeleeDamage*)` | `Unit::SendSpellNonMeleeDamageLog()` | Full spell damage with all mitigation |
| `OnSendHealSpellLog(HealInfo const&, bool)` | `Unit::SendHealSpellLog()` | Heal amount, overheal, absorb, crit |
| `OnSendSpellMiss(Unit*, Unit*, uint32, SpellMissInfo)` | `Unit::SendSpellMiss()` | Spell miss with miss type |
| `OnSendSpellDamageImmune(Unit*, Unit*, uint32)` | `Unit::SendSpellDamageImmune()` | Spell immunity |
| `OnSendSpellDamageResist(Unit*, Unit*, uint32)` | `Unit::SendSpellDamageResist()` | Full spell resist |
| `OnSendSpellNonMeleeReflectLog(SpellNonMeleeDamage*, Unit*)` | `Unit::SendSpellNonMeleeReflectLog()` | Spell reflect |
| `OnSendEnergizeSpellLog(Unit*, Unit*, uint32, uint32, Powers)` | `Unit::SendEnergizeSpellLog()` | Mana/rage/energy gain |
| `OnSendPeriodicAuraLog(Unit*, SpellPeriodicAuraLogInfo*)` | `Unit::SendPeriodicAuraLog()` | Periodic tick (DoT/HoT/energize) |
| `OnDealMeleeDamage(CalcDamageInfo*, DamageInfo*, uint32)` | `Unit::DealDamageShieldDamage()` | Damage shield (thorns) |

### GlobalScript (3 hooks)

| Hook | Inserted At | Data |
|------|------------|------|
| `OnSpellSendSpellGo(Spell*)` | `Spell::SendSpellGo()` | Spell cast success at `SPELL_GO` packet |
| `OnAuraApplicationClientUpdate(Unit*, Aura*, bool)` | `AuraApplication::ClientUpdate()` | Aura applied/removed at client notification |
| `OnChangeUpdateData(Object*, uint16, uint64)` | `Object::SetUInt32Value()` | Object field updates (unused by module currently) |

### PlayerScript (1 hook)

| Hook | Inserted At | Data |
|------|------------|------|
| `OnEnvironmentalDamage(Player*, EnviromentalDamage, uint32)` | `Player::EnvironmentalDamage()` | Environmental damage (lava, drowning, fall) |

### Core Files Modified

```
src/server/game/Entities/Unit/Unit.cpp          (10 hook call sites)
src/server/game/Entities/Player/Player.cpp      (1 hook call site)
src/server/game/Spells/Spell.cpp                (1 hook call site)
src/server/game/Spells/Auras/SpellAuras.cpp     (1 hook call site)
src/server/game/Entities/Object/Object.cpp      (1 hook call site)
src/server/game/Scripting/ScriptDefines/UnitScript.h/.cpp
src/server/game/Scripting/ScriptDefines/GlobalScript.h/.cpp
src/server/game/Scripting/ScriptDefines/PlayerScript.h/.cpp
src/server/game/Scripting/ScriptMgr.h
```

## Future Work

- Boss encounter start/end events via `OnBeforeSetBossState`
- Encounter credit detection via `OnAfterUpdateEncounterState`
- `DISPEL` events
- Per-target hit/miss breakdown in `SPELL_CAST_SUCCESS` (from `Spell::m_UniqueTargetInfo`)
- Submit custom hooks as an upstream AzerothCore PR
