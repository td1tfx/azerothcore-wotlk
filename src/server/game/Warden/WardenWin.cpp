/*
 * Copyright (C) 2016+     AzerothCore <www.azerothcore.org>, released under GNU GPL v2 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-GPL2
 * Copyright (C) 2008-2016 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 */

#include "Cryptography/HMACSHA1.h"
#include "Cryptography/WardenKeyGeneration.h"
#include "Common.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Log.h"
#include "Opcodes.h"
#include "ByteBuffer.h"
#include <openssl/md5.h>
#include "Database/DatabaseEnv.h"
#include "World.h"
#include "Player.h"
#include "Util.h"
#include "WardenWin.h"
#include "WardenModuleWin.h"
#include "WardenCheckMgr.h"
#include "AccountMgr.h"

// GUILD is the shortest string that has no client validation (RAID only sends if in a raid group)
static constexpr char _luaEvalPrefix[] = "local S,T,R=SendAddonMessage,function()";
static constexpr char _luaEvalMidfix[] = " end R=S and T()if R then S('_TW',";
static constexpr char _luaEvalPostfix[] = ",'GUILD')end";

static_assert((sizeof(_luaEvalPrefix)-1 + sizeof(_luaEvalMidfix)-1 + sizeof(_luaEvalPostfix)-1 + WARDEN_MAX_LUA_CHECK_LENGTH) == 255);

static constexpr uint8 GetCheckPacketBaseSize(uint8 type)
{
    switch (type)
    {
    case DRIVER_CHECK:
    case MPQ_CHECK: return 1;
    case LUA_EVAL_CHECK: return 1 + sizeof(_luaEvalPrefix) - 1 + sizeof(_luaEvalMidfix) - 1 + 4 + sizeof(_luaEvalPostfix) - 1;
    case PAGE_CHECK_A: return (4 + 1);
    case PAGE_CHECK_B: return (4 + 1);
    case MODULE_CHECK: return (4 + SHA_DIGEST_LENGTH);
    case MEM_CHECK: return (1 + 4 + 1);
    default: return 0;
    }
}

static uint16 GetCheckPacketSize(WardenCheck const* check)
{
    if (!check)
    {
        return 0;
    }

    uint16 size = 1 + GetCheckPacketBaseSize(check->Type); // 1 byte check type
    if (!check->Str.empty())
    {
        size += (static_cast<uint16>(check->Str.length()) + 1); // 1 byte string length
    }

    BigNumber tempNumber = check->Data;
    if (!tempNumber.GetNumBytes())
    {
        size += tempNumber.GetNumBytes();
    }
    return size;
}

// Returns config id for specific type id
static WorldIntConfigs GetMaxWardenChecksForType(uint8 type)
{
    // Should never be higher type than defined
    ASSERT(type < MAX_WARDEN_CHECK_TYPES);

    switch (type)
    {
    case WARDEN_CHECK_MEM_TYPE:
        return CONFIG_WARDEN_NUM_MEM_CHECKS;
    case WARDEN_CHECK_LUA_TYPE:
        return CONFIG_WARDEN_NUM_LUA_CHECKS;
    default:
        break;
    }

    return CONFIG_WARDEN_NUM_OTHER_CHECKS;
}

WardenWin::WardenWin() : Warden(), _serverTicks(0) { }

WardenWin::~WardenWin()
{
}

void WardenWin::Init(WorldSession* session, BigNumber* k)
{
    _session = session;
    // Generate Warden Key
    SHA1Randx WK(k->AsByteArray().get(), k->GetNumBytes());
    WK.Generate(_inputKey, 16);
    WK.Generate(_outputKey, 16);

    memcpy(_seed, Module.Seed, 16);

    _inputCrypto.Init(_inputKey);
    _outputCrypto.Init(_outputKey);
#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
    sLog->outDebug(LOG_FILTER_WARDEN, "Server side warden for client %u initializing...", session->GetAccountId());
    sLog->outDebug(LOG_FILTER_WARDEN, "C->S Key: %s", ByteArrayToHexStr(_inputKey, 16).c_str());
    sLog->outDebug(LOG_FILTER_WARDEN, "S->C Key: %s", ByteArrayToHexStr(_outputKey, 16).c_str());
    sLog->outDebug(LOG_FILTER_WARDEN, "  Seed: %s", ByteArrayToHexStr(_seed, 16).c_str());
    sLog->outDebug(LOG_FILTER_WARDEN, "Loading Module...");
#endif

    _module = GetModuleForClient();

#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
    sLog->outDebug(LOG_FILTER_WARDEN, "Module Key: %s", ByteArrayToHexStr(_module->Key, 16).c_str());
    sLog->outDebug(LOG_FILTER_WARDEN, "Module ID: %s", ByteArrayToHexStr(_module->Id, 16).c_str());
#endif
    RequestModule();
}

