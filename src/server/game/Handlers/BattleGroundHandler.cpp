/*
 * Copyright (C) 2008-2016 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Common.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "ArenaTeamMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "ArenaTeam.h"
#include "BattlegroundMgr.h"
#include "Battleground.h"
#include "Chat.h"
#include "Language.h"
#include "Log.h"
#include "Player.h"
#include "Object.h"
#include "Opcodes.h"
#include "DisableMgr.h"
#include "Group.h"
#include "Battlefield.h"
#include "BattlefieldMgr.h"
#include "NPCPackets.h"

void WorldSession::HandleBattlemasterHelloOpcode(WorldPackets::NPC::Hello& hello)
{
    Creature* unit = GetPlayer()->GetMap()->GetCreature(hello.Unit);
    if (!unit)
        return;

    if (!unit->IsBattleMaster())                             // it's not battlemaster
        return;

    // Stop the npc if moving
    unit->StopMoving();

    BattlegroundTypeId bgTypeId = sBattlegroundMgr->GetBattleMasterBG(unit->GetEntry());

    if (!_player->GetBGAccessByLevel(bgTypeId))
    {
                                                            // temp, must be gossip message...
        SendNotification(LANG_YOUR_BG_LEVEL_REQ_ERROR);
        return;
    }

    sBattlegroundMgr->SendBattlegroundList(_player, hello.Unit, bgTypeId);
}

void WorldSession::HandleBattlemasterJoinOpcode(WorldPackets::Battleground::BattlemasterJoin& battlemasterJoin)
{
    bool isPremade = false;
    Group* grp = NULL;
    uint32 bgTypeId_ = battlemasterJoin.QueueID & 0xFFFF;
    if (!sBattlemasterListStore.LookupEntry(bgTypeId_))
    {
        TC_LOG_ERROR("network", "Battleground: invalid bgtype (%u) received. possible cheater? %s", bgTypeId_, _player->GetGUID().ToString().c_str());
        return;
    }

    if (DisableMgr::IsDisabledFor(DISABLE_TYPE_BATTLEGROUND, bgTypeId_, NULL))
    {
        ChatHandler(this).PSendSysMessage(LANG_BG_DISABLED);
        return;
    }

    BattlegroundTypeId bgTypeId = BattlegroundTypeId(bgTypeId_);

    // can do this, since it's battleground, not arena
    BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, 0);
    BattlegroundQueueTypeId bgQueueTypeIdRandom = BattlegroundMgr::BGQueueTypeId(BATTLEGROUND_RB, 0);

    // ignore if player is already in BG
    if (_player->InBattleground())
        return;

    // get bg instance or bg template if instance not found
    Battleground* bg = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bg)
        return;

    // expected bracket entry
    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bg->GetMapId(), _player->getLevel());
    if (!bracketEntry)
        return;

    GroupJoinBattlegroundResult err = ERR_BATTLEGROUND_NONE;

    // check queue conditions
    if (!battlemasterJoin.JoinAsGroup)
    {
        if (GetPlayer()->isUsingLfg())
        {
            WorldPackets::Battleground::BattlefieldStatusFailed battlefieldStatus;
            sBattlegroundMgr->BuildBattlegroundStatusFailed(&battlefieldStatus, bg, _player, 0, 0, ERR_LFG_CANT_USE_BATTLEGROUND);
            SendPacket(battlefieldStatus.Write());
            return;
        }

        // check Deserter debuff
        if (!_player->CanJoinToBattleground(bg))
        {
            WorldPackets::Battleground::BattlefieldStatusFailed battlefieldStatus;
            sBattlegroundMgr->BuildBattlegroundStatusFailed(&battlefieldStatus, bg, _player, 0, 0, ERR_GROUP_JOIN_BATTLEGROUND_DESERTERS);
            SendPacket(battlefieldStatus.Write());
            return;
        }

        if (_player->GetBattlegroundQueueIndex(bgQueueTypeIdRandom) < PLAYER_MAX_BATTLEGROUND_QUEUES)
        {
            // player is already in random queue
            WorldPackets::Battleground::BattlefieldStatusFailed battlefieldStatus;
            sBattlegroundMgr->BuildBattlegroundStatusFailed(&battlefieldStatus, bg, _player, 0, 0, ERR_IN_RANDOM_BG);
            SendPacket(battlefieldStatus.Write());
            return;
        }

        if (_player->InBattlegroundQueue() && bgTypeId == BATTLEGROUND_RB)
        {
            // player is already in queue, can't start random queue
            WorldPackets::Battleground::BattlefieldStatusFailed battlefieldStatus;
            sBattlegroundMgr->BuildBattlegroundStatusFailed(&battlefieldStatus, bg, _player, 0, 0, ERR_IN_NON_RANDOM_BG);
            SendPacket(battlefieldStatus.Write());
            return;
        }

        // check if already in queue
        if (_player->GetBattlegroundQueueIndex(bgQueueTypeId) < PLAYER_MAX_BATTLEGROUND_QUEUES)
            // player is already in this queue
            return;

        // check if has free queue slots
        if (!_player->HasFreeBattlegroundQueueId())
        {
            WorldPackets::Battleground::BattlefieldStatusFailed battlefieldStatus;
            sBattlegroundMgr->BuildBattlegroundStatusFailed(&battlefieldStatus, bg, _player, 0, 0, ERR_BATTLEGROUND_TOO_MANY_QUEUES);
            SendPacket(battlefieldStatus.Write());
            return;
        }

        // check Freeze debuff
        if (_player->HasAura(9454))
            return;

        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
        GroupQueueInfo* ginfo = bgQueue.AddGroup(_player, NULL, bgTypeId, bracketEntry, 0, false, isPremade, 0, 0);

        uint32 avgTime = bgQueue.GetAverageQueueWaitTime(ginfo, bracketEntry->GetBracketId());
        uint32 queueSlot = _player->AddBattlegroundQueueId(bgQueueTypeId);

        WorldPackets::Battleground::BattlefieldStatusQueued battlefieldStatus;
        sBattlegroundMgr->BuildBattlegroundStatusQueued(&battlefieldStatus, bg, _player, queueSlot, ginfo->JoinTime, avgTime, ginfo->ArenaType, false);
        SendPacket(battlefieldStatus.Write());

        TC_LOG_DEBUG("bg.battleground", "Battleground: player joined queue for bg queue type %u bg type %u, %s, NAME %s",
            bgQueueTypeId, bgTypeId, _player->GetGUID().ToString().c_str(), _player->GetName().c_str());
    }
    else
    {
        grp = _player->GetGroup();

        if (!grp)
            return;

        if (grp->GetLeaderGUID() != _player->GetGUID())
            return;

        ObjectGuid errorGuid;
        err = grp->CanJoinBattlegroundQueue(bg, bgQueueTypeId, 0, bg->GetMaxPlayersPerTeam(), false, 0, errorGuid);
        isPremade = (grp->GetMembersCount() >= bg->GetMinPlayersPerTeam());

        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
        GroupQueueInfo* ginfo = NULL;
        uint32 avgTime = 0;

        if (!err)
        {
            TC_LOG_DEBUG("bg.battleground", "Battleground: the following players are joining as group:");
            ginfo = bgQueue.AddGroup(_player, grp, bgTypeId, bracketEntry, 0, false, isPremade, 0, 0);
            avgTime = bgQueue.GetAverageQueueWaitTime(ginfo, bracketEntry->GetBracketId());
        }

        for (GroupReference* itr = grp->GetFirstMember(); itr != NULL; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member)
                continue;   // this should never happen

            if (err)
            {
                WorldPackets::Battleground::BattlefieldStatusFailed battlefieldStatus;
                sBattlegroundMgr->BuildBattlegroundStatusFailed(&battlefieldStatus, bg, _player, 0, 0, err, &errorGuid);
                member->SendDirectMessage(battlefieldStatus.Write());
                continue;
            }

            // add to queue
            uint32 queueSlot = member->AddBattlegroundQueueId(bgQueueTypeId);

            WorldPackets::Battleground::BattlefieldStatusQueued battlefieldStatus;
            sBattlegroundMgr->BuildBattlegroundStatusQueued(&battlefieldStatus, bg, member, queueSlot, ginfo->JoinTime, avgTime, ginfo->ArenaType, true);
            member->SendDirectMessage(battlefieldStatus.Write());
            TC_LOG_DEBUG("bg.battleground", "Battleground: player joined queue for bg queue type %u bg type %u, %s, NAME %s",
                bgQueueTypeId, bgTypeId, member->GetGUID().ToString().c_str(), member->GetName().c_str());
        }
        TC_LOG_DEBUG("bg.battleground", "Battleground: group end");
    }

    sBattlegroundMgr->ScheduleQueueUpdate(0, 0, bgQueueTypeId, bgTypeId, bracketEntry->GetBracketId());
}

void WorldSession::HandlePVPLogDataOpcode(WorldPackets::Battleground::PVPLogDataRequest& /*pvpLogDataRequest*/)
{
    Battleground* bg = _player->GetBattleground();
    if (!bg)
        return;

    // Prevent players from sending BuildPvpLogDataPacket in an arena except for when sent in BattleGround::EndBattleGround.
    if (bg->isArena())
        return;

    WorldPackets::Battleground::PVPLogData pvpLogData;
    bg->BuildPvPLogDataPacket(pvpLogData);
    SendPacket(pvpLogData.Write());
}

