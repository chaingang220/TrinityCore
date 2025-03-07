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

#include "WorldSocket.h"
#include "AuthenticationPackets.h"
#include "BigNumber.h"
#include "CharacterPackets.h"
#include "Opcodes.h"
#include "ScriptMgr.h"
#include "SHA1.h"
#include "PacketLog.h"
#include "World.h"

#include <zlib.h>
#include <memory>

#pragma pack(push, 1)

struct CompressedWorldPacket
{
    uint32 UncompressedSize;
    uint32 UncompressedAdler;
    uint32 CompressedAdler;
};

#pragma pack(pop)

using boost::asio::ip::tcp;

std::string const WorldSocket::ServerConnectionInitialize("WORLD OF WARCRAFT CONNECTION - SERVER TO CLIENT");
std::string const WorldSocket::ClientConnectionInitialize("WORLD OF WARCRAFT CONNECTION - CLIENT TO SERVER");
uint32 const WorldSocket::MinSizeForCompression = 0x400;

uint32 const SizeOfClientHeader[2][2] =
{
    { 2, 0 },
    { 6, 4 }
};

uint32 const SizeOfServerHeader[2] = { sizeof(uint16) + sizeof(uint32), sizeof(uint32) };
WorldSocket::WorldSocket(tcp::socket&& socket) : Socket(std::move(socket)),
    _type(CONNECTION_TYPE_REALM), _authSeed(rand32()), _OverSpeedPings(0),
    _worldSession(nullptr), _authed(false), _compressionStream(nullptr), _initialized(false)
{
    _headerBuffer.Resize(SizeOfClientHeader[0][0]);
}

WorldSocket::~WorldSocket()
{
    if (_compressionStream)
    {
        deflateEnd(_compressionStream);
        delete _compressionStream;
    }
}

void WorldSocket::Start()
{
    std::string ip_address = GetRemoteIpAddress().to_string();
    PreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_IP_INFO);
    stmt->setString(0, ip_address);
    stmt->setUInt32(1, inet_addr(ip_address.c_str()));

    {
        std::lock_guard<std::mutex> guard(_queryLock);
        _queryCallback = io_service().wrap(std::bind(&WorldSocket::CheckIpCallback, this, std::placeholders::_1));
        _queryFuture = LoginDatabase.AsyncQuery(stmt);
    }
}

void WorldSocket::CheckIpCallback(PreparedQueryResult result)
{
    if (result)
    {
        bool banned = false;
        do
        {
            Field* fields = result->Fetch();
            if (fields[0].GetUInt64() != 0)
                banned = true;

            if (!fields[1].GetString().empty())
                _ipCountry = fields[1].GetString();

        } while (result->NextRow());

        if (banned)
        {
            SendAuthResponseError(AUTH_REJECT);
            TC_LOG_ERROR("network", "WorldSocket::CheckIpCallback: Sent Auth Response (IP %s banned).", GetRemoteIpAddress().to_string().c_str());
            DelayedCloseSocket();
            return;
        }
    }

    AsyncRead();

    MessageBuffer initializer;
    ServerPktHeader header;
    header.Setup.Size = ServerConnectionInitialize.size();
    initializer.Write(&header, sizeof(header.Setup.Size));
    initializer.Write(ServerConnectionInitialize.c_str(), ServerConnectionInitialize.length());

    std::unique_lock<std::mutex> guard(_writeLock);
    QueuePacket(std::move(initializer), guard);
}

bool WorldSocket::Update()
{
    if (!BaseSocket::Update())
        return false;

    {
        std::lock_guard<std::mutex> guard(_queryLock);
        if (_queryFuture.valid() && _queryFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            auto callback = std::move(_queryCallback);
            _queryCallback = nullptr;
            callback(_queryFuture.get());
        }
    }

    return true;
}

void WorldSocket::HandleSendAuthSession()
{
    _encryptSeed.SetRand(16 * 8);
    _decryptSeed.SetRand(16 * 8);

    WorldPackets::Auth::AuthChallenge challenge;
    challenge.Challenge = _authSeed;
    memcpy(&challenge.DosChallenge[0], _encryptSeed.AsByteArray(16).get(), 16);
    memcpy(&challenge.DosChallenge[4], _decryptSeed.AsByteArray(16).get(), 16);
    challenge.DosZeroBits = 1;

    SendPacketAndLogOpcode(*challenge.Write());
}

void WorldSocket::OnClose()
{
    {
        std::lock_guard<std::mutex> sessionGuard(_worldSessionLock);
        _worldSession = nullptr;
    }
}

