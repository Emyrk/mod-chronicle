/*
 * Copyright (C) 2024+ Chronicle <https://github.com/Emyrk/chronicle>
 * Released under GNU AGPL v3 license
 */

#include "Chronicle.h"

#include "Config.h"
#include "GameTime.h"
#include "Guild.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "Pet.h"
#include "Player.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellAuras.h"
#include "World.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

// ===== EventFormatter =====

std::string EventFormatter::Guid(ObjectGuid guid)
{
    char buf[20];
    snprintf(buf, sizeof(buf), "0x%016llX",
             static_cast<unsigned long long>(guid.GetRawValue()));
    return buf;
}

uint64 EventFormatter::Now()
{
    // Real wall-clock milliseconds since Unix epoch.
    auto now = std::chrono::system_clock::now();
    return static_cast<uint64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch())
            .count());
}

// ---------------------------------------------------------------------------
// UnitFlags — compute WotLK COMBATLOG_OBJECT_* flags from Unit properties.
// ---------------------------------------------------------------------------
uint32 EventFormatter::UnitFlags(Unit* unit)
{
    if (!unit)
        return 0;

    uint32 flags = 0;

    // Type flags
    if (unit->IsPlayer())
        flags |= 0x0400;  // COMBATLOG_OBJECT_TYPE_PLAYER
    else if (unit->IsGuardian())
        flags |= 0x2000;  // COMBATLOG_OBJECT_TYPE_GUARDIAN
    else if (unit->IsPet())
        flags |= 0x1000;  // COMBATLOG_OBJECT_TYPE_PET
    else
        flags |= 0x0800;  // COMBATLOG_OBJECT_TYPE_NPC

    // Control flags
    if (unit->IsPlayer() || unit->GetOwnerGUID().IsPlayer())
        flags |= 0x0100;  // COMBATLOG_OBJECT_CONTROL_PLAYER
    else
        flags |= 0x0200;  // COMBATLOG_OBJECT_CONTROL_NPC

    // Reaction — approximate: players + player pets = friendly, NPCs = hostile
    if (unit->IsPlayer() || unit->GetOwnerGUID().IsPlayer())
        flags |= 0x0010;  // COMBATLOG_OBJECT_REACTION_FRIENDLY
    else
        flags |= 0x0040;  // COMBATLOG_OBJECT_REACTION_HOSTILE

    // Affiliation — players/pets in the instance are raid members
    if (unit->IsPlayer())
        flags |= 0x0004;  // COMBATLOG_OBJECT_AFFILIATION_RAID
    else if (unit->GetOwnerGUID().IsPlayer())
        flags |= 0x0004;  // COMBATLOG_OBJECT_AFFILIATION_RAID (pet of raid member)
    else
        flags |= 0x0008;  // COMBATLOG_OBJECT_AFFILIATION_OUTSIDER

    return flags;
}

// ---------------------------------------------------------------------------
// BaseParams — the 6 fields present on every standard WotLK event.
// ---------------------------------------------------------------------------
std::string EventFormatter::BaseParams(Unit* source, Unit* dest)
{
    std::ostringstream ss;
    ss << Guid(source ? source->GetGUID() : ObjectGuid::Empty)
       << ",\"" << (source ? source->GetName() : "") << "\""
       << ",0x0"
       << "," << Guid(dest ? dest->GetGUID() : ObjectGuid::Empty)
       << ",\"" << (dest ? dest->GetName() : "") << "\""
       << ",0x0";
    return ss.str();
}

// ===== Chronicle Extension Events (CHRONICLE_*) =====

static std::string ClassToString(uint8 cls)
{
    switch (cls)
    {
        case 1:  return "WARRIOR";
        case 2:  return "PALADIN";
        case 3:  return "HUNTER";
        case 4:  return "ROGUE";
        case 5:  return "PRIEST";
        case 6:  return "DEATHKNIGHT";
        case 7:  return "SHAMAN";
        case 8:  return "MAGE";
        case 9:  return "WARLOCK";
        case 11: return "DRUID";
        default: return "UNKNOWN";
    }
}