void WorldSession::HandleBattlefieldListOpcode(WorldPackets::Battleground::BattlefieldListRequest& battlefieldList)
{
    BattlemasterListEntry const* battlemasterListEntry = sBattlemasterListStore.LookupEntry(battlefieldList.ListID);
    if (!battlemasterListEntry)
    {
        TC_LOG_DEBUG("bg.battleground", "BattlegroundHandler: invalid bgtype (%u) with player (Name: %s, %s) received.", battlefieldList.ListID, _player->GetName().c_str(), _player->GetGUID().ToString().c_str());
        return;
    }

    sBattlegroundMgr->SendBattlegroundList(_player, ObjectGuid::Empty, BattlegroundTypeId(battlefieldList.ListID));
}

void WorldSession::HandleBattleFieldPortOpcode(WorldPackets::Battleground::BattlefieldPort& battlefieldPort)
{
    if (!_player->InBattlegroundQueue())
    {
        TC_LOG_DEBUG("bg.battleground", "CMSG_BATTLEFIELD_PORT %s Slot: %u, Unk: %u, Time: %u, AcceptedInvite: %u. Player not in queue!",
            GetPlayerInfo().c_str(), battlefieldPort.Ticket.Id, battlefieldPort.Ticket.Type, battlefieldPort.Ticket.Time, uint32(battlefieldPort.AcceptedInvite));
        return;
    }

    BattlegroundQueueTypeId bgQueueTypeId = _player->GetBattlegroundQueueTypeId(battlefieldPort.Ticket.Id);
    if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
    {
        TC_LOG_DEBUG("bg.battleground", "CMSG_BATTLEFIELD_PORT %s Slot: %u, Unk: %u, Time: %u, AcceptedInvite: %u. Invalid queueSlot!",
            GetPlayerInfo().c_str(), battlefieldPort.Ticket.Id, battlefieldPort.Ticket.Type, battlefieldPort.Ticket.Time, uint32(battlefieldPort.AcceptedInvite));
        return;
    }

    BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);

    //we must use temporary variable, because GroupQueueInfo pointer can be deleted in BattlegroundQueue::RemovePlayer() function
    GroupQueueInfo ginfo;
    if (!bgQueue.GetPlayerGroupInfoData(_player->GetGUID(), &ginfo))
    {
        TC_LOG_DEBUG("bg.battleground", "CMSG_BATTLEFIELD_PORT %s Slot: %u, Unk: %u, Time: %u, AcceptedInvite: %u. Player not in queue (No player Group Info)!",
            GetPlayerInfo().c_str(), battlefieldPort.Ticket.Id, battlefieldPort.Ticket.Type, battlefieldPort.Ticket.Time, uint32(battlefieldPort.AcceptedInvite));
        return;
    }
    // if action == 1, then player must have been invited to join
    if (!ginfo.IsInvitedToBGInstanceGUID && battlefieldPort.AcceptedInvite)
    {
        TC_LOG_DEBUG("bg.battleground", "CMSG_BATTLEFIELD_PORT %s Slot: %u, Unk: %u, Time: %u, AcceptedInvite: %u. Player is not invited to any bg!",
            GetPlayerInfo().c_str(), battlefieldPort.Ticket.Id, battlefieldPort.Ticket.Type, battlefieldPort.Ticket.Time, uint32(battlefieldPort.AcceptedInvite));
        return;
    }

    BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(bgQueueTypeId);
    // BGTemplateId returns BATTLEGROUND_AA when it is arena queue.
    // Do instance id search as there is no AA bg instances.
    Battleground* bg = sBattlegroundMgr->GetBattleground(ginfo.IsInvitedToBGInstanceGUID, bgTypeId == BATTLEGROUND_AA ? BATTLEGROUND_TYPE_NONE : bgTypeId);
    if (!bg)
    {
        if (battlefieldPort.AcceptedInvite)
        {
            TC_LOG_DEBUG("bg.battleground", "CMSG_BATTLEFIELD_PORT %s Slot: %u, Unk: %u, Time: %u, AcceptedInvite: %u. Cant find BG with id %u!",
                GetPlayerInfo().c_str(), battlefieldPort.Ticket.Id, battlefieldPort.Ticket.Type, battlefieldPort.Ticket.Time, uint32(battlefieldPort.AcceptedInvite), ginfo.IsInvitedToBGInstanceGUID);
            return;
        }

        bg = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        if (!bg)
        {
            TC_LOG_ERROR("network", "BattlegroundHandler: bg_template not found for type id %u.", bgTypeId);
            return;
        }
    }

    TC_LOG_DEBUG("bg.battleground", "CMSG_BATTLEFIELD_PORT %s Slot: %u, Unk: %u, Time: %u, AcceptedInvite: %u.",
        GetPlayerInfo().c_str(), battlefieldPort.Ticket.Id, battlefieldPort.Ticket.Type, battlefieldPort.Ticket.Time, uint32(battlefieldPort.AcceptedInvite));

    // get real bg type
    bgTypeId = bg->GetTypeID();

    // expected bracket entry
    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bg->GetMapId(), _player->getLevel());
    if (!bracketEntry)
        return;

    //some checks if player isn't cheating - it is not exactly cheating, but we cannot allow it
    if (battlefieldPort.AcceptedInvite && ginfo.ArenaType == 0)
    {
        //if player is trying to enter battleground (not arena!) and he has deserter debuff, we must just remove him from queue
        if (!_player->CanJoinToBattleground(bg))
        {
            //send bg command result to show nice message
            WorldPackets::Battleground::BattlefieldStatusFailed battlefieldStatus;
            sBattlegroundMgr->BuildBattlegroundStatusFailed(&battlefieldStatus, bg, _player, battlefieldPort.Ticket.Id, 0, ERR_GROUP_JOIN_BATTLEGROUND_DESERTERS);
            SendPacket(battlefieldPort.Write());
            battlefieldPort.AcceptedInvite = false;
            TC_LOG_DEBUG("bg.battleground", "Player %s (%s) has a deserter debuff, do not port him to battleground!", _player->GetName().c_str(), _player->GetGUID().ToString().c_str());
        }
        //if player don't match battleground max level, then do not allow him to enter! (this might happen when player leveled up during his waiting in queue
        if (_player->getLevel() > bg->GetMaxLevel())
        {
            TC_LOG_DEBUG("network", "Player %s (%s) has level (%u) higher than maxlevel (%u) of battleground (%u)! Do not port him to battleground!",
                _player->GetName().c_str(), _player->GetGUID().ToString().c_str(), _player->getLevel(), bg->GetMaxLevel(), bg->GetTypeID());
            battlefieldPort.AcceptedInvite = false;
        }
    }

    if (battlefieldPort.AcceptedInvite)
    {
        // check Freeze debuff
        if (_player->HasAura(9454))
            return;

        if (!_player->IsInvitedForBattlegroundQueueType(bgQueueTypeId))
            return;                                 // cheating?

        if (!_player->InBattleground())
            _player->SetBattlegroundEntryPoint();

        // resurrect the player
        if (!_player->IsAlive())
        {
            _player->ResurrectPlayer(1.0f);
            _player->SpawnCorpseBones();
        }
        // stop taxi flight at port
        if (_player->IsInFlight())
        {
            _player->GetMotionMaster()->MovementExpired();
            _player->CleanupAfterTaxiFlight();
        }

        WorldPackets::Battleground::BattlefieldStatusActive battlefieldStatus;
        sBattlegroundMgr->BuildBattlegroundStatusActive(&battlefieldStatus, bg, _player, battlefieldPort.Ticket.Id, _player->GetBattlegroundQueueJoinTime(bgQueueTypeId), bg->GetArenaType());
        SendPacket(battlefieldStatus.Write());

        // remove battleground queue status from BGmgr
        bgQueue.RemovePlayer(_player->GetGUID(), false);
        // this is still needed here if battleground "jumping" shouldn't add deserter debuff
        // also this is required to prevent stuck at old battleground after SetBattlegroundId set to new
        if (Battleground* currentBg = _player->GetBattleground())
            currentBg->RemovePlayerAtLeave(_player->GetGUID(), false, true);

        // set the destination instance id
        _player->SetBattlegroundId(bg->GetInstanceID(), bgTypeId);
        // set the destination team
        _player->SetBGTeam(ginfo.Team);

        // bg->HandleBeforeTeleportToBattleground(_player);
        sBattlegroundMgr->SendToBattleground(_player, ginfo.IsInvitedToBGInstanceGUID, bgTypeId);
        // add only in HandleMoveWorldPortAck()
        // bg->AddPlayer(_player, team);
        TC_LOG_DEBUG("bg.battleground", "Battleground: player %s (%s) joined battle for bg %u, bgtype %u, queue type %u.", _player->GetName().c_str(), _player->GetGUID().ToString().c_str(), bg->GetInstanceID(), bg->GetTypeID(), bgQueueTypeId);
    }
    else // leave queue
    {
        // if player leaves rated arena match before match start, it is counted as he played but he lost
        if (ginfo.IsRated && ginfo.IsInvitedToBGInstanceGUID)
        {
            ArenaTeam* at = sArenaTeamMgr->GetArenaTeamById(ginfo.Team);
            if (at)
            {
                TC_LOG_DEBUG("bg.battleground", "UPDATING memberLost's personal arena rating for %s by opponents rating: %u, because he has left queue!", _player->GetGUID().ToString().c_str(), ginfo.OpponentsTeamRating);
                at->MemberLost(_player, ginfo.OpponentsMatchmakerRating);
                at->SaveToDB();
            }
        }

        WorldPackets::Battleground::BattlefieldStatusNone battlefieldStatus;
        battlefieldStatus.Ticket = battlefieldPort.Ticket;
        SendPacket(battlefieldStatus.Write());

        _player->RemoveBattlegroundQueueId(bgQueueTypeId);  // must be called this way, because if you move this call to queue->removeplayer, it causes bugs
        bgQueue.RemovePlayer(_player->GetGUID(), true);
        // player left queue, we should update it - do not update Arena Queue
        if (!ginfo.ArenaType)
            sBattlegroundMgr->ScheduleQueueUpdate(ginfo.ArenaMatchmakerRating, ginfo.ArenaType, bgQueueTypeId, bgTypeId, bracketEntry->GetBracketId());

        TC_LOG_DEBUG("bg.battleground", "Battleground: player %s (%s) left queue for bgtype %u, queue type %u.", _player->GetName().c_str(), _player->GetGUID().ToString().c_str(), bg->GetTypeID(), bgQueueTypeId);
    }
}

