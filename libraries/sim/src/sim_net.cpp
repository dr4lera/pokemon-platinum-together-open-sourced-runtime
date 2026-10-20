#include <enet/enet.h>
#include <nitro.h>
#include <simulator/sim.h>
#include <simulator/config/sim_config.h>
#include <simulator/sim_net.h>
#include <simulator/wmsp_indicate.h>
#include <simulator/sim_net_validation.h>
#include <cstddef>
#include <atomic>

extern "C" void WMSP_Wait4Wm9(void);

/* A WM receive callback borrows the ENet payload until its ARM9 FIFO callback
 * finishes. ReceiveMPData waits for that acknowledgement before this guard
 * releases the packet. Malformed/unhandled packets use the same release path. */
struct ReceivedPacket {
    ENetPacket* packet;
    ~ReceivedPacket() { if(packet) enet_packet_destroy(packet); }
};

#ifdef __WIN32__
    #include <winsock2.h>
    #include <ws2tcpip.h>

    #define socket_t    SOCKET
    #define sockaddr_t  SOCKADDR
    #define sockaddr_in_t  SOCKADDR_IN
#else
    #include <unistd.h>
    #include <netinet/in.h>
    #include <sys/select.h>
    #include <sys/socket.h>

    #define socket_t    int
    #define sockaddr_t  struct sockaddr
    #define sockaddr_in_t  struct sockaddr_in
    #define closesocket close
#endif

#ifndef INVALID_SOCKET
    #define INVALID_SOCKET  (socket_t)-1
#endif

#ifdef SDK_TRACY_ENABLE
#include "tracy/TracyC.h"
#endif

enum {
    SIM_NET_CHANNEL_MGMT, // Control, commands, etc
    SIM_NET_CHANNEL_APPDATA, // NDS application data
};

static SIM_Net_t net = {};
static std::atomic<const SIM_Net_Provider_t*> applicationProvider{nullptr};

BOOL SIM_Net_InstallProvider(const SIM_Net_Provider_t *provider)
{
    if (!provider || provider->version != 1 || !provider->process || !provider->beacon
        || !provider->startHost || !provider->startClient || !provider->getAid
        || !provider->getPlayerId || !provider->getState || !provider->getPlayerCount
        || !provider->getPlayers || !provider->setGameInfo || !provider->setMPState
        || !provider->join || !provider->disconnect || !provider->send || !provider->scan)
        return FALSE;
    const SIM_Net_Provider_t *expected = nullptr;
    if (const auto *installed = applicationProvider.load(std::memory_order_acquire)) return installed == provider ? TRUE : FALSE;
    if (net.state != SIM_NET_STATE_DISCONNECTED || net.enetHost != nullptr) return FALSE;
    /* WM can advertise before a transport is selected. Preserve its current
     * metadata/role when an application explicitly selects this provider. */
    provider->setGameInfo(provider->context, net.curPlayer.ggid, net.curPlayer.tgid,
        net.curPlayer.userGameInfo, net.curPlayer.userGameInfoLength);
    provider->setMPState(provider->context, net.curPlayer.mpState);
    return applicationProvider.compare_exchange_strong(expected, provider,
        std::memory_order_release, std::memory_order_acquire) ? TRUE : FALSE;
}

static int NetThread(void * arg);

static void InitMyPlayer();
static void SendMyPlayer();

static void InitPacketHeader(SIM_Net_PacketHeader_t * header);
static BOOL CheckPacketHeader(SIM_Net_PacketHeader_t * header);

static void ProcessHostEvent(ENetEvent * event);
static void ProcessClientEvent(ENetEvent * event);

static void ProcessMPParentEvent(ENetEvent * event);
static void ProcessMPChildEvent(ENetEvent * event);

static void ReceiveMPData(ENetEvent * event);

static SIM_Net_Player_t* EstablishedPlayer(ENetPeer* peer)
{
    if(!peer) return nullptr;
    for(int i=0; i<SIM_NET_MAX_PLAYERS; ++i) {
        if(net.peerList[i] == peer && peer->data == &net.playerList[i]) return &net.playerList[i];
    }
    return nullptr;
}

static void HostUpdatePlayerList();

void SIM_Net_Init()
{
    if(net.state != SIM_NET_STATE_UNINITIALIZED) {
        SIM_assert_always_msg("Net is already initialized!");
        return;
    }

    net.playerListMutex = SDL_CreateMutex();
    net.joinReqSemaphore = SDL_CreateSemaphore(0);

    net.state = SIM_NET_STATE_DISCONNECTED;
}

void SIM_Net_Process()
{
    if (auto *p = applicationProvider.load(std::memory_order_acquire)) { p->process(p->context); return; }
    if(net.enetHost == NULL || (net.state != SIM_NET_STATE_CLIENT && net.state != SIM_NET_STATE_HOST)) {
        return;
    }
    
    ENetEvent event;
    while(enet_host_service(net.enetHost, &event, 0) > 0) {
        ReceivedPacket received{event.type == ENET_EVENT_TYPE_RECEIVE ? event.packet : nullptr};
        #ifdef SDK_TRACY_ENABLE
        TracyCZone(ctx, 1);
        #endif
        if(net.state == SIM_NET_STATE_CLIENT) {
            ProcessClientEvent(&event);
        } else if(net.state == SIM_NET_STATE_HOST) {
            ProcessHostEvent(&event);
        }
        #ifdef SDK_TRACY_ENABLE
        TracyCZoneEnd(ctx);
        #endif
    }
}

