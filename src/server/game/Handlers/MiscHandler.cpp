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
#include "Language.h"
#include "DatabaseEnv.h"
#include "WorldPacket.h"
#include "Opcodes.h"
#include "Log.h"
#include "Player.h"
#include "GossipDef.h"
#include "World.h"
#include "ObjectMgr.h"
#include "GuildMgr.h"
#include "WorldSession.h"
#include "Chat.h"
#include "zlib.h"
#include "ObjectAccessor.h"
#include "Object.h"
#include "Battleground.h"
#include "OutdoorPvP.h"
#include "AccountMgr.h"
#include "DBCEnums.h"
#include "ScriptMgr.h"
#include "MapManager.h"
#include "Group.h"
#include "Spell.h"
#include "SpellPackets.h"
#include "CharacterPackets.h"
#include "ClientConfigPackets.h"
#include "MiscPackets.h"
#include "AchievementPackets.h"
#include "WhoPackets.h"
#include "InstancePackets.h"

void WorldSession::HandleRepopRequest(WorldPackets::Misc::RepopRequest& /*packet*/)
{
    if (GetPlayer()->IsAlive() || GetPlayer()->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        return;

    if (GetPlayer()->HasAuraType(SPELL_AURA_PREVENT_RESURRECTION))
        return; // silently return, client should display the error by itself

    // the world update order is sessions, players, creatures
    // the netcode runs in parallel with all of these
    // creatures can kill players
    // so if the server is lagging enough the player can
    // release spirit after he's killed but before he is updated
    if (GetPlayer()->getDeathState() == JUST_DIED)
    {
        TC_LOG_DEBUG("network", "HandleRepopRequestOpcode: got request after player %s(%s) was killed and before he was updated",
            GetPlayer()->GetName().c_str(), GetPlayer()->GetGUID().ToString().c_str());
        GetPlayer()->KillPlayer();
    }

    //this is spirit release confirm?
    GetPlayer()->RemovePet(NULL, PET_SAVE_NOT_IN_SLOT, true);
    GetPlayer()->BuildPlayerRepop();
    GetPlayer()->RepopAtGraveyard();
}

void WorldSession::HandleWhoOpcode(WorldPackets::Who::WhoRequestPkt& whoRequest)
{
    WorldPackets::Who::WhoRequest& request = whoRequest.Request;

    TC_LOG_DEBUG("network", "WorldSession::HandleWhoOpcode: MinLevel: %u, MaxLevel: %u, Name: %s (VirtualRealmName: %s), Guild: %s (GuildVirtualRealmName: %s), RaceFilter: %d, ClassFilter: %d, Areas: " SZFMTD ", Words: " SZFMTD ".",
        request.MinLevel, request.MaxLevel, request.Name.c_str(), request.VirtualRealmName.c_str(), request.Guild.c_str(), request.GuildVirtualRealmName.c_str(),
        request.RaceFilter, request.ClassFilter, whoRequest.Areas.size(), request.Words.size());

    // zones count, client limit = 10 (2.0.10)
    // can't be received from real client or broken packet
    if (whoRequest.Areas.size() > 10)
        return;

    // user entered strings count, client limit=4 (checked on 2.0.10)
    // can't be received from real client or broken packet
    if (request.Words.size() > 4)
        return;

    /// @todo: handle following packet values
    /// VirtualRealmNames
    /// ShowEnemies
    /// ShowArenaPlayers
    /// ExactName
    /// ServerInfo

    std::vector<std::wstring> wWords;
    wWords.resize(request.Words.size());
    for (size_t i = 0; i < request.Words.size(); ++i)
    {
        TC_LOG_DEBUG("network", "WorldSession::HandleWhoOpcode: Word: %s", request.Words[i].Word.c_str());

        // user entered string, it used as universal search pattern(guild+player name)?
        if (!Utf8toWStr(request.Words[i].Word, wWords[i]))
            continue;

        wstrToLower(wWords[i]);
    }

    std::wstring wPlayerName;
    std::wstring wGuildName;

    if (!(Utf8toWStr(request.Name, wPlayerName) && Utf8toWStr(request.Guild, wGuildName)))
        return;

    wstrToLower(wPlayerName);
    wstrToLower(wGuildName);

    // client send in case not set max level value 100 but Trinity supports 255 max level,
    // update it to show GMs with characters after 100 level
    if (whoRequest.Request.MaxLevel >= MAX_LEVEL)
        whoRequest.Request.MaxLevel = STRONG_MAX_LEVEL;

    uint32 team = _player->GetTeam();

    uint32 gmLevelInWhoList  = sWorld->getIntConfig(CONFIG_GM_LEVEL_IN_WHO_LIST);

    WorldPackets::Who::WhoResponsePkt response;

    boost::shared_lock<boost::shared_mutex> lock(*HashMapHolder<Player>::GetLock());

    HashMapHolder<Player>::MapType const& m = ObjectAccessor::GetPlayers();
    for (HashMapHolder<Player>::MapType::const_iterator itr = m.begin(); itr != m.end(); ++itr)
    {
        Player* target = itr->second;
        // player can see member of other team only if has RBAC_PERM_TWO_SIDE_WHO_LIST
        if (target->GetTeam() != team && !HasPermission(rbac::RBAC_PERM_TWO_SIDE_WHO_LIST))
            continue;

        // player can see MODERATOR, GAME MASTER, ADMINISTRATOR only if has RBAC_PERM_WHO_SEE_ALL_SEC_LEVELS
        if (target->GetSession()->GetSecurity() > AccountTypes(gmLevelInWhoList) && !HasPermission(rbac::RBAC_PERM_WHO_SEE_ALL_SEC_LEVELS))
            continue;

        // do not process players which are not in world
        if (!target->IsInWorld())
            continue;

        // check if target is globally visible for player
        if (!target->IsVisibleGloballyFor(_player))
            continue;

        // check if target's level is in level range
        uint8 lvl = target->getLevel();
        if (lvl < request.MinLevel || lvl > request.MaxLevel)
            continue;

        // check if class matches classmask
        if (request.ClassFilter >= 0 && !(request.ClassFilter & (1 << target->getClass())))
            continue;

        // check if race matches racemask
        if (request.RaceFilter >= 0 && !(request.RaceFilter & (1 << target->getRace())))
            continue;

        if (!whoRequest.Areas.empty())
        {
            if (std::find(whoRequest.Areas.begin(), whoRequest.Areas.end(), target->GetZoneId()) == whoRequest.Areas.end())
                continue;
        }

        std::wstring wTargetName;

        if (!Utf8toWStr(target->GetName(), wTargetName))
            continue;

        wstrToLower(wTargetName);

        if (!wPlayerName.empty() && wTargetName.find(wPlayerName) == std::wstring::npos)
            continue;

        Guild* targetGuild = target->GetGuild();
        std::wstring wTargetGuildName;

        if (!Utf8toWStr(targetGuild ? targetGuild->GetName() : "", wTargetGuildName))
            continue;

        wstrToLower(wTargetGuildName);

        if (!wGuildName.empty() && wTargetGuildName.find(wGuildName) == std::wstring::npos)
            continue;

        if (!wWords.empty())
        {
            std::string aName;
            if (AreaTableEntry const* areaEntry = GetAreaEntryByAreaID(target->GetZoneId()))
                aName = areaEntry->AreaName_lang;

            bool show = false;
            for (size_t i = 0; i < wWords.size(); ++i)
            {
                if (!wWords[i].empty())
                {
                    if (wTargetName.find(wWords[i]) != std::wstring::npos ||
                        wTargetGuildName.find(wWords[i]) != std::wstring::npos ||
                        Utf8FitTo(aName, wWords[i]))
                    {
                        show = true;
                        break;
                    }
                }
            }

            if (!show)
                continue;
        }

        WorldPackets::Who::WhoEntry whoEntry;
        if (!whoEntry.PlayerData.Initialize(target->GetGUID(), target))
            continue;

        if (targetGuild)
        {
            whoEntry.GuildGUID = targetGuild->GetGUID();
            whoEntry.GuildVirtualRealmAddress = GetVirtualRealmAddress();
            whoEntry.GuildName = targetGuild->GetName();
        }

        whoEntry.AreaID = target->GetZoneId();
        whoEntry.IsGM = target->IsGameMaster();

        response.Response.Entries.push_back(whoEntry);

        // 50 is maximum player count sent to client - can be overridden
        // through config, but is unstable
        if (response.Response.Entries.size() >= sWorld->getIntConfig(CONFIG_MAX_WHO))
            break;
    }

    SendPacket(response.Write());
}

void WorldSession::HandleLogoutRequestOpcode(WorldPackets::Character::LogoutRequest& /*logoutRequest*/)
{
    ObjectGuid lguid = GetPlayer()->GetLootGUID();
    if (!lguid.IsEmpty())
        DoLootRelease(lguid);

    bool instantLogout = (GetPlayer()->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING) && !GetPlayer()->IsInCombat()) ||
                         GetPlayer()->IsInFlight() || HasPermission(rbac::RBAC_PERM_INSTANT_LOGOUT);

    /// TODO: Possibly add RBAC permission to log out in combat
    bool canLogoutInCombat = GetPlayer()->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING);

    uint32 reason = 0;
    if (GetPlayer()->IsInCombat() && !canLogoutInCombat)
        reason = 1;
    else if (GetPlayer()->IsFalling())
        reason = 3;                                         // is jumping or falling
    else if (GetPlayer()->duel || GetPlayer()->HasAura(9454)) // is dueling or frozen by GM via freeze command
        reason = 2;                                         // FIXME - Need the correct value

    WorldPackets::Character::LogoutResponse logoutResponse;
    logoutResponse.LogoutResult = reason;
    logoutResponse.Instant = instantLogout;
    SendPacket(logoutResponse.Write());

    if (reason)
    {
        SetLogoutStartTime(0);
        return;
    }

    // instant logout in taverns/cities or on taxi or for admins, gm's, mod's if its enabled in worldserver.conf
    if (instantLogout)
    {
        LogoutPlayer(true);
        return;
    }

    // not set flags if player can't free move to prevent lost state at logout cancel
    if (GetPlayer()->CanFreeMove())
    {
        if (GetPlayer()->GetStandState() == UNIT_STAND_STATE_STAND)
            GetPlayer()->SetStandState(UNIT_STAND_STATE_SIT);
        GetPlayer()->SetRooted(true);
        GetPlayer()->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED);
    }

    SetLogoutStartTime(time(NULL));
}