void WorldSession::HandleBattlefieldLeaveOpcode(WorldPackets::Battleground::BattlefieldLeave& /*battlefieldLeave*/)
{
    // not allow leave battleground in combat
    if (_player->IsInCombat())
        if (Battleground* bg = _player->GetBattleground())
            if (bg->GetStatus() != STATUS_WAIT_LEAVE)
                return;

    _player->LeaveBattleground();
}

void WorldSession::HandleRequestBattlefieldStatusOpcode(WorldPackets::Battleground::RequestBattlefieldStatus& /*requestBattlefieldStatus*/)
{
    // we must update all queues here
    Battleground* bg = NULL;
    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
    {
        BattlegroundQueueTypeId bgQueueTypeId = _player->GetBattlegroundQueueTypeId(i);
        if (!bgQueueTypeId)
            continue;
        BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(bgQueueTypeId);
        uint8 arenaType = BattlegroundMgr::BGArenaType(bgQueueTypeId);
        if (bgTypeId == _player->GetBattlegroundTypeId())
        {
            bg = _player->GetBattleground();
            //i cannot check any variable from player class because player class doesn't know if player is in 2v2 / 3v3 or 5v5 arena
            //so i must use bg pointer to get that information
            if (bg && bg->GetArenaType() == arenaType)
            {
                WorldPackets::Battleground::BattlefieldStatusActive battlefieldStatus;
                sBattlegroundMgr->BuildBattlegroundStatusActive(&battlefieldStatus, bg, _player, i, _player->GetBattlegroundQueueJoinTime(bgQueueTypeId), arenaType);
                SendPacket(battlefieldStatus.Write());
                continue;
            }
        }

        //we are sending update to player about queue - he can be invited there!
        //get GroupQueueInfo for queue status
        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
        GroupQueueInfo ginfo;
        if (!bgQueue.GetPlayerGroupInfoData(_player->GetGUID(), &ginfo))
            continue;
        if (ginfo.IsInvitedToBGInstanceGUID)
        {
            bg = sBattlegroundMgr->GetBattleground(ginfo.IsInvitedToBGInstanceGUID, bgTypeId);
            if (!bg)
                continue;


            WorldPackets::Battleground::BattlefieldStatusNeedConfirmation battlefieldStatus;
            sBattlegroundMgr->BuildBattlegroundStatusNeedConfirmation(&battlefieldStatus, bg, _player, i, _player->GetBattlegroundQueueJoinTime(bgQueueTypeId), getMSTimeDiff(getMSTime(), ginfo.RemoveInviteTime), arenaType);
            SendPacket(battlefieldStatus.Write());
        }
        else
        {
            bg = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
            if (!bg)
                continue;

            // expected bracket entry
            PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bg->GetMapId(), _player->getLevel());
            if (!bracketEntry)
                continue;

            uint32 avgTime = bgQueue.GetAverageQueueWaitTime(&ginfo, bracketEntry->GetBracketId());
            WorldPackets::Battleground::BattlefieldStatusQueued battlefieldStatus;
            sBattlegroundMgr->BuildBattlegroundStatusQueued(&battlefieldStatus, bg, _player, i, _player->GetBattlegroundQueueJoinTime(bgQueueTypeId), avgTime, arenaType, ginfo.Players.size() > 1);
            SendPacket(battlefieldStatus.Write());
        }
    }
}

