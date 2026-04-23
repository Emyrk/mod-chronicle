/*
 * Copyright (C) 2024+ Chronicle <https://github.com/Emyrk/chronicle>
 * Released under GNU AGPL v3 license
 *
 * Server-side combat log generator for Chronicle.
 * Produces per-instance log files in WotLK combat log format with
 * Chronicle-specific extension events (CHRONICLE_*).
 *
 * Line format: <unix_millis>  EVENT,field,field,...
 *   - Standard WotLK events: SWING_DAMAGE, SPELL_DAMAGE, SPELL_HEAL, etc.
 *   - Chronicle extensions:  CHRONICLE_HEADER, CHRONICLE_ZONE_INFO,
 *                             CHRONICLE_COMBATANT_INFO, CHRONICLE_UNIT_INFO
 */

#ifndef MOD_CHRONICLE_H
#define MOD_CHRONICLE_H

#include "ObjectGuid.h"
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

class Aura;
class AuraApplication;
class Map;
class Player;
class Spell;
class SpellInfo;
class Unit;
enum AuraRemoveMode : uint8;

// ---------------------------------------------------------------------------
// EventFormatter — produces WotLK-style comma-separated combat log lines
// with unix millisecond timestamps.
//
// Format:  <unix_millis>  EVENT_TYPE,sourceGUID,"sourceName",sourceFlags,
//                         destGUID,"destName",destFlags,[event params...]
// GUIDs:   0x%016llX of ObjectGuid::GetRawValue()
// ---------------------------------------------------------------------------
class EventFormatter
{
public:
    static std::string Guid(ObjectGuid guid);

    // Returns wall-clock milliseconds since Unix epoch.
    static uint64 Now();

    // Compute WotLK-style unit flags from Unit properties.
    // Flag reference (from WoW API):
    //   Type:        PLAYER=0x0400, NPC=0x0800, PET=0x1000, GUARDIAN=0x2000
    //   Control:     PLAYER=0x0100, NPC=0x0200
    //   Reaction:    FRIENDLY=0x0010, NEUTRAL=0x0020, HOSTILE=0x0040
    //   Affiliation: MINE=0x0001, PARTY=0x0002, RAID=0x0004, OUTSIDER=0x0008
    static uint32 UnitFlags(Unit* unit);

    // Base params: sourceGUID,"sourceName",sourceFlags,destGUID,"destName",destFlags
    static std::string BaseParams(Unit* source, Unit* dest);

    // --- Chronicle extension events (CHRONICLE_*) ---
    static std::string Header(std::string const& realmName);
    static std::string ZoneInfo(std::string const& zoneName, uint32 mapId,
                                uint32 instanceId, std::string const& instanceType);
    static std::string CombatantInfo(Player* player);
    static std::string UnitInfo(Unit* unit);

    // --- Standard WotLK combat events ---
    static std::string SwingDamage(Unit* attacker, Unit* victim, uint32 damage);
    static std::string SpellDamage(Unit* attacker, Unit* victim, SpellInfo const* spell, int32 damage);
    static std::string SpellHeal(Unit* healer, Unit* target, SpellInfo const* spell, uint32 amount);
    static std::string UnitDied(Unit* killer, Unit* victim);
    static std::string SpellAuraApplied(Unit* target, Aura* aura);
    static std::string SpellAuraRemoved(Unit* target, AuraApplication* aurApp);
    static std::string SpellCastSuccess(Unit* caster, SpellInfo const* spell);
};

// ---------------------------------------------------------------------------
// CombatLogWriter — one per active instance.  Owns an std::ofstream.
// ---------------------------------------------------------------------------
class CombatLogWriter
{
public:
    CombatLogWriter(std::string const& dir, uint32 mapId, uint32 instanceId);
    ~CombatLogWriter();

    void WriteLine(std::string const& line);
    void Flush();
    void Close();
    bool IsOpen() const;
    std::string const& GetPath() const { return _path; }

private:
    std::ofstream _file;
    std::string   _path;
};

// ---------------------------------------------------------------------------
// InstanceTracker — singleton.  Maps instanceId → CombatLogWriter.
// ---------------------------------------------------------------------------
class InstanceTracker
{
public:
    static InstanceTracker& Instance();

    void LoadConfig();
    bool IsEnabled() const { return _enabled; }

    // Called from AllMapScript hooks
    void OnPlayerEnterInstance(Map* map, Player* player);
    void OnPlayerLeaveInstance(Map* map, Player* player);
    void RemoveInstance(uint32 instanceId);

    // Called from UnitScript / AllSpellScript hooks — resolves unit→map→writer.
    void WriteForUnit(Unit* unit, std::string const& line);

    // Emits CHRONICLE_UNIT_INFO for a unit the first time it's seen in an instance.
    // Called from combat hooks before writing damage/heal/death events.
    void EnsureUnitInfo(Unit* unit);

private:
    InstanceTracker() = default;

    CombatLogWriter* GetOrCreateWriter(Map* map);

    std::mutex _mutex;
    std::unordered_map<uint32, std::unique_ptr<CombatLogWriter>> _writers;
    // Set of (instanceId, playerGuid) pairs to avoid duplicate COMBATANT_INFO
    std::unordered_map<uint32, std::unordered_set<uint64>> _seenPlayers;
    // Set of (instanceId, unitGuid) pairs to avoid duplicate UNIT_INFO
    std::unordered_map<uint32, std::unordered_set<uint64>> _seenUnits;

    bool        _enabled   = false;
    std::string _logDir    = "chronicle_logs";
    std::string _realmName = "AzerothCore";
};

#endif // MOD_CHRONICLE_H
