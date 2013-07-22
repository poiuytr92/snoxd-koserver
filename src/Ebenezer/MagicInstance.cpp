﻿#include "stdafx.h"
#include "Map.h"
#include "../shared/KOSocketMgr.h"
#include "MagicProcess.h"
#include "MagicInstance.h"

using std::string;
using std::vector;

void MagicInstance::Run()
{
	SkillUseResult result;
	if (pSkill == nullptr)
		pSkill = g_pMain->m_MagictableArray.GetData(nSkillID);

	if (pSkillCaster == nullptr)
		pSkillCaster = g_pMain->GetUnitPtr(sCasterID);

	if (sTargetID != -1 && pSkillTarget == nullptr)
		pSkillTarget = g_pMain->GetUnitPtr(sTargetID);

	if (pSkill == nullptr
		|| pSkillCaster == nullptr
		|| (result = UserCanCast()) == SkillUseFail)
	{
		SendSkillFailed();
		return;
	}

	// If the skill's already been handled (e.g. death taunts), 
	// we don't need to do anything further.
	if (result == SkillUseHandled)
		return;
	
	bool bInitialResult;
	switch (bOpcode)
	{
		case MAGIC_CASTING:
		case MAGIC_FAIL:
			SendSkill(bOpcode == MAGIC_CASTING); // only send casting packets to the region, not fail packets.
			break;

		case MAGIC_FLYING:
		{
			// Handle arrow & mana checking/removals.
			if (pSkillCaster->isPlayer())
			{
				CUser * pCaster = TO_USER(pSkillCaster);
				_MAGIC_TYPE2 * pType = g_pMain->m_Magictype2Array.GetData(nSkillID);

				// NOTE: Not all skills that use MAGIC_FLYING are type 2 skills.
				// Some type 3 skills use it (such as "Vampiric Fire"). 
				// For these we should really apply the same flying logic, but for now we'll just ignore it.
				if (pType != nullptr)
				{
					// Throwing knives are differentiated by the fact "NeedArrow" is set to 0.
					// We still need to check for & take 1 throwing knife in this case however.
					uint8 bCount = pType->bNeedArrow;
					if (!bCount)
						bCount = 1;

					if (pType == nullptr
						// The user does not have enough arrows! We should point them in the right direction. ;)
						|| (!pCaster->CheckExistItem(pSkill->iUseItem, bCount))
						// Ensure user has enough mana for this skill
						|| pSkill->sMsp > pSkillCaster->GetMana())
					{
						SendSkillFailed();
						return;
					}

					// Add all flying arrow instances to the user's list for hit detection
					FastGuard lock(pCaster->m_arrowLock);
					for (size_t i = 0; i < bCount; i++)
						pCaster->m_flyingArrows.push_back(Arrow(pType->iNum, UNIXTIME));

					// Remove the arrows
					pCaster->RobItem(pSkill->iUseItem, bCount);
				}
				// for non-type 2 skills, ensure we check the user's mana.
				else if (pSkill->sMsp > pSkillCaster->GetMana())
				{
					SendSkillFailed();
					return;
				}

				// Take the required mana for this skill
				pCaster->MSpChange(-(pSkill->sMsp));
			}

			SendSkill(true); // send this to the region
		} break;

		case MAGIC_EFFECTING:
			// Hacky check for a transformation item (Disguise Totem, Disguise Scroll)
			// These apply when first type's set to 0, second type's set and obviously, there's a consumable item.
			// Need to find a better way of handling this.
			if (!bIsRecastingSavedMagic
				&& (pSkill->bType[0] == 0 && pSkill->bType[1] != 0 && pSkill->iUseItem != 0
				&& (pSkillCaster->isPlayer() && TO_USER(pSkillCaster)->CheckExistItem(pSkill->iUseItem))))
			{
				SendTransformationList();
				return;
			}

			bInitialResult = ExecuteSkill(pSkill->bType[0]);
			if (bInitialResult)
			{
				ExecuteSkill(pSkill->bType[1]);

				if (pSkill->bType[0] != 2)
					ConsumeItem();
			}
			break;

		case MAGIC_CANCEL:
		case MAGIC_CANCEL2:
			Type3Cancel();	//Damage over Time skills.
			Type4Cancel();	//Buffs
			Type6Cancel();	//Transformations
			Type9Cancel();	//Stealth, lupine etc.
			break;

		case MAGIC_TYPE4_EXTEND:
			Type4Extend();
			break;
	}
}

SkillUseResult MagicInstance::UserCanCast()
{
	if (pSkill == nullptr)
		return SkillUseFail;
	
	// We don't need to check anything as we're just canceling our buffs.
	if (bOpcode == MAGIC_CANCEL || bOpcode == MAGIC_CANCEL2
		// Also don't need to check anything if we're forwarding a fail packet.
		|| bOpcode == MAGIC_FAIL
		// Or are reapplying persistent buffs.
		|| bIsRecastingSavedMagic)
		return SkillUseOK;

	// Ensure the caster can use skills (i.e. they're not incapacitated, or have skills blocked, etc).
	// Additionally, unless it's resurrection-related, dead players cannot use skills.
	if (!pSkillCaster->canUseSkills()
		|| (pSkillCaster->isDead() && pSkill->bType[0] != 5)) 
		return SkillUseFail;

	// If we're using an AOE, and the target is specified... something's not right.
	if ((pSkill->bMoral >= MORAL_AREA_ENEMY
			&& pSkill->bMoral <= MORAL_SELF_AREA)
		&& sTargetID != -1)
	{
		// Items that proc skills require the target ID to be fixed up.
		// There's no other means of detecting who to target.
		if (!bIsItemProc)
			return SkillUseFail;

		sTargetID = -1;
	}

	// NPCs do not need most of these checks.
	if (pSkillCaster->isPlayer())
	{
		if (pSkill->sSkill != 0
			&& (TO_USER(pSkillCaster)->m_sClass != (pSkill->sSkill / 10)
			|| pSkillCaster->GetLevel() < pSkill->sSkillLevel))
			return SkillUseFail;

		if ((pSkillCaster->GetMana() - pSkill->sMsp) < 0)
			return SkillUseFail;

		// If we're in a snow war, we're only ever allowed to use the snowball skill.
		if (pSkillCaster->GetZoneID() == ZONE_SNOW_BATTLE && g_pMain->m_byBattleOpen == SNOW_BATTLE 
			&& nSkillID != SNOW_EVENT_SKILL)
			return SkillUseFail;

		// Handle death taunts (i.e. pressing the spacebar on a corpse).
		// NOTE: These skills don't really have any other generic means of identification.
		if (pSkillTarget != nullptr
			&& pSkill->bMoral == MORAL_ENEMY
			&& pSkill->bType[0] == 3 
			&& pSkill->bType[1] == 0
			// Target player must be a corpse.
			&& pSkillTarget->isDead())
		{
			_MAGIC_TYPE3 * pType3 = g_pMain->m_Magictype3Array.GetData(pSkill->iNum);
			if (pType3 == nullptr)
				return SkillUseFail;

			// Skill mustn't do any damage or such.
			if (pType3->bDirectType == 0
				&& pType3->sFirstDamage == 0
				&& pType3->sTimeDamage == 0)
			{
				// We also need to tweak the packet being sent.
				bOpcode = MAGIC_EFFECTING;
				sData[1] = 1;
				SendSkill();
				return SkillUseHandled;
			}
		}

		// Archer & transformation skills will handle item checking themselves
		if ((pSkill->bType[0] != 2 && pSkill->bType[0] != 6) 
			// The user does not meet the item's requirements or does not have any of said item.
			&& (pSkill->iUseItem != 0
				&& !TO_USER(pSkillCaster)->CanUseItem(pSkill->iUseItem))) 
			return SkillUseFail;

		// Some skills also require class-specific stones which are taken instead of UseItem.
		// In this case, UseItem is considered a required item and not consumed on skill use.
		if (pSkill->nBeforeAction >= ClassWarrior && pSkill->nBeforeAction <= ClassPriest)
			nConsumeItem = CLASS_STONE_BASE_ID + (pSkill->nBeforeAction * 1000);
		else
			nConsumeItem = pSkill->iUseItem;

		if ((pSkill->bType[0] != 2 && pSkill->bType[0] != 6) 
			// The user does not meet the item's requirements or does not have any of said item.
			&& (pSkill->iUseItem != 0
				&& !TO_USER(pSkillCaster)->CanUseItem(nConsumeItem))) 
			return SkillUseFail;

		// We cannot use CSW transformations outside of Delos (or when CSW is not enabled.)
		if (pSkill->bType[0] == 6
			&& (nSkillID / 10000) == 45
			&& pSkillCaster->GetZoneID() != ZONE_DELOS)
			return SkillUseFail;

	#if !defined(DEBUG)
		// For skills that are unlocked via quests, ensure the user has actually 
		// completed the quest...
		// NOTE: GMs are excluded.
		if (!pSkillCaster->isGM()
			&& pSkill->sEtc != 0
			&& !TO_USER(pSkillCaster)->CheckExistEvent(pSkill->sEtc, 2))
			return SkillUseFail;
	#endif

		if (pSkill->bType[0] < 4
			&& pSkillTarget != nullptr
			&& !pSkillCaster->isInAttackRange(pSkillTarget, pSkill))
			return SkillUseFail;
	}

	if ((bOpcode == MAGIC_EFFECTING || bOpcode == MAGIC_CASTING) 
		&& !IsAvailable())
		return SkillUseFail;

	// Instant casting affects the next cast skill only, and is then removed.
	if (bOpcode == MAGIC_EFFECTING && pSkillCaster->canInstantCast())
		bInstantCast = true;

	// In case we made it to here, we can cast! Hurray!
	return SkillUseOK;
}

/**
 * @brief	Checks primary type 3 (DOT/HOT) skill prerequisites before executing the skill.
 *
 * @return	true if it succeeds, false if it fails.
 */
bool MagicInstance::CheckType3Prerequisites()
{
	_MAGIC_TYPE3 * pType = g_pMain->m_Magictype3Array.GetData(nSkillID);
	if (pType == nullptr)
		return false;

	// Handle AOE prerequisites
	if (sTargetID == -1)
	{
		// No need to handle any prerequisite logic for NPCs/mobs casting AOEs.
		if (!pSkillCaster->isPlayer())
			return true;

		if (pSkill->bMoral == MORAL_PARTY_ALL
			&& pType->sTimeDamage > 0)
		{
			// Players may not cast group healing spells whilst transformed
			// into a monster (skills with IDs of 45###). 
			if (TO_USER(pSkillCaster)->isMonsterTransformation())
				return false;

			// Official behaviour means we cannot cast a group healing spell
			// if we currently have an active restoration spells on us.
			// This behaviour seems fairly illogical, but it's how it works.
			for (int i = 0; i < MAX_TYPE3_REPEAT; i++)
			{
				if (pSkillCaster->m_durationalSkills[i].m_sHPAmount > 0)
					return false;
			}
		}

		// No other reason to reject AOE spells.
		return true;
	}
	// Handle prerequisites for skills cast on NPCs.
	else if (sTargetID >= NPC_BAND)
	{
		if (pSkillTarget == nullptr)
			return false;
		
		// Unless the zone is Delos, or it's a healing skill, we can continue on our merry way.
		if (pSkillCaster->GetZoneID() != 30
			|| (pType->sFirstDamage <= 0 && pType->sTimeDamage <= 0))
			return true;

		// We cannot heal gates! That would be bad, very bad.
		if (TO_NPC(pSkillTarget)->GetType() == NPC_GATE) // note: official only checks byType 50
			return false;

		// Otherwise, officially there's no reason we can't heal NPCs (more specific logic later).
		return true;
	}
	// Handle prerequisites for skills cast on players.
	else
	{
		// We only care about friendly non-AOE spells.
		if (pSkill->bMoral > MORAL_PARTY)
			return true;

		if (pSkillTarget == nullptr 
			|| !pSkillTarget->isPlayer()
			|| pSkillTarget->isDead())
			return false;

		// If the spell is a healing/restoration spell...
		if (pType->sTimeDamage > 0)
		{
			// Official behaviour means we cannot cast a restoration spell
			// if the target currently has an active restoration spell on them.
			for (int i = 0; i < MAX_TYPE3_REPEAT; i++)
			{
				if (pSkillTarget->m_durationalSkills[i].m_sHPAmount > 0)
					return false;
			}
		}

		// It appears that the server should reject any attacks or heals
		// on players that have transformed into monsters.
		if (TO_USER(pSkillTarget)->isMonsterTransformation()
			&& !pSkillCaster->CanAttack(pSkillTarget))
			return false;

		return true;
	}
}