void WorldSession::HandleBattlemasterJoinArena(WorldPacket& recvData)
{
    uint8 arenaslot;                                        // 2v2, 3v3 or 5v5

    recvData >> arenaslot;

    // ignore if we already in BG or BG queue
    if (_player->InBattleground())
        return;

    uint32 arenaRating = 0;
    uint32 matchmakerRating = 0;

    uint8 arenatype = ArenaTeam::GetTypeBySlot(arenaslot);

    //check existance
    Battleground* bg = sBattlegroundMgr->GetBattlegroundTemplate(BATTLEGROUND_AA);
    if (!bg)
    {
        TC_LOG_ERROR("network", "Battleground: template bg (all arenas) not found");
        return;
    }

    if (DisableMgr::IsDisabledFor(DISABLE_TYPE_BATTLEGROUND, BATTLEGROUND_AA, NULL))
    {
        ChatHandler(this).PSendSysMessage(LANG_ARENA_DISABLED);
        return;
    }

    BattlegroundTypeId bgTypeId = bg->GetTypeID();
    BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, arenatype);
    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bg->GetMapId(), _player->getLevel());
    if (!bracketEntry)
        return;

    GroupJoinBattlegroundResult err = ERR_BATTLEGROUND_NONE;

    Group* grp = _player->GetGroup();
    // no group found, error
    if (!grp)
        return;
    if (grp->GetLeaderGUID() != _player->GetGUID())
        return;

    uint32 ateamId = _player->GetArenaTeamId(arenaslot);
    // check real arenateam existence only here (if it was moved to group->CanJoin .. () then we would ahve to get it twice)
    ArenaTeam* at = sArenaTeamMgr->GetArenaTeamById(ateamId);
    if (!at)
    {
        _player->GetSession()->SendNotInArenaTeamPacket(arenatype);
        return;
    }

    // get the team rating for queueing
    arenaRating = at->GetRating();
    matchmakerRating = at->GetAverageMMR(grp);
    // the arenateam id must match for everyone in the group

    if (arenaRating <= 0)
        arenaRating = 1;

    BattlegroundQueue &bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);

    uint32 avgTime = 0;
    GroupQueueInfo* ginfo = NULL;

    ObjectGuid errorGuid;
    err = grp->CanJoinBattlegroundQueue(bg, bgQueueTypeId, arenatype, arenatype, true, arenaslot, errorGuid);
    if (!err)
    {
        TC_LOG_DEBUG("bg.battleground", "Battleground: arena team id %u, leader %s queued with matchmaker rating %u for type %u", _player->GetArenaTeamId(arenaslot), _player->GetName().c_str(), matchmakerRating, arenatype);

        ginfo = bgQueue.AddGroup(_player, grp, bgTypeId, bracketEntry, arenatype, true, false, arenaRating, matchmakerRating, ateamId);
        avgTime = bgQueue.GetAverageQueueWaitTime(ginfo, bracketEntry->GetBracketId());
    }

    for (GroupReference* itr = grp->GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member)
            continue;

        if (err)
        {
            WorldPackets::Battleground::BattlefieldStatusFailed battlefieldStatus;
            sBattlegroundMgr->BuildBattlegroundStatusFailed(&battlefieldStatus, bg, _player, 0, arenatype, err, &errorGuid);
            member->SendDirectMessage(battlefieldStatus.Write());
            continue;
        }

        // add to queue
        uint32 queueSlot = member->AddBattlegroundQueueId(bgQueueTypeId);

        WorldPackets::Battleground::BattlefieldStatusQueued battlefieldStatus;
        sBattlegroundMgr->BuildBattlegroundStatusQueued(&battlefieldStatus, bg, member, queueSlot, ginfo->JoinTime, avgTime, arenatype, true);
        member->SendDirectMessage(battlefieldStatus.Write());

        TC_LOG_DEBUG("bg.battleground", "Battleground: player joined queue for arena as group bg queue type %u bg type %u, %s, NAME %s", bgQueueTypeId, bgTypeId, member->GetGUID().ToString().c_str(), member->GetName().c_str());
    }

    sBattlegroundMgr->ScheduleQueueUpdate(matchmakerRating, arenatype, bgQueueTypeId, bgTypeId, bracketEntry->GetBracketId());
}