void WorldSocket::ReadHandler()
{
    if (!IsOpen())
        return;

    MessageBuffer& packet = GetReadBuffer();
    while (packet.GetActiveSize() > 0)
    {
        if (_headerBuffer.GetRemainingSpace() > 0)
        {
            // need to receive the header
            std::size_t readHeaderSize = std::min(packet.GetActiveSize(), _headerBuffer.GetRemainingSpace());
            _headerBuffer.Write(packet.GetReadPointer(), readHeaderSize);
            packet.ReadCompleted(readHeaderSize);

            if (_headerBuffer.GetRemainingSpace() > 0)
            {
                // Couldn't receive the whole header this time.
                ASSERT(packet.GetActiveSize() == 0);
                break;
            }

            // We just received nice new header
            if (!ReadHeaderHandler())
            {
                CloseSocket();
                return;
            }
        }

        // We have full read header, now check the data payload
        if (_packetBuffer.GetRemainingSpace() > 0)
        {
            // need more data in the payload
            std::size_t readDataSize = std::min(packet.GetActiveSize(), _packetBuffer.GetRemainingSpace());
            _packetBuffer.Write(packet.GetReadPointer(), readDataSize);
            packet.ReadCompleted(readDataSize);

            if (_packetBuffer.GetRemainingSpace() > 0)
            {
                // Couldn't receive the whole data this time.
                ASSERT(packet.GetActiveSize() == 0);
                break;
            }
        }

        // just received fresh new payload
        ReadDataHandlerResult result = ReadDataHandler();
        _headerBuffer.Reset();
        if (result != ReadDataHandlerResult::Ok)
        {
            if (result != ReadDataHandlerResult::WaitingForQuery)
                CloseSocket();

            return;
        }
    }

    AsyncRead();
}

void WorldSocket::ExtractOpcodeAndSize(ClientPktHeader const* header, uint32& opcode, uint32& size) const
{
    if (_authCrypt.IsInitialized())
    {
        opcode = header->Normal.Command;
        size = header->Normal.Size;
    }
    else
    {
        opcode = header->Setup.Command;
        size = header->Setup.Size;
        if (_initialized)
            size -= 4;
    }
}

void WorldSocket::SetWorldSession(WorldSession* session)
{
    std::lock_guard<std::mutex> sessionGuard(_worldSessionLock);
    _worldSession = session;
    _authed = true;
}

bool WorldSocket::ReadHeaderHandler()
{
    ASSERT(_headerBuffer.GetActiveSize() == SizeOfClientHeader[_initialized][_authCrypt.IsInitialized()], "Header size " SZFMTD " different than expected %u", _headerBuffer.GetActiveSize(), SizeOfClientHeader[_initialized][_authCrypt.IsInitialized()]);

    _authCrypt.DecryptRecv(_headerBuffer.GetReadPointer(), _headerBuffer.GetActiveSize());

    ClientPktHeader* header = reinterpret_cast<ClientPktHeader*>(_headerBuffer.GetReadPointer());
    uint32 opcode;
    uint32 size;

    ExtractOpcodeAndSize(header, opcode, size);

    if (!ClientPktHeader::IsValidSize(size) || (_initialized && !ClientPktHeader::IsValidOpcode(opcode)))
    {
        TC_LOG_ERROR("network", "WorldSocket::ReadHeaderHandler(): client %s sent malformed packet (size: %u, cmd: %u)",
            GetRemoteIpAddress().to_string().c_str(), size, opcode);
        return false;
    }

    _packetBuffer.Resize(size);
    return true;
}