/**
 * @brief	Checks primary type 4 (buff/debuff) skill prerequisites before executing the skill.
 *
 * @return	true if it succeeds, false if it fails.
 */
bool MagicInstance::CheckType4Prerequisites()
{
	_MAGIC_TYPE4 * pType = g_pMain->m_Magictype4Array.GetData(nSkillID);

	// Certain transformation (type 6) skills state they have an associated
	// type 4 skill but do not have any entry in the table. Consider these OK.
	if (pType == nullptr)
		return (pSkill->bType[0] == 6);

	if (sTargetID < 0 || sTargetID >= MAX_USER)
	{
		if (sTargetID < NPC_BAND // i.e. it's an AOE
			|| pType->bBuffType != BUFF_TYPE_HP_MP // doesn't matter who we heal.
			|| pType->sMaxHPPct != 99) // e.g. skills like Dispel Magic / Sweet Kiss
			return true;

		return false;
	}

	// At this point, it can only be a user -- so ensure the ID was invalid (this was looked up earlier).
	if (pSkillTarget == nullptr
		|| !pSkillTarget->isPlayer())
		return false;

	if (TO_USER(pSkillTarget)->isTransformed())
	{
		// Can't use buff scrolls/pots when transformed into anything but NPCs.
		if (!TO_USER(pSkillTarget)->isNPCTransformation() 
			&& (nSkillID >= 500000
			// Can't use bezoars etc when transformed 
			// (this should really be a whitelist, but this is blocked explicitly in 1.298)
			|| pType->bBuffType == BUFF_TYPE_SIZE))
		{
			SendSkillFailed();
			return false;
		}
	}

	// If the specified target already has this buff (debuffs are required to reset)
	// we should fail this skill. 
	// NOTE: AOE buffs are exempt.
	if (pType->isBuff())
	{
		FastGuard lock(pSkillTarget->m_buffLock);
		if (pSkillTarget->m_buffMap.find(pType->bBuffType) 
			!= pSkillTarget->m_buffMap.end())
			return false;
	}

	// TO-DO: Allow for siege-weapon only buffs (e.g. "Physical Attack Scroll")

	return true;
}

bool MagicInstance::CheckType6Prerequisites()
{
	if (!pSkillCaster->isPlayer())
		return true;

	_MAGIC_TYPE6 * pType = g_pMain->m_Magictype6Array.GetData(nSkillID);
	if (pType == nullptr)
		return false;

	CUser * pCaster = TO_USER(pSkillCaster);
	switch (pType->bUserSkillUse)
	{
	// For monster transformations (TransformationSkillUseMonster), nBeforeAction is set to the item 
	// used for showing the transformation list & UseItem is the consumable item.
	case TransformationSkillUseMonster:
		// Ensure they have the item for showing the transformation list
		if (!pCaster->CanUseItem(pSkill->nBeforeAction)
			// Ensure they have the required item for the skill.
			|| !pCaster->CanUseItem(pSkill->iUseItem))
			return false;

		break;

	// For all other transformations, all we care about is UseItem (BeforeAction is set to 0 in these cases).
	default:
		// Ensure they have the item for showing the transformation list
		if (!pCaster->CanUseItem(pSkill->iUseItem))
			return false;
		break;
	}

	// Perform class check, if applicable.
	bool bAllowedClass = (pType->sClass == 0);
	if (bAllowedClass)
		return true;

	// NOTE: sClass is a 4 digit number (e.g. 1111) with a digit per class 
	// in the order warrior/rogue/mage/priest with '1' representing 'allowed' & 
	// anything else as forbidden.
	switch (pCaster->GetBaseClassType())
	{
	case ClassWarrior:
		bAllowedClass = ((pType->sClass / 1000)) == 1;
		break;

	case ClassRogue:
		bAllowedClass = ((pType->sClass % 1000) / 100) == 1;
		break;

	case ClassMage:
		bAllowedClass = (((pType->sClass % 1000) % 100) / 10) == 1;
		break;

	case ClassPriest:
		bAllowedClass = (((pType->sClass % 1000) % 100) % 10) == 1;
		break;
	}

	return bAllowedClass;
}

bool MagicInstance::ExecuteSkill(uint8 bType)
{
	// Implement player-specific logic before skills are executed.
	if (pSkillCaster->isPlayer())
	{
		// If a player is stealthed, and they are casting a type 1/2/3/7 skill
		// it is classed as an attack, so they should be unstealthed.
		if (TO_USER(pSkillCaster)->m_bInvisibilityType != INVIS_NONE
			&& ((bType >= 1 && bType <= 3) || (bType == 7)))
		{
			CMagicProcess::RemoveStealth(pSkillCaster, INVIS_DISPEL_ON_MOVE);
			CMagicProcess::RemoveStealth(pSkillCaster, INVIS_DISPEL_ON_ATTACK);
		}
	}

	switch (bType)
	{
		case 1: return ExecuteType1();
		case 2: return ExecuteType2();
		case 3: return ExecuteType3();
		case 4: return ExecuteType4();
		case 5: return ExecuteType5();
		case 6: return ExecuteType6();
		case 7: return ExecuteType7();
		case 8: return ExecuteType8();
		case 9: return ExecuteType9();
	}

	return false;
}

void MagicInstance::SendTransformationList()
{
	if (!pSkillCaster->isPlayer())
		return;

	Packet result(WIZ_MAGIC_PROCESS, uint8(MAGIC_TRANSFORM_LIST));
	result << nSkillID;
	TO_USER(pSkillCaster)->Send(&result);
}

/**
 * @brief	Sends the skill failed packet to the caster.
 *
 * @param	sTargetID	Target ID to override with, as some use cases 
 * 						require.
 */
void MagicInstance::SendSkillFailed(int16 sTargetID /*= -1*/)
{
	if (pSkillCaster == nullptr)
		return;

	Packet result;
	sData[3] = (bOpcode == MAGIC_CASTING ? SKILLMAGIC_FAIL_CASTING : SKILLMAGIC_FAIL_NOEFFECT);
	BuildSkillPacket(result, sCasterID, sTargetID == -1 ? this->sTargetID : sTargetID, MAGIC_FAIL, nSkillID, sData);

	// No need to proceed if we're not sending fail packets.
	if (!bSendFail
		|| !pSkillCaster->isPlayer())
		return;

	TO_USER(pSkillCaster)->Send(&result);
}

/**
 * @brief	Builds skill packet using the specified data.
 *
 * @param	result			Buffer to store the packet in.
 * @param	sSkillCaster	Skill caster's ID.
 * @param	sSkillTarget	Skill target's ID.
 * @param	opcode			Skill system opcode.
 * @param	nSkillID		Identifier for the skill.
 * @param	sData			Array of additional misc skill data.
 */
void MagicInstance::BuildSkillPacket(Packet & result, int16 sSkillCaster, int16 sSkillTarget, int8 opcode, 
									 uint32 nSkillID, int16 sData[7])
{
	// On skill failure, flag the skill as failed.
	if (opcode == MAGIC_FAIL)
	{
		bSkillSuccessful = false;

		// No need to proceed if we're not sending fail packets.
		if (!bSendFail)
			return;
	}

	result.Initialize(WIZ_MAGIC_PROCESS);
	result	<< opcode << nSkillID << sSkillCaster << sSkillTarget
			<< sData[0] << sData[1] << sData[2] << sData[3]
			<< sData[4] << sData[5] << sData[6];
}

/**
 * @brief	Builds and sends skill packet using the specified data to pUnit.
 *
 * @param	pUnit			The unit to send the packet to. If an NPC is specified, 
 * 							bSendToRegion is assumed true.
 * @param	bSendToRegion	true to send the packet to pUser's region.
 * @param	sSkillCaster	Skill caster's ID.
 * @param	sSkillTarget	Skill target's ID.
 * @param	opcode			Skill system opcode.
 * @param	nSkillID		Identifier for the skill.
 * @param	sData			Array of additional misc skill data.
 */
void MagicInstance::BuildAndSendSkillPacket(Unit * pUnit, bool bSendToRegion, int16 sSkillCaster, int16 sSkillTarget, int8 opcode, uint32 nSkillID, int16 sData[7])
{
	Packet result;
	BuildSkillPacket(result, sSkillCaster, sSkillTarget, opcode, nSkillID, sData);

	// No need to proceed if we're not sending fail packets.
	if (opcode == MAGIC_FAIL
		&& !bSendFail)
		return;

	if (bSendToRegion || !pUnit->isPlayer())
		pUnit->SendToRegion(&result);
	else
		TO_USER(pUnit)->Send(&result);
}

/**
 * @brief	Sends the skill data in the current context to pUnit. 
 * 			If pUnit is nullptr, it will assume the caster.
 *
 * @param	bSendToRegion	true to send the packet to pUnit's region. 
 * 							If pUnit is an NPC, this is assumed true.
 * @param	pUser		 	The unit to send the packet to.
 * 							If pUnit is an NPC, it will send to pUnit's region regardless.
 */
void MagicInstance::SendSkill(bool bSendToRegion /*= true*/, Unit * pUnit /*= nullptr*/)
{
	// If pUnit is nullptr, it will assume the caster.
	if (pUnit == nullptr)
		pUnit = pSkillCaster;

	// Build the skill packet using the skill data in the current context.
	BuildAndSendSkillPacket(pUnit, bSendToRegion, 
		this->sCasterID, this->sTargetID, this->bOpcode, this->nSkillID, this->sData);
}

