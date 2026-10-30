/*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

// Party swap, for benilla's single-player party: one client plays one member of a party whose
// other members are party bots, and `.party swap <name>` moves the client to another member in
// place. Nothing logs out and nothing leaves the world.
//
// Each character keeps its own session, account and MasterPlayer (action buttons, social, mail):
// Player::SaveToDB writes the session's account, so players never change sessions. What moves
// is the client connection, from the old character's session to the new one's, and the bot role,
// the other way. The new character then gets the login burst a reconnect sends
// (WorldSession::HandlePlayerLogin with the character already online), without
// SMSG_LOGIN_VERIFY_WORLD, so the client shows no loading screen.

#include "PlayerBotMgr.h"
#include "PartyBotAI.h"
#include "Bag.h"
#include "Language.h"
#include "Chat.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MasterPlayer.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerBroadcaster.h"
#include "SocialMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

static PartyBotAI* GetPartyBotAI(Player const* player)
{
    PlayerBotEntry* bot = player ? player->GetSession()->GetBot() : nullptr;
    return bot ? dynamic_cast<PartyBotAI*>(bot->ai.get()) : nullptr;
}

// Why the client playing `client` cannot take over `target` now; empty when it can.
static std::string PartySwapBlocker(Player* client, Player* target)
{
    if (!client || !client->IsInWorld())
        return "you are not in the world";
    WorldSession* session = client->GetSession();
    if (!session->GetSocket() || session->GetBot())
        return "you are not played by a client";
    if (session->PlayerLoading() || session->PlayerLogout() || session->IsLogingOut())
        return "you are logging in or out";
    if (!target || !target->IsInWorld())
        return "that character is not in the world";
    if (target == client)
        return "you already play that character";

    PlayerBotEntry* bot = target->GetSession()->GetBot();
    PartyBotAI* ai = GetPartyBotAI(target);
    if (!ai)
        return "that character is not a party bot";
    if (bot->state != PB_STATE_ONLINE || bot->requestRemoval || !ai->m_initialized)
        return "that party bot is loading or leaving";
    if (ai->m_leaderGuid != client->GetObjectGuid())
        return "that party bot follows someone else";

    Group* group = client->GetGroup();
    if (!group || group != target->GetGroup())
        return "that character is not in your group";
    if (client->GetMap() != target->GetMap())
        return "that character is on another map";
    if (client->IsBeingTeleported() || target->IsBeingTeleported())
        return "a teleport is under way";
    if (client->IsTaxiFlying() || target->IsTaxiFlying())
        return "a flight is under way";
    return "";
}

// Removes from the client every item object of `player`'s, as it holds them while it plays
// `player`: equipped, bags and their contents, bank, buyback and keyring.
static void DestroyItemsFor(Player const* player, Player const* client)
{
    for (uint8 slot = PLAYER_SLOT_START; slot < PLAYER_SLOT_END; ++slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;
        if (item->IsBag())
        {
            Bag const* bag = static_cast<Bag const*>(item);
            for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                if (Item* inside = bag->GetItemByPos(uint8(i)))
                    inside->DestroyForPlayer(client);
        }
        item->DestroyForPlayer(client);
    }
}

bool ChatHandler::HandlePartySwapCommand(char* args)
{
    Player* client = m_session->GetPlayer();

    std::string name = ExtractPlayerNameFromLink(&args);
    if (name.empty())
    {
        SendSysMessage(LANG_PLAYER_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }
    ObjectGuid const targetGuid = sObjectMgr.GetPlayerGuidByName(name);
    Player* target = targetGuid.IsEmpty() ? nullptr : sObjectAccessor.FindPlayer(targetGuid);

    std::string const blocker = PartySwapBlocker(client, target);
    if (!blocker.empty())
    {
        PSendSysMessage("Cannot swap to %s: %s.", name.c_str(), blocker.c_str());
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[PartySwap] %s -> %s: refused, %s",
            client ? client->GetName() : "?", name.c_str(), blocker.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    // Chat commands can run on a map or async thread: the swap itself waits for the world
    // thread's single-threaded point (World::Update, before the sessions update).
    ObjectGuid const clientGuid = client->GetObjectGuid();
    uint32 const queuedAt = WorldTimer::getMSTime();
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[PartySwap] %s -> %s: queued", client->GetName(), target->GetName());
    sWorld.GetMessager().AddMessage([clientGuid, targetGuid, queuedAt](World*)
    {
        sPlayerBotMgr.SwapPartyControl(clientGuid, targetGuid, queuedAt);
    });
    return true;
}

void PlayerBotMgr::SwapPartyControl(ObjectGuid clientGuid, ObjectGuid botGuid, uint32 queuedAt)
{
    uint32 const start = WorldTimer::getMSTime();
    Player* oldPlayer = sObjectAccessor.FindPlayer(clientGuid);
    Player* newPlayer = sObjectAccessor.FindPlayer(botGuid);

    // The world may have moved on since the command was checked.
    std::string const blocker = PartySwapBlocker(oldPlayer, newPlayer);
    if (!blocker.empty())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[PartySwap] %s -> %s: refused, %s",
            clientGuid.GetString().c_str(), botGuid.GetString().c_str(), blocker.c_str());
        if (oldPlayer && oldPlayer->GetSession()->GetSocket())
            ChatHandler(oldPlayer).PSendSysMessage("Cannot swap: %s.", blocker.c_str());
        return;
    }

    WorldSession* oldSession = oldPlayer->GetSession();
    WorldSession* newSession = newPlayer->GetSession();
    auto const iter = m_bots.find(newPlayer->GetGUIDLow());
    if (iter == m_bots.end() || iter->second.get() != newSession->GetBot())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[PartySwap] %s is a party bot PlayerBotMgr does not hold", newPlayer->GetName());
        return;
    }
    std::shared_ptr<PlayerBotEntry> const entry = iter->second;
    PartyBotAI const* oldAI = static_cast<PartyBotAI const*>(entry->ai.get());

    // 1. Close what the old character's client had open, while the client still hears it.
    if (!oldPlayer->GetLootGuid().IsEmpty())
        oldSession->DoLootRelease(oldPlayer->GetLootGuid());
    if (oldPlayer->GetTrader())
        oldPlayer->TradeCancel(true);

    // 2. The connection moves to the new character's session, and the bot role to the old one's.
    newSession->TakeOverClient(*oldSession);
    oldSession->InitCheatData(oldPlayer);
    newSession->InitCheatData(newPlayer);
    if (oldPlayer->m_broadcaster)
        oldPlayer->m_broadcaster->ChangeSocket(nullptr);
    if (newPlayer->m_broadcaster)
        newPlayer->m_broadcaster->ChangeSocket(newSession->GetSocket());
    else
        newPlayer->CreatePacketBroadcaster();

    // 3. The old character is now moved by the server: apply the changes that waited for its
    //    client's acks, and stop whatever its client was holding down.
    oldPlayer->ResolvePendingMovementChanges(true, true);
    oldPlayer->SetSplineDonePending(false);
    if (oldPlayer->IsMoving())
        oldPlayer->StopMoving(true);

    // 4. The bot entry now drives the old character, with an AI of its own: an AI's spell data
    //    and role are its character's class's. It starts initialized, so it does not teleport or
    //    refill the character as a bot loaded from the database would.
    std::string const oldName = oldPlayer->GetName();
    m_bots.erase(iter);
    entry->playerGUID = oldPlayer->GetGUIDLow();
    entry->accountId = oldSession->GetAccountId();
    entry->name = oldName;
    PartyBotAI* ai = new PartyBotAI(newPlayer, oldPlayer->GetMapId(), oldPlayer->GetInstanceId(),
        oldPlayer->GetPositionX(), oldPlayer->GetPositionY(), oldPlayer->GetPositionZ(), oldPlayer->GetOrientation());
    ai->m_marksToCC = oldAI->m_marksToCC;
    ai->m_marksToFocus = oldAI->m_marksToFocus;
    ai->botEntry = entry.get();
    ai->m_initialized = true;
    entry->ai.reset(ai); // the old AI detaches from the new character (PlayerBotAI::Remove)
    m_bots[entry->playerGUID] = entry;
    oldPlayer->SetAI(ai);
    ai->SetPlayer(oldPlayer);
    if (oldPlayer->IsGameMaster())
        oldPlayer->SetGameMaster(false);
    ai->AutoAssignRole();
    ai->ResetSpellData();
    ai->PopulateSpellData();

    // The new character's motion was its AI's (following, chasing): it stops where it is. Its
    // client has not claimed it yet, so the stop needs no CMSG_MOVE_SPLINE_DONE.
    newPlayer->GetMotionMaster()->Clear(false, true);
    newPlayer->GetMotionMaster()->MoveIdle();
    if (newPlayer->IsMoving() || !newPlayer->movespline->Finalized())
        newPlayer->StopMoving(true);
    newPlayer->SetSplineDonePending(false);

    // 5. Leaders: every party bot following the old character follows the new one, which leads
    //    the group (a party bot that leads its group removes itself).
    for (auto const& bot : m_bots)
        if (PartyBotAI* other = bot.second->ai ? dynamic_cast<PartyBotAI*>(bot.second->ai.get()) : nullptr)
            if (other->m_leaderGuid == clientGuid)
                other->m_leaderGuid = botGuid;
    Group* group = newPlayer->GetGroup();
    if (!group->IsLeader(botGuid))
        group->ChangeLeader(botGuid);

    // 6. The client: our notice first, then the new character's login burst.
    WorldPacket notice(SMSG_PARTY_SWAP, 16);
    notice << uint64(clientGuid.GetRawValue());
    notice << uint64(botGuid.GetRawValue());
    newSession->SendPacket(&notice);

    DestroyItemsFor(oldPlayer, newPlayer);
    newPlayer->SendPacketsAtRelogin();
    // No SMSG_ACCOUNT_DATA_MD5: the party shares one UI configuration, so the client keeps what it
    // holds and fetches no character's stored data (macros, bindings, chat) on a swap.
    newPlayer->GetSocial()->SendFriendList();
    newPlayer->GetSocial()->SendIgnoreList();
    if (!newPlayer->IsAlive())
        newPlayer->SendCorpseReclaimDelay(true);
    newPlayer->SendInitialPacketsBeforeAddToMap();
    // The self object and items, and the visible set rebuilt: the old character comes back to
    // the client as an ordinary player.
    newPlayer->GetMap()->ExistingPlayerLogin(newPlayer);
    newPlayer->SendInitialPacketsAfterAddToMap(false);
    newPlayer->SendInitWorldStates(newPlayer->GetCachedZoneId());
    newPlayer->PetSpellInitialize();
    newPlayer->SendMirrorTimers(true);
    // No SMSG_CLIENT_CONTROL_UPDATE, which a reconnect sends: as at a login, the client claims
    // its new character itself (CMSG_SET_ACTIVE_MOVER), on SMSG_PARTY_SWAP.
    if (sWorld.getConfig(CONFIG_BOOL_SEND_LOOT_ROLL_UPON_RECONNECT))
        group->SendLootStartRollsForPlayer(newPlayer);
    // The group as the new character sees it (SMSG_GROUP_LIST lists the others, not oneself):
    // the client still holds the old character's list, which names the new one and not the old.
    group->SendUpdate();

    uint32 const end = WorldTimer::getMSTime();
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartySwap] %s -> %s: done. Client on %s's session (account %u); %s runs PartyBotAI (account %u, role %u) under %s. Waited %u ms, swap took %u ms",
        oldName.c_str(), newPlayer->GetName(), newPlayer->GetName(), newSession->GetAccountId(),
        oldName.c_str(), oldSession->GetAccountId(), uint32(ai->m_role), newPlayer->GetName(),
        WorldTimer::getMSTimeDiff(queuedAt, start), WorldTimer::getMSTimeDiff(start, end));
    ChatHandler(newPlayer).PSendSysMessage("Now playing %s.", newPlayer->GetName());
}

// Makes the client's character the leader of its group. A swap moves the lead, and the group
// keeps it across a restart, so the launcher takes it back before loading the party bots: a
// party bot that leads its group removes itself.
bool ChatHandler::HandlePartyLeadCommand(char* /*args*/)
{
    Player* client = m_session->GetPlayer();
    if (!client->GetGroup())
    {
        SendSysMessage("You are not in a group.");
        SetSentErrorMessage(true);
        return false;
    }
    ObjectGuid const clientGuid = client->GetObjectGuid();
    sWorld.GetMessager().AddMessage([clientGuid](World*)
    {
        Player* client = sObjectAccessor.FindPlayer(clientGuid);
        Group* group = client ? client->GetGroup() : nullptr;
        if (!group || group->IsLeader(clientGuid))
            return;
        group->ChangeLeader(clientGuid);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[PartySwap] %s leads the group", client->GetName());
    });
    return true;
}