WorldSocket::ReadDataHandlerResult WorldSocket::ReadDataHandler()
{
    if (_initialized)
    {
        ClientPktHeader* header = reinterpret_cast<ClientPktHeader*>(_headerBuffer.GetReadPointer());
        uint32 cmd;
        uint32 size;

        ExtractOpcodeAndSize(header, cmd, size);

        OpcodeClient opcode = static_cast<OpcodeClient>(cmd);

        WorldPacket packet(opcode, std::move(_packetBuffer), GetConnectionType());

        if (sPacketLog->CanLogPacket())
            sPacketLog->LogPacket(packet, CLIENT_TO_SERVER, GetRemoteIpAddress(), GetRemotePort(), GetConnectionType());

        std::unique_lock<std::mutex> sessionGuard(_worldSessionLock, std::defer_lock);

        switch (opcode)
        {
            case CMSG_PING:
                LogOpcodeText(opcode, sessionGuard);
                return HandlePing(packet) ? ReadDataHandlerResult::Ok : ReadDataHandlerResult::Error;
            case CMSG_AUTH_SESSION:
            {
                LogOpcodeText(opcode, sessionGuard);
                if (_authed)
                {
                    // locking just to safely log offending user is probably overkill but we are disconnecting him anyway
                    if (sessionGuard.try_lock())
                        TC_LOG_ERROR("network", "WorldSocket::ProcessIncoming: received duplicate CMSG_AUTH_SESSION from %s", _worldSession->GetPlayerInfo().c_str());
                    return ReadDataHandlerResult::Error;
                }

                std::shared_ptr<WorldPackets::Auth::AuthSession> authSession = std::make_shared<WorldPackets::Auth::AuthSession>(std::move(packet));
                authSession->Read();
                HandleAuthSession(authSession);
                return ReadDataHandlerResult::WaitingForQuery;
            }
            case CMSG_AUTH_CONTINUED_SESSION:
            {
                LogOpcodeText(opcode, sessionGuard);
                if (_authed)
                {
                    // locking just to safely log offending user is probably overkill but we are disconnecting him anyway
                    if (sessionGuard.try_lock())
                        TC_LOG_ERROR("network", "WorldSocket::ProcessIncoming: received duplicate CMSG_AUTH_CONTINUED_SESSION from %s", _worldSession->GetPlayerInfo().c_str());
                    return ReadDataHandlerResult::Error;
                }

                std::shared_ptr<WorldPackets::Auth::AuthContinuedSession> authSession = std::make_shared<WorldPackets::Auth::AuthContinuedSession>(std::move(packet));
                authSession->Read();
                HandleAuthContinuedSession(authSession);
                return ReadDataHandlerResult::WaitingForQuery;
            }
            case CMSG_KEEP_ALIVE:
                LogOpcodeText(opcode, sessionGuard);
                break;
            case CMSG_LOG_DISCONNECT:
                LogOpcodeText(opcode, sessionGuard);
                packet.rfinish();   // contains uint32 disconnectReason;
                break;
            case CMSG_ENABLE_NAGLE:
                LogOpcodeText(opcode, sessionGuard);
                SetNoDelay(false);
                break;
            case CMSG_CONNECT_TO_FAILED:
            {
                sessionGuard.lock();

                LogOpcodeText(opcode, sessionGuard);
                WorldPackets::Auth::ConnectToFailed connectToFailed(std::move(packet));
                connectToFailed.Read();
                HandleConnectToFailed(connectToFailed);
                break;
            }
            default:
            {
                sessionGuard.lock();

                LogOpcodeText(opcode, sessionGuard);

                if (!_worldSession)
                {
                    TC_LOG_ERROR("network.opcode", "ProcessIncoming: Client not authed opcode = %u", uint32(opcode));
                    return ReadDataHandlerResult::Error;
                }

                OpcodeHandler const* handler = opcodeTable[opcode];
                if (!handler)
                {
                    TC_LOG_ERROR("network.opcode", "No defined handler for opcode %s sent by %s", GetOpcodeNameForLogging(static_cast<OpcodeClient>(packet.GetOpcode())).c_str(), _worldSession->GetPlayerInfo().c_str());
                    break;
                }

                // Our Idle timer will reset on any non PING opcodes.
                // Catches people idling on the login screen and any lingering ingame connections.
                _worldSession->ResetTimeOutTime();

                // Copy the packet to the heap before enqueuing
                _worldSession->QueuePacket(new WorldPacket(std::move(packet)));
                break;
            }
        }
    }
    else
    {
        std::string initializer(reinterpret_cast<char const*>(_packetBuffer.GetReadPointer()), std::min(_packetBuffer.GetActiveSize(), ClientConnectionInitialize.length()));
        if (initializer != ClientConnectionInitialize)
            return ReadDataHandlerResult::Error;

        _compressionStream = new z_stream();
        _compressionStream->zalloc = (alloc_func)NULL;
        _compressionStream->zfree = (free_func)NULL;
        _compressionStream->opaque = (voidpf)NULL;
        _compressionStream->avail_in = 0;
        _compressionStream->next_in = NULL;
        int32 z_res = deflateInit2(_compressionStream, sWorld->getIntConfig(CONFIG_COMPRESSION), Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
        if (z_res != Z_OK)
        {
            TC_LOG_ERROR("network", "Can't initialize packet compression (zlib: deflateInit) Error code: %i (%s)", z_res, zError(z_res));
            return ReadDataHandlerResult::Error;
        }

        _initialized = true;
        _headerBuffer.Resize(SizeOfClientHeader[1][0]);
        _packetBuffer.Reset();
        HandleSendAuthSession();
    }

    return ReadDataHandlerResult::Ok;
}

void WorldSocket::LogOpcodeText(OpcodeClient opcode, std::unique_lock<std::mutex> const& guard) const
{
    if (!guard)
    {
        TC_LOG_TRACE("network.opcode", "C->S: %s %s", GetRemoteIpAddress().to_string().c_str(), GetOpcodeNameForLogging(opcode).c_str());
    }
    else
    {
        TC_LOG_TRACE("network.opcode", "C->S: %s %s", (_worldSession ? _worldSession->GetPlayerInfo() : GetRemoteIpAddress().to_string()).c_str(),
            GetOpcodeNameForLogging(opcode).c_str());
    }
}

void WorldSocket::SendPacketAndLogOpcode(WorldPacket const& packet)
{
    TC_LOG_TRACE("network.opcode", "S->C: %s %s", GetRemoteIpAddress().to_string().c_str(), GetOpcodeNameForLogging(static_cast<OpcodeServer>(packet.GetOpcode())).c_str());
    SendPacket(packet);
}

void WorldSocket::SendPacket(WorldPacket const& packet)
{
    if (!IsOpen())
        return;

    if (sPacketLog->CanLogPacket())
        sPacketLog->LogPacket(packet, SERVER_TO_CLIENT, GetRemoteIpAddress(), GetRemotePort(), GetConnectionType());

    uint32 packetSize = packet.size();
    uint32 sizeOfHeader = SizeOfServerHeader[_authCrypt.IsInitialized()];
    if (packetSize > MinSizeForCompression && _authCrypt.IsInitialized())
        packetSize = compressBound(packetSize) + sizeof(CompressedWorldPacket);

    std::unique_lock<std::mutex> guard(_writeLock);

#ifndef TC_SOCKET_USE_IOCP
    if (_writeQueue.empty() && _writeBuffer.GetRemainingSpace() >= sizeOfHeader + packetSize)
        WritePacketToBuffer(packet, _writeBuffer);
    else
#endif
    {
        MessageBuffer buffer(sizeOfHeader + packetSize);
        WritePacketToBuffer(packet, buffer);
        QueuePacket(std::move(buffer), guard);
    }
}

void WorldSocket::WritePacketToBuffer(WorldPacket const& packet, MessageBuffer& buffer)
{
    ServerPktHeader header;
    uint32 sizeOfHeader = SizeOfServerHeader[_authCrypt.IsInitialized()];
    uint32 opcode = packet.GetOpcode();
    uint32 packetSize = packet.size();

    // Reserve space for buffer
    uint8* headerPos = buffer.GetWritePointer();
    buffer.WriteCompleted(sizeOfHeader);

    if (packetSize > MinSizeForCompression && _authCrypt.IsInitialized())
    {
        CompressedWorldPacket cmp;
        cmp.UncompressedSize = packetSize + 4;
        cmp.UncompressedAdler = adler32(adler32(0x9827D8F1, (Bytef*)&opcode, 4), packet.contents(), packetSize);

        // Reserve space for compression info - uncompressed size and checksums
        uint8* compressionInfo = buffer.GetWritePointer();
        buffer.WriteCompleted(sizeof(CompressedWorldPacket));

        uint32 compressedSize = CompressPacket(buffer.GetWritePointer(), packet);

        cmp.CompressedAdler = adler32(0x9827D8F1, buffer.GetWritePointer(), compressedSize);

        memcpy(compressionInfo, &cmp, sizeof(CompressedWorldPacket));
        buffer.WriteCompleted(compressedSize);
        packetSize = compressedSize + sizeof(CompressedWorldPacket);

        opcode = SMSG_COMPRESSED_PACKET;
    }
    else if (!packet.empty())
        buffer.Write(packet.contents(), packet.size());

    if (_authCrypt.IsInitialized())
    {
        header.Normal.Size = packetSize;
        header.Normal.Command = opcode;
        _authCrypt.EncryptSend((uint8*)&header, sizeOfHeader);
    }
    else
    {
        header.Setup.Size = packetSize + 4;
        header.Setup.Command = opcode;
    }

    memcpy(headerPos, &header, sizeOfHeader);
}

uint32 WorldSocket::CompressPacket(uint8* buffer, WorldPacket const& packet)
{
    uint32 opcode = packet.GetOpcode();
    uint32 bufferSize = deflateBound(_compressionStream, packet.size() + sizeof(opcode));

    _compressionStream->next_out = buffer;
    _compressionStream->avail_out = bufferSize;
    _compressionStream->next_in = (Bytef*)&opcode;
    _compressionStream->avail_in = sizeof(uint32);

    int32 z_res = deflate(_compressionStream, Z_NO_FLUSH);
    if (z_res != Z_OK)
    {
        TC_LOG_ERROR("network", "Can't compress packet opcode (zlib: deflate) Error code: %i (%s, msg: %s)", z_res, zError(z_res), _compressionStream->msg);
        return 0;
    }

    _compressionStream->next_in = (Bytef*)packet.contents();
    _compressionStream->avail_in = packet.size();

    z_res = deflate(_compressionStream, Z_SYNC_FLUSH);
    if (z_res != Z_OK)
    {
        TC_LOG_ERROR("network", "Can't compress packet data (zlib: deflate) Error code: %i (%s, msg: %s)", z_res, zError(z_res), _compressionStream->msg);
        return 0;
    }

    return bufferSize - _compressionStream->avail_out;
}

struct AccountInfo
{
    struct
    {
        uint32 Id;
        bool IsLockedToIP;
        std::string LastIP;
        std::string LockCountry;
        LocaleConstant Locale;
        std::string OS;
        bool IsBanned;

    } BattleNet;

    struct
    {
        uint32 Id;
        BigNumber SessionKey;
        uint8 Expansion;
        int64 MuteTime;
        uint32 Recruiter;
        bool IsRectuiter;
        AccountTypes Security;
        bool IsBanned;
    } Game;

    bool IsBanned() const { return BattleNet.IsBanned || Game.IsBanned; }

    explicit AccountInfo(Field* fields)
    {
        //           0             1           2          3                4            5           6          7            8      9     10          11
        // SELECT a.id, a.sessionkey, ba.last_ip, ba.locked, ba.lock_country, a.expansion, a.mutetime, ba.locale, a.recruiter, ba.os, ba.id, aa.gmLevel,
        //                                                              12                                                            13    14
        // bab.unbandate > UNIX_TIMESTAMP() OR bab.unbandate = bab.bandate, ab.unbandate > UNIX_TIMESTAMP() OR ab.unbandate = ab.bandate, r.id
        // FROM account a LEFT JOIN battlenet_accounts ba ON a.battlenet_account = ba.id LEFT JOIN account_access aa ON a.id = aa.id AND aa.RealmID IN (-1, ?)
        // LEFT JOIN battlenet_account_bans bab ON ba.id = bab.id LEFT JOIN account_banned ab ON a.id = ab.id LEFT JOIN account r ON a.id = r.recruiter
        // WHERE a.username = ? ORDER BY aa.RealmID DESC LIMIT 1
        Game.Id = fields[0].GetUInt32();
        Game.SessionKey.SetHexStr(fields[1].GetCString());
        BattleNet.LastIP = fields[2].GetString();
        BattleNet.IsLockedToIP = fields[3].GetBool();
        BattleNet.LockCountry = fields[4].GetString();
        Game.Expansion = fields[5].GetUInt8();
        Game.MuteTime = fields[6].GetInt64();
        BattleNet.Locale = LocaleConstant(fields[7].GetUInt8());
        Game.Recruiter = fields[8].GetUInt32();
        BattleNet.OS = fields[9].GetString();
        BattleNet.Id = fields[10].GetUInt32();
        Game.Security = AccountTypes(fields[11].GetUInt8());
        BattleNet.IsBanned = fields[12].GetUInt64() != 0;
        Game.IsBanned = fields[13].GetUInt64() != 0;
        Game.IsRectuiter = fields[14].GetUInt32() != 0;

        uint32 world_expansion = sWorld->getIntConfig(CONFIG_EXPANSION);
        if (Game.Expansion > world_expansion)
            Game.Expansion = world_expansion;

        if (BattleNet.Locale >= TOTAL_LOCALES)
            BattleNet.Locale = LOCALE_enUS;
    }
};

void WorldSocket::HandleAuthSession(std::shared_ptr<WorldPackets::Auth::AuthSession> authSession)
{
    // Client switches packet headers after sending CMSG_AUTH_SESSION
    _headerBuffer.Resize(SizeOfClientHeader[1][1]);

    // Get the account information from the auth database
    PreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_ACCOUNT_INFO_BY_NAME);
    stmt->setInt32(0, int32(realm.Id.Realm));
    stmt->setString(1, authSession->Account);

    {
        std::lock_guard<std::mutex> guard(_queryLock);
        _queryCallback = io_service().wrap(std::bind(&WorldSocket::HandleAuthSessionCallback, this, authSession, std::placeholders::_1));
        _queryFuture = LoginDatabase.AsyncQuery(stmt);
    }
}