ClientWardenModule* WardenWin::GetModuleForClient()
{
    ClientWardenModule* mod = new ClientWardenModule;

    uint32 length = sizeof(Module.Module);

    // data assign
    mod->CompressedSize = length;
    mod->CompressedData = new uint8[length];
    memcpy(mod->CompressedData, Module.Module, length);
    memcpy(mod->Key, Module.ModuleKey, 16);

    // md5 hash
    MD5_CTX ctx;
    MD5_Init(&ctx);
    MD5_Update(&ctx, mod->CompressedData, length);
    MD5_Final((uint8*)&mod->Id, &ctx);

    return mod;
}

void WardenWin::InitializeModule()
{
#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
    sLog->outDebug(LOG_FILTER_WARDEN, "Initialize module");
#endif

    // Create packet structure
    WardenInitModuleRequest Request;
    Request.Command1 = WARDEN_SMSG_MODULE_INITIALIZE;
    Request.Size1 = 20;
    Request.Unk1 = 1;
    Request.Unk2 = 0;
    Request.Type = 1;
    Request.String_library1 = 0;
    Request.Function1[0] = 0x00024F80;                      // 0x00400000 + 0x00024F80 SFileOpenFile
    Request.Function1[1] = 0x000218C0;                      // 0x00400000 + 0x000218C0 SFileGetFileSize
    Request.Function1[2] = 0x00022530;                      // 0x00400000 + 0x00022530 SFileReadFile
    Request.Function1[3] = 0x00022910;                      // 0x00400000 + 0x00022910 SFileCloseFile
    Request.CheckSumm1 = BuildChecksum(&Request.Unk1, SHA_DIGEST_LENGTH);

    Request.Command2 = WARDEN_SMSG_MODULE_INITIALIZE;
    Request.Size2 = 8;
    Request.Unk3 = 4;
    Request.Unk4 = 0;
    Request.String_library2 = 0;
    Request.Function2 = 0x00419210;                         // 0x00400000 + 0x00419210 FrameScript::Execute
    Request.Function2_set = 1;
    Request.CheckSumm2 = BuildChecksum(&Request.Unk3, 8);

    Request.Command3 = WARDEN_SMSG_MODULE_INITIALIZE;
    Request.Size3 = 8;
    Request.Unk5 = 1;
    Request.Unk6 = 1;
    Request.String_library3 = 0;
    Request.Function3 = 0x0046AE20;                         // 0x00400000 + 0x0046AE20 PerformanceCounter
    Request.Function3_set = 1;
    Request.CheckSumm3 = BuildChecksum(&Request.Unk5, 8);

    EndianConvert(Request.Size1);
    EndianConvert(Request.CheckSumm1);
    EndianConvert(Request.Function1[0]);
    EndianConvert(Request.Function1[1]);
    EndianConvert(Request.Function1[2]);
    EndianConvert(Request.Function1[3]);
    EndianConvert(Request.Size2);
    EndianConvert(Request.CheckSumm2);
    EndianConvert(Request.Function2);
    EndianConvert(Request.Size3);
    EndianConvert(Request.CheckSumm3);
    EndianConvert(Request.Function3);

    // Encrypt with warden RC4 key.
    EncryptData(reinterpret_cast<uint8*>(&Request), sizeof(WardenInitModuleRequest));

    WorldPacket pkt(SMSG_WARDEN_DATA, sizeof(WardenInitModuleRequest));
    pkt.append(reinterpret_cast<uint8*>(&Request), sizeof(WardenInitModuleRequest));
    _session->SendPacket(&pkt);
}

