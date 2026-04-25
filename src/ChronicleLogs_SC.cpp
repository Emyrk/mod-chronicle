/*
 * Copyright (C) 2024+ Chronicle <https://github.com/Emyrk/chronicle>
 * Released under GNU AGPL v3 license
 *
 * AzerothCore hook scripts that capture combat events and feed them
 * to Chronicle module classes for writing as WotLK-format combat log lines.
 */

#include "Chronicle.h"

#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "Unit.h"

// ===========================================================================
// UnitScript — captures combat events at the "send to client" point,
// providing complete damage/heal/miss data including absorb, resist,
// block, crit, overkill, overheal, etc.
// ===========================================================================
class ChronicleUnitScript : public UnitScript
{
public:
    ChronicleUnitScript() : UnitScript("ChronicleUnitScript", true, {
        UNITHOOK_ON_SEND_ATTACK_STATE_UPDATE,
        UNITHOOK_ON_SEND_SPELL_NON_MELEE_DAMAGE_LOG,
        UNITHOOK_ON_SEND_HEAL_SPELL_LOG,
        UNITHOOK_ON_SEND_SPELL_MISS,
        UNITHOOK_ON_SEND_SPELL_DAMAGE_IMMUNE,
        UNITHOOK_ON_SEND_SPELL_DAMAGE_RESIST,
        UNITHOOK_ON_SEND_SPELL_NON_MELEE_REFLECT_LOG,
        UNITHOOK_ON_SEND_ENERGIZE_SPELL_LOG,
        UNITHOOK_ON_SEND_PERIODIC_AURA_LOG,
        UNITHOOK_ON_DEAL_DAMAGE_SHIELD_DAMAGE,
        UNITHOOK_ON_UNIT_DEATH,
        UNITHOOK_ON_UNIT_ENTER_EVADE_MODE,
        UNITHOOK_ON_UNIT_ENTER_COMBAT,
    }) { }