void WorldSession::HandleLogoutCancelOpcode(WorldPackets::Character::LogoutCancel& /*logoutCancel*/)
{
    // Player have already logged out serverside, too late to cancel
    if (!GetPlayer())
        return;

    SetLogoutStartTime(0);

    SendPacket(WorldPackets::Character::LogoutCancelAck().Write());

    // not remove flags if can't free move - its not set in Logout request code.
    if (GetPlayer()->CanFreeMove())
    {
        //!we can move again
        GetPlayer()->SetRooted(false);

        //! Stand Up
        GetPlayer()->SetStandState(UNIT_STAND_STATE_STAND);

        //! DISABLE_ROTATE
        GetPlayer()->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED);
    }

    TC_LOG_DEBUG("network", "WORLD: Sent SMSG_LOGOUT_CANCEL_ACK Message");
}

void WorldSession::HandleTogglePvP(WorldPackets::Misc::TogglePvP& /*packet*/)
{
    bool inPvP = GetPlayer()->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP);

    GetPlayer()->ApplyModFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP, !inPvP);
    GetPlayer()->ApplyModFlag(PLAYER_FLAGS, PLAYER_FLAGS_PVP_TIMER, inPvP);

    if (GetPlayer()->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP))
    {
        if (!GetPlayer()->IsPvP() || GetPlayer()->pvpInfo.EndTimer)
            GetPlayer()->UpdatePvP(true, true);
    }
    else
    {
        if (!GetPlayer()->pvpInfo.IsHostile && GetPlayer()->IsPvP())
            GetPlayer()->pvpInfo.EndTimer = time(nullptr); // start toggle-off
    }
}