static std::string RaceToString(uint8 race)
{
    switch (race)
    {
        case 1:  return "Human";
        case 2:  return "Orc";
        case 3:  return "Dwarf";
        case 4:  return "NightElf";
        case 5:  return "Scourge";
        case 6:  return "Tauren";
        case 7:  return "Gnome";
        case 8:  return "Troll";
        case 10: return "BloodElf";
        case 11: return "Draenei";
        default: return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// CHRONICLE_HEADER — emitted once when log file is created.
// ---------------------------------------------------------------------------
std::string EventFormatter::Header(std::string const& realmName)
{
    std::ostringstream ss;
    ss << Now() << "  CHRONICLE_HEADER"
       << ",\"" << realmName << "\""
       << ",\"3.3.5a\""
       << ",12340";
    return ss.str();
}

// ---------------------------------------------------------------------------
// CHRONICLE_ZONE_INFO — emitted once per instance.
// ---------------------------------------------------------------------------
std::string EventFormatter::ZoneInfo(std::string const& zoneName, uint32 mapId,
                                     uint32 instanceId, std::string const& instanceType)
{
    std::ostringstream ss;
    ss << Now() << "  CHRONICLE_ZONE_INFO"
       << ",\"" << zoneName << "\""
       << "," << mapId
       << "," << instanceId
       << ",\"" << instanceType << "\"";
    return ss.str();
}

// ---------------------------------------------------------------------------
// CHRONICLE_COMBATANT_INFO — emitted when a player enters the instance.
// ---------------------------------------------------------------------------
std::string EventFormatter::CombatantInfo(Player* player)
{
    std::ostringstream ss;
    ss << Now() << "  CHRONICLE_COMBATANT_INFO"
       << "," << Guid(player->GetGUID())
       << ",\"" << player->GetName() << "\""
       << ",\"" << ClassToString(player->getClass()) << "\""
       << ",\"" << RaceToString(player->getRace()) << "\""
       << "," << static_cast<int>(player->getGender())
       << "," << static_cast<int>(player->GetLevel());

    Guild* guild = player->GetGuild();
    ss << ",\"" << (guild ? guild->GetName() : "") << "\"";

    // Gear: 19 slots, &-separated, each as itemId:enchantId:suffixId:0
    ss << ",\"";
    for (uint8 slot = 0; slot < 19; ++slot)
    {
        if (slot > 0)
            ss << "&";
        Item* item = player->GetItemByPos(255 /*INVENTORY_SLOT_BAG_0*/, slot);
        if (item)
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "%u:%u:%d:0",
                     item->GetEntry(),
                     item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT),
                     item->GetItemRandomPropertyId());
            ss << buf;
        }
        else
        {
            ss << "0";
        }
    }
    ss << "\"";
    return ss.str();
}

// ---------------------------------------------------------------------------
// CHRONICLE_UNIT_INFO — emitted first time a GUID appears in combat.
// ---------------------------------------------------------------------------
std::string EventFormatter::UnitInfo(Unit* unit)
{
    std::ostringstream ss;
    ss << Now() << "  CHRONICLE_UNIT_INFO"
       << "," << Guid(unit->GetGUID())
       << ",\"" << unit->GetName() << "\""
       << "," << static_cast<int>(unit->GetLevel())
       << ",0x0";

    ObjectGuid ownerGuid = unit->GetOwnerGUID();
    if (!ownerGuid.IsEmpty())
        ss << "," << Guid(ownerGuid);
    else
        ss << ",0x0000000000000000";

    ss << "," << unit->GetMaxHealth();
    return ss.str();
}

// ---------------------------------------------------------------------------
// CHRONICLE_UNIT_EVADE — emitted when a creature enters evade mode.
// Fields: guid, "name", why (EvadeReason enum)
// ---------------------------------------------------------------------------
std::string EventFormatter::UnitEvade(Unit* unit, uint8 evadeReason)
{
    std::ostringstream ss;
    ss << Now() << "  CHRONICLE_UNIT_EVADE"
       << "," << Guid(unit->GetGUID())
       << ",\"" << unit->GetName() << "\""
       << "," << static_cast<int>(evadeReason);
    return ss.str();
}

// ---------------------------------------------------------------------------
// CHRONICLE_UNIT_COMBAT — emitted when a unit enters combat with a victim.
// Fields: unitGuid, "unitName", victimGuid, "victimName"
// ---------------------------------------------------------------------------
std::string EventFormatter::UnitCombat(Unit* unit, Unit* victim)
{
    std::ostringstream ss;
    ss << Now() << "  CHRONICLE_UNIT_COMBAT"
       << "," << Guid(unit->GetGUID())
       << ",\"" << unit->GetName() << "\""
       << "," << Guid(victim ? victim->GetGUID() : ObjectGuid::Empty)
       << ",\"" << (victim ? victim->GetName() : "") << "\"";
    return ss.str();
}

// ===== Standard WotLK Combat Events =====