void SIM_Net_SendBeaconIndication()
{
    if (auto *p = applicationProvider.load(std::memory_order_acquire)) { p->beacon(p->context); return; }
    // Send Beacon notification to WM
    if(net.curPlayer.mpState == SIM_NET_MPSTATE_PARENT) {
        WMSP_IndicateSendBeacon();
    }
}

BOOL SIM_Net_StartHost()
{
    if (auto *p = applicationProvider.load(std::memory_order_acquire)) return p->startHost(p->context);
    if(net.state != SIM_NET_STATE_DISCONNECTED) {
        return FALSE;
    }

    ENetAddress myAddr;
    myAddr.host = ENET_HOST_ANY;
    myAddr.port = SIM_NET_PORT;

    net.enetHost = enet_host_create(&myAddr, 16, 2, 0, 0);
    if(net.enetHost == NULL) {
        return FALSE;
    }

    InitMyPlayer();

    net.curPlayer.id = 0;
    net.curPlayer.status = SIM_NET_PLAYERSTATUS_HOST;

    memcpy(&net.playerList[0], &net.curPlayer, sizeof(SIM_Net_Player_t));

    net.playerCount = 1;
    net.state = SIM_NET_STATE_HOST;
    return TRUE;
}

BOOL SIM_Net_StartClient(char * ipAddress)
{
    if (auto *p = applicationProvider.load(std::memory_order_acquire)) return p->startClient(p->context, ipAddress);
    if(net.state != SIM_NET_STATE_DISCONNECTED) {
        return FALSE;
    }

    net.enetHost = enet_host_create(NULL, 16, 2, 0, 0);
    if(net.enetHost == NULL) {
        return FALSE;
    }
    
    InitMyPlayer();

    ENetAddress myAddr;
    enet_address_set_host(&myAddr, ipAddress);
    myAddr.port = SIM_NET_PORT;

    ENetPeer * myPeer = enet_host_connect(net.enetHost, &myAddr, 2, 0);
    if(myPeer == NULL) {
        enet_host_destroy(net.enetHost);
        net.enetHost = NULL;
        return FALSE;
    }

    int connStatus = 0;

    u64 startTick = SDL_GetTicks64();
    const int timeoutDuration = 5000;
    ENetEvent event;
    while(TRUE) {
        u64 curTick = SDL_GetTicks64();
        if(curTick < startTick) {
            break;
        }

        if(curTick - startTick >= timeoutDuration) {
            break;
        }
        if(enet_host_service(net.enetHost, &event, 10) > 0) {
            ReceivedPacket received{event.type == ENET_EVENT_TYPE_RECEIVE ? event.packet : nullptr};
            if (connStatus == 0 && event.type == ENET_EVENT_TYPE_CONNECT)
            {
                connStatus = 1;
            } else if(connStatus == 1 && event.type == ENET_EVENT_TYPE_RECEIVE) {
                if(event.channelID != SIM_NET_CHANNEL_MGMT) {
                    continue;
                }
                if(event.packet->dataLength < sizeof(SIM_Net_ClientInitPacket_t)) {
                    continue;
                }
                
                SIM_Net_ClientInitPacket_t *  packetData = (SIM_Net_ClientInitPacket_t *)event.packet->data;
                
                if(!CheckPacketHeader(&packetData->common.header)) {
                    continue;
                }

                if(packetData->common.cmd != SIM_NET_MGMTPACKETCMD_CLIENT_INIT) {
                    continue;
                }

                if(!SIM_Net_ValidPlayerId(packetData->playerId) || packetData->playerId == 0) {
                    continue;
                }

                net.curPlayer.id = packetData->playerId;

                //Send player information to the host
                SIM_Net_PlayerInfoPacket_t playerInfoPacket;
                InitPacketHeader(&playerInfoPacket.common.header);
                playerInfoPacket.common.cmd = SIM_NET_MGMTPACKETCMD_PLAYER_INFO;
                memcpy(&playerInfoPacket.player, &net.curPlayer, sizeof(SIM_Net_Player_t));
                ENetPacket * pkt = enet_packet_create(&playerInfoPacket, sizeof(SIM_Net_PlayerInfoPacket_t), ENET_PACKET_FLAG_RELIABLE);
                if(!pkt) break;
                if(enet_peer_send(event.peer, SIM_NET_CHANNEL_MGMT, pkt) != 0) {
                    enet_packet_destroy(pkt);
                    break;
                }

                connStatus = 2;
                break;
            } else if(event.type == ENET_EVENT_TYPE_DISCONNECT) {
                connStatus = 0;
                break;
            }
        }
    }


    if(connStatus != 2) {
        enet_host_destroy(net.enetHost);
        net.enetHost = NULL;
        return FALSE;
    }

    net.peerList[0] = myPeer;
    myPeer->data = &net.playerList[0];
    net.state = SIM_NET_STATE_CLIENT;
    return TRUE;
}

int SIM_Net_GetAID()
{
    if (auto *p = applicationProvider.load(std::memory_order_acquire)) return p->getAid(p->context);
    return net.curPlayer.aid;
}

int SIM_Net_GetCurrentPlayerID()
{
    if (auto *p = applicationProvider.load(std::memory_order_acquire)) return p->getPlayerId(p->context);
    return net.curPlayer.id;
}

SIM_Net_State_t SIM_Net_GetCurrentState()
{
    if (auto *p = applicationProvider.load(std::memory_order_acquire)) return p->getState(p->context);
    return net.state;
}