void WorldSocket::HandleAuthSessionCallback(std::shared_ptr<WorldPackets::Auth::AuthSession> authSession, PreparedQueryResult result)
{
    // Stop if the account is not found
    if (!result)
    {
        // We can not log here, as we do not know the account. Thus, no accountId.
        SendAuthResponseError(AUTH_UNKNOWN_ACCOUNT);
        TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: Sent Auth Response (unknown account).");
        DelayedCloseSocket();
        return;
    }

    AccountInfo account(result->Fetch());

    // For hook purposes, we get Remoteaddress at this point.
    std::string address = GetRemoteIpAddress().to_string();

    // As we don't know if attempted login process by ip works, we update last_attempt_ip right away
    PreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_LAST_ATTEMPT_IP);
    stmt->setString(0, address);
    stmt->setString(1, authSession->Account);
    LoginDatabase.Execute(stmt);
    // This also allows to check for possible "hack" attempts on account

    // even if auth credentials are bad, try using the session key we have - client cannot read auth response error without it
    _authCrypt.Init(&account.Game.SessionKey);

    // First reject the connection if packet contains invalid data or realm state doesn't allow logging in
    if (sWorld->IsClosed())
    {
        SendAuthResponseError(AUTH_REJECT);
        TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: World closed, denying client (%s).", GetRemoteIpAddress().to_string().c_str());
        DelayedCloseSocket();
        return;
    }

    if (authSession->RealmID != realm.Id.Realm)
    {
        SendAuthResponseError(REALM_LIST_REALM_NOT_FOUND);
        TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: Sent Auth Response (bad realm).");
        DelayedCloseSocket();
        return;
    }

    // Must be done before WorldSession is created
    bool wardenActive = sWorld->getBoolConfig(CONFIG_WARDEN_ENABLED);
    if (wardenActive && account.BattleNet.OS != "Win" && account.BattleNet.OS != "Wn64" && account.BattleNet.OS != "Mc64")
    {
        SendAuthResponseError(AUTH_REJECT);
        TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: Client %s attempted to log in using invalid client OS (%s).", address.c_str(), account.BattleNet.OS.c_str());
        DelayedCloseSocket();
        return;
    }

    if (!account.BattleNet.Id || authSession->LoginServerType != 1)
    {
        SendAuthResponseError(AUTH_REJECT);
        TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: Client %s (%s) attempted to log in using deprecated login method (GRUNT).", authSession->Account.c_str(), address.c_str());
        DelayedCloseSocket();
        return;
    }

    // Check that Key and account name are the same on client and server
    uint32 t = 0;

    SHA1Hash sha;
    sha.UpdateData(authSession->Account);
    sha.UpdateData((uint8*)&t, 4);
    sha.UpdateData((uint8*)&authSession->LocalChallenge, 4);
    sha.UpdateData((uint8*)&_authSeed, 4);
    sha.UpdateBigNumbers(&account.Game.SessionKey, NULL);
    sha.Finalize();

    if (memcmp(sha.GetDigest(), authSession->Digest, SHA_DIGEST_LENGTH) != 0)
    {
        SendAuthResponseError(AUTH_FAILED);
        TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: Authentication failed for account: %u ('%s') address: %s", account.Game.Id, authSession->Account.c_str(), address.c_str());
        DelayedCloseSocket();
        return;
    }

    ///- Re-check ip locking (same check as in auth).
    if (account.BattleNet.IsLockedToIP)
    {
        if (account.BattleNet.LastIP != address)
        {
            SendAuthResponseError(AUTH_FAILED);
            TC_LOG_DEBUG("network", "WorldSocket::HandleAuthSession: Sent Auth Response (Account IP differs. Original IP: %s, new IP: %s).", account.BattleNet.LastIP.c_str(), address.c_str());
            // We could log on hook only instead of an additional db log, however action logger is config based. Better keep DB logging as well
            sScriptMgr->OnFailedAccountLogin(account.Game.Id);
            DelayedCloseSocket();
            return;
        }
    }
    else if (!account.BattleNet.LockCountry.empty() && account.BattleNet.LockCountry != "00" && !_ipCountry.empty())
    {
        if (account.BattleNet.LockCountry != _ipCountry)
        {
            SendAuthResponseError(AUTH_FAILED);
            TC_LOG_DEBUG("network", "WorldSocket::HandleAuthSession: Sent Auth Response (Account country differs. Original country: %s, new country: %s).", account.BattleNet.LockCountry.c_str(), _ipCountry.c_str());
            // We could log on hook only instead of an additional db log, however action logger is config based. Better keep DB logging as well
            sScriptMgr->OnFailedAccountLogin(account.Game.Id);
            DelayedCloseSocket();
            return;
        }
    }

    int64 mutetime = account.Game.MuteTime;
    //! Negative mutetime indicates amount of seconds to be muted effective on next login - which is now.
    if (mutetime < 0)
    {
        mutetime = time(NULL) + llabs(mutetime);

        stmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_MUTE_TIME_LOGIN);
        stmt->setInt64(0, mutetime);
        stmt->setUInt32(1, account.Game.Id);
        LoginDatabase.Execute(stmt);
    }

    if (account.IsBanned())
    {
        SendAuthResponseError(AUTH_BANNED);
        TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: Sent Auth Response (Account banned).");
        sScriptMgr->OnFailedAccountLogin(account.Game.Id);
        DelayedCloseSocket();
        return;
    }

    // Check locked state for server
    AccountTypes allowedAccountType = sWorld->GetPlayerSecurityLimit();
    TC_LOG_DEBUG("network", "Allowed Level: %u Player Level %u", allowedAccountType, account.Game.Security);
    if (allowedAccountType > SEC_PLAYER && account.Game.Security < allowedAccountType)
    {
        SendAuthResponseError(AUTH_UNAVAILABLE);
        TC_LOG_DEBUG("network", "WorldSocket::HandleAuthSession: User tries to login but his security level is not enough");
        sScriptMgr->OnFailedAccountLogin(account.Game.Id);
        DelayedCloseSocket();
        return;
    }

    TC_LOG_DEBUG("network", "WorldSocket::HandleAuthSession: Client '%s' authenticated successfully from %s.", authSession->Account.c_str(), address.c_str());

    // Update the last_ip in the database as it was successful for login
    stmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_LAST_IP);

    stmt->setString(0, address);
    stmt->setString(1, authSession->Account);

    LoginDatabase.Execute(stmt);

    // At this point, we can safely hook a successful login
    sScriptMgr->OnAccountLogin(account.Game.Id);

    _authed = true;
    _worldSession = new WorldSession(account.Game.Id, std::move(authSession->Account), account.BattleNet.Id, shared_from_this(), account.Game.Security,
        account.Game.Expansion, mutetime, account.BattleNet.Locale, account.Game.Recruiter, account.Game.IsRectuiter);
    _worldSession->ReadAddonsInfo(authSession->AddonInfo);

    // Initialize Warden system only if it is enabled by config
    if (wardenActive)
        _worldSession->InitWarden(&account.Game.SessionKey, account.BattleNet.OS);

    _queryCallback = io_service().wrap(std::bind(&WorldSocket::LoadSessionPermissionsCallback, this, std::placeholders::_1));
    _queryFuture = _worldSession->LoadPermissionsAsync();
    AsyncRead();
}