bool MagicInstance::IsAvailable()
{
	CUser* pParty = nullptr;
	int modulator = 0, Class = 0, skill_mod = 0;

	if (pSkill == nullptr)
		goto fail_return;

	switch (pSkill->bMoral)
	{
		// Enforce self-casting skills *unless* we're an NPC.
		// Quest NPCs, naturally, use skills set as self-buffs.
		case MORAL_SELF:
			if (pSkillCaster->isPlayer() 
				&& pSkillCaster != pSkillTarget)
				goto fail_return;
			break;

		case MORAL_FRIEND_WITHME:
			if (pSkillTarget != pSkillCaster 
				&& pSkillCaster->isHostileTo(pSkillTarget))
				goto fail_return;
			break;

		case MORAL_FRIEND_EXCEPTME:
			if (pSkillCaster == pSkillTarget
				|| pSkillCaster->isHostileTo(pSkillTarget))
				goto fail_return;
			break;

		case MORAL_PARTY:
		{
			// NPCs can't *be* in parties.
			if (pSkillCaster->isNPC()
				|| (pSkillTarget != nullptr && pSkillTarget->isNPC()))
				goto fail_return;

			// We're definitely a user, so...
			CUser *pCaster = TO_USER(pSkillCaster);

			// If the caster's not in a party, make sure the target's not someone other than themselves.
			if ((!pCaster->isInParty() && pSkillCaster != pSkillTarget)
				// Also ensure that if there is a target, they're in the same party.
				|| (pSkillTarget != nullptr && 
					TO_USER(pSkillTarget)->GetPartyID() != pCaster->GetPartyID()))
				goto fail_return;
		} break;

		case MORAL_NPC:
			if (pSkillTarget == nullptr
				|| !pSkillTarget->isNPC()
				|| pSkillCaster->isHostileTo(pSkillTarget))
				goto fail_return;
			break;

		case MORAL_ENEMY:
			// Allow for archery skills with no defined targets (e.g. an arrow from 'multiple shot' not hitting any targets). 
			// These should be ignored, not fail.
			if (pSkillTarget != nullptr
				// Nation alone cannot dictate whether a unit can attack another.
				// As such, we must check behaviour specific to these entities.
				// For example: same nation players attacking each other in an arena.
				&& !pSkillCaster->isHostileTo(pSkillTarget))
				goto fail_return;
			break;	

		case MORAL_CORPSE_FRIEND:
			// We need to revive *something*.
			if (pSkillTarget == nullptr
				// Are we allowed to revive this person?
				|| pSkillCaster->isHostileTo(pSkillTarget)
				// We cannot revive ourselves.
				|| pSkillCaster == pSkillTarget
				// We can't revive living targets.
				|| pSkillTarget->isAlive())
				goto fail_return;
			break;

		case MORAL_CLAN:
		{
			// NPCs cannot be in clans.
			if (pSkillCaster->isNPC()
				|| (pSkillTarget != nullptr && pSkillTarget->isNPC()))
				goto fail_return;

			// We're definitely a user, so....
			CUser * pCaster = TO_USER(pSkillCaster);

			// If the caster's not in a clan, make sure the target's not someone other than themselves.
			if ((!pCaster->isInClan() && pSkillCaster != pSkillTarget)
				// If we're targeting someone, that target must be in our clan.
				|| (pSkillTarget != nullptr 
					&& TO_USER(pSkillTarget)->GetClanID() != pCaster->GetClanID()))
				goto fail_return;
		} break;

		case MORAL_SIEGE_WEAPON:
			if (pSkillCaster->isPlayer()
				|| !TO_USER(pSkillCaster)->isSiegeTransformation())
				goto fail_return;
			break;
	}

	// Check skill prerequisites
	for (int i = 0; i < 2; i++) 
	{
		switch (pSkill->bType[i])
		{
		case 3: 
			if (!CheckType3Prerequisites())
				return false;
			break;

		case 4:
			if (!CheckType4Prerequisites())
				return false;
			break;

		case 6:
			if (!CheckType6Prerequisites())
				return false;
			break;
		}
	}

	// This only applies to users casting skills. NPCs are fine and dandy (we can trust THEM at least).
	if (pSkillCaster->isPlayer())
	{
		if (pSkill->bType[0] == 3)
		{
			_MAGIC_TYPE3 * pType3 = g_pMain->m_Magictype3Array.GetData(pSkill->iNum);
			_ITEM_TABLE * pTable;
			if (pType3 == nullptr)
				goto fail_return;

			// Allow for skills that block potion use.
			if (!pSkillCaster->canUsePotions()
				&& pType3->bDirectType == 1		// affects target's HP (magic numbers! yay!)
				&& pType3->sFirstDamage > 0		// healing only
				&& pSkill->iUseItem != 0		// requiring an item (i.e. pots) 
				// To avoid conflicting with priest skills that require items (e.g. "Laying of hands")
				// we need to lookup the item itself for the information we need to ignore it.
				&& (pTable = g_pMain->GetItemPtr(pSkill->iUseItem)) != nullptr
				// Item-required healing skills are class-specific. 
				// We DO NOT want to block these skills.
				&& pTable->m_bClass == 0)
				goto fail_return;
		}

		modulator = pSkill->sSkill % 10;     // Hacking prevention!
		if( modulator != 0 ) {	
			Class = pSkill->sSkill / 10;
			if( Class != TO_USER(pSkillCaster)->GetClass() ) goto fail_return;
			if( pSkill->sSkillLevel > TO_USER(pSkillCaster)->m_bstrSkill[modulator] ) goto fail_return;
		}
		else if (pSkill->sSkillLevel > pSkillCaster->GetLevel()) 
			goto fail_return;

		if (pSkill->bType[0] == 1) {	// Weapons verification in case of COMBO attack (another hacking prevention).
			if (pSkill->sSkill == 1055 || pSkill->sSkill == 2055) {		// Weapons verification in case of dual wielding attacks !		
				_ITEM_TABLE *pLeftHand = TO_USER(pSkillCaster)->GetItemPrototype(LEFTHAND),
							*pRightHand = TO_USER(pSkillCaster)->GetItemPrototype(RIGHTHAND);

				if ((pLeftHand != nullptr && !pLeftHand->isSword() && !pLeftHand->isAxe() && !pLeftHand->isMace())
					|| (pRightHand != nullptr && !pRightHand->isSword() && !pRightHand->isAxe() && !pRightHand->isMace()))
					return false;
			}
			else if (pSkill->sSkill == 1056 || pSkill->sSkill == 2056) {	// Weapons verification in case of 2 handed attacks !
				_ITEM_TABLE	*pRightHand = TO_USER(pSkillCaster)->GetItemPrototype(RIGHTHAND);

				if (TO_USER(pSkillCaster)->GetItem(LEFTHAND)->nNum != 0
					|| (pRightHand != nullptr 
						&& !pRightHand->isSword() && !pRightHand->isAxe() 
						&& !pRightHand->isMace() && !pRightHand->isSpear()))
					return false;
			}
		}

		// Handle MP/HP/item loss.
		if (bOpcode == MAGIC_EFFECTING) 
		{
			int total_hit = pSkillCaster->m_sTotalHit;

			if (pSkill->bType[0] == 2 && pSkill->bFlyingEffect != 0) // Type 2 related...
				return true;		// Do not reduce MP/SP when flying effect is not 0.

#if 0 // dodgy check preventing legitimate behaviour
			if (pSkill->bType[0] == 1 && sData[0] > 1)
				return true;		// Do not reduce MP/SP when combo number is higher than 0.
#endif
 
			if (pSkill->sMsp > pSkillCaster->GetMana())
				goto fail_return;

			// If the PLAYER uses an item to cast a spell.
 			if (pSkillCaster->isPlayer()
				&& (pSkill->bType[0] == 3 || pSkill->bType[0] == 4))
			{
				if (pSkill->iUseItem != 0) {
					_ITEM_TABLE* pItem = nullptr;				// This checks if such an item exists.
					pItem = g_pMain->GetItemPtr(pSkill->iUseItem);
					if( !pItem ) return false;

					if ((pItem->m_bClass != 0 && !TO_USER(pSkillCaster)->JobGroupCheck(pItem->m_bClass))
						|| (pItem->m_bReqLevel != 0 && TO_USER(pSkillCaster)->GetLevel() < pItem->m_bReqLevel))	
						return false;
				}
			}
			if (pSkill->bType[0] != 4 || (pSkill->bType[0] == 4 && sTargetID == -1))
				pSkillCaster->MSpChange(-(pSkill->sMsp));

			// Skills that require HP rather than MP.
			if (pSkill->sHP > 0 
				&& pSkill->sMsp == 0 
				&& pSkill->sHP < 10000) // Hack (used officially) to allow for skills like "Sacrifice"
			{
				if (pSkill->sHP > pSkillCaster->GetHealth()) goto fail_return;
				pSkillCaster->HpChange(-pSkill->sHP);
			}

			// Support skills like "Sacrifice", that sacrifice your HP for another's.
			if (pSkill->sHP >= 10000)
			{
				// Can't cast this on ourself.
				if (pSkillCaster == pSkillTarget)
					return false;

				// Take 10,000 HP from the caster (note: DB is set to 10,0001 but server will always take 10,000...)
				pSkillCaster->HpChange(-10000); 
			}
		}
	}

	return true;

fail_return: 
	SendSkillFailed();

	return false;
}

bool MagicInstance::ExecuteType1()
{	
	if (pSkill == nullptr)
		return false;

	int damage = 0;
	bool bResult = false;

	_MAGIC_TYPE1* pType = g_pMain->m_Magictype1Array.GetData(nSkillID);
	if (pType == nullptr)
		return false;

	if (pSkillTarget != nullptr && !pSkillTarget->isDead())
	{
		bResult = 1;

		uint16 sAdditionalDamage = pType->sAddDamage;

		// Give increased damage in war zones (as per official 1.298~1.325)
		// This may need to be revised to use the last nation to win the war, or more accurately, 
		// the nation that last won the war 3 times in a row (whether official behaves this way now is unknown).
		if (pSkillCaster->isPlayer()
			&& pSkillTarget->isPlayer())
		{
			if (pSkillCaster->GetMap()->isWarZone())
				sAdditionalDamage /= 2;
			else
				sAdditionalDamage /= 3;
		}

		damage = pSkillCaster->GetDamage(pSkillTarget, pSkill);

		// Only add additional damage if the target's not currently blocking it.
		// NOTE: Not sure whether to count this as physical or magic damage.
		// Using physical damage, due to the nature of these skills.
		if (!pSkillTarget->m_bBlockPhysical)
			damage += sAdditionalDamage;
	
		pSkillTarget->HpChange(-damage, pSkillCaster);

		if (pSkillTarget->m_bReflectArmorType != 0 && pSkillCaster != pSkillTarget)
			ReflectDamage(damage, pSkillTarget);
	}

	// This should only be sent once. I don't think there's reason to enforce this, as no skills behave otherwise
	sData[3] = (damage == 0 ? SKILLMAGIC_FAIL_ATTACKZERO : 0);

	// Send the skill data in the current context to the caster's region
	SendSkill();

	return bResult;
}