int SIM_Net_GetPlayerCount()
{
    if (auto *p = applicationProvider.load(std::memory_order_acquire)) return p->getPlayerCount(p->context);
    return net.playerCount;
}

const SIM_Net_Player_t * SIM_Net_GetPlayerList()
{
    if (auto *p = applicationProvider.load(std::memory_order_acquire)) return p->getPlayers(p->context);
    return net.playerList;
}

void SIM_Net_SetGameInfo(int ggid, int tgid, u8 * userGameInfo, int userGameInfoLength)
{
    if (auto *p = applicationProvider.load(std::memory_order_acquire)) { p->setGameInfo(p->context, ggid, tgid, userGameInfo, userGameInfoLength); return; }
    if(userGameInfoLength < 0) return;
    net.curPlayer.ggid = ggid;
    net.curPlayer.tgid = tgid;
    if(userGameInfo) {
        if(userGameInfoLength > 112) {
            userGameInfoLength = 112;
        }
        memcpy(net.curPlayer.userGameInfo, userGameInfo, userGameInfoLength);
        net.curPlayer.userGameInfoLength = userGameInfoLength;
    }
    SendMyPlayer();
}

void SIM_Net_SetMPState(SIM_Net_MPState_t state)
{
    if (auto *p = applicationProvider.load(std::memory_order_acquire)) { p->setMPState(p->context, state); return; }
    net.curPlayer.mpState = state;
    SendMyPlayer();
}

BOOL SIM_Net_JoinMP(u8 * bssid, u8 * ssid)
{
    if (auto *p = applicationProvider.load(std::memory_order_acquire)) return p->join(p->context, bssid, ssid);
    // Find the connected player with matching mac address (bssid)
    int idNo = -1;
    for(int i=0; i < SIM_NET_MAX_PLAYERS; i++) {
        SIM_Net_Player_t * player = &net.playerList[i];
        if((player->status == SIM_NET_PLAYERSTATUS_CLIENT || player->status == SIM_NET_PLAYERSTATUS_HOST)
        && (player->mpState == SIM_NET_MPSTATE_PARENT)) {
            // Compare the mac address
            if(memcmp(player->ndsMacAddr, bssid, 6) == 0) {
                idNo = i;
            }
        }
    }

    if(idNo < 0) {
        return FALSE;
    }

    // Send the join request
    SIM_Net_JoinReqPacket_t myJoinRequest = {};
    InitPacketHeader(&myJoinRequest.common.header);
    myJoinRequest.common.cmd = SIM_NET_MPPACKETCMD_JOIN_REQ;
    myJoinRequest.playerId = net.curPlayer.id;
    memcpy(myJoinRequest.ssid, ssid, WM_SIZE_CHILD_SSID);

    ENetPacket * pkt = enet_packet_create(&myJoinRequest, sizeof(SIM_Net_JoinReqPacket_t), ENET_PACKET_FLAG_RELIABLE);
    if(!pkt) return FALSE;
    if(!net.peerList[idNo] || enet_peer_send(net.peerList[idNo], SIM_NET_CHANNEL_APPDATA, pkt) != 0) {
        enet_packet_destroy(pkt);
        return FALSE;
    }

    //Wait for Join Response
    if(SDL_SemWaitTimeout(net.joinReqSemaphore, 5000) != 0) {
        // Timeout
        return FALSE;
    }



    return TRUE;
}

BOOL SIM_Net_DisconnectMP(u16 aidBitmap, u16 * resultBitmap)
{
    if (auto *p = applicationProvider.load(std::memory_order_acquire)) return p->disconnect(p->context, aidBitmap, resultBitmap);
    BOOL result = FALSE;
    SIM_Net_DisconnectPacket_t disconnectPacket = {};
    InitPacketHeader(&disconnectPacket.common.header);
    disconnectPacket.common.cmd = SIM_NET_MPPACKETCMD_DISCONNECT;
    disconnectPacket.sendingAid = net.curPlayer.aid;

    ENetPacket * pkt = enet_packet_create(&disconnectPacket, sizeof(SIM_Net_DisconnectPacket_t), ENET_PACKET_FLAG_RELIABLE);
    if(!pkt) return FALSE;
    
    for(int i=0; i < SIM_NET_MAX_PLAYERS; i++ ) {
        if(!(net.playerList[i].status == SIM_NET_PLAYERSTATUS_CLIENT || net.playerList[i].status == SIM_NET_PLAYERSTATUS_HOST)) {
            continue;
        }

        if(!(net.playerList[i].mpState == SIM_NET_MPSTATE_PARENT || net.playerList[i].mpState == SIM_NET_MPSTATE_CHILD)) {
            // Not a parent or child device
            continue;
        }

        if(net.playerList[i].aid >= 16 || !((1 << net.playerList[i].aid) & aidBitmap)) {
            // Not target AID
            continue;
        }

        if(!net.peerList[i]) {
            // No peer
            continue;
        }

        if(enet_peer_send(net.peerList[i], SIM_NET_CHANNEL_APPDATA, pkt) != 0) continue;
        result = TRUE;
        if(resultBitmap) {
            *resultBitmap = *resultBitmap | (1 << net.playerList[i].aid);
        }

        if(net.curPlayer.mpState == SIM_NET_MPSTATE_PARENT) {
            // Free up the AID slot on the parent. TODO: We might need to free in another spot if for example we have a client disconnect
            net.aidBitmask &= ~(1 << net.playerList[i].aid);
        }
    }

    if(!result) enet_packet_destroy(pkt);

    //TODO: Do we need to reset the AID and MP state here? or does a different API do that?
    if(result) {
        if(net.curPlayer.mpState == SIM_NET_MPSTATE_CHILD) {
            net.curPlayer.aid = 0;
            net.curPlayer.mpState = SIM_NET_MPSTATE_OFF; // Not sure if we actually need to turn this off here. but doing it for good measure
            SendMyPlayer();
        }
    }

    return TRUE;
}