void WorldSession::HandleSetPvP(WorldPackets::Misc::SetPvP& packet)
{
    GetPlayer()->ApplyModFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP, packet.EnablePVP);
    GetPlayer()->ApplyModFlag(PLAYER_FLAGS, PLAYER_FLAGS_PVP_TIMER, !packet.EnablePVP);

    if (GetPlayer()->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP))
    {
        if (!GetPlayer()->IsPvP() || GetPlayer()->pvpInfo.EndTimer)
            GetPlayer()->UpdatePvP(true, true);
    }
    else
    {
        if (!GetPlayer()->pvpInfo.IsHostile && GetPlayer()->IsPvP())
            GetPlayer()->pvpInfo.EndTimer = time(nullptr); // start set-off
    }
}

void WorldSession::HandlePortGraveyard(WorldPackets::Misc::PortGraveyard& /*packet*/)
{
    if (GetPlayer()->IsAlive() || !GetPlayer()->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        return;
    GetPlayer()->RepopAtGraveyard();
}

void WorldSession::HandleRequestCemeteryList(WorldPackets::Misc::RequestCemeteryList& /*packet*/)
{
    uint32 zoneId = _player->GetZoneId();
    uint32 team = _player->GetTeam();

    std::vector<uint32> graveyardIds;
    auto range = sObjectMgr->GraveYardStore.equal_range(zoneId);

    for (auto it = range.first; it != range.second && graveyardIds.size() < 16; ++it) // client max
    {
        if (it->second.team == 0 || it->second.team == team)
            graveyardIds.push_back(it->first);
    }

    if (graveyardIds.empty())
    {
        TC_LOG_DEBUG("network", "No graveyards found for zone %u for %s (team %u) in CMSG_REQUEST_CEMETERY_LIST",
            zoneId, _player->GetGUID().ToString().c_str(), team);
        return;
    }

    WorldPackets::Misc::RequestCemeteryListResponse packet;
    packet.IsGossipTriggered = false;
    packet.CemeteryID.reserve(graveyardIds.size());

    for (uint32 id : graveyardIds)
        packet.CemeteryID.push_back(id);

    SendPacket(packet.Write());
}

void WorldSession::HandleSetSelectionOpcode(WorldPackets::Misc::SetSelection& packet)
{
    _player->SetSelection(packet.Selection);
}

void WorldSession::HandleStandStateChangeOpcode(WorldPackets::Misc::StandStateChange& packet)
{
    _player->SetStandState(packet.StandState);
}

void WorldSession::HandleReclaimCorpse(WorldPackets::Misc::ReclaimCorpse& /*packet*/)
{
    if (_player->IsAlive())
        return;

    // do not allow corpse reclaim in arena
    if (_player->InArena())
        return;

    // body not released yet
    if (!_player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        return;

    Corpse* corpse = _player->GetCorpse();
    if (!corpse)
        return;

    // prevent resurrect before 30-sec delay after body release not finished
    if (time_t(corpse->GetGhostTime() + _player->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP)) > time_t(time(NULL)))
        return;

    if (!corpse->IsWithinDistInMap(_player, CORPSE_RECLAIM_RADIUS, true))
        return;

    // resurrect
    _player->ResurrectPlayer(_player->InBattleground() ? 1.0f : 0.5f);

    // spawn bones
    _player->SpawnCorpseBones();
}

void WorldSession::HandleResurrectResponse(WorldPackets::Misc::ResurrectResponse& packet)
{
    if (GetPlayer()->IsAlive())
        return;

    if (packet.Response != 0) // Accept = 0 Decline = 1 Timeout = 2
    {
        GetPlayer()->ClearResurrectRequestData();           // reject
        return;
    }

    if (!GetPlayer()->IsResurrectRequestedBy(packet.Resurrecter))
        return;

    GetPlayer()->ResurrectUsingRequestData();
}

void WorldSession::HandleAreaTriggerOpcode(WorldPackets::Misc::AreaTrigger& packet)
{
    Player* player = GetPlayer();
    if (player->IsInFlight())
    {
        TC_LOG_DEBUG("network", "HandleAreaTriggerOpcode: Player '%s' (%s) in flight, ignore Area Trigger ID:%u",
            player->GetName().c_str(), player->GetGUID().ToString().c_str(), packet.AreaTriggerID);
        return;
    }

    AreaTriggerEntry const* atEntry = sAreaTriggerStore.LookupEntry(packet.AreaTriggerID);
    if (!atEntry)
    {
        TC_LOG_DEBUG("network", "HandleAreaTriggerOpcode: Player '%s' (%s) send unknown (by DBC) Area Trigger ID:%u",
            player->GetName().c_str(), player->GetGUID().ToString().c_str(), packet.AreaTriggerID);
        return;
    }

    if (!player->IsInAreaTriggerRadius(atEntry))
    {
        TC_LOG_DEBUG("network", "HandleAreaTriggerOpcode: Player '%s' (%s) too far, ignore Area Trigger ID: %u",
            player->GetName().c_str(), player->GetGUID().ToString().c_str(), packet.AreaTriggerID);
        return;
    }

    if (player->isDebugAreaTriggers)
        ChatHandler(player->GetSession()).PSendSysMessage(packet.Entered ? LANG_DEBUG_AREATRIGGER_ENTERED : LANG_DEBUG_AREATRIGGER_LEFT, packet.AreaTriggerID);

    if (sScriptMgr->OnAreaTrigger(player, atEntry, packet.Entered))
        return;

    if (player->IsAlive())
    {
        if (uint32 questId = sObjectMgr->GetQuestForAreaTrigger(packet.AreaTriggerID))
        {
            Quest const* qInfo = sObjectMgr->GetQuestTemplate(questId);
            if (qInfo && player->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
            {
                for (uint8 j = 0; j < qInfo->Objectives.size(); ++j)
                {
                    if (qInfo->Objectives[j].Type == QUEST_OBJECTIVE_AREATRIGGER)
                    {
                        player->SetQuestObjectiveData(qInfo, j, int32(true));
                        break;
                    }
                }

                if (player->CanCompleteQuest(questId))
                    player->CompleteQuest(questId);
            }
        }
    }

    if (sObjectMgr->IsTavernAreaTrigger(packet.AreaTriggerID))
    {
        // set resting flag we are in the inn
        player->SetRestFlag(REST_FLAG_IN_TAVERN, atEntry->ID);

        if (sWorld->IsFFAPvPRealm())
            player->RemoveByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_FFA_PVP);

        return;
    }

    if (Battleground* bg = player->GetBattleground())
        if (bg->GetStatus() == STATUS_IN_PROGRESS)
            bg->HandleAreaTrigger(player, packet.AreaTriggerID, packet.Entered);

    if (OutdoorPvP* pvp = player->GetOutdoorPvP())
        if (pvp->HandleAreaTrigger(_player, packet.AreaTriggerID, packet.Entered))
            return;

    AreaTriggerStruct const* at = sObjectMgr->GetAreaTrigger(packet.AreaTriggerID);
    if (!at)
        return;

    bool teleported = false;
    if (player->GetMapId() != at->target_mapId)
    {
        if (!sMapMgr->CanPlayerEnter(at->target_mapId, player, false))
            return;

        if (Group* group = player->GetGroup())
            if (group->isLFGGroup() && player->GetMap()->IsDungeon())
                teleported = player->TeleportToBGEntryPoint();
    }

    if (!teleported)
        player->TeleportTo(at->target_mapId, at->target_X, at->target_Y, at->target_Z, at->target_Orientation, TELE_TO_NOT_LEAVE_TRANSPORT);
}

void WorldSession::HandleUpdateAccountData(WorldPackets::ClientConfig::UserClientUpdateAccountData& packet)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_UPDATE_ACCOUNT_DATA: type %u, time %u, decompressedSize %u",
        packet.DataType, packet.Time, packet.Size);

    if (packet.DataType > NUM_ACCOUNT_DATA_TYPES)
        return;

    if (packet.Size == 0)                               // erase
    {
        SetAccountData(AccountDataType(packet.DataType), 0, "");
        return;
    }

    if (packet.Size > 0xFFFF)
    {
        TC_LOG_ERROR("network", "UAD: Account data packet too big, size %u", packet.Size);
        return;
    }

    ByteBuffer dest;
    dest.resize(packet.Size);

    uLongf realSize = packet.Size;
    if (uncompress(dest.contents(), &realSize, packet.CompressedData.contents(), packet.CompressedData.size()) != Z_OK)
    {
        TC_LOG_ERROR("network", "UAD: Failed to decompress account data");
        return;
    }

    std::string adata;
    dest >> adata;

    SetAccountData(AccountDataType(packet.DataType), packet.Time, adata);
}