bool MagicInstance::ExecuteType2()
{
	/*
		NOTE: 
			Archery skills work differently to most other skills.
			
			When an archery skill is used, the client sends MAGIC_FLYING (instead of MAGIC_CASTING) 
			to show the arrows flying in the air to their targets.
			
			The client chooses the target(s) to be hit by the arrows.

			When an arrow hits a target, it will send MAGIC_EFFECTING which triggers this handler.
			An arrow sent may not necessarily hit a target.

			As such, for archery skills that use multiple arrows, not all n arrows will necessarily
			hit their target, and thus they will not necessarily call this handler all n times.

			What this means is, we must remove all n arrows from the user in MAGIC_FLYING, otherwise
			it is not guaranteed all arrows will be hit and thus removed.
			(and we can't just go and take all n items each time an arrow hits, that could potentially 
			mean 25 arrows are removed [5 each hit] when using "Arrow Shower"!)

			However, via the use of hacks, this MAGIC_FLYING step can be skipped -- so we must also check 
			to ensure that there arrows are indeed flying, to prevent users from spamming the skill
			without using arrows.
	 */
	if (pSkill == nullptr)
		return false;

	int damage = 0;
	bool bResult = false;
	float total_range = 0.0f;	// These variables are used for range verification!
	int sx, sz, tx, tz;

	_MAGIC_TYPE2 *pType = g_pMain->m_Magictype2Array.GetData(nSkillID);
	if (pType == nullptr)
		return false;

	int range = 0;

	// If we need arrows, then we require a bow.
	// This check is needed to allow for throwing knives (the sole exception at this time.
	// In this case, 'NeedArrow' is set to 0 -- we do not need a bow to use throwing knives, obviously.
	if (pType->bNeedArrow > 0)
	{
		_ITEM_TABLE * pTable = nullptr;
		if (pSkillCaster->isPlayer())
		{
			// Not wearing a left-handed bow
			pTable = TO_USER(pSkillCaster)->GetItemPrototype(LEFTHAND);
			if (pTable == nullptr || !pTable->isBow())
			{
				pTable = TO_USER(pSkillCaster)->GetItemPrototype(RIGHTHAND);

				// Not wearing a right-handed (2h) bow either!
				if (pTable == nullptr || !pTable->isBow())
					return false;
			}
		}
		else 
		{
			// TO-DO: Verify this. It's more a placeholder than anything. 
			pTable = g_pMain->GetItemPtr(TO_NPC(pSkillCaster)->m_iWeapon_1);
			if (pTable == nullptr)
				return false; 
		}
		
		// For arrow skills, we require a bow & its range.
		range = pTable->m_sRange;
	}
	else
	{
		// For non-arrow skills (i.e. throwing knives) we should use the skill's range.
		range = pSkill->sRange;
	}

	// is this checked already?
	if (pSkillTarget == nullptr || pSkillTarget->isDead())
		goto packet_send;
	
	total_range = pow(((pType->sAddRange * range) / 100.0f), 2.0f) ;     // Range verification procedure.
	sx = (int)pSkillCaster->GetX(); tx = (int)pSkillTarget->GetX();
	sz = (int)pSkillCaster->GetZ(); tz = (int)pSkillTarget->GetZ();
	
	if ((pow((float)(sx - tx), 2.0f) + pow((float)(sz - tz), 2.0f)) > total_range)	   // Target is out of range, exit.
		goto packet_send;
	
	if (pSkillCaster->isPlayer())
	{
		CUser * pUser = TO_USER(pSkillCaster);
		FastGuard lock(pUser->m_arrowLock);

		// No arrows currently flying.
		if (pUser->m_flyingArrows.empty())
			goto packet_send;

		ArrowList::iterator arrowItr;
		bool bFoundArrow = false;
		for (auto itr = pUser->m_flyingArrows.begin(); itr != pUser->m_flyingArrows.end();)
		{
			if (UNIXTIME >= itr->tFlyingTime + ARROW_EXPIRATION_TIME)
			{
				itr = pUser->m_flyingArrows.erase(itr);
			}
			else
			{
				if (itr->nSkillID == nSkillID)
				{
					arrowItr = itr; /* don't break out here to ensure we remove all expired arrows */
					bFoundArrow = true;
				}

				++itr;
			}
		}

		// No flying arrow matching this skill's criteria was found.
		// User's probably cheating.
		if (!bFoundArrow)
			goto packet_send;

		// Remove this instance's arrow now that we've found it.
		pUser->m_flyingArrows.erase(arrowItr);
	}

	damage = pSkillCaster->GetDamage(pSkillTarget, pSkill);  // Get damage points of enemy.	

	pSkillTarget->HpChange(-damage, pSkillCaster);     // Reduce target health point.
	if (pSkillTarget->m_bReflectArmorType != 0 && pSkillCaster != pSkillTarget)
		ReflectDamage(damage, pSkillTarget);

	bResult = true;

packet_send:
	// This should only be sent once. I don't think there's reason to enforce this, as no skills behave otherwise
	sData[3] = (damage == 0 ? SKILLMAGIC_FAIL_ATTACKZERO : 0);

	// Send the skill data in the current context to the caster's region
	SendSkill();

	return bResult;
}

// Applied when a magical attack, healing, and mana restoration is done.
bool MagicInstance::ExecuteType3()
{	
	int damage = 0, duration_damage = 0;
	vector<Unit *> casted_member;

	_MAGIC_TYPE3* pType = g_pMain->m_Magictype3Array.GetData(nSkillID);
	if (pType == nullptr) 
		return false;

	// If the target's a group of people...
	if (sTargetID == -1)
	{
		std::vector<uint16> unitList;
		g_pMain->GetUnitListFromSurroundingRegions(pSkillCaster, &unitList);
		if(pType->sFirstDamage > 0 || pType->sTimeDamage > 0)
			casted_member.push_back(pSkillCaster);
		foreach (itr, unitList)
		{		
			Unit * pTarget = g_pMain->GetUnitPtr(*itr);
			if (pSkillCaster != pTarget
				&& !pTarget->isDead() && !pTarget->isBlinking()
				&& CMagicProcess::UserRegionCheck(pSkillCaster, pTarget, pSkill, pType->bRadius, sData[0], sData[2]))
				casted_member.push_back(pTarget);
		}

		// If you managed to not hit anything with your AoE, you're still gonna have a cooldown (You should l2aim)
		if (casted_member.empty() || (sTargetID == -1 && casted_member.empty()))
		{
			SendSkill();
			return true;			
		}
	}
	else
	{	// If the target was a single unit.
		if (pSkillTarget == nullptr 
			|| pSkillTarget->isDead() 
			|| (pSkillTarget->isPlayer() 
				&& TO_USER(pSkillTarget)->isBlinking())) 
			return false;
		
		casted_member.push_back(pSkillTarget);
	}
	
	// Anger explosion requires the caster be a player
	// and a full anger gauge (which will be reset after use).
	if (pType->bDirectType == 18)
	{
		// Only players can cast this skill.
		if (!pSkillCaster->isPlayer()
			|| !TO_USER(pSkillCaster)->hasFullAngerGauge())
			return false;

		// Reset the anger gauge
		TO_USER(pSkillCaster)->UpdateAngerGauge(0);
	}

	sData[1] = 1;
	foreach (itr, casted_member)
	{
		Unit * pTarget = *itr; // it's checked above, not much need to check it again

		// If you are casting an attack spell.
		if ((pType->sFirstDamage < 0) && (pType->bDirectType == 1 || pType->bDirectType == 8) 
			&& (nSkillID < 400000) 
			&& (pType->bDirectType != 11 && pType->bDirectType != 13))
			damage = GetMagicDamage(pTarget, pType->sFirstDamage, pType->bAttribute);
		else 
			damage = pType->sFirstDamage;

		// Allow for complete magic damage blocks.
		if (damage < 0 && pTarget->m_bBlockMagic)
			continue;

		if (pSkillCaster->isPlayer())
		{
			if (pSkillCaster->GetZoneID() == ZONE_SNOW_BATTLE && g_pMain->m_byBattleOpen == SNOW_BATTLE)
				damage = -10;
		}

		// Non-durational spells.
		if (pType->bDuration == 0) 
		{
			switch (pType->bDirectType)
			{
			// Affects target's HP
			case 1:
				// "Critical Point" buff gives a chance to double HP from pots or the rogue skill "Minor heal".
				if (damage > 0 && pSkillCaster->hasBuff(BUFF_TYPE_DAMAGE_DOUBLE)
					&& CheckPercent(500))
					damage *= 2;

				pTarget->HpChangeMagic(damage, pSkillCaster, (AttributeType) pType->bAttribute);
				if (pTarget->m_bReflectArmorType != 0 && pTarget != pSkillCaster)
					ReflectDamage(damage, pTarget);
			break;

			// Affects target's MP
			case 2:
			case 3:
				pTarget->MSpChange(damage);			
				break;

			// "Magic Hammer" repairs equipped items.
			case 4:
				if (pTarget->isPlayer())
				{
					TO_USER(pTarget)->ItemWoreOut(ATTACK, -damage);
					TO_USER(pTarget)->ItemWoreOut(DEFENCE, -damage);
				}
				break;

			// Increases/decreases target's HP by a percentage
			case 5:
				if (pType->sFirstDamage < 100)
					damage = (pType->sFirstDamage * pTarget->GetHealth()) / -100;
				else
					damage = (pTarget->GetMaxHealth() * (pType->sFirstDamage - 100)) / 100;

				pTarget->HpChangeMagic(damage, pSkillCaster);
				break;
			
			// Caster absorbs damage based on percentage of target's HP. Players only.
			case 8:
				if (pTarget->isPlayer())
				{
					if (pType->sFirstDamage < 100)
						damage = (pType->sFirstDamage * pTarget->GetHealth()) / -100;
					else
						damage = (pTarget->GetMaxHealth() * (pType->sFirstDamage - 100)) / 100;

					pTarget->HpChangeMagic(damage, pSkillCaster);
					pSkillCaster->HpChangeMagic(-(damage));
				}
				break;

			// Caster absorbs damage based on percentage of target's max HP
			case 9:
				if (pType->sFirstDamage < 100)
					damage = (pType->sFirstDamage * pTarget->GetHealth()) / -100;
				else
					damage = (pTarget->GetMaxHealth() * (pType->sFirstDamage - 100)) / 100;

				pTarget->HpChangeMagic(damage, pSkillCaster);
				pSkillCaster->HpChangeMagic(-(damage));
				break;

			// Inflicts true damage (i.e. disregards Ac/resistances/buffs, etc).
			case 11:
				pTarget->HpChange(damage, pSkillCaster);
				break;

			// Used by "Destination scroll" (whatever that is)
			case 12:
				continue;

			// Chance (how often?) to reduce the opponent's armor and weapon durability by sFirstDamage
			case 13:
				if (pTarget->isPlayer() && CheckPercent(500)) // using 50% for now.
				{
					TO_USER(pTarget)->ItemWoreOut(ATTACK, damage);
					TO_USER(pTarget)->ItemWoreOut(DEFENCE, damage);
				}
				break;

			// Drains target's MP, gives half of it to the caster as HP. Players only.
			// NOTE: Non-durational form (as in 1.8xx). This was made durational later (database configured).
			case 16:
				if (pTarget->isPlayer())
				{
					pTarget->MSpChange(pType->sFirstDamage);
					pSkillCaster->HpChangeMagic(-(pType->sFirstDamage) / 2);
				}
				break;
			}
		}
		// Durational spells! Durational spells only involve HP.
		else if (pType->bDuration != 0) 
		{
			if (damage != 0)		// In case there was first damage......
				pTarget->HpChangeMagic(damage, pSkillCaster);			// Initial damage!!!

			if (pTarget->isAlive()) 
			{
				// HP booster (should this actually just be using sFirstDamage as the percent of max HP, i.e. 105 being 5% of max HP each increment?)
				if (pType->bDirectType == 14)
					duration_damage = (int)(pSkillCaster->GetLevel() * (1 + pSkillCaster->GetLevel() / 30.0)) + 3;
				else if (pType->sTimeDamage < 0 && pType->bAttribute != 4) 
					duration_damage = GetMagicDamage(pTarget, pType->sTimeDamage, pType->bAttribute);
				else 
					duration_damage = pType->sTimeDamage;

				// Allow for complete magic damage blocks.
				if (duration_damage < 0 && pTarget->m_bBlockMagic)
					continue;

				// Setup DOT (damage over time)
				for (int k = 0; k < MAX_TYPE3_REPEAT; k++) 
				{
					Unit::MagicType3 * pEffect = &pTarget->m_durationalSkills[k];
					if (pEffect->m_byUsed)
						continue;

					pEffect->m_byUsed = true;
					pEffect->m_tHPLastTime = 0;
					pEffect->m_bHPInterval = 2;					// interval of 2s between each damage loss/HP gain 

					// number of ticks required at a rate of 2s per tick over the total duration of the skill
					float tickCount = (float) pType->bDuration / (float) pEffect->m_bHPInterval;

					// amount of HP to be given/taken every tick at a rate of 2s per tick
					pEffect->m_sHPAmount = (int16)(duration_damage / tickCount);

					pEffect->m_bTickCount = 0;
					pEffect->m_bTickLimit = (uint8) tickCount;
					pEffect->m_sSourceID = sCasterID;
					pEffect->m_byAttribute = pType->bAttribute;
					break;
				}

				pTarget->m_bType3Flag = true;
			}

			// Send the status updates (i.e. DOT or position indicators) to the party/user
			if (pTarget->isPlayer()
				// Ignore healing spells, not sure if this will break any skills.
				&& pType->sTimeDamage < 0)
			{
				TO_USER(pTarget)->SendUserStatusUpdate(pType->bAttribute == POISON_R ? USER_STATUS_POISON : USER_STATUS_DOT, USER_STATUS_INFLICT);
			}
		}

		// Send the skill data in the current context to the caster's region, with the target explicitly set.
		// In the case of AOEs, this will mean the AOE will show the AOE's effect on the user (not show the AOE itself again).
		if (pSkill->bType[1] == 0 || pSkill->bType[1] == 3)
			BuildAndSendSkillPacket(pSkillCaster, true, sCasterID, pTarget->GetID(), bOpcode, nSkillID, sData);

		// Tell the AI server we're healing someone (so that they can choose to pick on the healer!)
		if (pType->bDirectType == 1 && damage > 0
			&& sCasterID != sTargetID)
		{
			Packet result(AG_HEAL_MAGIC);
			result << sCasterID;
			g_pMain->Send_AIServer(&result);
		}
	}

	// Allow for AOE effects.
	if (pSkill->bType[0] == 3
		&& sTargetID == -1)
		SendSkill();

	return true;
}