BOOL SIM_Net_SendMPData(u16 aidBitmap, u8 port, u8 * data, int dataLen)
{
    if (auto *p = applicationProvider.load(std::memory_order_acquire)) return p->send(p->context, aidBitmap, port, data, dataLen);
    if(!SIM_Net_ValidMPData(net.curPlayer.aid, port, dataLen,
                           dataLen < 0 ? 0 : (size_t)dataLen, WM_SIZE_MP_DATA_MAX)
       || (dataLen > 0 && !data)) return FALSE;
    int mpDataPacketSize = dataLen + sizeof(SIM_Net_MpDataPacket_t);
    SIM_Net_MpDataPacket_t * mpDataPacket = (SIM_Net_MpDataPacket_t *)malloc(mpDataPacketSize);
    if(!mpDataPacket) return FALSE;
    memset(mpDataPacket, 0, mpDataPacketSize);
    InitPacketHeader(&mpDataPacket->common.header);
    mpDataPacket->common.cmd = SIM_NET_MPPACKETCMD_DATA;
    mpDataPacket->port = port;
    mpDataPacket->dataLen = dataLen;
    mpDataPacket->aid = net.curPlayer.aid;
    if(dataLen) memcpy(mpDataPacket->data, data, dataLen);
    ENetPacket * pkt = enet_packet_create(mpDataPacket, mpDataPacketSize, ENET_PACKET_FLAG_RELIABLE);
    if(!pkt) { free(mpDataPacket); return FALSE; }

    printf("[SEND] ");
    for(int i=0; i < dataLen; i++) {
        printf("%02x", data[i]);
    }
    printf("\n");

    int numSent = 0;

    for(int i=0; i < SIM_NET_MAX_PLAYERS; i++) {
        if(!(net.playerList[i].status == SIM_NET_PLAYERSTATUS_CLIENT || net.playerList[i].status == SIM_NET_PLAYERSTATUS_HOST)) {
            continue;
        }

        if(!(net.playerList[i].mpState == SIM_NET_MPSTATE_PARENT || net.playerList[i].mpState == SIM_NET_MPSTATE_CHILD)) {
            // Not a parent or child device
            continue;
        }

        if(net.playerList[i].aid >= 16 || !((1 << net.playerList[i].aid) & aidBitmap)) {
            // Not target AID
            continue;
        }

        if(!net.peerList[i]) {
            // No peer
            continue;
        }

        if(enet_peer_send(net.peerList[i], SIM_NET_CHANNEL_APPDATA, pkt) == 0) numSent++;
    }

    free(mpDataPacket);
    if(numSent == 0) enet_packet_destroy(pkt);
    return numSent > 0;
}

int SIM_Net_Scan(WMBssDesc * bssDescOut)
{
    if (auto *p = applicationProvider.load(std::memory_order_acquire)) return p->scan(p->context, bssDescOut);
    int numParentsFound = 0;

    for(int i=0; i < SIM_NET_MAX_PLAYERS; i++) {
        if(net.playerList[i].status == SIM_NET_PLAYERSTATUS_CLIENT || net.playerList[i].status == SIM_NET_PLAYERSTATUS_HOST) {
            if(net.playerList[i].mpState == SIM_NET_MPSTATE_PARENT) {
                // Found a parent
                memcpy(bssDescOut[numParentsFound].bssid, net.playerList[i].ndsMacAddr, 6);
                bssDescOut[numParentsFound].gameInfo.magicNumber = WM_GAMEINFO_MAGIC_NUMBER;
                bssDescOut[numParentsFound].gameInfoLength = WM_GAMEINFO_LENGTH_MIN;
                bssDescOut[numParentsFound].gameInfo.gameNameCount_attribute = WM_ATTR_FLAG_ENTRY;
                bssDescOut[numParentsFound].gameInfo.ggid = net.playerList[i].ggid;
                bssDescOut[numParentsFound].gameInfo.tgid = net.playerList[i].tgid;
                if(net.playerList[i].userGameInfoLength > 0) {
                    bssDescOut[numParentsFound].gameInfo.userGameInfoLength = net.playerList[i].userGameInfoLength;
                    memcpy(bssDescOut[numParentsFound].gameInfo.userGameInfo, net.playerList[i].userGameInfo, net.playerList[i].userGameInfoLength);
                }

                numParentsFound++;
            }
        }
    }

    return numParentsFound;
}

void SIM_Net_StartThread() {
    SDL_CreateThread(NetThread, "NET", NULL);
}

static int NetThread(void * arg)
{
    while(1) {
        SDL_Delay(1);
        SIM_Net_Process();
    }
}

static void InitMyPlayer()
{
    memset(&net.curPlayer, 0, sizeof(SIM_Net_Player_t));
    SIM_config_type * myConfig = SIM_GetConfigPtr();

    // Get username from OS Owner Info
    OSOwnerInfo myOwnerInfo;
    OS_GetOwnerInfo(&myOwnerInfo);

    for(int i=0; i < SIM_NET_USERNAME_LEN; i++) {
        char curChar = myOwnerInfo.nickName[i];
        net.curPlayer.userName[i] = curChar;
    }

    // Copy NDS MAC address
    memcpy(net.curPlayer.ndsMacAddr, myConfig->macAddr, sizeof(net.curPlayer.ndsMacAddr));
}