void WorldSession::HandleRequestAccountData(WorldPackets::ClientConfig::RequestAccountData& request)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_REQUEST_ACCOUNT_DATA: type %u", request.DataType);

    if (request.DataType >= NUM_ACCOUNT_DATA_TYPES)
        return;

    AccountData const* adata = GetAccountData(AccountDataType(request.DataType));

    WorldPackets::ClientConfig::UpdateAccountData data;
    data.Player = _player ? _player->GetGUID() : ObjectGuid::Empty;
    data.Time = adata->Time;
    data.Size = adata->Data.size();
    data.DataType = request.DataType;

    uLongf destSize = compressBound(data.Size);

    data.CompressedData.resize(destSize);

    if (data.Size && compress(data.CompressedData.contents(), &destSize, (uint8 const*)adata->Data.c_str(), data.Size) != Z_OK)
    {
        TC_LOG_ERROR("network", "RAD: Failed to compress account data");
        return;
    }

    data.CompressedData.resize(destSize);

    SendPacket(data.Write());
}

void WorldSession::HandleSetActionButtonOpcode(WorldPackets::Spells::SetActionButton& packet)
{
    uint32 action = ACTION_BUTTON_ACTION(packet.Action);
    uint32 type = ACTION_BUTTON_TYPE(packet.Action);

    TC_LOG_DEBUG("network", "CMSG_SET_ACTION_BUTTON Button: %u Action: %u Type: %u", packet.Index, action, type);

    if (!packet.Action)
        GetPlayer()->RemoveActionButton(packet.Index);
    else
        GetPlayer()->AddActionButton(packet.Index, action, type);
}

void WorldSession::HandleCompleteCinematic(WorldPackets::Misc::CompleteCinematic& /*packet*/)
{
}

void WorldSession::HandleNextCinematicCamera(WorldPackets::Misc::NextCinematicCamera& /*packet*/)
{
}