void WorldSession::HandleReportPvPAFK(WorldPackets::Battleground::ReportPvPPlayerAFK& reportPvPPlayerAFK)
{
    Player* reportedPlayer = ObjectAccessor::FindPlayer(reportPvPPlayerAFK.Offender);
    if (!reportedPlayer)
    {
        TC_LOG_DEBUG("bg.battleground", "WorldSession::HandleReportPvPAFK: player not found");
        return;
    }

    TC_LOG_DEBUG("bg.battleground", "WorldSession::HandleReportPvPAFK: %s reported %s", _player->GetName().c_str(), reportedPlayer->GetName().c_str());

    reportedPlayer->ReportedAfkBy(_player);
}

void WorldSession::HandleRequestRatedBattlefieldInfo(WorldPacket& recvData)
{
    uint8 unk;
    recvData >> unk;

    TC_LOG_DEBUG("bg.battleground", "WorldSession::HandleRequestRatedBattlefieldInfo: unk = %u", unk);

    /// @Todo: perfome research in this case
    /// The unk fields are related to arenas
    WorldPacket data(SMSG_RATED_BATTLEFIELD_INFO, 72);
    data << uint32(0);      // BgWeeklyWins20vs20
    data << uint32(0);      // BgWeeklyPlayed20vs20
    data << uint32(0);      // BgWeeklyPlayed15vs15
    data << uint32(0);
    data << uint32(0);      // BgWeeklyWins10vs10
    data << uint32(0);
    data << uint32(0);
    data << uint32(0);
    data << uint32(0);      // BgWeeklyWins15vs15
    data << uint32(0);
    data << uint32(0);
    data << uint32(0);
    data << uint32(0);
    data << uint32(0);
    data << uint32(0);
    data << uint32(0);      // BgWeeklyPlayed10vs10
    data << uint32(0);
    data << uint32(0);

    SendPacket(&data);
}