bool MagicInstance::ExecuteType4()
{
	int damage = 0;

	vector<Unit *> casted_member;
	if (pSkill == nullptr)
		return false;

	_MAGIC_TYPE4* pType = g_pMain->m_Magictype4Array.GetData(nSkillID);
	if (pType == nullptr)
		return false;

	if (!bIsRecastingSavedMagic 
		&& sTargetID >= 0 
		&& pSkillTarget->HasSavedMagic(nSkillID))
		return false;

	if (sTargetID == -1)
	{
		std::vector<uint16> unitList;

		g_pMain->GetUnitListFromSurroundingRegions(pSkillCaster, &unitList);
		foreach (itr, unitList)
		{		
			Unit * pTarget = g_pMain->GetUnitPtr(*itr);
			if (!pTarget->isDead() && !pTarget->isBlinking()
				&& CMagicProcess::UserRegionCheck(pSkillCaster, pTarget, pSkill, pType->bRadius, sData[0], sData[2]))
				casted_member.push_back(pTarget);
		}

		if (casted_member.empty())
		{		
			if (pSkillCaster->isPlayer())
				SendSkill();

			return false;
		}
	}
	else 
	{
		// If the target was another single unit.
		if (pSkillTarget == nullptr 
			|| pSkillTarget->isDead() || (pSkillTarget->isBlinking() && !bIsRecastingSavedMagic)) 
			return false;

		casted_member.push_back(pSkillTarget);
	}

	foreach (itr, casted_member)
	{
		uint8 bResult = 1;
		Unit * pTarget = *itr;
		_BUFF_TYPE4_INFO pBuffInfo;
		bool bAllowCastOnSelf = false;
		uint16 sDuration = pType->sDuration;

		// A handful of skills (Krowaz, currently) should use the caster as the target.
		// As such, we should correct this before any other buff/debuff logic is handled.
		switch (pType->bBuffType)
		{
		case BUFF_TYPE_UNDEAD:
		case BUFF_TYPE_UNSIGHT:
		case BUFF_TYPE_BLOCK_PHYSICAL_DAMAGE:
		case BUFF_TYPE_BLOCK_MAGICAL_DAMAGE:
			if (!pSkillCaster->isPlayer())
				continue;

			pTarget = pSkillCaster;
			bAllowCastOnSelf = true;
			break;
		}

		bool bBlockingDebuffs = pTarget->m_bBlockCurses;

		// Skill description: Blocks all curses and has a chance to reflect the curse back onto the caster.
		// NOTE: the exact rate is undefined, so we'll have to guess and improve later.
		if (pType->isDebuff() && pTarget->m_bReflectCurses)
		{
			const short reflectChance = 25; // % chance to reflect.
			if (CheckPercent(reflectChance * 10))
			{
				pTarget = pSkillCaster; // skill has been reflected, the target is now the caster.
				bBlockingDebuffs = (pTarget->m_bBlockCurses || pTarget->m_bReflectCurses); 
				bAllowCastOnSelf = true;
			}
			// Didn't reflect, so we'll just block instead.
			else 
			{
				bBlockingDebuffs = true;
			}
		}

		pTarget->m_buffLock.Acquire();
		Type4BuffMap::iterator buffItr = pTarget->m_buffMap.find(pType->bBuffType);

		// Identify whether or not a skill (buff/debuff) with this buff type was already cast on the player.
		// NOTE:	Buffs will already be cast on a user when trying to recast. 
		//			We should not error out in this case.
		bool bSkillTypeAlreadyOnTarget = (!bIsRecastingSavedMagic && buffItr != pTarget->m_buffMap.end());

		pTarget->m_buffLock.Release();

		// Debuffs 'stack', in that the expiry time is reset each time.
		// Debuffs also take precedence over buffs of the same nature, so we should ensure they get removed 
		// rather than just stacking the modifiers, as the client only supports one (de)buff of that type active.
		if (bSkillTypeAlreadyOnTarget && pType->isDebuff())
		{
			CMagicProcess::RemoveType4Buff(pType->bBuffType, pTarget);
			bSkillTypeAlreadyOnTarget = false;
		}

		// If this skill is a debuff, and we are in the crossfire, 
		// we should not bother debuffing ourselves (that would be bad!)
		// Note that we should allow us if there's an explicit override (i.e. with Krowaz self-debuffs)
		if (!bAllowCastOnSelf 
			&& pType->isDebuff() && pTarget == pSkillCaster)
			continue;
		
		// If the user already has this buff type cast on them (debuffs should just reset the duration)
		if ((bSkillTypeAlreadyOnTarget && pType->isBuff())
			// or it's a curse (debuff), and we're blocking them 
			|| (pType->isDebuff() && bBlockingDebuffs)
			// or we couldn't grant the (de)buff...
			|| !CMagicProcess::GrantType4Buff(pSkill, pType, pSkillCaster, pTarget, bIsRecastingSavedMagic))
		{
			if (sTargetID != -1 // only error out when buffing a target, otherwise we break the mass-(de)buff.
				// Only buffs should error here, unless it's a debuff & the user's blocking it.
				&& (pType->isBuff() || (pType->isDebuff() && bBlockingDebuffs)))
			{
				bResult = 0;
				goto fail_return;
			}

			// Debuffs of any kind, or area buffs should be ignored and carry on.
			// Usually - debuffs specifically - they correspond with attack skills which should
			// not be reset on fail.
			continue;
		}

		// Only players can store persistent skills.
		if (nSkillID > 500000 && pTarget->isPlayer())
		{
			// Persisting effects will already exist in the map if we're recasting it. 
			if (!bIsRecastingSavedMagic)
				pTarget->InsertSavedMagic(nSkillID, pType->sDuration);
			else
				sDuration = pTarget->GetSavedMagicDuration(nSkillID);
		}

		if (pSkillCaster->isPlayer()
			&& (sTargetID != -1 && pSkill->bType[0] == 4))
			pSkillCaster->MSpChange( -(pSkill->sMsp) );

		// We do not want to reinsert debuffs into the map (which had their expiry times reset above).
		if (!bSkillTypeAlreadyOnTarget)
		{
			pBuffInfo.m_nSkillID = nSkillID;
			pBuffInfo.m_bIsBuff = pType->bIsBuff;

			pBuffInfo.m_bDurationExtended = false;
			pBuffInfo.m_tEndTime = UNIXTIME + sDuration;

			// Add the buff into the buff map.
			pTarget->AddType4Buff(pType->bBuffType, pBuffInfo);
		}

		// Update character stats.
		if (pTarget->isPlayer())
		{
			TO_USER(pTarget)->SetUserAbility();
			TO_USER(pTarget)->Send2AI_UserUpdateInfo();
		}

	fail_return:
		if (pSkill->bType[1] == 0 || pSkill->bType[1] == 4)
		{
			Unit *pTmp = (pSkillCaster->isPlayer() ? pSkillCaster : pTarget);
			int16 sDataCopy[] = 
			{
				sData[0], bResult, sData[2], sDuration,
				sData[4], pType->bSpeed, sData[6]
			};

			BuildAndSendSkillPacket(pTmp, true, sCasterID, pTarget->GetID(), bOpcode, nSkillID, sDataCopy);

			if (pSkill->bMoral >= MORAL_ENEMY
				&& pTarget->isPlayer())
			{
				UserStatus status = USER_STATUS_POISON;
				if (pType->bBuffType == BUFF_TYPE_SPEED || pType->bBuffType == BUFF_TYPE_SPEED2)
					status = USER_STATUS_SPEED;

				TO_USER(pTarget)->SendUserStatusUpdate(status, USER_STATUS_INFLICT);
			}
		}
		
		if (bResult == 0
			&& pSkillCaster->isPlayer())
			SendSkillFailed((*itr)->GetID());
	}
	return true;
}