void WorldSocket::LoadSessionPermissionsCallback(PreparedQueryResult result)
{
    // RBAC must be loaded before adding session to check for skip queue permission
    _worldSession->GetRBACData()->LoadFromDBCallback(result);

    sWorld->AddSession(_worldSession);
}

void WorldSocket::HandleAuthContinuedSession(std::shared_ptr<WorldPackets::Auth::AuthContinuedSession> authSession)
{
    WorldSession::ConnectToKey key;
    key.Raw = authSession->Key;

    _type = ConnectionType(key.Fields.ConnectionType);
    if (_type != CONNECTION_TYPE_INSTANCE)
    {
        SendAuthResponseError(AUTH_UNKNOWN_ACCOUNT);
        DelayedCloseSocket();
        return;
    }

    // Client switches packet headers after sending CMSG_AUTH_CONTINUED_SESSION
    _headerBuffer.Resize(SizeOfClientHeader[1][1]);

    uint32 accountId = uint32(key.Fields.AccountId);
    PreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_ACCOUNT_INFO_CONTINUED_SESSION);
    stmt->setUInt32(0, accountId);

    {
        std::lock_guard<std::mutex> guard(_queryLock);
        _queryCallback = io_service().wrap(std::bind(&WorldSocket::HandleAuthContinuedSessionCallback, this, authSession, std::placeholders::_1));
        _queryFuture = LoginDatabase.AsyncQuery(stmt);
    }
}

