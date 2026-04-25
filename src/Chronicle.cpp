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
#include "SpellMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "World.h"
#include "DBCStores.h"

#include <zlib.h>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>

#include <algorithm>
#include <chrono>
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
// This is incomplete compared to the client side logs.
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
// BuildTalentString — produces the same format as the vanilla Lua addon:
//   "215303100000000000}055051000050122231}00000000000000000000"
// One digit per talent slot (the invested rank, 0 if unlearned), three tabs
// separated by '}', talents ordered by (Row, Col) within each tab.
// ---------------------------------------------------------------------------
static std::string BuildTalentString(Player* player)
{
    uint32 const* tabPages = GetTalentTabPages(player->getClass());
    if (!tabPages)
        return "";

    uint8 activeSpec = player->GetActiveSpec();
    const PlayerTalentMap& talentMap = player->GetTalentMap();

    std::string result;
    for (uint8 tab = 0; tab < 3; ++tab)
    {
        if (tab > 0)
            result += '}';

        uint32 tabId = tabPages[tab];
        if (!tabId)
            continue;

        // Collect talents for this tab.
        struct TalentSlot { uint32 row; uint32 col; uint8 rank; };
        std::vector<TalentSlot> slots;

        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* talent = sTalentStore.LookupEntry(i);
            if (!talent || talent->TalentTab != tabId)
                continue;

            // Determine the player's rank in this talent (0 = unlearned).
            uint8 rank = 0;
            for (uint8 r = MAX_TALENT_RANK; r > 0; --r)
            {
                uint32 spellId = talent->RankID[r - 1];
                if (!spellId)
                    continue;
                auto itr = talentMap.find(spellId);
                if (itr != talentMap.end()
                    && itr->second->State != PLAYERSPELL_REMOVED
                    && itr->second->IsInSpec(activeSpec))
                {
                    rank = r;
                    break;
                }
            }

            slots.push_back({ talent->Row, talent->Col, rank });
        }

        // Sort by (Row, Col) to match the Lua GetTalentInfo ordering.
        std::sort(slots.begin(), slots.end(), [](const TalentSlot& a, const TalentSlot& b) {
            return a.row != b.row ? a.row < b.row : a.col < b.col;
        });

        for (auto const& s : slots)
            result += static_cast<char>('0' + s.rank);
    }

    return result;
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

    // Talents: "ranks}ranks}ranks" — one digit per talent per tab, '}'-separated.
    ss << ",\"" << BuildTalentString(player) << "\"";

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

// Helper: map MeleeHitOutcome to miss-type string.
static const char* MeleeHitOutcomeToMissType(MeleeHitOutcome outcome)
{
    switch (outcome)
    {
        case MELEE_HIT_MISS:    return "MISS";
        case MELEE_HIT_DODGE:   return "DODGE";
        case MELEE_HIT_PARRY:   return "PARRY";
        case MELEE_HIT_BLOCK:   return "BLOCK";
        case MELEE_HIT_EVADE:   return "EVADE";
        default:                return "MISS";
    }
}

// Helper: map SpellMissInfo to miss-type string.
static const char* SpellMissInfoToString(SpellMissInfo info)
{
    switch (info)
    {
        case SPELL_MISS_MISS:    return "MISS";
        case SPELL_MISS_RESIST:  return "RESIST";
        case SPELL_MISS_DODGE:   return "DODGE";
        case SPELL_MISS_PARRY:   return "PARRY";
        case SPELL_MISS_BLOCK:   return "BLOCK";
        case SPELL_MISS_EVADE:   return "EVADE";
        case SPELL_MISS_IMMUNE:
        case SPELL_MISS_IMMUNE2: return "IMMUNE";
        case SPELL_MISS_DEFLECT: return "DEFLECT";
        case SPELL_MISS_ABSORB:  return "ABSORB";
        case SPELL_MISS_REFLECT: return "REFLECT";
        default:                 return "MISS";
    }
}

// Helper: emit the spell prefix triple: spellId,"spellName",spellSchool(hex)
static void AppendSpellPrefix(std::ostringstream& ss, uint32 spellId,
                               char const* spellName, uint32 schoolMask)
{
    ss << "," << spellId
       << ",\"" << (spellName ? spellName : "") << "\""
       << ",0x" << std::hex << schoolMask << std::dec;
}

