# ChronicleServer — Agent Guide

## What This Is

**ChronicleServer** is a server-side AzerothCore module (`mod-chronicle`) that generates combat log files from inside the game server. It produces the same V2 pipe-delimited format that [Chronicle](https://github.com/Emyrk/chronicle) already parses from its client-side WoW addon ([ChronicleCompanion](https://github.com/Emyrk/ChronicleCompanion)).

**The big idea:** Instead of relying on each player running an addon to capture combat logs, the server itself records everything. This means:
- 100% coverage — every player in every instance, no opt-in required
- No client-side data loss from disconnects, crashes, or addon bugs
- Server has authoritative data (no client-side spoofing)
- Works for private servers where you control the server binary

This started as a proof-of-concept and **it works** — the module compiles, loads into AzerothCore, and produces log files that Chronicle can parse. The goal now is to make it production-quality.

## Architecture

```
AzerothCore Server
├── UnitScript hooks ──────► EventFormatter ──► CombatLogWriter ──► .log file
├── AllSpellScript hooks ──►      │                    │
├── AllMapScript hooks ────►      │                    │
└── WorldScript hooks ─────►      │                    │
                                  │                    │
                           InstanceTracker (singleton)
                           Maps instanceId → writer
                           Tracks seen GUIDs per instance
```

### Three Main Classes

| Class | Role |
|-------|------|
| **EventFormatter** | Static methods that produce V2 pipe-delimited strings. Pure formatting, no state. |
| **CombatLogWriter** | Owns an `std::ofstream` for one instance. One file per dungeon/raid instance. |
| **InstanceTracker** | Singleton. Maps `instanceId → CombatLogWriter`. Manages dedup sets (`_seenPlayers`, `_seenUnits`). All methods are mutex-protected. |

### Hook Classes (in `ChronicleLogs_SC.cpp`)

| Hook | AC Event | Chronicle Event |
|------|----------|-----------------|
| `ChronicleUnitScript` | `OnDamage` | `SWING` (melee auto-attacks) |
| | `ModifySpellDamageTaken` | `SPELL_DMG` |
| | `ModifyHealReceived` | `HEAL` |
| | `OnAuraApply` | `BUFF_ADD` / `DEBUFF_ADD` |
| | `OnAuraRemove` | `BUFF_REM` / `DEBUFF_REM` |
| | `OnUnitDeath` | `DEATH` |
| `ChronicleAllSpellScript` | `OnSpellCast` | `SPELL_GO` |
| `ChronicleAllMapScript` | `OnPlayerEnterAll` | Creates writer, emits `HEADER` + `ZONE_INFO` + `COMBATANT_INFO` |
| | `OnPlayerLeaveAll` | (no-op currently, writer stays open) |
| | `OnDestroyInstance` | Closes writer, cleans up tracking sets |
| `ChronicleWorldScript` | `OnBeforeConfigLoad` | Loads module config |

### Key Pattern: EnsureUnitInfo

Before writing any combat event, hooks call `InstanceTracker::EnsureUnitInfo(unit)` for both source and target. This emits a `UNIT_INFO` line the first time a GUID appears in an instance, giving Chronicle the GUID→name mapping, level, owner, buffs, and max health.

## V2 Log Format

Each line: `<unix_millis>|EVENT_TYPE|field1|field2|...`

GUIDs: `0x%016llX` of AzerothCore's `ObjectGuid::GetRawValue()`. The bit layout matches Chronicle's GUID parsing (entity type in high bits).

### Format Reference

The **source of truth** for V2 formats is the Chronicle parser at:
```
github.com/Emyrk/chronicle/combatlog/parser/vanilla/parserv2/matcher.go
```

And the addon that originally produces them:
```
github.com/Emyrk/ChronicleCompanion/advancedlogging/core.lua
```

### Events We Emit

```
ts|HEADER|playerGuid|realmName|zoneName|addonVersion|superWoWVersion|namPowerVersion|xp3Version|wowVersion|wowBuild|wowBuildDate|localTime|utcTime
ts|ZONE_INFO|zoneName|instanceId|inInstance|instanceType|isGhost
ts|COMBATANT_INFO|guid|name|CLASS|Race|gender|guildName|rankName|rank|gear|talents|petName|petGuid
ts|UNIT_INFO|guid|isPlayer|name|canCooperate|ownerGuid|buffs|level|challenges|maxHealth
ts|SWING|attackerGuid|targetGuid|damage|hitInfo|victimState|components|blocked|absorbed|resisted
ts|SPELL_DMG|targetGuid|casterGuid|spellId|damage|blocked,absorbed,resisted|hitInfo|school|effect1,effect2,effect3,auraType
ts|HEAL|targetGuid|casterGuid|spellId|amount|isCrit|isPeriodic
ts|DEATH|victimGuid|killerGuid
ts|BUFF_ADD|targetGuid|luaSlot|spellId|stacks|auraLevel|auraSlot|state
ts|BUFF_REM|targetGuid|luaSlot|spellId|stacks|auraLevel|auraSlot|state
ts|DEBUFF_ADD|targetGuid|luaSlot|spellId|stacks|auraLevel|auraSlot|state
ts|DEBUFF_REM|targetGuid|luaSlot|spellId|stacks|auraLevel|auraSlot|state
ts|SPELL_GO|itemId|spellId|casterGuid|targetGuid|castFlags|targetsHit|targetsMissed
```

### Events We Don't Emit Yet

These are all supported by Chronicle's parser but not yet implemented server-side:

| Event | What it is | Difficulty |
|-------|-----------|------------|
| `MISS` | Dodge, parry, resist, immune, block | Medium — need `OnCalcMeleeMissChance` or similar |
| `ENERGIZE` | Mana/rage/energy gains | Easy — `OnPowerChange` hook |
| `ENV_DMG` | Lava, falling, drowning damage | Easy — `OnEnvironmentalDamage` hook |
| `DMG_SHIELD` | Thorns, retribution aura | Medium — `OnDamageTaken` with shield flag |
| `DISPEL` | Buff/debuff dispels | Medium — `OnDispel` hook |
| `SPELL_START` | Spell cast started (for cast bars) | Easy — `OnSpellStart` hook |
| `SPELL_FAIL` | Spell cast interrupted/failed | Easy — `OnSpellFailed` hook |
| `LOOT` | Item drops | Low priority |
| `COMBATANT_TALENTS` | Talent spec | Medium — talent API |
| `AURA_CAST` | Aura-triggered spell | Low priority |

## Known Issues & Limitations

### Duplicate SWING + SPELL_DMG
`OnDamage` fires for ALL damage including spells, so spell hits appear as both `SPELL_DMG` (from `ModifySpellDamageTaken`) and `SWING` (from `OnDamage`). Fix: use a thread-local flag set in `ModifySpellDamageTaken` to suppress the duplicate in `OnDamage`.

### No Crit/Periodic Flags
`ModifySpellDamageTaken` and `ModifyHealReceived` don't expose hit result info (crit, periodic). The `HEAL` and `SPELL_DMG` events always report `isCrit=0`, `isPeriodic=0`. Need hooks that fire after hit resolution, or tap into `DealDamage`/`HealBySpell` which have the full `DamageInfo`/`HealInfo`.

### canCooperate Approximation
`UNIT_INFO.canCooperate` uses `IsPlayer()` as a proxy (players=friendly, NPCs=hostile). The real addon uses `UnitCanCooperate("player", guid)`. Server-side there's no single "logging player" perspective.

### DEATH Has Extra Field
Chronicle's parser reads `DEATH|victimGuid` (one field). We emit `DEATH|victimGuid|killerGuid` (two fields). The parser ignores the extra field, but the killer info is wasted. Consider contributing a parser change to use it.

### Absorb/Block/Resist Always Zero
`SWING` and `SPELL_DMG` always report 0 for block, absorb, and resist values. Need access to the damage calculation result (`DamageInfo`) which isn't available in the current hooks.

### No Boss Encounter Detection
No `ENCOUNTER_START`/`ENCOUNTER_END` events. Chronicle detects encounters by matching mob GUIDs against its creature database, but explicit encounter boundaries would be more reliable. Could hook into `InstanceScript::SetBossState()`.

## Build & Test

### With AzerothCore Docker

```bash
# Clone AzerothCore
git clone https://github.com/azerothcore/azerothcore-wotlk.git

# Copy this module into the modules directory
# (Docker COPY doesn't follow symlinks — must be a real copy)
cp -r /path/to/ChronicleServer azerothcore-wotlk/modules/mod-chronicle

# Build and run
cd azerothcore-wotlk
docker compose build --no-cache ac-worldserver
docker compose up
```

The server is ready when you see `AzerothCore rev. ... ready.` in the worldserver logs, or when port 8085 is accepting connections.

### Rebuild Cycle

After changing module code:
```bash
cd azerothcore-wotlk
rm -rf modules/mod-chronicle
cp -r /path/to/ChronicleServer modules/mod-chronicle
docker compose build --no-cache ac-worldserver
docker compose up
```

The `rm -rf` + fresh copy is important — `cp -r` over an existing directory doesn't always overwrite everything, and Docker layer caching can be aggressive.

### Testing with a WoW Client

1. Connect a 3.3.5a client to `127.0.0.1:3724`
2. Create account via worldserver console: `account create test test` / `account set gmlevel test 3 -1`
3. Create character, use `.tele deadmines` to enter a dungeon
4. Fight mobs — log files appear in `./env/dist/logs/chronicle_logs/`
5. Upload to Chronicle or inspect with `tail -f`

### Configuration

Config auto-copies to `env/dist/etc/mod_chronicle.conf` on first run:
```ini
Chronicle.Enable = 1
Chronicle.LogDir = "chronicle_logs"
```

## File Structure

```
src/
├── Chronicle.h              # Class declarations: EventFormatter, CombatLogWriter, InstanceTracker
├── Chronicle.cpp            # All implementations
├── ChronicleLogs_SC.cpp     # Hook classes that capture events and call into the above
└── MP_loader.cpp            # AzerothCore module registration boilerplate
conf/
└── mod_chronicle.conf.dist          # Default config
```

## What Needs to Happen Next

### Priority 1: Fix Data Quality
- [ ] Eliminate duplicate SWING events for spell damage (thread-local suppression flag)
- [ ] Add crit/periodic detection to SPELL_DMG and HEAL (switch to hooks with DamageInfo/HealInfo)
- [ ] Populate absorb/block/resist values in damage events

### Priority 2: Missing Events
- [ ] MISS events (dodge, parry, resist, immune, block)
- [ ] ENERGIZE events (mana/rage/energy gains)
- [ ] SPELL_START and SPELL_FAIL
- [ ] ENV_DMG (environmental damage)
- [ ] DMG_SHIELD (thorns etc.)
- [ ] DISPEL events

### Priority 3: Infrastructure
- [ ] Auto-upload completed logs to Chronicle API (HTTP POST on instance destroy)
- [ ] Log rotation / cleanup (don't fill disk forever)
- [ ] Boss encounter detection (hook InstanceScript::SetBossState)
- [ ] Real-time streaming (instead of file-based, push events directly to Chronicle)
- [ ] Unit tests (mock AzerothCore types, verify format strings)

### Priority 4: Multi-Server Support
- [ ] Abstract away AzerothCore-specific APIs behind an interface
- [ ] Support other WotLK cores (TrinityCore, CMaNGOS)
- [ ] Support Vanilla/TBC cores (different spell/combat APIs)