// One line per party member: who plays it, what moves it, where it is and what it fights.
bool ChatHandler::HandlePartyStatusCommand(char* /*args*/)
{
    Player* client = m_session->GetPlayer();
    Group* group = client->GetGroup();
    std::vector<ObjectGuid> members;
    if (group)
        for (auto const& slot : group->GetMemberSlots())
            members.push_back(slot.guid);
    else
        members.push_back(client->GetObjectGuid());

    for (ObjectGuid const& guid : members)
    {
        Player* member = sObjectAccessor.FindPlayer(guid);
        if (!member)
        {
            PSendSysMessage("[PartyStatus] %s: not in the world", guid.GetString().c_str());
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[PartyStatus] %s: not in the world", guid.GetString().c_str());
            continue;
        }
        WorldSession* session = member->GetSession();
        PartyBotAI* ai = GetPartyBotAI(member);
        Player* leader = ai ? ObjectAccessor::FindPlayerNotInWorld(ai->m_leaderGuid) : nullptr;
        Unit* victim = member->GetVictim();

        ObjectGuid const& mover = session->GetClientMoverGuid();
        std::string const moverText = mover.IsEmpty() ? "none" : std::to_string(mover.GetCounter());

        char line[512];
        snprintf(line, sizeof(line),
            "[PartyStatus] %s guid=%u account=%u session=%s mover=%s ai=%s leader=%s dist=%.1f "
            "group_leader=%u hp=%u/%u combat=%u victim=%s victim_hp=%u pending_moves=%u spline_ack=%u pos=%.1f,%.1f,%.1f",
            member->GetName(), member->GetGUIDLow(), session->GetAccountId(),
            session->GetSocket() ? "client" : (session->GetBot() ? "bot" : "none"),
            moverText.c_str(),
            ai ? "PartyBotAI" : (member->AI() ? "other" : "none"),
            leader ? leader->GetName() : "-",
            leader ? member->GetDistance(leader) : -1.0f,
            uint32(group && group->IsLeader(guid)), member->GetHealth(), member->GetMaxHealth(),
            uint32(member->IsInCombat()), victim ? victim->GetName() : "-", victim ? victim->GetHealth() : 0,
            uint32(member->HasPendingMovementChange()), uint32(member->HasPendingSplineDone()),
            member->GetPositionX(), member->GetPositionY(), member->GetPositionZ());
        SendSysMessage(line);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "%s", line);
    }
    return true;
}