// Helper: emit damage suffix: amount,overkill,school,resisted,blocked,absorbed,critical,glancing,crushing
static void AppendDamageSuffix(std::ostringstream& ss, uint32 amount,
                                int32 overkill, uint32 school,
                                uint32 resisted, uint32 blocked, uint32 absorbed,
                                bool critical, bool glancing, bool crushing)
{
    ss << "," << amount
       << "," << (overkill > 0 ? overkill : 0)
       << "," << school
       << "," << resisted
       << "," << blocked
       << "," << absorbed
       << "," << (critical  ? "1" : "nil")
       << "," << (glancing  ? "1" : "nil")
       << "," << (crushing  ? "1" : "nil");
}

// ---------------------------------------------------------------------------
// SWING_DAMAGE — melee auto-attack hit.
// ---------------------------------------------------------------------------
std::string EventFormatter::SwingDamage(CalcDamageInfo* damageInfo)
{
    Unit* attacker = damageInfo->attacker;
    Unit* target   = damageInfo->target;
    uint32 amount  = damageInfo->damages[0].damage;
    int32  overkill = static_cast<int32>(amount) - static_cast<int32>(target ? target->GetHealth() : amount);
    uint32 school   = damageInfo->damages[0].damageSchoolMask;
    uint32 resisted = damageInfo->damages[0].resist;
    uint32 blocked  = damageInfo->blocked_amount;
    uint32 absorbed = damageInfo->damages[0].absorb;
    bool   crit     = damageInfo->hitOutCome == MELEE_HIT_CRIT;
    bool   glancing = damageInfo->hitOutCome == MELEE_HIT_GLANCING;
    bool   crushing = damageInfo->hitOutCome == MELEE_HIT_CRUSHING;

    std::ostringstream ss;
    ss << Now() << "  SWING_DAMAGE,"
       << BaseParams(attacker, target);
    AppendDamageSuffix(ss, amount, overkill, school, resisted, blocked, absorbed,
                       crit, glancing, crushing);
    return ss.str();
}