void WorldSession::HandleSetActionBarToggles(WorldPackets::Character::SetActionBarToggles& packet)
{
    if (!GetPlayer())                                        // ignore until not logged (check needed because STATUS_AUTHED)
    {
        if (packet.Mask != 0)
            TC_LOG_ERROR("network", "WorldSession::HandleSetActionBarToggles in not logged state with value: %u, ignored", uint32(packet.Mask));
        return;
    }

    GetPlayer()->SetByteValue(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTES_OFFSET_ACTION_BAR_TOGGLES, packet.Mask);
}

void WorldSession::HandlePlayedTime(WorldPackets::Character::RequestPlayedTime& packet)
{
    WorldPackets::Character::PlayedTime playedTime;
    playedTime.TotalTime = _player->GetTotalPlayedTime();
    playedTime.LevelTime = _player->GetLevelPlayedTime();
    playedTime.TriggerEvent = packet.TriggerScriptEvent;  // 0-1 - will not show in chat frame
    SendPacket(playedTime.Write());
}

void WorldSession::HandleWorldTeleportOpcode(WorldPackets::Misc::WorldTeleport& worldTeleport)
{
    if (GetPlayer()->IsInFlight())
    {
        TC_LOG_DEBUG("network", "Player '%s' (%s) in flight, ignore worldport command.",
            GetPlayer()->GetName().c_str(), GetPlayer()->GetGUID().ToString().c_str());
        return;
    }

    TC_LOG_DEBUG("network", "CMSG_WORLD_TELEPORT: Player = %s, map = %u, x = %f, y = %f, z = %f, o = %f",
        GetPlayer()->GetName().c_str(), worldTeleport.MapID, worldTeleport.Pos.x, worldTeleport.Pos.y, worldTeleport.Pos.z, worldTeleport.Facing);

    if (HasPermission(rbac::RBAC_PERM_OPCODE_WORLD_TELEPORT))
        GetPlayer()->TeleportTo(worldTeleport.MapID, worldTeleport.Pos.x, worldTeleport.Pos.y, worldTeleport.Pos.z, worldTeleport.Facing);
    else
        SendNotification(LANG_YOU_NOT_HAVE_PERMISSION);
}

void WorldSession::HandleWhoIsOpcode(WorldPackets::Who::WhoIsRequest& packet)
{
    TC_LOG_DEBUG("network", "Received whois command from player %s for character %s",
        GetPlayer()->GetName().c_str(), packet.CharName.c_str());

    if (!HasPermission(rbac::RBAC_PERM_OPCODE_WHOIS))
    {
        SendNotification(LANG_YOU_NOT_HAVE_PERMISSION);
        return;
    }

    if (packet.CharName.empty() || !normalizePlayerName(packet.CharName))
    {
        SendNotification(LANG_NEED_CHARACTER_NAME);
        return;
    }

    Player* player = ObjectAccessor::FindConnectedPlayerByName(packet.CharName);
    if (!player)
    {
        SendNotification(LANG_PLAYER_NOT_EXIST_OR_OFFLINE, packet.CharName.c_str());
        return;
    }

    PreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_ACCOUNT_WHOIS);
    stmt->setUInt32(0, player->GetSession()->GetAccountId());

    PreparedQueryResult result = LoginDatabase.Query(stmt);
    if (!result)
    {
        SendNotification(LANG_ACCOUNT_FOR_PLAYER_NOT_FOUND, packet.CharName.c_str());
        return;
    }

    Field* fields = result->Fetch();
    std::string acc = fields[0].GetString();
    if (acc.empty())
        acc = "Unknown";

    std::string email = fields[1].GetString();
    if (email.empty())
        email = "Unknown";

    std::string lastip = fields[2].GetString();
    if (lastip.empty())
        lastip = "Unknown";

    WorldPackets::Who::WhoIsResponse response;
    response.AccountName = packet.CharName + "'s " + "account is " + acc + ", e-mail: " + email + ", last ip: " + lastip;
    SendPacket(response.Write());
}

void WorldSession::HandleComplainOpcode(WorldPacket& recvData)
{
    uint8 spam_type;                                        // 0 - mail, 1 - chat
    ObjectGuid spammer_guid;
    uint32 unk1 = 0;
    uint32 unk2 = 0;
    uint32 unk3 = 0;
    uint32 unk4 = 0;
    std::string description = "";
    recvData >> spam_type;                                 // unk 0x01 const, may be spam type (mail/chat)
    recvData >> spammer_guid;                              // player guid
    switch (spam_type)
    {
        case 0:
            recvData >> unk1;                              // const 0
            recvData >> unk2;                              // probably mail id
            recvData >> unk3;                              // const 0
            break;
        case 1:
            recvData >> unk1;                              // probably language
            recvData >> unk2;                              // message type?
            recvData >> unk3;                              // probably channel id
            recvData >> unk4;                              // time
            recvData >> description;                       // spam description string (messagetype, channel name, player name, message)
            break;
    }

    // NOTE: all chat messages from this spammer automatically ignored by spam reporter until logout in case chat spam.
    // if it's mail spam - ALL mails from this spammer automatically removed by client

    // Complaint Received message
    WorldPacket data(SMSG_COMPLAINT_RESULT, 2);
    data << uint8(0); // value 1 resets CGChat::m_complaintsSystemStatus in client. (unused?)
    data << uint8(0); // value 0xC generates a "CalendarError" in client.
    SendPacket(&data);

    TC_LOG_DEBUG("network", "REPORT SPAM: type %u, %s, unk1 %u, unk2 %u, unk3 %u, unk4 %u, message %s",
        spam_type, spammer_guid.ToString().c_str(), unk1, unk2, unk3, unk4, description.c_str());
}