bool MagicInstance::ExecuteType5()
{
	// Disallow anyone that isn't a player.
	if (!pSkillCaster->isPlayer()
		|| pSkill == nullptr)
		return false;

	_MAGIC_TYPE5* pType = g_pMain->m_Magictype5Array.GetData(nSkillID);
	if (pType == nullptr)
		return false;

	vector<CUser *> casted_member;

	// Targeting a group of people (e.g. party)
	if (sTargetID == -1)
	{
		SessionMap & sessMap = g_pMain->m_socketMgr.GetActiveSessionMap();
		foreach (itr, sessMap)
		{
			CUser * pTUser = TO_USER(itr->second);
			if (!pTUser->isInGame())
				continue;

			// If the target's dead, only allow resurrection/self-resurrection spells.
			if (pTUser->isDead() 
				&& (pType->bType != RESURRECTION && pType->bType != RESURRECTION_SELF))
				continue;

			// If the target's alive, we don't need to resurrect them.
			if (!pTUser->isDead() 
					&& (pType->bType == RESURRECTION || pType->bType == RESURRECTION_SELF))
				continue;

			// Ensure the target's applicable for this skill.
			if (CMagicProcess::UserRegionCheck(pSkillCaster, pTUser, pSkill, pSkill->sRange, sData[0], sData[2]))
				casted_member.push_back(pTUser);
		}
		g_pMain->m_socketMgr.ReleaseLock();
	}
	// Targeting a single person
	else
	{
		if (pSkillTarget == nullptr
			|| !pSkillTarget->isPlayer())
			return false;

		// If the target's dead, only allow resurrection/self-resurrection spells.
		if (pSkillTarget->isDead() 
			&& (pType->bType != RESURRECTION && pType->bType != RESURRECTION_SELF))
			return false;

			// If the target's alive, we don't need to resurrect them.
		if (!pSkillTarget->isDead() 
				&& (pType->bType == RESURRECTION || pType->bType == RESURRECTION_SELF))
			return false;

		casted_member.push_back(TO_USER(pSkillTarget));
	}

	foreach (itr, casted_member)
	{
		Type4BuffMap::iterator buffIterator;
		CUser * pTUser = (*itr);
		int skillCount = 0;
		bool bRemoveDOT = false;

		switch (pType->bType)
		{
			// Remove all DOT skills
			case REMOVE_TYPE3:
				for (int i = 0; i < MAX_TYPE3_REPEAT; i++)
				{
					Unit::MagicType3 * pEffect = &pTUser->m_durationalSkills[i];
					if (!pEffect->m_byUsed)
						continue;

					// Ignore healing-over-time skills
					if (pEffect->m_sHPAmount >= 0)
					{
						skillCount++;
						continue;
					}

					pEffect->Reset();
					// TO-DO: Wrap this up (ugh, I feel so dirty)
					Packet result(WIZ_MAGIC_PROCESS, uint8(MAGIC_DURATION_EXPIRED));
					result << uint8(200); // removes DOT skill
					pTUser->Send(&result); 
					bRemoveDOT = true;
				}

				if (skillCount == 0)
				{
					pTUser->m_bType3Flag = false;
					if (bRemoveDOT)
						pTUser->SendUserStatusUpdate(USER_STATUS_DOT, USER_STATUS_CURE);
				}
				break;

			case REMOVE_TYPE4: // Remove type 4 debuffs
			{
				FastGuard lock(pTUser->m_buffLock);
				Type4BuffMap buffMap = pTUser->m_buffMap; // copy the map so we can't break it while looping
				foreach (itr, buffMap)
				{
					if (itr->second.isDebuff())
						CMagicProcess::RemoveType4Buff(itr->first, pTUser);
				}

				// NOTE: This originally checked to see if there were any active debuffs.
				// As this seems pointless (as we're removing all of them), it was removed
				// however leaving this note in, in case this behaviour in certain conditions
				// is required.
				pTUser->SendUserStatusUpdate(USER_STATUS_POISON, USER_STATUS_CURE);
			} break;
			
			case RESURRECTION_SELF:
				if (pSkillCaster != pTUser)
					continue;

			case RESURRECTION:
				pTUser->Regene(1, nSkillID);
				break;

			case REMOVE_BLESS:
			{
				if (CMagicProcess::RemoveType4Buff(BUFF_TYPE_HP_MP, pTUser))
				{
					if (!pTUser->isDebuffed()) 
						pTUser->SendUserStatusUpdate(USER_STATUS_POISON, USER_STATUS_CURE);
				}
			} break;
		}

		if (pSkill->bType[1] == 0 || pSkill->bType[1] == 5)
		{
			// Send the packet to the caster.
			sData[1] = 1;
			BuildAndSendSkillPacket(pSkillCaster, true, sCasterID, (*itr)->GetID(), bOpcode, nSkillID, sData); 
		}
	}

	return true;
}

bool MagicInstance::ExecuteType6()
{
	if (pSkill == nullptr
		|| !pSkillCaster->isPlayer())
		return false;

	CUser * pCaster = TO_USER(pSkillCaster);
	_MAGIC_TYPE6 * pType = g_pMain->m_Magictype6Array.GetData(nSkillID);
	uint16 sDuration = 0;

	if (pType == nullptr
			// Allow NPC transformations in PVP zones
			|| (pType->bUserSkillUse != TransformationSkillUseNPC
				// All PVP zones.
				&& pSkillCaster->GetMap()->canAttackOtherNation()
				// Exclude home zones (which can be invaded).
				&& pSkillCaster->GetZoneID() > ELMORAD)
		|| (!bIsRecastingSavedMagic && pCaster->isTransformed())
		// All buffs must be removed before using transformation skills
		|| (pType->bUserSkillUse != TransformationSkillUseNPC && pSkillCaster->isBuffed()))
	{
		// If we're recasting it, then it's either already cast on us (i.e. we're changing zones)
		// or we're relogging. We need to remove it now, as the client doesn't see us using it.
		if (bIsRecastingSavedMagic)
			Type6Cancel(true);

		return false;
	}

	// We can ignore all these checks if we're just recasting on relog.
	if (!bIsRecastingSavedMagic)
	{
		if (pSkillTarget->HasSavedMagic(nSkillID))
			return false;

		// Monster transformations require a transformation list.
		if (pType->bUserSkillUse == TransformationMonster)
			pCaster->RobItem(pSkill->nBeforeAction);

		// User's casting a new skill. Use the full duration.
		sDuration = pType->sDuration;
	}
	else 
	{
		// Server's recasting the skill (kept on relog, zone change, etc.)
		int16 tmp = pSkillCaster->GetSavedMagicDuration(nSkillID);

		// Has it expired (or not been set?) -- just in case.
		if (tmp <= 0)
			return false;

		// it's positive, so no harm here in casting.
		sDuration = tmp;
	}

	switch (pType->bUserSkillUse)
	{
	case TransformationSkillUseMonster:
		pCaster->m_transformationType = TransformationMonster;
		break;

	case TransformationSkillUseNPC:
		pCaster->m_transformationType = TransformationNPC;
		break;

	case TransformationSkillUseSiege:
	case TransformationSkillUseSiege2:
		pCaster->m_transformationType = TransformationSiege;
		break;

	default: // anything 
		return false;
	}

	// TO-DO : Save duration, and obviously end
	pCaster->m_sTransformID = pType->sTransformID;
	pCaster->m_tTransformationStartTime = UNIXTIME;
	pCaster->m_sTransformationDuration = sDuration;

	pSkillCaster->StateChangeServerDirect(3, nSkillID);

	// TO-DO : Give the users ALL TEH BONUSES!!

	sData[1] = 1;
	sData[3] = sDuration;
	SendSkill();

	pSkillCaster->InsertSavedMagic(nSkillID, sDuration);
	return true;
}

bool MagicInstance::ExecuteType7()
{
	return true;
}

// Warp, resurrection, and summon spells.
bool MagicInstance::ExecuteType8()
{	
	if (pSkill == nullptr)
		return false;

	vector<CUser *> casted_member;
	_MAGIC_TYPE8* pType = g_pMain->m_Magictype8Array.GetData(nSkillID);
	if (pType == nullptr)
		return false;

	if (sTargetID == -1)
	{
		// TO-DO: Localise this loop to make it not suck (the life out of the server).
		SessionMap & sessMap = g_pMain->m_socketMgr.GetActiveSessionMap();
		foreach (itr, sessMap)
		{		
			CUser* pTUser = TO_USER(itr->second);
			if (CMagicProcess::UserRegionCheck(pSkillCaster, pTUser, pSkill, pType->sRadius, sData[0], sData[2]))
				casted_member.push_back(pTUser);
		}
		g_pMain->m_socketMgr.ReleaseLock();

		if (casted_member.empty()) 
			return false;	
	}
	else 
	{	// If the target was another single player.
		CUser* pTUser = g_pMain->GetUserPtr(sTargetID);
		if (pTUser == nullptr) 
			return false;
		
		casted_member.push_back(pTUser);
	}

	foreach (itr, casted_member)
	{
		uint8 bResult = 0;
		_OBJECT_EVENT* pEvent = nullptr;
		CUser* pTUser = *itr;
		// If we're in a home zone, we'll want the coordinates from there. Otherwise, assume our own home zone.
		_HOME_INFO* pHomeInfo = g_pMain->m_HomeArray.GetData(pTUser->GetZoneID() <= ZONE_ELMORAD ? pTUser->GetZoneID() : pTUser->GetNation());
		if (pHomeInfo == nullptr)
			return false;

		if (pType->bWarpType != 11) 
		{   // Warp or summon related: targets CANNOT be dead.
			if (pTUser->isDead()
				// Players can have their teleports blocked by skills.
				|| !pTUser->canTeleport())
				goto packet_send;
		}
		// Resurrection related: we're reviving DEAD targets.
		else if (!pTUser->isDead()) 
			goto packet_send;

		// Is target already warping?			
		if (pTUser->m_bWarp)
			goto packet_send;

		switch(pType->bWarpType) {	
			case 1:		// Send source to resurrection point.
				// Send the packet to the target.
				sData[1] = 1;
				BuildAndSendSkillPacket(*itr, true, sCasterID, (*itr)->GetID(), bOpcode, nSkillID, sData); 
				if (pTUser->GetMap() == nullptr)
					continue;

				pEvent = pTUser->GetMap()->GetObjectEvent(pTUser->m_sBind);

				if (pEvent != nullptr)
					pTUser->Warp(uint16(pEvent->fPosX * 10), uint16(pEvent->fPosZ * 10));	
				else if (pTUser->GetZoneID() <= ELMORAD) 
				{
					if (pTUser->GetNation() == KARUS)
						pTUser->Warp(uint16((pHomeInfo->KarusZoneX + myrand(0, pHomeInfo->KarusZoneLX)) * 10), uint16((pHomeInfo->KarusZoneZ + myrand(0, pHomeInfo->KarusZoneLZ)) * 10));
					else
						pTUser->Warp(uint16((pHomeInfo->ElmoZoneX + myrand(0, pHomeInfo->ElmoZoneLX)) * 10), uint16((pHomeInfo->ElmoZoneZ + + myrand(0, pHomeInfo->ElmoZoneLZ)) * 10));
				}
				else if (pTUser->GetMap()->isWarZone())
					pTUser->Warp(uint16((pHomeInfo->BattleZoneX + myrand(0, pHomeInfo->BattleZoneLX)) * 10), uint16((pHomeInfo->BattleZoneZ + myrand(0, pHomeInfo->BattleZoneLZ)) * 10));
				else if (pTUser->GetMap()->canAttackOtherNation())
					pTUser->Warp(uint16((pHomeInfo->FreeZoneX + myrand(0, pHomeInfo->FreeZoneLX)) * 10), uint16((pHomeInfo->FreeZoneZ + myrand(0, pHomeInfo->FreeZoneLZ)) * 10));
				else
					pTUser->Warp(uint16(pTUser->GetMap()->m_fInitX * 10), uint16(pTUser->GetMap()->m_fInitZ * 10));
				break;

			case 2:		// Send target to teleport point WITHIN the zone.
				// LATER!!!
				break;
			case 3:		// Send target to teleport point OUTSIDE the zone.
				// LATER!!!
				break;
			case 5:		// Send target to a hidden zone.
				// LATER!!!
				break;
			
			case 11:	// Resurrect a dead player.
			{
				// Send the packet to the target.
				sData[1] = 1;
				BuildAndSendSkillPacket(*itr, true, sCasterID, (*itr)->GetID(), bOpcode, nSkillID, sData); 

				pTUser->m_bResHpType = USER_STANDING;     // Target status is officially alive now.
				pTUser->HpChange(pTUser->m_iMaxHp, pSkillCaster);	 // Refill HP.	
				pTUser->ExpChange( pType->sExpRecover/100 );     // Increase target experience.
				
				Packet result(AG_USER_REGENE);
				result << uint16((*itr)->GetSocketID()) << uint16(pTUser->GetHealth());
				g_pMain->Send_AIServer(&result);
			} break;

			case 12:	// Summon a target within the zone.	
				// Cannot teleport users from other zones.
				if (pSkillCaster->GetZoneID() != pTUser->GetZoneID()
					// Cannot teleport ourselves.
					|| pSkillCaster == pSkillTarget)
					goto packet_send;

				// Send the packet to the target.
				sData[1] = 1;
				BuildAndSendSkillPacket(*itr, true, sCasterID, (*itr)->GetID(), bOpcode, nSkillID, sData); 
				pTUser->Warp(pSkillCaster->GetSPosX(), pSkillCaster->GetSPosZ());
				break;

			case 13:	// Summon a target outside the zone.			
				if (pSkillCaster->GetZoneID() == pTUser->GetZoneID())	  // Different zone? 
					goto packet_send;

				// Send the packet to the target.
				sData[1] = 1;
				BuildAndSendSkillPacket(*itr, true, sCasterID, (*itr)->GetID(), bOpcode, nSkillID, sData); 

				pTUser->ZoneChange(pSkillCaster->GetZoneID(), pSkillCaster->GetX(), pSkillCaster->GetZ()) ;
				pTUser->UserInOut(INOUT_RESPAWN);
				break;

			case 20:	// Teleport the source (radius) meters forward
			{
				// Calculate difference between where user is now and where they were previously
				// to figure out an orientation.
				// Should really use m_sDirection, but not sure what the value is exactly.
				float	warp_x = pTUser->GetX() - pTUser->m_oldx, 
						warp_z = pTUser->GetZ() - pTUser->m_oldz;

				// Unable to work out orientation, so we'll just fail (won't be necessary with m_sDirection).
				float	distance = sqrtf(warp_x*warp_x + warp_z*warp_z);
				if (distance == 0.0f)
					goto packet_send;

				warp_x /= distance; warp_z /= distance;
				warp_x *= pType->sRadius; warp_z *= pType->sRadius;
				warp_x += pTUser->m_oldx; warp_z += pTUser->m_oldz;

				sData[1] = 1;
				BuildAndSendSkillPacket(*itr, true, sCasterID, (*itr)->GetID(), bOpcode, nSkillID, sData); 
				pTUser->Warp(uint16(warp_x * 10), uint16(warp_z * 10));
			} break;

			case 21:	// Summon a monster within a zone.
				// LATER!!! 
				break;

			// This is used by Wild Advent (70 rogue skill) and Descent, teleport the user to the target user (Moral check to distinguish between the 2 skills)
			case 25:
				float dest_x, dest_z = 0.0f;
				// If we're not even in the same zone, I can't teleport to you!
				if (pTUser->GetZoneID() != pSkillCaster->GetZoneID()
					|| (pSkill->bMoral < MORAL_ENEMY && pSkillCaster->isHostileTo(pTUser)))
					return false;
					
				dest_x = pTUser->GetX();
				dest_z = pTUser->GetZ();

				if (pSkillCaster->isPlayer())
					TO_USER(pSkillCaster)->Warp(uint16(dest_x * 10), uint16(dest_z * 10));
				break;
		}

		bResult = 1;
		
packet_send:
		// Send the packet to the caster.
		sData[1] = bResult;
		BuildAndSendSkillPacket(pSkillCaster, true, sCasterID, (*itr)->GetID(), bOpcode, nSkillID, sData); 
	}
	return true;
}