void WorldSocket::HandleAuthContinuedSessionCallback(std::shared_ptr<WorldPackets::Auth::AuthContinuedSession> authSession, PreparedQueryResult result)
{
    if (!result)
    {
        SendAuthResponseError(AUTH_UNKNOWN_ACCOUNT);
        DelayedCloseSocket();
        return;
    }

    WorldSession::ConnectToKey key;
    key.Raw = authSession->Key;

    uint32 accountId = uint32(key.Fields.AccountId);
    Field* fields = result->Fetch();
    std::string login = fields[0].GetString();
    BigNumber k;
    k.SetHexStr(fields[1].GetCString());

    _authCrypt.Init(&k, _encryptSeed.AsByteArray().get(), _decryptSeed.AsByteArray().get());

    SHA1Hash sha;
    sha.UpdateData(login);
    sha.UpdateBigNumbers(&k, NULL);
    sha.UpdateData((uint8*)&_authSeed, 4);
    sha.Finalize();

    if (memcmp(sha.GetDigest(), authSession->Digest, sha.GetLength()))
    {
        SendAuthResponseError(AUTH_UNKNOWN_ACCOUNT);
        TC_LOG_ERROR("network", "WorldSocket::HandleAuthContinuedSession: Authentication failed for account: %u ('%s') address: %s", accountId, login.c_str(), GetRemoteIpAddress().to_string().c_str());
        DelayedCloseSocket();
        return;
    }

    sWorld->AddInstanceSocket(shared_from_this(), authSession->Key);
    AsyncRead();
}