void WorldSession::HandleFarSightOpcode(WorldPackets::Misc::FarSight& packet)
{
    if (packet.Enable)
    {
        TC_LOG_DEBUG("network", "Added FarSight %s to %s", _player->GetGuidValue(PLAYER_FARSIGHT).ToString().c_str(), _player->GetGUID().ToString().c_str());
        if (WorldObject* target = _player->GetViewpoint())
            _player->SetSeer(target);
        else
            TC_LOG_ERROR("network", "Player %s (%s) requests non-existing seer %s", _player->GetName().c_str(), _player->GetGUID().ToString().c_str(), _player->GetGuidValue(PLAYER_FARSIGHT).ToString().c_str());
    }
    else
    {
        TC_LOG_DEBUG("network", "%s set vision to self", _player->GetGUID().ToString().c_str());
        _player->SetSeer(_player);
    }

    GetPlayer()->UpdateVisibilityForPlayer();
}

void WorldSession::HandleSetTitleOpcode(WorldPackets::Character::SetTitle& packet)
{
    // -1 at none
    if (packet.TitleID > 0 && packet.TitleID < MAX_TITLE_INDEX)
    {
       if (!GetPlayer()->HasTitle(packet.TitleID))
            return;
    }
    else
        packet.TitleID = 0;

    GetPlayer()->SetUInt32Value(PLAYER_CHOSEN_TITLE, packet.TitleID);
}

void WorldSession::HandleTimeSyncResponse(WorldPackets::Misc::TimeSyncResponse& packet)
{
    // Prevent crashing server if queue is empty
    if (_player->m_timeSyncQueue.empty())
    {
        TC_LOG_ERROR("network", "Received CMSG_TIME_SYNC_RESPONSE from player %s without requesting it (hacker?)", _player->GetName().c_str());
        return;
    }

    if (packet.SequenceIndex != _player->m_timeSyncQueue.front())
        TC_LOG_ERROR("network", "Wrong time sync counter from player %s (cheater?)", _player->GetName().c_str());

    TC_LOG_DEBUG("network", "Time sync received: counter %u, client ticks %u, time since last sync %u", packet.SequenceIndex, packet.ClientTime, packet.ClientTime - _player->m_timeSyncClient);

    uint32 ourTicks = packet.ClientTime + (getMSTime() - _player->m_timeSyncServer);

    // diff should be small
    TC_LOG_DEBUG("network", "Our ticks: %u, diff %u, latency %u", ourTicks, ourTicks - packet.ClientTime, GetLatency());

    _player->m_timeSyncClient = packet.ClientTime;
    _player->m_timeSyncQueue.pop();
}

void WorldSession::HandleResetInstancesOpcode(WorldPackets::Instance::ResetInstances& /*packet*/)
{
    if (Group* group = _player->GetGroup())
    {
        if (group->IsLeader(_player->GetGUID()))
            group->ResetInstances(INSTANCE_RESET_ALL, false, false, _player);
    }
    else
        _player->ResetInstances(INSTANCE_RESET_ALL, false, false);
}

void WorldSession::HandleSetDungeonDifficultyOpcode(WorldPackets::Misc::SetDungeonDifficulty& setDungeonDifficulty)
{
    DifficultyEntry const* difficultyEntry = sDifficultyStore.LookupEntry(setDungeonDifficulty.DifficultyID);
    if (!difficultyEntry)
    {
        TC_LOG_DEBUG("network", "WorldSession::HandleSetDungeonDifficultyOpcode: %s sent an invalid instance mode %d!",
            _player->GetGUID().ToString().c_str(), setDungeonDifficulty.DifficultyID);
        return;
    }

    if (difficultyEntry->InstanceType != MAP_INSTANCE)
    {
        TC_LOG_DEBUG("network", "WorldSession::HandleSetDungeonDifficultyOpcode: %s sent an non-dungeon instance mode %d!",
            _player->GetGUID().ToString().c_str(), difficultyEntry->ID);
        return;
    }

    if (!(difficultyEntry->Flags & DIFFICULTY_FLAG_CAN_SELECT))
    {
        TC_LOG_DEBUG("network", "WorldSession::HandleSetDungeonDifficultyOpcode: %s sent unselectable instance mode %d!",
            _player->GetGUID().ToString().c_str(), difficultyEntry->ID);
        return;
    }

    Difficulty difficultyID = Difficulty(difficultyEntry->ID);
    if (difficultyID == _player->GetDungeonDifficultyID())
        return;

    // cannot reset while in an instance
    Map* map = _player->FindMap();
    if (map && map->IsDungeon())
    {
        TC_LOG_DEBUG("network", "WorldSession::HandleSetDungeonDifficultyOpcode: player (Name: %s, %s) tried to reset the instance while player is inside!",
            _player->GetName().c_str(), _player->GetGUID().ToString().c_str());
        return;
    }

    Group* group = _player->GetGroup();
    if (group)
    {
        if (group->IsLeader(_player->GetGUID()))
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
            {
                Player* groupGuy = itr->GetSource();
                if (!groupGuy)
                    continue;

                if (!groupGuy->IsInMap(groupGuy))
                    return;

                if (groupGuy->GetMap()->IsNonRaidDungeon())
                {
                    TC_LOG_DEBUG("network", "WorldSession::HandleSetDungeonDifficultyOpcode: %s tried to reset the instance while group member (Name: %s, %s) is inside!",
                        _player->GetGUID().ToString().c_str(), groupGuy->GetName().c_str(), groupGuy->GetGUID().ToString().c_str());
                    return;
                }
            }
            // the difficulty is set even if the instances can't be reset
            //_player->SendDungeonDifficulty(true);
            group->ResetInstances(INSTANCE_RESET_CHANGE_DIFFICULTY, false, false, _player);
            group->SetDungeonDifficultyID(difficultyID);
        }
    }
    else
    {
        _player->ResetInstances(INSTANCE_RESET_CHANGE_DIFFICULTY, false, false);
        _player->SetDungeonDifficultyID(difficultyID);
        _player->SendDungeonDifficulty();
    }
}