bool MagicInstance::ExecuteType9()
{
	if (pSkill == nullptr
		// Only players can use these skills.
		|| !pSkillCaster->isPlayer())
		return false;

	_MAGIC_TYPE9* pType = g_pMain->m_Magictype9Array.GetData(nSkillID);
	if (pType == nullptr)
		return false;

	CUser * pCaster = TO_USER(pSkillCaster);
	FastGuard lock(pCaster->m_buffLock);
	Type9BuffMap & buffMap = pCaster->m_type9BuffMap;

	// Ensure this type of skill isn't already in use.
	if (buffMap.find(pType->bStateChange) != buffMap.end())
	{
		sData[1] = 0;
		SendSkillFailed();
		return false;
	}

	sData[1] = 1;
	
	if (pType->bStateChange <= 2 
		&& pCaster->canStealth())
	{
		// Cannot stealth when already stealthed.
		// This prevents us from using both types 1 and 2 (i.e. dispel on move & dispel on attack)
		// at the same time.
		if (pCaster->m_bInvisibilityType != INVIS_NONE)
		{
			sData[1] = 0;
			SendSkillFailed();
			return false;
		}

		// Invisibility perk does NOT apply when using these skills on monsters.
		if (pSkillTarget->isPlayer())
		{
			pCaster->StateChangeServerDirect(7, pType->bStateChange); // Update the client to be invisible
			buffMap.insert(std::make_pair(pType->bStateChange, _BUFF_TYPE9_INFO(nSkillID, UNIXTIME + pType->sDuration)));
		}

		// Update all players nearby to tell them we're now invisible.
		SendSkill();
	}
	else if (pType->bStateChange >= 3 && pType->bStateChange <= 4)
	{
		Packet result(WIZ_STEALTH, uint8(1));
		result << pType->sRadius;

		// If the player's in a party, apply this skill to all members of the party.
		if (pCaster->isInParty() && pType->bStateChange == 4)
		{
			_PARTY_GROUP * pParty = g_pMain->GetPartyPtr(pCaster->GetPartyID());
			if (pParty == nullptr)
				return false;

			for (int i = 0; i < MAX_PARTY_USERS; i++)
			{
				CUser *pUser = g_pMain->GetUserPtr(pParty->uid[i]);
				if (pUser == nullptr)
					continue;

				FastGuard buffLock(pUser->m_buffLock);

				// If this user already has this skill active, we don't need to reapply it.
				if (pUser->m_type9BuffMap.find(pType->bStateChange) 
					!= pUser->m_type9BuffMap.end())
					continue;

				pUser->m_type9BuffMap.insert(std::make_pair(pType->bStateChange, _BUFF_TYPE9_INFO(nSkillID, UNIXTIME + pType->sDuration)));
				pUser->Send(&result);

				// Ensure every user in the party is given the skill icon in the corner of the screen.
				BuildAndSendSkillPacket(pUser, false, sCasterID, pUser->GetID(), bOpcode, nSkillID, sData);
			}
		}
		else // not in a party, so just apply this skill to us.
		{
			buffMap.insert(std::make_pair(pType->bStateChange, _BUFF_TYPE9_INFO(nSkillID, UNIXTIME + pType->sDuration)));
			pCaster->Send(&result);

			// Ensure we are given the skill icon in the corner of the screen.
			SendSkill(false); // only update us, as only we need to know that we can see invisible players.
		}
	}

	return true;
}

short MagicInstance::GetMagicDamage(Unit *pTarget, int total_hit, int attribute)
{	
	int32 damage = 0, temp_hit = 0, righthand_damage = 0, attribute_damage = 0;
	int random = 0, total_r = 0 ;
	uint8 result; 

	if (pTarget == nullptr
		|| pSkillCaster == nullptr
		|| pTarget->isDead()
		|| pSkillCaster->isDead())
		return 0;

	// Trigger item procs
	if (pTarget->isPlayer()
		&& pSkillCaster->isPlayer())
	{
		pSkillCaster->OnAttack(pTarget, AttackTypeMagic);
		pTarget->OnDefend(pSkillCaster, AttackTypeMagic);
	}

	if (pTarget->m_bBlockMagic)
		return 0;

	int16 sMagicAmount = 0;
	if (pSkillCaster->isNPC())
	{
		result = pSkillCaster->GetHitRate(pTarget->m_fTotalHitrate / pSkillCaster->m_fTotalEvasionrate); 
	}
	else
	{
		CUser *pUser = TO_USER(pSkillCaster);
		uint8 bCha = pUser->GetStat(STAT_CHA);
		if (bCha > 86)
			sMagicAmount = bCha - 86;

		sMagicAmount += pUser->m_sMagicAttackAmount;
		total_hit *= bCha / 186;
		result = SUCCESS;
	}
		
	if (result != FAIL) 
	{
		// In case of SUCCESS.... 
		switch (attribute)
		{
			case FIRE_R: 
				total_r = (pTarget->m_sFireR + pTarget->m_bAddFireR) * pTarget->m_bPctFireR / 100;
				break;
			case COLD_R:
				total_r = (pTarget->m_sColdR + pTarget->m_bAddColdR) * pTarget->m_bPctColdR / 100;
				break;
			case LIGHTNING_R:
				total_r = (pTarget->m_sLightningR + pTarget->m_bAddLightningR) * pTarget->m_bPctLightningR / 100;
				break;
			case MAGIC_R:
				total_r = (pTarget->m_sMagicR + pTarget->m_bAddMagicR) * pTarget->m_bPctMagicR / 100;
				break;
			case DISEASE_R:
				total_r = (pTarget->m_sDiseaseR + pTarget->m_bAddDiseaseR) * pTarget->m_bPctDiseaseR / 100;
				break;
			case POISON_R:			
				total_r = (pTarget->m_sPoisonR + pTarget->m_bAddPoisonR) * pTarget->m_bPctPoisonR / 100;
				break;
		}

		total_r += pTarget->m_bResistanceBonus;
		
		if (pSkillCaster->isPlayer()) 
		{
			CUser *pUser = TO_USER(pSkillCaster);

			// double the staff's damage when using a skill of the same attribute as the staff
			_ITEM_TABLE *pRightHand = pUser->GetItemPrototype(RIGHTHAND);
			if (pRightHand != nullptr && pRightHand->isStaff()
				&& pUser->GetItemPrototype(LEFTHAND) == nullptr)
			{
				FastGuard lock(pSkillCaster->m_equippedItemBonusLock);
				righthand_damage = pRightHand->m_sDamage;
				auto itr = pSkillCaster->m_equippedItemBonuses.find(RIGHTHAND);
				if (itr != pSkillCaster->m_equippedItemBonuses.end())
				{
					auto bonusItr = itr->second.find(attribute);
					if (bonusItr != itr->second.end()) 
						attribute_damage = pRightHand->m_sDamage; 
				}
			}
			else 
			{
				righthand_damage = 0;
			}

			// Add on any elemental skill damage
			FastGuard lock(pSkillCaster->m_equippedItemBonusLock);
			foreach (itr, pSkillCaster->m_equippedItemBonuses)
			{
				uint8 bSlot = itr->first;
				foreach (bonusItr, itr->second)
				{
					uint8 bType = bonusItr->first; 
					int16 sAmount = bonusItr->second;
					int16 sTempResist = 0;

					switch (bType)
					{
						case ITEM_TYPE_FIRE: 
							sTempResist = (pTarget->m_sFireR + pTarget->m_bAddFireR) * pTarget->m_bPctFireR / 100;
							break;
						case ITEM_TYPE_COLD:
							sTempResist = (pTarget->m_sColdR + pTarget->m_bAddColdR) * pTarget->m_bPctColdR / 100;
							break;
						case ITEM_TYPE_LIGHTNING:
							sTempResist = (pTarget->m_sLightningR + pTarget->m_bAddLightningR) * pTarget->m_bPctLightningR / 100;
							break;
						case ITEM_TYPE_POISON:
							sTempResist = (pTarget->m_sPoisonR + pTarget->m_bAddPoisonR) * pTarget->m_bPctPoisonR / 100;
							break;
					}

					sTempResist += pTarget->m_bResistanceBonus;
					if (bType >= ITEM_TYPE_FIRE 
						&& bType <= ITEM_TYPE_POISON)
					{
						if (sTempResist > 200) 
							sTempResist = 200;

						// add attribute damage amount to right-hand damage instead of attribute
						// so it doesn't bother taking into account caster level (as it would with the staff attributes).
						righthand_damage += sAmount - sAmount * sTempResist / 200;
					}
				}
			}

		}

		damage = (short)(230 * total_hit / (total_r + 250));
		random = myrand(0, damage);
		damage = (short)(random * 0.3f + (damage * 0.85f)) - sMagicAmount;

		if (pSkillCaster->isNPC())
			damage -= ((3 * righthand_damage) + (3 * attribute_damage));
		else if (attribute != MAGIC_R)	// Only if the staff has an attribute.
			damage -= (short)(((righthand_damage * 0.8f) + (righthand_damage * pSkillCaster->GetLevel()) / 60) + ((attribute_damage * 0.8f) + (attribute_damage * pSkillCaster->GetLevel()) / 30));
		if (pTarget->m_bMagicDamageReduction < 100)
			damage = damage * pTarget->m_bMagicDamageReduction / 100;
	}

	// Apply boost for skills matching weather type.
	// This isn't actually used officially, but I think it's neat...
	GetWeatherDamage(damage, attribute);
	damage /= 3;

	// Implement damage cap.
	if (damage > MAX_DAMAGE)
		damage = MAX_DAMAGE;

	return (short)(damage);
}