// ---------------------------------------------------------------------------
// SWING_MISSED — melee auto-attack miss/dodge/parry/etc.
// ---------------------------------------------------------------------------
std::string EventFormatter::SwingMissed(CalcDamageInfo* damageInfo)
{
    std::ostringstream ss;
    ss << Now() << "  SWING_MISSED,"
       << BaseParams(damageInfo->attacker, damageInfo->target)
       << "," << MeleeHitOutcomeToMissType(damageInfo->hitOutCome)
       << "," << (damageInfo->attackType == OFF_ATTACK ? "1" : "nil")
       << ",0";
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_DAMAGE — spell direct damage with absorb/resist/block.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellDamage(SpellNonMeleeDamage* log)
{
    std::ostringstream ss;
    ss << Now() << "  SPELL_DAMAGE,"
       << BaseParams(log->attacker, log->target);
    AppendSpellPrefix(ss, log->spellInfo->Id, log->spellInfo->SpellName[0],
                      log->schoolMask);
    // SpellNonMeleeDamage doesn't carry crit/glancing/crushing flags
    AppendDamageSuffix(ss, log->damage, static_cast<int32>(log->overkill),
                       log->schoolMask, log->resist, log->blocked, log->absorb,
                       false, false, false);
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_MISSED — spell miss/dodge/parry/immune/resist/reflect etc.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellMissed(Unit* attacker, Unit* victim,
                                         uint32 spellId, SpellMissInfo missInfo)
{
    SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);

    std::ostringstream ss;
    ss << Now() << "  SPELL_MISSED,"
       << BaseParams(attacker, victim);
    AppendSpellPrefix(ss, spellId,
                      spell ? spell->SpellName[0] : "",
                      spell ? spell->SchoolMask : 0);
    ss << "," << SpellMissInfoToString(missInfo)
       << ",nil"   // isOffHand
       << ",0";    // amountMissed
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_HEAL — healing with overheal and crit.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellHeal(HealInfo const& healInfo, bool critical)
{
    Unit* healer = healInfo.GetHealer();
    Unit* target = healInfo.GetTarget();
    SpellInfo const* spell = healInfo.GetSpellInfo();
    uint32 amount      = healInfo.GetHeal();
    uint32 overhealing = amount - healInfo.GetEffectiveHeal();
    uint32 absorbed    = healInfo.GetAbsorb();

    std::ostringstream ss;
    ss << Now() << "  SPELL_HEAL,"
       << BaseParams(healer, target);
    AppendSpellPrefix(ss, spell->Id, spell->SpellName[0], spell->SchoolMask);
    ss << "," << amount
       << "," << overhealing
       << "," << absorbed
       << "," << (critical ? "1" : "nil");
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_ENERGIZE — mana/rage/energy restore.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellEnergize(Unit* caster, Unit* target,
                                           uint32 spellId, uint32 amount,
                                           Powers powerType)
{
    SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);

    std::ostringstream ss;
    ss << Now() << "  SPELL_ENERGIZE,"
       << BaseParams(caster, target);
    AppendSpellPrefix(ss, spellId,
                      spell ? spell->SpellName[0] : "",
                      spell ? spell->SchoolMask : 0);
    ss << "," << amount
       << "," << static_cast<int>(powerType);
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_PERIODIC_DAMAGE — periodic damage tick.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellPeriodicDamage(Unit* victim,
                                                 SpellPeriodicAuraLogInfo* pInfo)
{
    AuraEffect const* auraEff = pInfo->auraEff;
    Unit* caster = auraEff->GetCaster();
    SpellInfo const* spell = auraEff->GetSpellInfo();

    std::ostringstream ss;
    ss << Now() << "  SPELL_PERIODIC_DAMAGE,"
       << BaseParams(caster, victim);
    AppendSpellPrefix(ss, spell->Id, spell->SpellName[0], spell->SchoolMask);
    AppendDamageSuffix(ss, pInfo->damage, static_cast<int32>(pInfo->overDamage),
                       spell->SchoolMask, pInfo->resist, 0, pInfo->absorb,
                       pInfo->critical, false, false);
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_PERIODIC_HEAL — periodic heal tick.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellPeriodicHeal(Unit* victim,
                                               SpellPeriodicAuraLogInfo* pInfo)
{
    AuraEffect const* auraEff = pInfo->auraEff;
    Unit* caster = auraEff->GetCaster();
    SpellInfo const* spell = auraEff->GetSpellInfo();

    std::ostringstream ss;
    ss << Now() << "  SPELL_PERIODIC_HEAL,"
       << BaseParams(caster, victim);
    AppendSpellPrefix(ss, spell->Id, spell->SpellName[0], spell->SchoolMask);
    ss << "," << pInfo->damage
       << "," << pInfo->overDamage
       << "," << pInfo->absorb
       << "," << (pInfo->critical ? "1" : "nil");
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_PERIODIC_ENERGIZE — periodic mana/energy tick.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellPeriodicEnergize(Unit* victim,
                                                   SpellPeriodicAuraLogInfo* pInfo)
{
    AuraEffect const* auraEff = pInfo->auraEff;
    Unit* caster = auraEff->GetCaster();
    SpellInfo const* spell = auraEff->GetSpellInfo();

    std::ostringstream ss;
    ss << Now() << "  SPELL_PERIODIC_ENERGIZE,"
       << BaseParams(caster, victim);
    AppendSpellPrefix(ss, spell->Id, spell->SpellName[0], spell->SchoolMask);
    // pInfo->damage is the energy amount; overDamage is meaningless here
    ss << "," << pInfo->damage
       << ",0";   // powerType not available in SpellPeriodicAuraLogInfo
    return ss.str();
}

// ---------------------------------------------------------------------------
// DAMAGE_SHIELD — thorns, retribution aura, etc.
// ---------------------------------------------------------------------------
std::string EventFormatter::DamageShield(DamageInfo* damageInfo, uint32 overkill)
{
    Unit* attacker = damageInfo->GetAttacker();
    Unit* victim   = damageInfo->GetVictim();
    SpellInfo const* spell = damageInfo->GetSpellInfo();
    uint32 amount   = damageInfo->GetDamage();
    uint32 school   = static_cast<uint32>(damageInfo->GetSchoolMask());
    uint32 resisted = damageInfo->GetResist();
    uint32 blocked  = damageInfo->GetBlock();
    uint32 absorbed = damageInfo->GetAbsorb();

    std::ostringstream ss;
    ss << Now() << "  DAMAGE_SHIELD,"
       << BaseParams(attacker, victim);
    if (spell)
        AppendSpellPrefix(ss, spell->Id, spell->SpellName[0], spell->SchoolMask);
    else
        AppendSpellPrefix(ss, 0, "", school);
    AppendDamageSuffix(ss, amount, static_cast<int32>(overkill), school,
                       resisted, blocked, absorbed, false, false, false);
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_AURA_APPLIED — aura applied with caster info.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellAuraApplied(Unit* caster, Unit* target,
                                              SpellInfo const* spell)
{
    bool isBuff = spell->IsPositive();

    std::ostringstream ss;
    ss << Now() << "  SPELL_AURA_APPLIED,"
       << BaseParams(caster, target);
    AppendSpellPrefix(ss, spell->Id, spell->SpellName[0], spell->SchoolMask);
    ss << "," << (isBuff ? "BUFF" : "DEBUFF");
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_AURA_REMOVED — aura removed with caster info.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellAuraRemoved(Unit* caster, Unit* target,
                                              SpellInfo const* spell)
{
    bool isBuff = spell->IsPositive();

    std::ostringstream ss;
    ss << Now() << "  SPELL_AURA_REMOVED,"
       << BaseParams(caster, target);
    AppendSpellPrefix(ss, spell->Id, spell->SpellName[0], spell->SchoolMask);
    ss << "," << (isBuff ? "BUFF" : "DEBUFF");
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_CAST_SUCCESS — spell cast completed.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellCastSuccess(Unit* caster, SpellInfo const* spell)
{
    std::ostringstream ss;
    ss << Now() << "  SPELL_CAST_SUCCESS,"
       << BaseParams(caster, caster);
    AppendSpellPrefix(ss, spell->Id, spell->SpellName[0], spell->SchoolMask);
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_SUMMON — a unit summoned a creature or game object.
// WotLK format: SPELL_SUMMON,srcGUID,"srcName",srcFlags,dstGUID,"dstName",dstFlags,spellId,"spellName",0xSchool
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellSummon(Unit* caster, SpellInfo const* spell,
                                         WorldObject* summoned)
{
    std::ostringstream ss;
    ss << Now() << "  SPELL_SUMMON,"
       << Guid(caster ? caster->GetGUID() : ObjectGuid::Empty)
       << ",\"" << (caster ? caster->GetName() : "") << "\""
       << ",0x0"
       << "," << Guid(summoned ? summoned->GetGUID() : ObjectGuid::Empty)
       << ",\"" << (summoned ? summoned->GetName() : "") << "\""
       << ",0x0";
    AppendSpellPrefix(ss, spell->Id, spell->SpellName[0], spell->SchoolMask);
    return ss.str();
}

// ---------------------------------------------------------------------------
// UNIT_DIED — a unit has died.
// ---------------------------------------------------------------------------
std::string EventFormatter::UnitDied(Unit* killer, Unit* victim)
{
    std::ostringstream ss;
    ss << Now() << "  UNIT_DIED,"
       << BaseParams(killer, victim);
    return ss.str();
}

// ---------------------------------------------------------------------------
// ENVIRONMENTAL_DAMAGE — fall, lava, drowning, etc.
// ---------------------------------------------------------------------------
static const char* EnvTypeToString(uint8 type)
{
    switch (type)
    {
        case 0:  return "Fatigue";   // DAMAGE_EXHAUSTED
        case 1:  return "Drowning";  // DAMAGE_DROWNING
        case 2:  return "Falling";   // DAMAGE_FALL
        case 3:  return "Lava";      // DAMAGE_LAVA
        case 4:  return "Slime";     // DAMAGE_SLIME
        case 5:  return "Fire";      // DAMAGE_FIRE
        case 6:  return "Falling";   // DAMAGE_FALL_TO_VOID
        default: return "Unknown";
    }
}

static uint32 EnvSchool(uint8 type)
{
    switch (type)
    {
        case 3:  return 4;  // DAMAGE_LAVA  → Fire
        case 4:  return 8;  // DAMAGE_SLIME → Nature
        case 5:  return 4;  // DAMAGE_FIRE  → Fire
        default: return 1;  // Physical
    }
}

std::string EventFormatter::EnvironmentalDamage(Player* victim,
                                                EnviromentalDamage type,
                                                uint32 damage)
{
    uint8 envType = static_cast<uint8>(type);
    uint32 school = EnvSchool(envType);

    std::ostringstream ss;
    ss << Now() << "  ENVIRONMENTAL_DAMAGE,"
       << "0x0000000000000000,\"\",0x0"   // source (environment — no unit)
       << "," << Guid(victim->GetGUID())
       << ",\"" << victim->GetName() << "\""
       << ",0x0"
       << "," << EnvTypeToString(envType);
    AppendDamageSuffix(ss, damage, 0, school, 0, 0, 0, false, false, false);
    return ss.str();
}

// ===== CombatLogWriter =====

CombatLogWriter::CombatLogWriter(std::string const& dir, uint32 mapId, uint32 instanceId,
                                 std::string const& mapName, std::string const& realmName)
    : _instanceId(instanceId), _mapName(mapName), _realmName(realmName)
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
    _enabled      = sConfigMgr->GetOption<bool>("Chronicle.Enable", true);
    _logDir       = sConfigMgr->GetOption<std::string>("Chronicle.LogDir", "chronicle_logs");
    _uploadURL    = sConfigMgr->GetOption<std::string>("Chronicle.UploadURL", "");
    _uploadSecret = sConfigMgr->GetOption<std::string>("Chronicle.UploadSecret", "");
    LOG_INFO("module", "Chronicle: enabled={}, logDir={}, upload={}",
             _enabled, _logDir, _uploadURL.empty() ? "disabled" : _uploadURL);
}

void InstanceTracker::UploadOrphanedLogs()
{
    if (_uploadURL.empty() || _uploadSecret.empty())
    {
        LOG_INFO("module", "Chronicle: skipping orphaned-log sweep — upload not configured");
        return;
    }

    // Resolve the same log directory that GetOrCreateWriter uses.
    std::string logsDir = sConfigMgr->GetOption<std::string>("LogsDir", "");
    std::string logPath;
    if (logsDir.empty())
        logPath = _logDir;
    else
        logPath = logsDir + "/" + _logDir;

    std::error_code ec;
    if (!std::filesystem::exists(logPath, ec) || !std::filesystem::is_directory(logPath, ec))
    {
        LOG_INFO("module", "Chronicle: no log directory at {} — nothing to sweep", logPath);
        return;
    }

    uint32 count = 0;
    for (auto const& entry : std::filesystem::directory_iterator(logPath, ec))
    {
        if (ec)
            break;
        if (!entry.is_regular_file())
            continue;
        auto const& p = entry.path();
        if (p.extension() != ".log")
            continue;

        std::string filePath = p.string();
        LOG_INFO("module", "Chronicle: uploading orphaned log {}", filePath);

        // We don't have the original instanceId/mapName/realmName, so these logs will not concatenate
        // with any existing logs on the server. But it's better than losing them entirely.
        std::thread(&InstanceTracker::UploadAndDelete, filePath,
                    _uploadURL, _uploadSecret,
                    /*instanceId=*/0, /*mapName=*/std::string(""),
                    /*realmName=*/std::string("")).detach();
        ++count;
    }

    if (count > 0)
        LOG_INFO("module", "Chronicle: queued {} orphaned log file(s) for upload", count);
    else
        LOG_INFO("module", "Chronicle: no orphaned logs found in {}", logPath);
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

    std::string realmName = sWorld->GetRealmName();
    auto writer = std::make_unique<CombatLogWriter>(logPath, map->GetId(), instanceId,
                                                     map->GetMapName(), realmName);
    if (!writer->IsOpen())
        return nullptr;

    // Write CHRONICLE_HEADER and CHRONICLE_ZONE_INFO
    writer->WriteLine(EventFormatter::Header(realmName));
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
    std::string filePath;
    uint32 instId = 0;
    std::string mapName;
    std::string realmName;
    {
        std::lock_guard<std::mutex> lock(_mutex);

        auto it = _writers.find(instanceId);
        if (it != _writers.end())
        {
            it->second->Close();
            filePath  = it->second->GetPath();
            instId    = it->second->GetInstanceId();
            mapName   = it->second->GetMapName();
            realmName = it->second->GetRealmName();
            _writers.erase(it);
        }
        _seenUnits.erase(instanceId);
    }

    // Upload in background thread (doesn't block game server)
    if (!filePath.empty() && !_uploadURL.empty() && !_uploadSecret.empty())
    {
        std::thread(&InstanceTracker::UploadAndDelete, filePath,
                    _uploadURL, _uploadSecret,
                    instId, std::move(mapName), std::move(realmName)).detach();
    }
}

// ---------------------------------------------------------------------------
// Upload helpers — zlib gzip + Boost.Beast HTTP/HTTPS, no shell-out.
// ---------------------------------------------------------------------------
namespace {

struct ParsedUrl
{
    std::string scheme;  // "http" or "https"
    std::string host;
    std::string port;
    std::string target;  // path: configured root path + /azerothcore/upload
};

// Parses the configured root URL and appends the given suffix to its path.
// Trailing slashes on the configured value are stripped before appending, so
// "https://host", "https://host/", "https://host/api", and "https://host/api/"
// all produce well-formed targets.
static bool ParseUrl(std::string const& rootUrl, std::string const& pathSuffix, ParsedUrl& out)
{
    std::string rest;
    if (rootUrl.substr(0, 8) == "https://") { out.scheme = "https"; rest = rootUrl.substr(8); }
    else if (rootUrl.substr(0, 7) == "http://") { out.scheme = "http";  rest = rootUrl.substr(7); }
    else return false;

    // Split host[:port] from the path component.
    auto slash = rest.find('/');
    std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    std::string path     = (slash == std::string::npos) ? "" : rest.substr(slash);

    auto colon = hostport.find(':');
    if (colon != std::string::npos)
    {
        out.host = hostport.substr(0, colon);
        out.port = hostport.substr(colon + 1);
    }
    else
    {
        out.host = hostport;
        out.port = (out.scheme == "https") ? "443" : "80";
    }

    // Strip trailing slash(es) from the configured path, then append the suffix.
    while (!path.empty() && path.back() == '/')
        path.pop_back();
    out.target = path + pathSuffix;
    return !out.host.empty();
}

// Gzip-compress bytes from a file. Returns empty vector on error.
static std::vector<Bytef> GzipFile(std::string const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::vector<Bytef> input(std::istreambuf_iterator<char>(file), {});

    z_stream zs{};
    // windowBits=31 → gzip wrapper (15 + 16)
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED,
                     31, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return {};

    std::vector<Bytef> output(deflateBound(&zs, static_cast<uLong>(input.size())) + 64);
    zs.next_in   = input.data();
    zs.avail_in  = static_cast<uInt>(input.size());
    zs.next_out  = output.data();
    zs.avail_out = static_cast<uInt>(output.size());

    int ret = deflate(&zs, Z_FINISH);
    deflateEnd(&zs);
    if (ret != Z_STREAM_END)
        return {};

    output.resize(zs.total_out);
    return output;
}

// Build a multipart/form-data body with the compressed log as the sole part.
static std::string BuildMultipart(std::vector<Bytef> const& compressed,
                                   std::string& contentTypeOut)
{
    static constexpr char kBoundary[] = "----ChronicleUploadBoundary7x83kq";
    contentTypeOut = "multipart/form-data; boundary=";
    contentTypeOut += kBoundary;

    std::string body;
    body += "--";
    body += kBoundary;
    body += "\r\nContent-Disposition: form-data; name=\"combat_log\"; filename=\"combat_log.gz\"\r\n";
    body += "Content-Type: application/gzip\r\n\r\n";
    body.append(reinterpret_cast<char const*>(compressed.data()), compressed.size());
    body += "\r\n--";
    body += kBoundary;
    body += "--\r\n";
    return body;
}

// Send an HTTP request and return the response status code.
// Throws boost::system::system_error or std::exception on network error.
static int DoSend(ParsedUrl const& parsed,
                  boost::beast::http::request<boost::beast::http::string_body>& req)
{
    namespace beast = boost::beast;
    namespace http  = beast::http;
    namespace net   = boost::asio;
    namespace ssl   = net::ssl;
    using tcp       = net::ip::tcp;

    net::io_context ioc;
    tcp::resolver   resolver(ioc);
    auto const endpoints = resolver.resolve(parsed.host, parsed.port);

    if (parsed.scheme == "https")
    {
        ssl::context ctx(ssl::context::tlsv12_client);
        ctx.set_default_verify_paths();
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
        SSL_set_tlsext_host_name(stream.native_handle(), parsed.host.c_str());
        beast::get_lowest_layer(stream).connect(endpoints);
        stream.handshake(ssl::stream_base::client);
        http::write(stream, req);
        beast::flat_buffer buf;
        http::response<http::string_body> res;
        http::read(stream, buf, res);
        beast::error_code ec;
        stream.shutdown(ec);  // ignore graceful-shutdown errors
        return static_cast<int>(res.result_int());
    }
    else
    {
        beast::tcp_stream stream(ioc);
        stream.connect(endpoints);
        http::write(stream, req);
        beast::flat_buffer buf;
        http::response<http::string_body> res;
        http::read(stream, buf, res);
        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);
        return static_cast<int>(res.result_int());
    }
}

} // anonymous namespace

// static
void InstanceTracker::UploadAndDelete(std::string path,
                                       std::string url,
                                       std::string secret,
                                       uint32 instanceId,
                                       std::string mapName,
                                       std::string realmName)
{
    // 1. Gzip-compress the log file in memory (zlib, windowBits=31 → gzip format).
    auto compressed = GzipFile(path);
    if (compressed.empty())
    {
        LOG_ERROR("module", "Chronicle: failed to gzip {}", path);
        return;
    }

    // 2. Parse the configured root URL and append the fixed upload path.
    //    Trailing slash on the configured value is handled: both
    //    "https://host" and "https://host/" resolve to /azerothcore/upload.
    ParsedUrl parsed;
    if (!ParseUrl(url, "/api/v1/azerothcore/upload", parsed))
    {
        LOG_ERROR("module", "Chronicle: invalid upload URL: {}", url);
        return;
    }

    // 3. Build multipart/form-data body.
    std::string contentType;
    std::string body = BuildMultipart(compressed, contentType);

    // 4. Build the HTTP request.
    namespace http = boost::beast::http;
    http::request<http::string_body> req{http::verb::post, parsed.target, 11};
    req.set(http::field::host,          parsed.host);
    req.set(http::field::content_type,  contentType);
    req.set(http::field::authorization, "Bearer " + secret);
    req.set("X-Chronicle-Instance-Id",   std::to_string(instanceId));
    req.set("X-Chronicle-Instance-Name", mapName);
    req.set("X-Chronicle-Realm-Name",    realmName);
    req.body() = std::move(body);
    req.prepare_payload();

    // 5. Send and handle the response.
    try
    {
        int status = DoSend(parsed, req);
        if (status == 201)
        {
            std::filesystem::remove(path);
            LOG_INFO("module", "Chronicle: uploaded and deleted {}", path);
        }
        else
        {
            LOG_ERROR("module", "Chronicle: upload failed for {} (HTTP {}) url={}", path, status, url);
        }
    }
    catch (std::exception const& e)
    {
        LOG_ERROR("module", "Chronicle: upload exception for {}: {}", path, e.what());
    }
}

// static
void InstanceTracker::PingRemote(std::string url, std::string secret)
{
    ParsedUrl parsed;
    if (!ParseUrl(url, "/api/v1/azerothcore/ping", parsed))
    {
        LOG_ERROR("module", "Chronicle: ping failed — invalid URL: {}", url);
        return;
    }

    namespace http = boost::beast::http;
    http::request<http::string_body> req{http::verb::get, parsed.target, 11};
    req.set(http::field::host,          parsed.host);
    req.set(http::field::authorization, "Bearer " + secret);
    req.prepare_payload();

    try
    {
        int status = DoSend(parsed, req);
        if (status == 200)
        {
            LOG_INFO("module", "Chronicle: ping OK (HTTP 200) — connected to {}", url);
        }
        else
        {
            LOG_ERROR("module", "Chronicle: ping failed (HTTP {}) — check UploadURL/UploadSecret", status);
        }
    }
    catch (std::exception const& e)
    {
        LOG_ERROR("module", "Chronicle: ping exception — {}", e.what());
    }
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