// Use when updating the current player info.
// If a client, sends a playerInfo message to the host
// If a host, update in the player list and broadcast the playerlist
static void SendMyPlayer()
{
    if(net.state == SIM_NET_STATE_CLIENT) {
        SIM_Net_PlayerInfoPacket_t playerInfoPacket = {0};
        InitPacketHeader(&playerInfoPacket.common.header);
        playerInfoPacket.common.cmd = SIM_NET_MGMTPACKETCMD_PLAYER_INFO;
        memcpy(&playerInfoPacket.player, &net.curPlayer, sizeof(SIM_Net_Player_t));
        ENetPacket * pkt = enet_packet_create(&playerInfoPacket, sizeof(SIM_Net_PlayerInfoPacket_t), ENET_PACKET_FLAG_RELIABLE);
        if(!pkt) return;
        int peerNo = -1;
        for(int i=0; i < SIM_NET_MAX_PLAYERS; i++) {
            if(net.playerList[i].status == SIM_NET_PLAYERSTATUS_HOST && net.peerList[i] != NULL) {
                peerNo = i;
                break;
            }
        }
        if(peerNo >= 0) {
            if(enet_peer_send(net.peerList[peerNo], SIM_NET_CHANNEL_MGMT, pkt) != 0) enet_packet_destroy(pkt);
        } else {
            printf("Error sending player info: Could not find host!\n");
            enet_packet_destroy(pkt);
        }
    } else if(net.state == SIM_NET_STATE_HOST) {
        SDL_LockMutex(net.playerListMutex);
        memcpy(&net.playerList[net.curPlayer.id], &net.curPlayer, sizeof(SIM_Net_Player_t));
        SDL_UnlockMutex(net.playerListMutex);
        HostUpdatePlayerList();
    }
}

static void InitPacketHeader(SIM_Net_PacketHeader_t * header)
{
    header->magicNum = htonl(SIM_NET_MAGIC_NUM);
    header->protocolVer = htonl(SIM_NET_PROTOCOL_VER);
}

static BOOL CheckPacketHeader(SIM_Net_PacketHeader_t * header)
{
    if(htonl(header->magicNum) != SIM_NET_MAGIC_NUM) {
        return FALSE;
    }
    if(ntohl(header->protocolVer) != SIM_NET_PROTOCOL_VER) {
        return FALSE;
    }
    return TRUE;
}

static void ProcessHostEvent(ENetEvent * event)
{
    switch(event->type) {
        case ENET_EVENT_TYPE_CONNECT:
            if(net.playerCount >= SIM_NET_MAX_PLAYERS) {
                //Lobby is full, block connection
                enet_peer_disconnect(event->peer, 0);
            } else {
                // Not full, accept connection
                // Send client init

                int idNo;
                for(idNo = 0; idNo < SIM_NET_MAX_PLAYERS; idNo++) {
                    if(net.playerList[idNo].status == SIM_NET_PLAYERSTATUS_NONE || idNo >= net.playerCount) {
                        break;
                    }
                }
                if(idNo < SIM_NET_MAX_PLAYERS) {
                    SIM_Net_ClientInitPacket_t clientInitPacket;
                    InitPacketHeader(&clientInitPacket.common.header);
                    clientInitPacket.common.cmd = SIM_NET_MGMTPACKETCMD_CLIENT_INIT;
                    clientInitPacket.playerId = idNo;


                    ENetPacket * myPacket = enet_packet_create(&clientInitPacket, sizeof(SIM_Net_ClientInitPacket_t), ENET_PACKET_FLAG_RELIABLE);
                    if(!myPacket) { enet_peer_disconnect(event->peer, 0); break; }
                    if(enet_peer_send(event->peer, SIM_NET_CHANNEL_MGMT, myPacket) != 0) {
                        enet_packet_destroy(myPacket);
                        enet_peer_disconnect(event->peer, 0);
                        break;
                    }

                    SDL_LockMutex(net.playerListMutex);

                    net.playerList[idNo].id = idNo;
                    net.playerList[idNo].status = SIM_NET_PLAYERSTATUS_CONNECTING;
                    net.playerList[idNo].ipAddr = event->peer->address.host;
                    event->peer->data = &net.playerList[idNo];
                    net.playerCount++;

                    SDL_UnlockMutex(net.playerListMutex);

                    net.peerList[idNo] = event->peer;
                }
            }
            break;
        
        case ENET_EVENT_TYPE_DISCONNECT:
            {
                SIM_Net_Player_t * player = (SIM_Net_Player_t*)event->peer->data;
                if(player == NULL) {
                    break;
                }

                int idNo = player->id;
                net.peerList[idNo] = NULL;
                player->id = 0;
                player->status = SIM_NET_PLAYERSTATUS_NONE;
                net.playerCount--;

                HostUpdatePlayerList();
            }
            break;

        case ENET_EVENT_TYPE_RECEIVE:
            {
                if(event->packet->dataLength < sizeof(SIM_Net_PacketHeader_t)) {
                    break;
                }

                SIM_Net_PacketHeader_t * header = (SIM_Net_PacketHeader_t *)event->packet->data;

                if(!CheckPacketHeader(header)) {
                    break;
                }

                if(event->channelID == SIM_NET_CHANNEL_MGMT) {
                    // Check MGMT packet details
                    if(event->packet->dataLength < sizeof(SIM_Net_MgmtPacketCommon_t)) {
                        break;
                    }
                    SIM_Net_MgmtPacketCommon_t * packetCommon = (SIM_Net_MgmtPacketCommon_t *)event->packet->data;
                    switch(packetCommon->cmd) {
                        case SIM_NET_MGMTPACKETCMD_CLIENT_INIT:
                            // A peer cannot reinitialize the host's node identity.
                            break;
                        case SIM_NET_MGMTPACKETCMD_PLAYER_INFO:
                            // Client sent us player info
                            {
                                if(event->packet->dataLength < sizeof(SIM_Net_PlayerInfoPacket_t)) {
                                    break;
                                }
                                SIM_Net_PlayerInfoPacket_t * playerInfoPkt = (SIM_Net_PlayerInfoPacket_t*)event->packet->data;

                                int idNo = playerInfoPkt->player.id;

                                if(!SIM_Net_ValidPlayerId(idNo) || net.peerList[idNo] != event->peer
                                   || event->peer->data != &net.playerList[idNo]
                                   || playerInfoPkt->player.userGameInfoLength > 112
                                   || playerInfoPkt->player.aid >= 16
                                   || playerInfoPkt->player.mpState < SIM_NET_MPSTATE_OFF
                                   || playerInfoPkt->player.mpState >= SIM_NET_MPSTATE_COUNT) break;

                                SDL_LockMutex(net.playerListMutex);

                                SIM_Net_Player_t *  thePlayer = &net.playerList[idNo];
                                memcpy(thePlayer, &playerInfoPkt->player, sizeof(SIM_Net_Player_t));
                                thePlayer->userName[SIM_NET_USERNAME_LEN] = '\0';
                                thePlayer->status = SIM_NET_PLAYERSTATUS_CLIENT;
                                thePlayer->ipAddr = event->peer->address.host;

                                SDL_UnlockMutex(net.playerListMutex);

                                HostUpdatePlayerList();
                            }
                            break;
                        default:
                            printf("NET: Unknown MGMT packet received. ID: %d\n", packetCommon->cmd);
                            break;
                    }
                }

                if(event->channelID == SIM_NET_CHANNEL_APPDATA) {
                    if(net.curPlayer.mpState == SIM_NET_MPSTATE_PARENT) {
                        ProcessMPParentEvent(event);
                    } else {
                        ProcessMPChildEvent(event);
                    }
                }
            }
        case ENET_EVENT_TYPE_NONE:
        default:
            break;
    }
}