// ---------------------------------------------------------------------------
// SWING_DAMAGE — melee auto-attack.
// ---------------------------------------------------------------------------
std::string EventFormatter::SwingDamage(Unit* attacker, Unit* victim, uint32 damage)
{
    std::ostringstream ss;
    ss << Now() << "  SWING_DAMAGE,"
       << BaseParams(attacker, victim)
       << "," << damage
       << ",0"              // overkill
       << ",1"              // school (physical)
       << ",0,0,0"          // resisted, blocked, absorbed
       << ",nil,nil,nil";   // critical, glancing, crushing
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_DAMAGE — spell direct damage.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellDamage(Unit* attacker, Unit* victim,
                                        SpellInfo const* spell, int32 damage)
{
    std::ostringstream ss;
    ss << Now() << "  SPELL_DAMAGE,"
       << BaseParams(attacker, victim)
       << "," << spell->Id
       << ",\"" << spell->SpellName[0] << "\""
       << ",0x" << std::hex << spell->SchoolMask << std::dec
       << "," << (damage > 0 ? damage : 0)
       << ",0"              // overkill
       << "," << spell->SchoolMask
       << ",0,0,0"          // resisted, blocked, absorbed
       << ",nil,nil,nil";   // critical, glancing, crushing
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_HEAL — healing.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellHeal(Unit* healer, Unit* target,
                                      SpellInfo const* spell, uint32 amount)
{
    std::ostringstream ss;
    ss << Now() << "  SPELL_HEAL,"
       << BaseParams(healer, target)
       << "," << spell->Id
       << ",\"" << spell->SpellName[0] << "\""
       << ",0x" << std::hex << spell->SchoolMask << std::dec
       << "," << amount
       << ",0"            // overheal
       << ",0"            // absorbed
       << ",nil";         // critical
    return ss.str();
}

// ---------------------------------------------------------------------------
// UNIT_DIED — a unit has died.
// ---------------------------------------------------------------------------
std::string EventFormatter::UnitDied(Unit* killer, Unit* victim)
{
    std::ostringstream ss;
    ss << Now() << "  UNIT_DIED,"
       << Guid(killer ? killer->GetGUID() : ObjectGuid::Empty)
       << ",\"" << (killer ? killer->GetName() : "") << "\""
       << ",0x0"
       << "," << Guid(victim ? victim->GetGUID() : ObjectGuid::Empty)
       << ",\"" << (victim ? victim->GetName() : "") << "\""
       << ",0x0";
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_AURA_APPLIED — aura applied.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellAuraApplied(Unit* target, Aura* aura)
{
    SpellInfo const* spell = aura->GetSpellInfo();
    bool isBuff = spell->IsPositive();

    std::ostringstream ss;
    ss << Now() << "  SPELL_AURA_APPLIED,"
       << BaseParams(target, target)
       << "," << spell->Id
       << ",\"" << spell->SpellName[0] << "\""
       << ",0x" << std::hex << spell->SchoolMask << std::dec
       << "," << (isBuff ? "BUFF" : "DEBUFF");
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_AURA_REMOVED — aura removed.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellAuraRemoved(Unit* target, AuraApplication* aurApp)
{
    Aura* aura = aurApp->GetBase();
    SpellInfo const* spell = aura->GetSpellInfo();
    bool isBuff = spell->IsPositive();

    std::ostringstream ss;
    ss << Now() << "  SPELL_AURA_REMOVED,"
       << BaseParams(target, target)
       << "," << spell->Id
       << ",\"" << spell->SpellName[0] << "\""
       << ",0x" << std::hex << spell->SchoolMask << std::dec
       << "," << (isBuff ? "BUFF" : "DEBUFF");
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_CAST_SUCCESS — spell cast completed.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellCastSuccess(Unit* caster, SpellInfo const* spell)
{
    std::ostringstream ss;
    ss << Now() << "  SPELL_CAST_SUCCESS,"
       << BaseParams(caster, caster)
       << "," << spell->Id
       << ",\"" << spell->SpellName[0] << "\""
       << ",0x" << std::hex << spell->SchoolMask << std::dec;
    return ss.str();
}

// ===== CombatLogWriter =====

CombatLogWriter::CombatLogWriter(std::string const& dir, uint32 mapId, uint32 instanceId)
{
    std::filesystem::create_directories(dir);

    auto now = std::chrono::system_clock::now();
    auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                     now.time_since_epoch())
                     .count();

    char filename[128];
    snprintf(filename, sizeof(filename), "instance_%u_%u_%lld.log",
             mapId, instanceId, static_cast<long long>(epoch));

    _path = dir + "/" + filename;
    _file.open(_path, std::ios::out | std::ios::app);

    if (_file.is_open())
    {
        LOG_INFO("module", "Chronicle: opened log file {}", _path);
    }
    else
    {
        LOG_ERROR("module", "Chronicle: FAILED to open log file {}", _path);
    }
}

CombatLogWriter::~CombatLogWriter()
{
    Close();
}

void CombatLogWriter::WriteLine(std::string const& line)
{
    if (_file.is_open())
        _file << line << std::endl;
}

void CombatLogWriter::Flush()
{
    if (_file.is_open())
        _file.flush();
}

void CombatLogWriter::Close()
{
    if (_file.is_open())
    {
        _file.flush();
        _file.close();
        LOG_INFO("module", "Chronicle: closed log file {}", _path);
    }
}

bool CombatLogWriter::IsOpen() const
{
    return _file.is_open();
}

// ===== InstanceTracker =====

InstanceTracker& InstanceTracker::Instance()
{
    static InstanceTracker instance;
    return instance;
}

void InstanceTracker::LoadConfig()
{
    _enabled   = sConfigMgr->GetOption<bool>("Chronicle.Enable", true);
    _logDir    = sConfigMgr->GetOption<std::string>("Chronicle.LogDir", "chronicle_logs");
    LOG_INFO("module", "Chronicle: enabled={}, logDir={}",
             _enabled, _logDir);
}

CombatLogWriter* InstanceTracker::GetOrCreateWriter(Map* map)
{
    if (!map || !map->IsDungeon())
        return nullptr;

    uint32 instanceId = map->GetInstanceId();
    auto it = _writers.find(instanceId);
    if (it != _writers.end())
        return it->second.get();

    std::string logsDir = sConfigMgr->GetOption<std::string>("LogsDir", "");
    std::string logPath;
    if (logsDir.empty())
        logPath = _logDir;
    else
        logPath = logsDir + "/" + _logDir;

    auto writer = std::make_unique<CombatLogWriter>(logPath, map->GetId(), instanceId);
    if (!writer->IsOpen())
        return nullptr;

    // Write CHRONICLE_HEADER and CHRONICLE_ZONE_INFO
    writer->WriteLine(EventFormatter::Header(sWorld->GetRealmName()));
    std::string instanceType = map->IsRaid() ? "raid" : "party";
    writer->WriteLine(EventFormatter::ZoneInfo(map->GetMapName(), map->GetId(), instanceId, instanceType));

    CombatLogWriter* ptr = writer.get();
    _writers[instanceId] = std::move(writer);
    return ptr;
}

void InstanceTracker::OnPlayerEnterInstance(Map* map, Player* player)
{
    if (!_enabled || !map || !map->IsDungeon())
        return;

    std::lock_guard<std::mutex> lock(_mutex);

    CombatLogWriter* writer = GetOrCreateWriter(map);
    if (!writer)
        return;

    writer->WriteLine(EventFormatter::CombatantInfo(player));
    writer->Flush();
}

void InstanceTracker::OnPlayerLeaveInstance(Map* map, Player* /*player*/)
{
    if (!_enabled || !map || !map->IsDungeon())
        return;
}

void InstanceTracker::RemoveInstance(uint32 instanceId)
{
    std::lock_guard<std::mutex> lock(_mutex);

    auto it = _writers.find(instanceId);
    if (it != _writers.end())
    {
        it->second->Close();
        _writers.erase(it);
    }
    _seenUnits.erase(instanceId);
}

void InstanceTracker::EnsureUnitInfo(Unit* unit)
{
    if (!_enabled || !unit)
        return;

    Map* map = unit->FindMap();
    if (!map || !map->IsDungeon())
        return;

    std::lock_guard<std::mutex> lock(_mutex);

    uint32 instId = map->GetInstanceId();
    uint64 raw = unit->GetGUID().GetRawValue();

    auto& seen = _seenUnits[instId];
    if (seen.count(raw))
        return;
    seen.insert(raw);

    auto it = _writers.find(instId);
    if (it == _writers.end())
        return;

    it->second->WriteLine(EventFormatter::UnitInfo(unit));
}

void InstanceTracker::WriteForUnit(Unit* unit, std::string const& line)
{
    if (!_enabled || !unit)
        return;

    Map* map = unit->FindMap();
    if (!map || !map->IsDungeon())
        return;

    std::lock_guard<std::mutex> lock(_mutex);

    uint32 instanceId = map->GetInstanceId();
    auto it = _writers.find(instanceId);
    if (it == _writers.end())
        return;

    it->second->WriteLine(line);
}