void WorldSession::HandleGetPVPOptionsEnabled(WorldPackets::Battleground::GetPVPOptionsEnabled& /*getPvPOptionsEnabled*/)
{
    // This packet is completely irrelevant, it triggers PVP_TYPES_ENABLED lua event but that is not handled in interface code as of 6.1.2
    WorldPackets::Battleground::PVPOptionsEnabled pvpOptionsEnabled;
    pvpOptionsEnabled.WargameArenas = false;
    pvpOptionsEnabled.RatedArenas = false;
    pvpOptionsEnabled.WargameBattlegrounds = false;
    pvpOptionsEnabled.ArenaSkirmish = false;
    pvpOptionsEnabled.PugBattlegrounds = true;
    pvpOptionsEnabled.RatedBattlegrounds = false;
    SendPacket(pvpOptionsEnabled.Write());
}

void WorldSession::HandleRequestPvpReward(WorldPacket& /*recvData*/)
{
    _player->SendPvpRewards();
}

void WorldSession::HandleAreaSpiritHealerQueryOpcode(WorldPackets::Battleground::AreaSpiritHealerQuery& areaSpiritHealerQuery)
{
    Creature* unit = GetPlayer()->GetMap()->GetCreature(areaSpiritHealerQuery.HealerGuid);
    if (!unit)
        return;

    if (!unit->IsSpiritService())                            // it's not spirit service
        return;

    if (Battleground* bg = _player->GetBattleground())
        sBattlegroundMgr->SendAreaSpiritHealerQueryOpcode(_player, bg, areaSpiritHealerQuery.HealerGuid);

    if (Battlefield* bf = sBattlefieldMgr->GetBattlefieldToZoneId(_player->GetZoneId()))
        bf->SendAreaSpiritHealerQueryOpcode(_player, areaSpiritHealerQuery.HealerGuid);
}