static void ProcessClientEvent(ENetEvent * event)
{
    switch(event->type) {
        case ENET_EVENT_TYPE_CONNECT:
            {
                int idNo = -1;
                
                for(int i=0; i < SIM_NET_MAX_PLAYERS; i++) {
                    SIM_Net_Player_t * player = &net.playerList[i];
                    if(i == net.curPlayer.id) {
                        continue;
                    }
                    if(player->status != SIM_NET_PLAYERSTATUS_CLIENT) {
                        continue;
                    }

                    if(player->ipAddr == event->peer->address.host) {
                        idNo = i;
                        break;
                    }
                }

                if(idNo < 0) {
                    enet_peer_disconnect(event->peer, 0);
                    break;
                }

                net.peerList[idNo] = event->peer;
                event->peer->data = &net.playerList[idNo];
            }
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            {
                SIM_Net_Player_t * player = (SIM_Net_Player_t*)event->peer->data;
                if(!player) {
                    break;
                }

                int idNo = player->id;
                net.peerList[idNo] = NULL;
                SDL_LockMutex(net.playerListMutex);
                player->status = SIM_NET_PLAYERSTATUS_DISCONNECTED;
                SDL_UnlockMutex(net.playerListMutex);


            }
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            {
                if(event->packet->dataLength < sizeof(SIM_Net_PacketHeader_t)) {
                    break;
                }

                SIM_Net_PacketHeader_t * header = (SIM_Net_PacketHeader_t *)event->packet->data;

                if(!CheckPacketHeader(header)) {
                    break;
                }

                if(event->channelID == SIM_NET_CHANNEL_MGMT) {
                    // Check MGMT packet details
                    if(event->packet->dataLength < sizeof(SIM_Net_MgmtPacketCommon_t)) {
                        break;
                    }
                    SIM_Net_MgmtPacketCommon_t * packetCommon = (SIM_Net_MgmtPacketCommon_t *)event->packet->data;
                    switch(packetCommon->cmd) {
                        case SIM_NET_MGMTPACKETCMD_PLAYER_LIST:
                            {
                                if(event->peer != net.peerList[0]
                                   || event->packet->dataLength < sizeof(SIM_Net_PlayerListPacket_t)) {
                                    break;
                                }
                                SIM_Net_PlayerListPacket_t * playerListPkt = (SIM_Net_PlayerListPacket_t*)event->packet->data;

                                if(playerListPkt->numPlayers > SIM_NET_MAX_PLAYERS) break;
                                bool valid = true;
                                for(int i=0; i < SIM_NET_MAX_PLAYERS; i++) {
                                    const auto& player = playerListPkt->playerList[i];
                                    if(!SIM_Net_ValidPlayerId(player.id) || player.aid >= 16
                                       || player.userGameInfoLength > 112
                                       || player.status < SIM_NET_PLAYERSTATUS_NONE || player.status >= SIM_NET_PLAYERSTATUS_COUNT
                                       || player.mpState < SIM_NET_MPSTATE_OFF || player.mpState >= SIM_NET_MPSTATE_COUNT) valid = false;
                                }
                                if(!valid) break;

                                net.playerCount = playerListPkt->numPlayers;
                                for(int i=0; i < SIM_NET_MAX_PLAYERS; i++) {
                                    memcpy(&net.playerList[i], &playerListPkt->playerList[i], sizeof(SIM_Net_Player_t));
                                    net.playerList[i].id = i;
                                    net.playerList[i].userName[SIM_NET_USERNAME_LEN] = '\0';
                                    //memcpy(net.playerList[i].userName, playerListPkt->playerList[i].userName, 10);
                                    //memcpy(net.playerList[i].ndsMacAddr, playerListPkt->playerList[i].ndsMacAddr, 6);
                                    //net.playerList[i]
                                    //net.playerList[i].status = playerListPkt->playerList[i].status;
                                    //net.playerList[i].mpState = playerListPkt->playerList[i].mpState;
                                    //net.playerList[i].ggid = playerListPkt->playerList[i].ggid;
                                    //net.playerList[i].tgid = playerListPkt->playerList[i].tgid;
                                }
                                memcpy(&net.playerList[net.curPlayer.id], &net.curPlayer, sizeof(SIM_Net_Player_t));
                            }
                            break;
                    }
                }

                if(event->channelID == SIM_NET_CHANNEL_APPDATA) {
                    if(net.curPlayer.mpState == SIM_NET_MPSTATE_PARENT) {
                        ProcessMPParentEvent(event);
                    } else {
                        ProcessMPChildEvent(event);
                    }
                    
                }
            }

            
            break;
        case ENET_EVENT_TYPE_NONE:
        default:
            break;
    }
}