void WardenWin::RequestHash()
{
#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
    sLog->outDebug(LOG_FILTER_WARDEN, "Request hash");
#endif

    // Create packet structure
    WardenHashRequest Request;
    Request.Command = WARDEN_SMSG_HASH_REQUEST;
    memcpy(Request.Seed, _seed, 16);

    // Encrypt with warden RC4 key.
    EncryptData(reinterpret_cast<uint8*>(&Request), sizeof(WardenHashRequest));

    WorldPacket pkt(SMSG_WARDEN_DATA, sizeof(WardenHashRequest));
    pkt.append(reinterpret_cast<uint8*>(&Request), sizeof(WardenHashRequest));
    _session->SendPacket(&pkt);
}

void WardenWin::HandleHashResult(ByteBuffer& buff)
{
    buff.rpos(buff.wpos());

    // Verify key
    if (memcmp(buff.contents() + 1, Module.ClientKeySeedHash, SHA_DIGEST_LENGTH) != 0)
    {
#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
        sLog->outDebug(LOG_FILTER_WARDEN, "Request hash reply: failed");
#endif
        ApplyPenalty(0, "Request hash reply: failed");
        return;
    }

#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
    sLog->outDebug(LOG_FILTER_WARDEN, "Request hash reply: succeed");
#endif

    // Change keys here
    memcpy(_inputKey, Module.ClientKeySeed, 16);
    memcpy(_outputKey, Module.ServerKeySeed, 16);

    _inputCrypto.Init(_inputKey);
    _outputCrypto.Init(_outputKey);

    _initialized = true;
}

