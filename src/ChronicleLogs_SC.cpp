/*
 * Copyright (C) 2024+ Chronicle <https://github.com/Emyrk/chronicle>
 * Released under GNU AGPL v3 license
 *
 * AzerothCore hook scripts that capture combat events and feed them
 * to Chronicle module classes for writing as WotLK-format combat log lines.
 */

#include "Chronicle.h"

#include "Map.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellAuras.h"
#include "Unit.h"

// ===========================================================================
// UnitScript — captures damage, healing, deaths, and aura events.
//
// We use specific hooks that provide SpellInfo where possible:
//   - OnDamage:               melee auto-attack → SWING_DAMAGE
//   - ModifySpellDamageTaken: spell damage       → SPELL_DAMAGE
//   - ModifyHealReceived:     healing            → SPELL_HEAL
//   - OnAuraApply/Remove:     SPELL_AURA_APPLIED / SPELL_AURA_REMOVED
//   - OnUnitDeath:            UNIT_DIED
// ===========================================================================
class ChronicleUnitScript : public UnitScript
{
public:
    ChronicleUnitScript() : UnitScript("ChronicleUnitScript", true, {
        UNITHOOK_ON_DAMAGE,
        UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
        UNITHOOK_MODIFY_HEAL_RECEIVED,
        UNITHOOK_ON_AURA_APPLY,
        UNITHOOK_ON_AURA_REMOVE,
        UNITHOOK_ON_UNIT_DEATH,
        UNITHOOK_ON_UNIT_ENTER_EVADE_MODE,
        UNITHOOK_ON_UNIT_ENTER_COMBAT,
    }) { }

    // -----------------------------------------------------------------------
    // OnDamage — fires for ALL damage (melee + spell).
    // We use this ONLY for melee auto-attacks (SWING_DAMAGE).
    // Spell damage is captured more accurately via ModifySpellDamageTaken.
    //
    // TODO: In Phase 2, use a thread-local flag set in ModifySpellDamageTaken
    // to suppress the duplicate SWING_DAMAGE for spell damage events.
    // -----------------------------------------------------------------------
    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!victim || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(attacker);
        InstanceTracker::Instance().EnsureUnitInfo(victim);

        InstanceTracker::Instance().WriteForUnit(
            victim, EventFormatter::SwingDamage(attacker, victim, damage));
    }

    // -----------------------------------------------------------------------
    // ModifySpellDamageTaken — fires for spell damage with full SpellInfo.
    // -----------------------------------------------------------------------
    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage,
                                SpellInfo const* spellInfo) override
    {
        if (!target || !spellInfo || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(attacker);
        InstanceTracker::Instance().EnsureUnitInfo(target);

        InstanceTracker::Instance().WriteForUnit(
            target, EventFormatter::SpellDamage(attacker, target, spellInfo, damage));
    }

    // -----------------------------------------------------------------------
    // ModifyHealReceived — fires for heals with full SpellInfo.
    // -----------------------------------------------------------------------
    void ModifyHealReceived(Unit* target, Unit* healer, uint32& heal,
                            SpellInfo const* spellInfo) override
    {
        if (!target || !spellInfo || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(healer);
        InstanceTracker::Instance().EnsureUnitInfo(target);

        InstanceTracker::Instance().WriteForUnit(
            target, EventFormatter::SpellHeal(healer, target, spellInfo, heal));
    }

    // -----------------------------------------------------------------------
    // OnAuraApply — aura applied → SPELL_AURA_APPLIED
    // -----------------------------------------------------------------------
    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (!unit || !aura || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().WriteForUnit(
            unit, EventFormatter::SpellAuraApplied(unit, aura));
    }

    // -----------------------------------------------------------------------
    // OnAuraRemove — aura removed → SPELL_AURA_REMOVED
    // -----------------------------------------------------------------------
    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode /*mode*/) override
    {
        if (!unit || !aurApp || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().WriteForUnit(
            unit, EventFormatter::SpellAuraRemoved(unit, aurApp));
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
// AllSpellScript — captures spell casts (SPELL_CAST_SUCCESS).
// ===========================================================================
class ChronicleAllSpellScript : public AllSpellScript
{
public:
    ChronicleAllSpellScript() : AllSpellScript("ChronicleAllSpellScript", {
        ALLSPELLHOOK_ON_CAST,
    }) { }

    void OnSpellCast(Spell* /*spell*/, Unit* caster, SpellInfo const* spellInfo,
                     bool /*skipCheck*/) override
    {
        if (!caster || !spellInfo || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(caster);

        InstanceTracker::Instance().WriteForUnit(
            caster, EventFormatter::SpellCastSuccess(caster, spellInfo));
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
    }) { }

    void OnBeforeConfigLoad(bool /*reload*/) override
    {
        InstanceTracker::Instance().LoadConfig();
    }
};

// ===========================================================================
// Script registration — called from MP_loader.cpp
// ===========================================================================
void AddChronicleScripts()
{
    new ChronicleUnitScript();
    new ChronicleAllSpellScript();
    new ChronicleAllMapScript();
    new ChronicleWorldScript();
}