    // -----------------------------------------------------------------------
    // OnSendAttackStateUpdate — melee hit/miss → SWING_DAMAGE or SWING_MISSED
    // -----------------------------------------------------------------------
    void OnSendAttackStateUpdate(CalcDamageInfo* damageInfo, int32 /*overkill*/) override
    {
        if (!damageInfo || !damageInfo->attacker || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(damageInfo->attacker);
        InstanceTracker::Instance().EnsureUnitInfo(damageInfo->target);

        if (damageInfo->hitOutCome == MELEE_HIT_MISS ||
            damageInfo->hitOutCome == MELEE_HIT_DODGE ||
            damageInfo->hitOutCome == MELEE_HIT_PARRY ||
            damageInfo->hitOutCome == MELEE_HIT_EVADE)
        {
            InstanceTracker::Instance().WriteForUnit(
                damageInfo->target, EventFormatter::SwingMissed(damageInfo));
        }
        else
        {
            InstanceTracker::Instance().WriteForUnit(
                damageInfo->target, EventFormatter::SwingDamage(damageInfo));
        }
    }

    // -----------------------------------------------------------------------
    // OnSendSpellNonMeleeDamageLog — spell damage → SPELL_DAMAGE
    // -----------------------------------------------------------------------
    void OnSendSpellNonMeleeDamageLog(SpellNonMeleeDamage* log) override
    {
        if (!log || !log->attacker || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(log->attacker);
        InstanceTracker::Instance().EnsureUnitInfo(log->target);

        InstanceTracker::Instance().WriteForUnit(
            log->target, EventFormatter::SpellDamage(log));
    }

    // -----------------------------------------------------------------------
    // OnSendHealSpellLog — healing → SPELL_HEAL
    // -----------------------------------------------------------------------
    void OnSendHealSpellLog(HealInfo const& healInfo, bool critical) override
    {
        if (!healInfo.GetHealer() || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(healInfo.GetHealer());
        InstanceTracker::Instance().EnsureUnitInfo(healInfo.GetTarget());

        InstanceTracker::Instance().WriteForUnit(
            healInfo.GetTarget(), EventFormatter::SpellHeal(healInfo, critical));
    }

    // -----------------------------------------------------------------------
    // OnSendSpellMiss — spell miss → SPELL_MISSED
    // -----------------------------------------------------------------------
    void OnSendSpellMiss(Unit* attacker, Unit* victim, uint32 spellID,
                         SpellMissInfo missInfo) override
    {
        if (!attacker || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(attacker);
        InstanceTracker::Instance().EnsureUnitInfo(victim);

        InstanceTracker::Instance().WriteForUnit(
            victim, EventFormatter::SpellMissed(attacker, victim, spellID, missInfo));
    }

    // -----------------------------------------------------------------------
    // OnSendSpellDamageImmune — spell immune → SPELL_MISSED (IMMUNE)
    // -----------------------------------------------------------------------
    void OnSendSpellDamageImmune(Unit* attacker, Unit* victim, uint32 spellId) override
    {
        if (!attacker || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(attacker);
        InstanceTracker::Instance().EnsureUnitInfo(victim);

        InstanceTracker::Instance().WriteForUnit(
            victim, EventFormatter::SpellMissed(attacker, victim, spellId, SPELL_MISS_IMMUNE));
    }

    // -----------------------------------------------------------------------
    // OnSendSpellDamageResist — full resist → SPELL_MISSED (RESIST)
    // -----------------------------------------------------------------------
    void OnSendSpellDamageResist(Unit* attacker, Unit* victim, uint32 spellId) override
    {
        if (!attacker || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(attacker);
        InstanceTracker::Instance().EnsureUnitInfo(victim);

        InstanceTracker::Instance().WriteForUnit(
            victim, EventFormatter::SpellMissed(attacker, victim, spellId, SPELL_MISS_RESIST));
    }

    // -----------------------------------------------------------------------
    // OnSendSpellNonMeleeReflectLog — reflected spell → SPELL_DAMAGE
    // -----------------------------------------------------------------------
    void OnSendSpellNonMeleeReflectLog(SpellNonMeleeDamage* log,
                                        Unit* /*attacker*/) override
    {
        if (!log || !log->attacker || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(log->attacker);
        InstanceTracker::Instance().EnsureUnitInfo(log->target);

        InstanceTracker::Instance().WriteForUnit(
            log->target, EventFormatter::SpellDamage(log));
    }

    // -----------------------------------------------------------------------
    // OnSendEnergizeSpellLog — mana/rage/energy → SPELL_ENERGIZE
    // -----------------------------------------------------------------------
    void OnSendEnergizeSpellLog(Unit* attacker, Unit* victim, uint32 spellID,
                                 uint32 damage, Powers powerType) override
    {
        if (!attacker || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(attacker);
        InstanceTracker::Instance().EnsureUnitInfo(victim);

        InstanceTracker::Instance().WriteForUnit(
            victim, EventFormatter::SpellEnergize(attacker, victim, spellID,
                                                   damage, powerType));
    }

    // -----------------------------------------------------------------------
    // OnSendPeriodicAuraLog — periodic tick dispatcher
    // -----------------------------------------------------------------------
    void OnSendPeriodicAuraLog(Unit* victim, SpellPeriodicAuraLogInfo* pInfo) override
    {
        if (!victim || !pInfo || !pInfo->auraEff || !InstanceTracker::Instance().IsEnabled())
            return;

        Unit* caster = pInfo->auraEff->GetCaster();
        InstanceTracker::Instance().EnsureUnitInfo(caster);
        InstanceTracker::Instance().EnsureUnitInfo(victim);

        switch (pInfo->auraEff->GetAuraType())
        {
            case SPELL_AURA_PERIODIC_DAMAGE:
            case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
            case SPELL_AURA_PERIODIC_LEECH:
                InstanceTracker::Instance().WriteForUnit(
                    victim, EventFormatter::SpellPeriodicDamage(victim, pInfo));
                break;
            case SPELL_AURA_PERIODIC_HEAL:
            case SPELL_AURA_OBS_MOD_HEALTH:
                InstanceTracker::Instance().WriteForUnit(
                    victim, EventFormatter::SpellPeriodicHeal(victim, pInfo));
                break;
            case SPELL_AURA_PERIODIC_ENERGIZE:
            case SPELL_AURA_OBS_MOD_POWER:
                InstanceTracker::Instance().WriteForUnit(
                    victim, EventFormatter::SpellPeriodicEnergize(victim, pInfo));
                break;
            default:
                break;
        }
    }

    // -----------------------------------------------------------------------
    // OnDealDamageShieldDamage — damage shield (thorns etc.) → DAMAGE_SHIELD
    // -----------------------------------------------------------------------
    void OnDealDamageShieldDamage(DamageInfo* damageInfo,
        uint32 overkill, uint32 /*spellId*/) override
    {
        if (!damageInfo || !InstanceTracker::Instance().IsEnabled())
            return;

        Unit* attacker = damageInfo->GetAttacker();
        Unit* victim   = damageInfo->GetVictim();

        InstanceTracker::Instance().EnsureUnitInfo(attacker);
        InstanceTracker::Instance().EnsureUnitInfo(victim);

        InstanceTracker::Instance().WriteForUnit(
            victim, EventFormatter::DamageShield(damageInfo, overkill));
    }

    // -----------------------------------------------------------------------
    // OnUnitDeath — unit died → UNIT_DIED
    // -----------------------------------------------------------------------
    void OnUnitDeath(Unit* unit, Unit* killer) override
    {
        if (!unit || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(unit);
        InstanceTracker::Instance().EnsureUnitInfo(killer);

        InstanceTracker::Instance().WriteForUnit(
            unit, EventFormatter::UnitDied(killer, unit));
    }

    // -----------------------------------------------------------------------
    // OnUnitEnterEvadeMode — creature resets → CHRONICLE_UNIT_EVADE
    // -----------------------------------------------------------------------
    void OnUnitEnterEvadeMode(Unit* unit, uint8 evadeReason) override
    {
        if (!unit || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(unit);

        InstanceTracker::Instance().WriteForUnit(
            unit, EventFormatter::UnitEvade(unit, evadeReason));
    }

    // -----------------------------------------------------------------------
    // OnUnitEnterCombat — unit enters combat → CHRONICLE_UNIT_COMBAT
    // -----------------------------------------------------------------------
    void OnUnitEnterCombat(Unit* unit, Unit* victim) override
    {
        if (!unit || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(unit);
        InstanceTracker::Instance().EnsureUnitInfo(victim);

        InstanceTracker::Instance().WriteForUnit(
            unit, EventFormatter::UnitCombat(unit, victim));
    }
};

// ===========================================================================
// GlobalScript — captures spell casts and aura application/removal.
// ===========================================================================
class ChronicleGlobalScript : public GlobalScript
{
public:
    ChronicleGlobalScript() : GlobalScript("ChronicleGlobalScript", {
        GLOBALHOOK_ON_SPELL_SEND_SPELL_GO,
        GLOBALHOOK_ON_AURA_APPLICATION_CLIENT_UPDATE,
        GLOBALHOOK_ON_SPELL_EXECUTE_LOG_SUMMON_OBJECT,
    }) { }

    // -----------------------------------------------------------------------
    // OnSpellSendSpellGo — spell cast completed → SPELL_CAST_SUCCESS
    // -----------------------------------------------------------------------
    void OnSpellSendSpellGo(Spell* spell) override
    {
        if (!spell || !spell->GetCaster() || !InstanceTracker::Instance().IsEnabled())
            return;

        Unit* caster = spell->GetCaster();
        SpellInfo const* spellInfo = spell->m_spellInfo;
        if (!spellInfo)
            return;

        InstanceTracker::Instance().EnsureUnitInfo(caster);

        InstanceTracker::Instance().WriteForUnit(
            caster, EventFormatter::SpellCastSuccess(caster, spellInfo));
    }

    // -----------------------------------------------------------------------
    // OnSpellExecuteLogSummonObject — creature/object summoned → SPELL_SUMMON
    // -----------------------------------------------------------------------
    void OnSpellExecuteLogSummonObject(Spell* spell, WorldObject* obj) override
    {
        if (!spell || !spell->GetCaster() || !obj || !InstanceTracker::Instance().IsEnabled())
            return;

        Unit* caster = spell->GetCaster();
        SpellInfo const* spellInfo = spell->m_spellInfo;
        if (!spellInfo)
            return;

        InstanceTracker::Instance().EnsureUnitInfo(caster);

        InstanceTracker::Instance().WriteForUnit(
            caster, EventFormatter::SpellSummon(caster, spellInfo, obj));
    }

    // -----------------------------------------------------------------------
    // OnAuraApplicationClientUpdate — aura applied/removed
    // -----------------------------------------------------------------------
    void OnAuraApplicationClientUpdate(Unit* target, Aura* aura, bool remove) override
    {
        if (!target || !aura || !InstanceTracker::Instance().IsEnabled())
            return;

        Unit* caster = aura->GetCaster();
        SpellInfo const* spell = aura->GetSpellInfo();
        if (!spell)
            return;

        InstanceTracker::Instance().EnsureUnitInfo(caster);
        InstanceTracker::Instance().EnsureUnitInfo(target);

        if (remove)
        {
            InstanceTracker::Instance().WriteForUnit(
                target, EventFormatter::SpellAuraRemoved(caster, target, spell));
        }
        else
        {
            InstanceTracker::Instance().WriteForUnit(
                target, EventFormatter::SpellAuraApplied(caster, target, spell));
        }
    }
};

// ===========================================================================
// PlayerScript — captures environmental damage (fall, lava, etc.).
// ===========================================================================
class ChroniclePlayerScript : public PlayerScript
{
public:
    ChroniclePlayerScript() : PlayerScript("ChroniclePlayerScript", {
        PLAYERHOOK_ON_ENVIRONMENTAL_DAMAGE,
    }) { }

    void OnEnvironmentalDamage(Player* player, EnviromentalDamage type,
                                uint32 damage) override
    {
        if (!player || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(player);

        InstanceTracker::Instance().WriteForUnit(
            player, EventFormatter::EnvironmentalDamage(player, type, damage));
    }
};

// ===========================================================================
// AllMapScript — captures player entering/leaving instances.
// ===========================================================================
class ChronicleAllMapScript : public AllMapScript
{
public:
    ChronicleAllMapScript() : AllMapScript("ChronicleAllMapScript", {
        ALLMAPHOOK_ON_PLAYER_ENTER_ALL,
        ALLMAPHOOK_ON_PLAYER_LEAVE_ALL,
        ALLMAPHOOK_ON_DESTROY_INSTANCE,
    }) { }

    void OnPlayerEnterAll(Map* map, Player* player) override
    {
        if (!map || !player || !map->IsDungeon())
            return;

        InstanceTracker::Instance().OnPlayerEnterInstance(map, player);
    }

    void OnPlayerLeaveAll(Map* map, Player* player) override
    {
        if (!map || !player || !map->IsDungeon())
            return;

        InstanceTracker::Instance().OnPlayerLeaveInstance(map, player);
    }

    void OnDestroyInstance(MapInstanced* /*mapInstanced*/, Map* map) override
    {
        if (!map)
            return;

        InstanceTracker::Instance().RemoveInstance(map->GetInstanceId());
    }
};

// ===========================================================================
// WorldScript — loads config on startup / reload.
// ===========================================================================
class ChronicleWorldScript : public WorldScript
{
public:
    ChronicleWorldScript() : WorldScript("ChronicleWorldScript", {
        WORLDHOOK_ON_BEFORE_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP,
    }) { }

    void OnBeforeConfigLoad(bool /*reload*/) override
    {
        InstanceTracker::Instance().LoadConfig();
    }

    void OnStartup() override
    {
        auto& tracker = InstanceTracker::Instance();
        if (!tracker.IsEnabled())
            return;

        std::string url    = tracker.GetUploadURL();
        std::string secret = tracker.GetUploadSecret();
        if (url.empty() || secret.empty())
        {
            LOG_WARN("module", "Chronicle: skipping startup ping — UploadURL or UploadSecret not configured");
            return;
        }

        // Upload any orphaned logs left over from a crash / unclean shutdown.
        tracker.UploadOrphanedLogs();

        // Run ping in a detached thread to avoid blocking server startup.
        std::thread(InstanceTracker::PingRemote, url, secret).detach();
    }
};

// ===========================================================================
// Script registration — called from MP_loader.cpp
// ===========================================================================
void AddChronicleScripts()
{
    new ChronicleUnitScript();
    new ChronicleGlobalScript();
    new ChroniclePlayerScript();
    new ChronicleAllMapScript();
    new ChronicleWorldScript();
}