void WorldSocket::HandleConnectToFailed(WorldPackets::Auth::ConnectToFailed& connectToFailed)
{
    if (_worldSession)
    {
        if (_worldSession->PlayerLoading())
        {
            switch (connectToFailed.Serial)
            {
                case WorldPackets::Auth::ConnectToSerial::WorldAttempt1:
                    _worldSession->SendConnectToInstance(WorldPackets::Auth::ConnectToSerial::WorldAttempt2);
                    break;
                case WorldPackets::Auth::ConnectToSerial::WorldAttempt2:
                    _worldSession->SendConnectToInstance(WorldPackets::Auth::ConnectToSerial::WorldAttempt3);
                    break;
                case WorldPackets::Auth::ConnectToSerial::WorldAttempt3:
                    _worldSession->SendConnectToInstance(WorldPackets::Auth::ConnectToSerial::WorldAttempt4);
                    break;
                case WorldPackets::Auth::ConnectToSerial::WorldAttempt4:
                    _worldSession->SendConnectToInstance(WorldPackets::Auth::ConnectToSerial::WorldAttempt5);
                    break;
                case WorldPackets::Auth::ConnectToSerial::WorldAttempt5:
                {
                    TC_LOG_ERROR("network", "%s failed to connect 5 times to world socket, aborting login", _worldSession->GetPlayerInfo().c_str());
                    _worldSession->AbortLogin(WorldPackets::Character::LoginFailureReason::NoWorld);
                    break;
                }
                default:
                    return;
            }
        }
        //else
        //{
        //    transfer_aborted when/if we get map node redirection
        //    SendPacketAndLogOpcode(*WorldPackets::Auth::ResumeComms().Write());
        //}
    }
}