static void ProcessMPParentEvent(ENetEvent * event)
{
    SIM_Net_Player_t* sender = EstablishedPlayer(event->peer);
    if(!sender || event->packet->dataLength < sizeof(SIM_Net_MpPacketCommon_t)) {
        return;
    }

    SIM_Net_MpPacketCommon_t * mpPacketBase = (SIM_Net_MpPacketCommon_t *)event->packet->data;

    switch(mpPacketBase->cmd) {
        case SIM_NET_MPPACKETCMD_JOIN_REQ:
            {
                if(event->packet->dataLength != sizeof(SIM_Net_JoinReqPacket_t)) break;
                auto* request = (SIM_Net_JoinReqPacket_t*)mpPacketBase;
                if(request->playerId != sender->id) break;
                // A retransmitted join must retain the child's allocated AID.
                bool alreadyJoined = sender->mpState == SIM_NET_MPSTATE_CHILD
                    && sender->aid > 0 && sender->aid < 16 && (net.aidBitmask & (1 << sender->aid));
                int aid = alreadyJoined ? sender->aid : -1;
                for(int i=1; !alreadyJoined && i < 16; i++) {
                    if(((net.aidBitmask >> i) & 1) == 0) {
                        aid = i;
                        net.aidBitmask = net.aidBitmask | (1 << aid);
                        break;
                    }
                }

                if(aid <= 0) {
                    break;
                }

                // Send a response with the AID
                SIM_Net_JoinRespPacket_t joinResponse = {};
                InitPacketHeader(&joinResponse.common.header);
                joinResponse.common.cmd = SIM_NET_MPPACKETCMD_JOIN_RESP;

                joinResponse.aid = aid;
                printf("NET: Received join request. Assigning AID %d.\n", aid);

                ENetPacket * pkt = enet_packet_create(&joinResponse, sizeof(SIM_Net_JoinRespPacket_t), ENET_PACKET_FLAG_RELIABLE);
                if(!pkt || enet_peer_send(event->peer, SIM_NET_CHANNEL_APPDATA, pkt) != 0) {
                    if(pkt) enet_packet_destroy(pkt);
                    if(!alreadyJoined) net.aidBitmask &= ~(1 << aid);
                    break;
                }

                if(event->peer->data != NULL) {
                    SDL_LockMutex(net.playerListMutex);
                    // Provisionally set the player's MP status and AID
                    SIM_Net_Player_t * player = (SIM_Net_Player_t*)event->peer->data;
                    player->aid = aid;
                    player->mpState = SIM_NET_MPSTATE_CHILD;

                    SDL_UnlockMutex(net.playerListMutex);
                }

                if(alreadyJoined) break;
                
                SIM_WMSP_Indicate_t indicate = {};
                indicate.cmd = SIM_NET_MPPACKETCMD_JOIN_REQ;
                indicate.aid = aid;
                indicate.dataLen = 0;
                SIM_Net_JoinReqPacket_t * joinReq = (SIM_Net_JoinReqPacket_t*)mpPacketBase;
                indicate.data = joinReq->ssid;
                
                WMSP_Indicate(&indicate);
                WMSP_Wait4Wm9();
            }
            break;
        case SIM_NET_MPPACKETCMD_JOIN_RESP:
            break;
        case SIM_NET_MPPACKETCMD_DISCONNECT:
            // Child device is disconnecting
            {
                if(event->packet->dataLength != sizeof(SIM_Net_DisconnectPacket_t)) break;
                SIM_Net_DisconnectPacket_t * disconnectPacket = (SIM_Net_DisconnectPacket_t*)mpPacketBase;
                if(disconnectPacket->sendingAid == 0 || disconnectPacket->sendingAid >= 16
                   || disconnectPacket->sendingAid != sender->aid) break;
                net.aidBitmask &= ~(1 << disconnectPacket->sendingAid);

                printf("NET: Parent received disconnect from AID %d", disconnectPacket->sendingAid);

                SIM_WMSP_Indicate_t indicate = {};
                indicate.cmd = SIM_NET_MPPACKETCMD_DISCONNECT;
                indicate.aid = disconnectPacket->sendingAid;

                WMSP_Indicate(&indicate);
                WMSP_Wait4Wm9();
            }
            break;
        case SIM_NET_MPPACKETCMD_DATA:
            ReceiveMPData(event);
            break;
        default:
            break;
    }
}