int32 MagicInstance::GetWeatherDamage(int32 damage, int attribute)
{
	// Give a 10% damage output boost based on weather (and skill's elemental attribute)
	if ((g_pMain->m_byWeather == WEATHER_FINE && attribute == AttributeFire)
		|| (g_pMain->m_byWeather == WEATHER_RAIN && attribute == AttributeLightning)
		|| (g_pMain->m_byWeather == WEATHER_SNOW && attribute == AttributeIce))
		damage = (damage * 110) / 100;

	return damage;
}

void MagicInstance::Type6Cancel(bool bForceRemoval /*= false*/)
{
	// NPCs cannot transform.
	if (!pSkillCaster->isPlayer()
		// Are we transformed? Note: if we're relogging, and we need to remove it, we should ignore this check.
		|| (!bForceRemoval && !TO_USER(pSkillCaster)->isTransformed()))
		return;

	CUser * pUser = TO_USER(pSkillCaster);
	Packet result(WIZ_MAGIC_PROCESS, uint8(MAGIC_CANCEL_TRANSFORMATION));

	// TO-DO: Reset stat changes, recalculate stats.
	pUser->m_transformationType = TransformationNone;
	pUser->Send(&result);

	pUser->RemoveSavedMagic(pUser->m_bAbnormalType);
	pUser->StateChangeServerDirect(3, ABNORMAL_NORMAL);
}

void MagicInstance::Type9Cancel(bool bRemoveFromMap /*= true*/)
{
	if (pSkillCaster == nullptr
		|| !pSkillCaster->isPlayer())
		return;

	_MAGIC_TYPE9 * pType = g_pMain->m_Magictype9Array.GetData(nSkillID);
	if (pType == nullptr)
		return;

	uint8 bResponse = 0;
	CUser * pCaster = TO_USER(pSkillCaster);
	FastGuard lock(pCaster->m_buffLock);
	Type9BuffMap & buffMap = pCaster->m_type9BuffMap;

	// If this skill isn't already applied, there's no reason to remove it.
	if (buffMap.find(pType->bStateChange) == buffMap.end())
		return;
	
	// Remove the buff from the map
	if (bRemoveFromMap)
		buffMap.erase(pType->bStateChange);

	// Stealths
	if (pType->bStateChange <= 2 
		|| (pType->bStateChange >= 5 && pType->bStateChange < 7))
	{
		pCaster->StateChangeServerDirect(7, INVIS_NONE);
		bResponse = 91;
	}
	// Lupine etc.
	else if (pType->bStateChange >= 3 && pType->bStateChange <= 4) 
	{
		pCaster->InitializeStealth();
		bResponse = 92;
	}
	// Guardian pet related
	else if (pType->bStateChange == 7)
	{
		Packet pet(WIZ_PET, uint8(1));
		pet << uint16(1) << uint16(6);
		pCaster->Send(&pet);
		bResponse = 93;
	}

	Packet result(WIZ_MAGIC_PROCESS, uint8(MAGIC_DURATION_EXPIRED));
	result << bResponse;
	pCaster->Send(&result);
}

void MagicInstance::Type4Cancel()
{
	if (pSkill == nullptr
		|| pSkillTarget != pSkillCaster)
		return;

	_MAGIC_TYPE4* pType = g_pMain->m_Magictype4Array.GetData(nSkillID);
	if (pType == nullptr
		|| pType->isDebuff())
		return;

	if (!CMagicProcess::RemoveType4Buff(pType->bBuffType, TO_USER(pSkillCaster)))
		return;

	TO_USER(pSkillCaster)->RemoveSavedMagic(nSkillID);
}

void MagicInstance::Type3Cancel()
{
	if (pSkill == nullptr
		|| pSkillCaster != pSkillTarget)
		return;

	// Should this take only the specified skill? I'm thinking so.
	_MAGIC_TYPE3* pType = g_pMain->m_Magictype3Array.GetData(nSkillID);
	if (pType == nullptr)
		return;

	for (int i = 0; i < MAX_TYPE3_REPEAT; i++)
	{
		Unit::MagicType3 * pEffect = &pSkillCaster->m_durationalSkills[i];
		if (!pEffect->m_byUsed
			// we can only cancel healing-over-time skills
			// damage-over-time skills must remain.
			|| pEffect->m_sHPAmount <= 0)
			continue;

		pEffect->Reset();
		break;	// we can only have one healing-over-time skill active.
				// since we've found it, no need to loop through the rest of the array.
	}

	if (pSkillCaster->isPlayer())
	{
		Packet result(WIZ_MAGIC_PROCESS, uint8(MAGIC_DURATION_EXPIRED));
		result << uint8(100); // remove the healing-over-time skill.
		TO_USER(pSkillCaster)->Send(&result); 
	}

	int buff_test = 0;
	for (int j = 0; j < MAX_TYPE3_REPEAT; j++)
	{
		if (pSkillCaster->m_durationalSkills[j].m_byUsed)
		{
			buff_test++;
			break;
		}
	}

	if (buff_test == 0) 
		pSkillCaster->m_bType3Flag = false;	

	if (pSkillCaster->isPlayer() 
		&& !pSkillCaster->m_bType3Flag)
		TO_USER(pSkillCaster)->SendUserStatusUpdate(USER_STATUS_DOT, USER_STATUS_CURE);
}

void MagicInstance::Type4Extend()
{
	if (pSkill == nullptr
		// Only players can extend buffs.
		|| !pSkillCaster->isPlayer()
		// Can't use on special items.
		|| nSkillID >= 500000)
		return;

	_MAGIC_TYPE4 *pType = g_pMain->m_Magictype4Array.GetData(nSkillID);
	if (pType == nullptr
		|| pType->isDebuff())
		return;

	FastGuard lock(pSkillTarget->m_buffLock);
	Type4BuffMap::iterator itr = pSkillTarget->m_buffMap.find(pType->bBuffType);

	// Can't extend a buff that hasn't been started.
	if (itr == pSkillCaster->m_buffMap.end()
		// Can't extend a buff that's already been extended.
		|| itr->second.m_bDurationExtended)
		return;

	// Require the "Duration Item" for buff duration extension.
	// The things we must do to future-proof things...
	bool bItemFound = false;
	for (int i = SLOT_MAX; i < INVENTORY_TOTAL; i++)
	{
		_ITEM_DATA * pItem = nullptr;
		_ITEM_TABLE * pTable = TO_USER(pSkillCaster)->GetItemPrototype(i, pItem);
		if (pTable == nullptr
			|| pTable->m_bKind != 255
			|| pTable->m_iEffect1 == 0)
			continue;

		_MAGIC_TABLE * pEffect = g_pMain->m_MagictableArray.GetData(pTable->m_iEffect1);
		if (pEffect == nullptr
			|| pEffect->bMoral != MORAL_EXTEND_DURATION
			|| !TO_USER(pSkillCaster)->RobItem(i, pTable))
			continue;

		bItemFound = true;
		break;
	}

	// No "Duration Item" was found.
	if (!bItemFound)
		return;

	// Extend the duration of a buff.
	itr->second.m_tEndTime += pType->sDuration;

	// Mark the buff as extended (we cannot extend it more than once).
	itr->second.m_bDurationExtended = true;

	Packet result(WIZ_MAGIC_PROCESS, uint8(MAGIC_TYPE4_EXTEND));
	result << uint32(nSkillID);
	TO_USER(pSkillTarget)->Send(&result);
}

void MagicInstance::ReflectDamage(int32 damage, Unit * pTarget)
{
	if (pSkillCaster == nullptr
		|| pTarget == nullptr)
		return;

	if (damage < 0)
		damage *= -1;

	int16 total_resistance_caster = 0;
	int32 reflect_damage = 0;

	switch (pTarget->m_bReflectArmorType)
	{
		case FIRE_DAMAGE:
			total_resistance_caster = (pSkillCaster->m_sFireR + pSkillCaster->m_bAddFireR) * pTarget->m_bPctFireR / 100;
			reflect_damage = ((230 * damage) / (total_resistance_caster + 250)) / 100 * 25;
			pSkillCaster->HpChange(-damage, pTarget);
		break;
		
		case ICE_DAMAGE:
			total_resistance_caster = (pSkillCaster->m_sColdR + pSkillCaster->m_bAddColdR) * pTarget->m_bPctColdR / 100;
			reflect_damage = ((230 * damage) / (total_resistance_caster + 250)) / 100 * 25;
			pSkillCaster->HpChange(-damage, pTarget);
		break;

		case LIGHTNING_DAMAGE:
			total_resistance_caster = (pSkillCaster->m_sLightningR + pSkillCaster->m_bAddLightningR) * pTarget->m_bPctLightningR / 100;
			reflect_damage = ((230 * damage) / (total_resistance_caster + 250)) / 100 * 25;
			pSkillCaster->HpChange(-damage, pTarget);
		break;
	}
}

void MagicInstance::ConsumeItem()
{
	if (nConsumeItem != 0 && pSkillCaster->isPlayer())
		TO_USER(pSkillCaster)->RobItem(nConsumeItem);

	if (bInstantCast)
		CMagicProcess::RemoveType4Buff(BUFF_TYPE_INSTANT_MAGIC, pSkillCaster);
}