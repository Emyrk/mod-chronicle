# mod-chronicle

A server-side AzerothCore module that generates combat log files compatible with
[Chronicle](https://github.com/Emyrk/chronicle)'s V2 pipe-delimited format.

Every dungeon/raid instance gets its own log file. Events are written in
real-time as combat happens, producing files that can be uploaded directly to
Chronicle for analysis.

## Events Captured (Phase 1)

| Event | Description |
|-------|-------------|
| `HEADER` | Written once when the instance log is created |
| `ZONE_INFO` | Instance zone name |
| `COMBATANT_INFO` | Player gear, guild, class, race (per player on enter) |
| `SWING` | Melee auto-attack damage |
| `SPELL_DMG` | Spell/ability damage (with spell ID and school) |
| `HEAL` | Healing (with spell ID) |
| `DEATH` | Unit deaths |
| `BUFF_ADD/REM` | Buff application/removal |
| `DEBUFF_ADD/REM` | Debuff application/removal |
| `SPELL_GO` | Spell cast completed |

## Setup

### 1. Link the Module into AzerothCore

The module must live inside the `modules/` directory of your AzerothCore source
tree. AC's CMake auto-discovers any subdirectory containing `.cpp` files.

```bash
cd research/azerothcore/azerothcore-wotlk/modules

# Option A: Symlink (local/non-Docker builds)
ln -s ../../mod-chronicle mod-chronicle

# Option B: Copy (required for Docker COPY)
cp -r ../../mod-chronicle mod-chronicle
```

> **Docker note:** `COPY modules /azerothcore/modules` in the Dockerfile doesn't
> follow symlinks. Use Option B for Docker builds, or add a bind mount (see below).

### 2. Build with Docker

```bash
cd research/azerothcore/azerothcore-wotlk

# Build everything (AC core + module) and start services
docker compose up --build
```

**First-time setup takes a while:**
- `ac-client-data-init` downloads ~2 GB of map/DBC data
- `ac-db-import` initializes MySQL schemas (acore_auth, acore_world, acore_characters)
- Full C++ build: 10–30 minutes depending on hardware

To fully reset and rebuild from scratch:
```bash
docker compose down -v
docker compose up --build
```

### 3. Configuration

The module config file is automatically copied to the AC `etc/` directory on
first run. Edit it at:

```
./env/dist/etc/mod_chronicle.conf
```

Default settings:
```ini
Chronicle.Enable = 1
Chronicle.LogDir = "chronicle_logs"
Chronicle.UploadURL = ""       # Set to Chronicle endpoint for auto-upload
Chronicle.UploadSecret = ""    # Shared secret for upload auth
```

When both `UploadURL` and `UploadSecret` are configured, log files are
automatically uploaded to Chronicle when an instance closes and deleted on
success.

### 4. Getting Combat Logs

The AC Docker setup mounts a logs volume:
- **Host:** `./env/dist/logs/`
- **Container:** `/azerothcore/env/dist/logs/`

Combat logs appear in a subdirectory:
```bash
ls -la ./env/dist/logs/chronicle_logs/

# Example output:
# instance_409_1_1714000000.log   (Molten Core, instance 1)
# instance_249_2_1714000100.log   (Onyxia's Lair, instance 2)
```

Watch logs in real-time:
```bash
tail -f ./env/dist/logs/chronicle_logs/instance_*.log
```

### 5. Connecting a WoW 3.3.5a Client

| Service | Address |
|---------|---------|
| Auth server | `127.0.0.1:3724` |
| World server | `127.0.0.1:8085` |
| MySQL | `127.0.0.1:3306` (root / password) |

**Client setup:**
1. Edit your WoW client's `realmlist.wtf`:
   ```
   set realmlist 127.0.0.1
   ```
2. Attach to the worldserver console:
   ```bash
   docker attach ac-worldserver
   ```
3. Create an account and set GM level:
   ```
   account create testuser testpass
   account set gmlevel testuser 3 -1
   ```
4. Log in with the WoW client.

**Quick test:** Use GM commands to teleport into a dungeon and kill mobs:
```
.tele deadmines
.damage 99999
```

Then check `./env/dist/logs/chronicle_logs/` for the generated log file.

### 6. Upload to Chronicle

```bash
# Upload a completed log file
curl -F "combat_log=@./env/dist/logs/chronicle_logs/instance_36_1_1714000000.log" \
     https://your-chronicle-instance.com/api/v1/logs/upload-v2

# Or with gzip compression:
gzip -c ./env/dist/logs/chronicle_logs/instance_36_1_1714000000.log | \
  curl -F "combat_log=@-;filename=combat.log.gz" \
       https://your-chronicle-instance.com/api/v1/logs/upload-v2
```

## Docker Development Workflow

For faster iteration (avoid full rebuilds), use the dev server profile with a
bind mount:

```yaml
# docker-compose.override.yml (create in azerothcore-wotlk root)
services:
  ac-dev-server:
    volumes:
      - ../../mod-chronicle:/azerothcore/modules/mod-chronicle
```

Then:
```bash
docker compose --profile dev up ac-dev-server ac-database
```

This mounts the module source directly into the container. You still need to
recompile inside the container after changes, but don't need to rebuild the
Docker image.

## Log Format

Each line follows Chronicle's V2 format:
```
<unix_millis>|EVENT_TYPE|field1|field2|...
```

GUIDs are 64-bit hex values: `0x000000000001C80A`

Example output:
```
1714000000000|HEADER|0x0000000000000000|AzerothCore||chronicle-server||||3.3.5a|12340||22.04.26 15:30:00|22.04.26 15:30:00
1714000000000|ZONE_INFO|22.04.26 15:30:00&Molten Core&0
1714000000001|COMBATANT_INFO|0x000000000004C53D|Testplayer|WARRIOR|Human|0|TestGuild|Officer|2|12345:0:0:0&nil&nil&...&nil|nil|nil|nil
1714000000500|SWING|0x000000000004C53D|0xF130002C3600BE05|1523|2|1|1|0|0|0
1714000001000|SPELL_DMG|0xF130002C3600BE05|0x000000000004C53D|6572|2500|0,0,0|1|1|0,0,0,0
1714000001500|HEAL|0x000000000004C53D|0x000000000004C53D|23880|1200|0|0
1714000002000|DEATH|0xF130002C3600BE05|0x000000000004C53D
```

## Future Work (Phase 2+)

- `MISS` events (dodge, parry, resist, immune)
- `ENERGIZE` events (mana/rage/energy gains)
- `ENV_DMG` (environmental damage: lava, falling, drowning)
- `DMG_SHIELD` (thorns/retribution aura damage)
- `DISPEL` events
- Crit/periodic flags on HEAL and SPELL_DMG
- Absorb/block/resist values in damage events
- Auto-upload completed logs to Chronicle API
- Boss encounter start/end detection via InstanceScript::SetBossState