void WorldSession::HandleAreaSpiritHealerQueueOpcode(WorldPackets::Battleground::AreaSpiritHealerQueue& areaSpiritHealerQueue)
{
    Creature* unit = GetPlayer()->GetMap()->GetCreature(areaSpiritHealerQueue.HealerGuid);
    if (!unit)
        return;

    if (!unit->IsSpiritService())                            // it's not spirit service
        return;

    if (Battleground* bg = _player->GetBattleground())
        bg->AddPlayerToResurrectQueue(areaSpiritHealerQueue.HealerGuid, _player->GetGUID());

    if (Battlefield* bf = sBattlefieldMgr->GetBattlefieldToZoneId(_player->GetZoneId()))
        bf->AddPlayerToResurrectQueue(areaSpiritHealerQueue.HealerGuid, _player->GetGUID());
}

void WorldSession::HandleHearthAndResurrect(WorldPackets::Battleground::HearthAndResurrect& /*hearthAndResurrect*/)
{
    if (_player->IsInFlight())
        return;

    if (/*Battlefield* bf = */sBattlefieldMgr->GetBattlefieldToZoneId(_player->GetZoneId()))
    {
        // bf->PlayerAskToLeave(_player); FIXME
        return;
    }

    AreaTableEntry const* atEntry = GetAreaEntryByAreaID(_player->GetAreaId());
    if (!atEntry || !(atEntry->Flags[0] & AREA_FLAG_CAN_HEARTH_AND_RESURRECT))
        return;

    _player->BuildPlayerRepop();
    _player->ResurrectPlayer(1.0f);
    _player->TeleportTo(_player->m_homebindMapId, _player->m_homebindX, _player->m_homebindY, _player->m_homebindZ, _player->GetOrientation());
}