void WorldSession::HandleSetRaidDifficultyOpcode(WorldPackets::Misc::SetRaidDifficulty& setRaidDifficulty)
{
    DifficultyEntry const* difficultyEntry = sDifficultyStore.LookupEntry(setRaidDifficulty.DifficultyID);
    if (!difficultyEntry)
    {
        TC_LOG_DEBUG("network", "WorldSession::HandleSetDungeonDifficultyOpcode: %s sent an invalid instance mode %u!",
            _player->GetGUID().ToString().c_str(), setRaidDifficulty.DifficultyID);
        return;
    }

    if (difficultyEntry->InstanceType != MAP_RAID)
    {
        TC_LOG_DEBUG("network", "WorldSession::HandleSetDungeonDifficultyOpcode: %s sent an non-dungeon instance mode %u!",
            _player->GetGUID().ToString().c_str(), difficultyEntry->ID);
        return;
    }

    if (!(difficultyEntry->Flags & DIFFICULTY_FLAG_CAN_SELECT))
    {
        TC_LOG_DEBUG("network", "WorldSession::HandleSetDungeonDifficultyOpcode: %s sent unselectable instance mode %u!",
            _player->GetGUID().ToString().c_str(), difficultyEntry->ID);
        return;
    }

    if (((difficultyEntry->Flags & DIFFICULTY_FLAG_LEGACY) >> 5) != setRaidDifficulty.Legacy)
    {
        TC_LOG_DEBUG("network", "WorldSession::HandleSetDungeonDifficultyOpcode: %s sent not matching legacy difficulty %u!",
            _player->GetGUID().ToString().c_str(), difficultyEntry->ID);
        return;
    }

    Difficulty difficultyID = Difficulty(difficultyEntry->ID);
    if (difficultyID == (setRaidDifficulty.Legacy ?  _player->GetLegacyRaidDifficultyID() : _player->GetRaidDifficultyID()))
        return;

    // cannot reset while in an instance
    Map* map = _player->FindMap();
    if (map && map->IsDungeon())
    {
        TC_LOG_DEBUG("network", "WorldSession::HandleSetRaidDifficultyOpcode: player (Name: %s, %s) tried to reset the instance while player is inside!",
            _player->GetName().c_str(), _player->GetGUID().ToString().c_str());
        return;
    }

    Group* group = _player->GetGroup();
    if (group)
    {
        if (group->IsLeader(_player->GetGUID()))
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
            {
                Player* groupGuy = itr->GetSource();
                if (!groupGuy)
                    continue;

                if (!groupGuy->IsInMap(groupGuy))
                    return;

                if (groupGuy->GetMap()->IsRaid())
                {
                    TC_LOG_DEBUG("network", "WorldSession::HandleSetRaidDifficultyOpcode: %s tried to reset the instance while group member (Name: %s, %s) is inside!",
                        _player->GetGUID().ToString().c_str(), groupGuy->GetName().c_str(), groupGuy->GetGUID().ToString().c_str());
                    return;
                }
            }
            // the difficulty is set even if the instances can't be reset
            group->ResetInstances(INSTANCE_RESET_CHANGE_DIFFICULTY, true, setRaidDifficulty.Legacy != 0, _player);
            if (setRaidDifficulty.Legacy)
                group->SetLegacyRaidDifficultyID(difficultyID);
            else
                group->SetRaidDifficultyID(difficultyID);
        }
    }
    else
    {
        _player->ResetInstances(INSTANCE_RESET_CHANGE_DIFFICULTY, true, setRaidDifficulty.Legacy != 0);
        if (setRaidDifficulty.Legacy)
            _player->SetLegacyRaidDifficultyID(difficultyID);
        else
            _player->SetRaidDifficultyID(difficultyID);

        _player->SendRaidDifficulty(setRaidDifficulty.Legacy != 0);
    }
}

void WorldSession::HandleRequestPetInfoOpcode(WorldPacket& /*recvData */)
{
    /*
        recvData.hexlike();
    */
}

void WorldSession::HandleSetTaxiBenchmarkOpcode(WorldPacket& recvData)
{
    uint8 mode;
    recvData >> mode;

    mode ? _player->SetFlag(PLAYER_FLAGS, PLAYER_FLAGS_TAXI_BENCHMARK) : _player->RemoveFlag(PLAYER_FLAGS, PLAYER_FLAGS_TAXI_BENCHMARK);

    TC_LOG_DEBUG("network", "Client used \"/timetest %d\" command", mode);
}

void WorldSession::HandleGuildSetFocusedAchievement(WorldPackets::Achievement::GuildSetFocusedAchievement& setFocusedAchievement)
{
    if (Guild* guild = sGuildMgr->GetGuildById(_player->GetGuildId()))
        guild->GetAchievementMgr().SendAchievementInfo(_player, setFocusedAchievement.AchievementID);
}

void WorldSession::HandleUITimeRequest(WorldPackets::Misc::UITimeRequest& /*request*/)
{
    WorldPackets::Misc::UITime response;
    response.Time = time(NULL);
    SendPacket(response.Write());
}

void WorldSession::SendSetPhaseShift(std::set<uint32> const& phaseIds, std::set<uint32> const& terrainswaps, std::set<uint32> const& worldMapAreaSwaps)
{
    WorldPackets::Misc::PhaseShift phaseShift;
    phaseShift.ClientGUID = _player->GetGUID();
    phaseShift.PersonalGUID = _player->GetGUID();
    phaseShift.PhaseShifts = phaseIds;
    phaseShift.VisibleMapIDs = terrainswaps;
    phaseShift.UiWorldMapAreaIDSwaps = worldMapAreaSwaps;
    SendPacket(phaseShift.Write());
}