static void ProcessMPChildEvent(ENetEvent * event)
{
    SIM_Net_Player_t* sender = EstablishedPlayer(event->peer);
    if(!sender || sender->mpState != SIM_NET_MPSTATE_PARENT
       || event->packet->dataLength < sizeof(SIM_Net_MpPacketCommon_t)) {
        return;
    }

    SIM_Net_MpPacketCommon_t * mpPacketBase = (SIM_Net_MpPacketCommon_t *)event->packet->data;

    switch(mpPacketBase->cmd) {
        case SIM_NET_MPPACKETCMD_JOIN_REQ:
            printf("NET Warning: Child Device received MP Join request.\n");
            break;
        case SIM_NET_MPPACKETCMD_JOIN_RESP:
            {
                if(event->packet->dataLength != sizeof(SIM_Net_JoinRespPacket_t)) break;
                SIM_Net_JoinRespPacket_t * joinRespPacket = (SIM_Net_JoinRespPacket_t*)event->packet->data;
                if(joinRespPacket->aid == 0 || joinRespPacket->aid >= 16) break;
                printf("NET: Received join response. AID %d.\n", joinRespPacket->aid);
                net.curPlayer.aid = joinRespPacket->aid;
                net.curPlayer.mpState = SIM_NET_MPSTATE_CHILD;
                SendMyPlayer();
                SDL_SemPost(net.joinReqSemaphore);
                break;
            }
        case SIM_NET_MPPACKETCMD_DISCONNECT:
            // Parent is kicking this child out of the group
            {
                if(event->packet->dataLength != sizeof(SIM_Net_DisconnectPacket_t)) break;
                SIM_Net_DisconnectPacket_t * disconnectPacket = (SIM_Net_DisconnectPacket_t*)mpPacketBase;

                if(disconnectPacket->sendingAid != 0) break;
                printf("NET: Child received disconnect. Setting MP state to OFF.\n");

                net.curPlayer.aid = 0;
                net.curPlayer.mpState = SIM_NET_MPSTATE_OFF;
                SendMyPlayer();

                SIM_WMSP_Indicate_t indicate = {};
                indicate.cmd = SIM_NET_MPPACKETCMD_DISCONNECT;
                indicate.aid = 0;

                WMSP_Indicate(&indicate);
                WMSP_Wait4Wm9();
            }
            break;

        case SIM_NET_MPPACKETCMD_DATA:
            ReceiveMPData(event);
            break;
        
        default:
            break;
    }
}

static void ReceiveMPData(ENetEvent * event)
{
    if(event->packet->dataLength < sizeof(SIM_Net_MpDataPacket_t)) return;
    SIM_Net_MpDataPacket_t * dataPacket = (SIM_Net_MpDataPacket_t *)event->packet->data;
    SIM_Net_Player_t* sender = EstablishedPlayer(event->peer);
    if((net.curPlayer.mpState != SIM_NET_MPSTATE_PARENT && net.curPlayer.mpState != SIM_NET_MPSTATE_CHILD)
       || !sender || dataPacket->aid != sender->aid
       || !SIM_Net_ValidMPData(dataPacket->aid, dataPacket->port, dataPacket->dataLen,
                              event->packet->dataLength - sizeof(SIM_Net_MpDataPacket_t), WM_SIZE_MP_DATA_MAX)) return;
    if(net.curPlayer.mpState == SIM_NET_MPSTATE_PARENT
       && (sender->mpState != SIM_NET_MPSTATE_CHILD || sender->aid == 0
           || !(net.aidBitmask & (1 << sender->aid)))) return;
    if(net.curPlayer.mpState == SIM_NET_MPSTATE_CHILD
       && (sender->mpState != SIM_NET_MPSTATE_PARENT || sender->aid != 0)) return;

    SIM_WMSP_Indicate_t indicate = {};
    indicate.cmd = SIM_NET_MPPACKETCMD_DATA;
    indicate.aid = dataPacket->aid;
    indicate.port = dataPacket->port;
    indicate.dataLen = dataPacket->dataLen;
    indicate.data = dataPacket->data;
    printf("[RECV] ");
    for(int i=0; i < dataPacket->dataLen; i++) {
        printf("%02x", dataPacket->data[i]);
    }
    printf("\n");

    WMSP_Indicate(&indicate);
    WMSP_Wait4Wm9();
}


static void HostUpdatePlayerList()
{
    SIM_Net_PlayerListPacket_t playerListPkt = {};

    InitPacketHeader(&playerListPkt.common.header);
    playerListPkt.common.cmd = SIM_NET_MGMTPACKETCMD_PLAYER_LIST;
    playerListPkt.numPlayers = net.playerCount;
    memcpy(playerListPkt.playerList, net.playerList, sizeof(playerListPkt.playerList));
    ENetPacket * pkt = enet_packet_create(&playerListPkt, sizeof(SIM_Net_PlayerListPacket_t), ENET_PACKET_FLAG_RELIABLE);
    if(pkt) enet_host_broadcast(net.enetHost, SIM_NET_CHANNEL_MGMT, pkt);
}