void WardenWin::RequestChecks()
{
#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
    sLog->outDebug(LOG_FILTER_WARDEN, "Request data");
#endif

    // If all checks were done, fill the todo list again
    for (uint8 i = 0; i < MAX_WARDEN_CHECK_TYPES; ++i)
    {
        if (_ChecksTodo[i].empty())
            _ChecksTodo[i].assign(sWardenCheckMgr->CheckIdPool[i].begin(), sWardenCheckMgr->CheckIdPool[i].end());
    }

    _serverTicks = World::GetGameTimeMS();
    _CurrentChecks.clear();

    // No pending checks
    if (_PendingChecks.empty())
    {
        for (uint8 checkType = 0; checkType < MAX_WARDEN_CHECK_TYPES; ++checkType)
        {
            for (uint32 y = 0; y < sWorld->getIntConfig(GetMaxWardenChecksForType(checkType)); ++y)
            {
                // If todo list is done break loop (will be filled on next Update() run)
                if (_ChecksTodo[checkType].empty())
                {
                    break;
                }

                // Get check id from the end and remove it from todo
                uint16 const id = _ChecksTodo[checkType].back();
                _ChecksTodo[checkType].pop_back();

                // Insert check to queue
                if (checkType == WARDEN_CHECK_LUA_TYPE)
                {
                    _CurrentChecks.push_front(id);
                }
                else
                {
                    _CurrentChecks.push_back(id);
                }
            }
        }
    }
    else
    {
        bool hasLuaChecks = false;
        for (uint16 const checkId : _PendingChecks)
        {
            WardenCheck const* check = sWardenCheckMgr->GetWardenDataById(checkId);
            if (!hasLuaChecks && check->Type == LUA_EVAL_CHECK)
            {
                hasLuaChecks = true;
            }

            _CurrentChecks.push_back(checkId);
        }

        // Always include lua checks
        if (!hasLuaChecks)
        {
            for (uint32 i = 0; i < sWorld->getIntConfig(GetMaxWardenChecksForType(WARDEN_CHECK_LUA_TYPE)); ++i)
            {
                // If todo list is done break loop (will be filled on next Update() run)
                if (_ChecksTodo[WARDEN_CHECK_LUA_TYPE].empty())
                {
                    break;
                }

                // Get check id from the end and remove it from todo
                uint16 const id = _ChecksTodo[WARDEN_CHECK_LUA_TYPE].back();
                _ChecksTodo[WARDEN_CHECK_LUA_TYPE].pop_back();

                // Lua checks must be always in front
                _CurrentChecks.push_front(id);
            }
        }
    }

    // Filter too high checks queue
    // Filtered checks will get passed in next checks
    uint16 expectedSize = 4;
    _PendingChecks.clear();
    acore::Containers::EraseIf(_CurrentChecks,
        [this, &expectedSize](uint16 id)
        {
            uint16 const thisSize = GetCheckPacketSize(sWardenCheckMgr->GetWardenDataById(id));
            if ((expectedSize + thisSize) > 500) // warden packets are truncated to 512 bytes clientside
            {
                _PendingChecks.push_back(id);
                return true;
            }
            expectedSize += thisSize;
            return false;
        }
    );

    ByteBuffer buff;
    buff << uint8(WARDEN_SMSG_CHEAT_CHECKS_REQUEST);

    for (uint16 const checkId : _CurrentChecks)
    {
        WardenCheck const* check = sWardenCheckMgr->GetWardenDataById(checkId);
        switch (check->Type)
        {
            case LUA_EVAL_CHECK:
            {
                buff << uint8(sizeof(_luaEvalPrefix) - 1 + check->Str.size() + sizeof(_luaEvalMidfix) - 1 + check->IdStr.size() + sizeof(_luaEvalPostfix) - 1);
                buff.append(_luaEvalPrefix, sizeof(_luaEvalPrefix) - 1);
                buff.append(check->Str.data(), check->Str.size());
                buff.append(_luaEvalMidfix, sizeof(_luaEvalMidfix) - 1);
                buff.append(check->IdStr.data(), check->IdStr.size());
                buff.append(_luaEvalPostfix, sizeof(_luaEvalPostfix) - 1);
                break;
            }
            case MPQ_CHECK:
            case DRIVER_CHECK:
            {
                buff << uint8(check->Str.size());
                buff.append(check->Str.c_str(), check->Str.size());
                break;
            }
        }
    }

    uint8 const xorByte = _inputKey[0];

    // Add TIMING_CHECK
    buff << uint8(0x00);
    buff << uint8(TIMING_CHECK ^ xorByte);

    uint8 index = 1;

    for (uint16 const checkId : _CurrentChecks)
    {
        WardenCheck const* check = sWardenCheckMgr->GetWardenDataById(checkId);
        buff << uint8(check->Type ^ xorByte);
        switch (check->Type)
        {
            case MEM_CHECK:
            {
                buff << uint8(0x00);
                buff << uint32(check->Address);
                buff << uint8(check->Length);
                break;
            }
            case PAGE_CHECK_A:
            case PAGE_CHECK_B:
            {
                BigNumber tempNumber = check->Data;
                buff.append(tempNumber.AsByteArray(0, false).get(), tempNumber.GetNumBytes());
                buff << uint32(check->Address);
                buff << uint8(check->Length);
                break;
            }
            case MPQ_CHECK:
            case LUA_EVAL_CHECK:
            {
                buff << uint8(index++);
                break;
            }
            case DRIVER_CHECK:
            {
                BigNumber tempNumber = check->Data;
                buff.append(tempNumber.AsByteArray(0, false).get(), tempNumber.GetNumBytes());
                buff << uint8(index++);
                break;
            }
            case MODULE_CHECK:
            {
                uint32 seed = rand32();
                buff << uint32(seed);
                HmacHash hmac(4, (uint8*)&seed);
                hmac.UpdateData(check->Str);
                hmac.Finalize();
                buff.append(hmac.GetDigest(), hmac.GetLength());
                break;
            }
            /*case PROC_CHECK:
            {
                buff.append(wd->i.AsByteArray(0, false).get(), wd->i.GetNumBytes());
                buff << uint8(index++);
                buff << uint8(index++);
                buff << uint32(wd->Address);
                buff << uint8(wd->Length);
                break;
            }*/
        }
    }
    buff << uint8(xorByte);
    buff.hexlike();

    // Encrypt with warden RC4 key
    EncryptData(buff.contents(), buff.size());

    WorldPacket pkt(SMSG_WARDEN_DATA, buff.size());
    pkt.append(buff);
    _session->SendPacket(&pkt);

    _dataSent = true;

#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
    std::stringstream stream;
    stream << "Sent check id's: ";
    for (uint16 checkId : _currentChecks)
    {
        stream << checkId << " ";
    }

    sLog->outDebug(LOG_FILTER_WARDEN, "%s", stream.str().c_str());
#endif
}