void WorldSocket::SendAuthResponseError(uint8 code)
{
    WorldPackets::Auth::AuthResponse response;
    response.Result = code;
    SendPacketAndLogOpcode(*response.Write());
}

bool WorldSocket::HandlePing(WorldPacket& recvPacket)
{
    using namespace std::chrono;

    uint32 ping;
    uint32 latency;

    // Get the ping packet content
    recvPacket >> ping;
    recvPacket >> latency;

    if (_LastPingTime == steady_clock::time_point())
    {
        _LastPingTime = steady_clock::now();
    }
    else
    {
        steady_clock::time_point now = steady_clock::now();

        steady_clock::duration diff = now - _LastPingTime;

        _LastPingTime = now;

        if (diff < seconds(27))
        {
            ++_OverSpeedPings;

            uint32 maxAllowed = sWorld->getIntConfig(CONFIG_MAX_OVERSPEED_PINGS);

            if (maxAllowed && _OverSpeedPings > maxAllowed)
            {
                std::unique_lock<std::mutex> sessionGuard(_worldSessionLock);

                if (_worldSession && !_worldSession->HasPermission(rbac::RBAC_PERM_SKIP_CHECK_OVERSPEED_PING))
                {
                    TC_LOG_ERROR("network", "WorldSocket::HandlePing: %s kicked for over-speed pings (address: %s)",
                        _worldSession->GetPlayerInfo().c_str(), GetRemoteIpAddress().to_string().c_str());

                    return false;
                }
            }
        }
        else
            _OverSpeedPings = 0;
    }

    {
        std::lock_guard<std::mutex> sessionGuard(_worldSessionLock);

        if (_worldSession)
        {
            _worldSession->SetLatency(latency);
            _worldSession->ResetClientTimeDelay();
        }
        else
        {
            TC_LOG_ERROR("network", "WorldSocket::HandlePing: peer sent CMSG_PING, but is not authenticated or got recently kicked, address = %s", GetRemoteIpAddress().to_string().c_str());
            return false;
        }
    }

    WorldPacket packet(SMSG_PONG, 4);
    packet << ping;
    SendPacketAndLogOpcode(packet);
    return true;
}