void WorldSession::HandleInstanceLockResponse(WorldPackets::Instance::InstanceLockResponse& packet)
{
    if (!_player->HasPendingBind())
    {
        TC_LOG_INFO("network", "InstanceLockResponse: Player %s (%s) tried to bind himself/teleport to graveyard without a pending bind!",
            _player->GetName().c_str(), _player->GetGUID().ToString().c_str());
        return;
    }

    if (packet.AcceptLock)
        _player->BindToInstance();
    else
        _player->RepopAtGraveyard();

    _player->SetPendingBind(0, 0);
}

void WorldSession::HandleUpdateMissileTrajectory(WorldPacket& recvPacket)
{
    ObjectGuid guid;
    uint32 spellId;
    float pitch, speed;
    float curX, curY, curZ;
    float targetX, targetY, targetZ;
    uint8 moveStop;

    recvPacket >> guid >> spellId >> pitch >> speed;
    recvPacket >> curX >> curY >> curZ;
    recvPacket >> targetX >> targetY >> targetZ;
    recvPacket >> moveStop;

    Unit* caster = ObjectAccessor::GetUnit(*_player, guid);
    Spell* spell = caster ? caster->GetCurrentSpell(CURRENT_GENERIC_SPELL) : NULL;
    if (!spell || spell->m_spellInfo->Id != spellId || !spell->m_targets.HasDst() || !spell->m_targets.HasSrc())
    {
        recvPacket.rfinish();
        return;
    }

    Position pos = *spell->m_targets.GetSrcPos();
    pos.Relocate(curX, curY, curZ);
    spell->m_targets.ModSrc(pos);

    pos = *spell->m_targets.GetDstPos();
    pos.Relocate(targetX, targetY, targetZ);
    spell->m_targets.ModDst(pos);

    spell->m_targets.SetPitch(pitch);
    spell->m_targets.SetSpeed(speed);

    if (moveStop)
    {
        uint32 opcode;
        recvPacket >> opcode;
        recvPacket.SetOpcode(CMSG_MOVE_STOP); // always set to CMSG_MOVE_STOP in client SetOpcode
        //HandleMovementOpcodes(recvPacket);
    }
}

void WorldSession::HandleViolenceLevel(WorldPackets::Misc::ViolenceLevel& /*violenceLevel*/)
{
    // do something?
}

void WorldSession::HandleObjectUpdateFailedOpcode(WorldPackets::Misc::ObjectUpdateFailed& objectUpdateFailed)
{
    TC_LOG_ERROR("network", "Object update failed for %s for player %s (%s)", objectUpdateFailed.ObjectGUID.ToString().c_str(), GetPlayerName().c_str(), _player->GetGUID().ToString().c_str());

    // If create object failed for current player then client will be stuck on loading screen
    if (_player->GetGUID() == objectUpdateFailed.ObjectGUID)
    {
        LogoutPlayer(true);
        return;
    }

    // Pretend we've never seen this object
    _player->m_clientGUIDs.erase(objectUpdateFailed.ObjectGUID);
}

void WorldSession::HandleObjectUpdateRescuedOpcode(WorldPackets::Misc::ObjectUpdateRescued& objectUpdateRescued)
{
    TC_LOG_ERROR("network", "Object update rescued for %s for player %s (%s)", objectUpdateRescued.ObjectGUID.ToString().c_str(), GetPlayerName().c_str(), _player->GetGUID().ToString().c_str());

    // Client received values update after destroying object
    // re-register object in m_clientGUIDs to send DestroyObject on next visibility update
    _player->m_clientGUIDs.insert(objectUpdateRescued.ObjectGUID);
}

void WorldSession::HandleSaveCUFProfiles(WorldPackets::Misc::SaveCUFProfiles& packet)
{
    if (packet.CUFProfiles.size() > MAX_CUF_PROFILES)
    {
        TC_LOG_ERROR("entities.player", "HandleSaveCUFProfiles - %s tried to save more than %i CUF profiles. Hacking attempt?", GetPlayerName().c_str(), MAX_CUF_PROFILES);
        return;
    }

    for (uint8 i = 0; i < packet.CUFProfiles.size(); ++i)
        GetPlayer()->SaveCUFProfile(i, std::move(packet.CUFProfiles[i]));

    for (uint8 i = packet.CUFProfiles.size(); i < MAX_CUF_PROFILES; ++i)
        GetPlayer()->SaveCUFProfile(i, nullptr);
}

void WorldSession::SendLoadCUFProfiles()
{
    Player* player = GetPlayer();

    WorldPackets::Misc::LoadCUFProfiles loadCUFProfiles;

    for (uint8 i = 0; i < MAX_CUF_PROFILES; i++)
        if (CUFProfile* cufProfile = player->GetCUFProfile(i))
            loadCUFProfiles.CUFProfiles.push_back(cufProfile);
    SendPacket(loadCUFProfiles.Write());
}

void WorldSession::HandleSetAdvancedCombatLogging(WorldPackets::ClientConfig::SetAdvancedCombatLogging& setAdvancedCombatLogging)
{
    _player->SetAdvancedCombatLogging(setAdvancedCombatLogging.Enable);
}

void WorldSession::HandleMountSpecialAnimOpcode(WorldPackets::Misc::MountSpecial& /*mountSpecial*/)
{
    WorldPackets::Misc::SpecialMountAnim specialMountAnim;
    specialMountAnim.UnitGUID = _player->GetGUID();
    GetPlayer()->SendMessageToSet(specialMountAnim.Write(), false);
}