void WardenWin::HandleData(ByteBuffer& buff)
{
#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
    sLog->outDebug(LOG_FILTER_WARDEN, "Handle data");
#endif

    _dataSent = false;
    _clientResponseTimer = 0;

    uint16 Length;
    buff >> Length;
    uint32 Checksum;
    buff >> Checksum;

    if (Length != (buff.size() - buff.rpos()))
    {
        buff.rfinish();
        ApplyPenalty(0, "Failed size checks in HandleData");
        return;
    }

    if (!IsValidCheckSum(Checksum, buff.contents() + buff.rpos(), Length))
    {
        buff.rpos(buff.wpos());
#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
        sLog->outDebug(LOG_FILTER_WARDEN, "CHECKSUM FAIL");
#endif
        ApplyPenalty(0, "Failed checksum in HandleData");
        return;
    }

    // TIMING_CHECK
    {
        uint8 result;
        buff >> result;
        // TODO: test it.
        if (result == 0x00)
        {
#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
            sLog->outDebug(LOG_FILTER_WARDEN, "TIMING CHECK FAIL result 0x00");
#endif
            ApplyPenalty(0, "TIMING CHECK FAIL result");
            return;
        }

        uint32 newClientTicks;
        buff >> newClientTicks;

#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
        uint32 ticksNow = World::GetGameTimeMS();
        uint32 ourTicks = newClientTicks + (ticksNow - _serverTicks);

        sLog->outDebug(LOG_FILTER_WARDEN, "ServerTicks %u", ticksNow);         // Now
        sLog->outDebug(LOG_FILTER_WARDEN, "RequestTicks %u", _serverTicks);    // At request
        sLog->outDebug(LOG_FILTER_WARDEN, "Ticks %u", newClientTicks);         // At response
        sLog->outDebug(LOG_FILTER_WARDEN, "Ticks diff %u", ourTicks - newClientTicks);
#endif
    }

    uint16 checkFailed = 0;

    for (uint16 const checkId : _CurrentChecks)
    {
        WardenCheck const* rd = sWardenCheckMgr->GetWardenDataById(checkId);
        uint8 const type = rd->Type;
        switch (type)
        {
            case MEM_CHECK:
            {
                uint8 Mem_Result;
                buff >> Mem_Result;

                if (Mem_Result != 0)
                {
#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
                    sLog->outDebug(LOG_FILTER_WARDEN, "RESULT MEM_CHECK not 0x00, CheckId %u account Id %u", checkId, _session->GetAccountId());
#endif
                    checkFailed = checkId;
                    continue;
                }

                WardenCheckResult const* rs = sWardenCheckMgr->GetWardenResultById(checkId);
                BigNumber tempNumber = rs->Result;
                if (memcmp(buff.contents() + buff.rpos(), tempNumber.AsByteArray(0, false).get(), rd->Length) != 0)
                {
#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
                    sLog->outDebug(LOG_FILTER_WARDEN, "RESULT MEM_CHECK fail CheckId %u account Id %u", checkId, _session->GetAccountId());
#endif
                    checkFailed = checkId;
                    buff.rpos(buff.rpos() + rd->Length);
                    continue;
                }

                buff.rpos(buff.rpos() + rd->Length);
#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
                sLog->outDebug(LOG_FILTER_WARDEN, "RESULT MEM_CHECK passed CheckId %u account Id %u", checkId, _session->GetAccountId());
#endif
                break;
            }
            case PAGE_CHECK_A:
            case PAGE_CHECK_B:
            case DRIVER_CHECK:
            case MODULE_CHECK:
                {
                    const uint8 byte = 0xE9;
                    if (memcmp(buff.contents() + buff.rpos(), &byte, sizeof(uint8)) != 0)
                    {
#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
                        if (type == PAGE_CHECK_A || type == PAGE_CHECK_B)
                            sLog->outDebug(LOG_FILTER_WARDEN, "RESULT PAGE_CHECK fail, CheckId %u account Id %u", checkId, _session->GetAccountId());

                        if (type == MODULE_CHECK)
                            sLog->outDebug(LOG_FILTER_WARDEN, "RESULT MODULE_CHECK fail, CheckId %u account Id %u", checkId, _session->GetAccountId());

                        if (type == DRIVER_CHECK)
                            sLog->outDebug(LOG_FILTER_WARDEN, "RESULT DRIVER_CHECK fail, CheckId %u account Id %u", checkId, _session->GetAccountId());
#endif
                        checkFailed = checkId;
                        buff.rpos(buff.rpos() + 1);
                        continue;
                    }

                    buff.rpos(buff.rpos() + 1);

#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
                    if (type == PAGE_CHECK_A || type == PAGE_CHECK_B)
                        sLog->outDebug(LOG_FILTER_WARDEN, "RESULT PAGE_CHECK passed CheckId %u account Id %u", checkId, _session->GetAccountId());
                    else if (type == MODULE_CHECK)
                        sLog->outDebug(LOG_FILTER_WARDEN, "RESULT MODULE_CHECK passed CheckId %u account Id %u", checkId, _session->GetAccountId());
                    else if (type == DRIVER_CHECK)
                        sLog->outDebug(LOG_FILTER_WARDEN, "RESULT DRIVER_CHECK passed CheckId %u account Id %u", checkId, _session->GetAccountId());
#endif
                break;
            }
            case LUA_EVAL_CHECK:
            {
                uint8 const result = buff.read<uint8>();
                if (result == 0)
                {
                    buff.read_skip(buff.read<uint8>()); // discard attached string
                }

#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
                sLog->outDebug(LOG_FILTER_WARDEN, "LUA_EVAL_CHECK CheckId %u account Id %u got in-warden dummy response", checkId, _session->GetAccountId()/* , result */);
#endif
                    break;
                }
            case MPQ_CHECK:
                {
                    uint8 Mpq_Result;
                    buff >> Mpq_Result;

                    if (Mpq_Result != 0)
                    {
#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
                        sLog->outDebug(LOG_FILTER_WARDEN, "RESULT MPQ_CHECK not 0x00 account id %u", _session->GetAccountId());
#endif
                        checkFailed = checkId;
                        continue;
                    }

                    WardenCheckResult const* rs = sWardenCheckMgr->GetWardenResultById(checkId);
                    BigNumber tempNumber = rs->Result;
                    if (memcmp(buff.contents() + buff.rpos(), tempNumber.AsByteArray(0, false).get(), SHA_DIGEST_LENGTH) != 0) // SHA1
                    {
#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
                        sLog->outDebug(LOG_FILTER_WARDEN, "RESULT MPQ_CHECK fail, CheckId %u account Id %u", checkId, _session->GetAccountId());
#endif
                        checkFailed = checkId;
                        buff.rpos(buff.rpos() + SHA_DIGEST_LENGTH);            // 20 bytes SHA1
                        continue;
                    }

                    buff.rpos(buff.rpos() + SHA_DIGEST_LENGTH);                // 20 bytes SHA1
#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
                    sLog->outDebug(LOG_FILTER_WARDEN, "RESULT MPQ_CHECK passed, CheckId %u account Id %u", checkId, _session->GetAccountId());
#endif
                    break;
                }
        }
    }

    if (checkFailed > 0)
    {
        ApplyPenalty(checkFailed, "");
    }

    // Set hold off timer, minimum timer should at least be 1 second
    uint32 const holdOff = sWorld->getIntConfig(CONFIG_WARDEN_CLIENT_CHECK_HOLDOFF);
    _checkTimer = (holdOff < 1 ? 1 : holdOff) * IN_MILLISECONDS;
}
