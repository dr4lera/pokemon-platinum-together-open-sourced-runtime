"""Compile actual simulated WM functions against the actual SDK WM declarations.

Only OS/transport execution is replaced. WMGameInfo, WMBssDesc, all scan/request/
callback structures, WMParentParam and WMStatus come from include/nitro/wm/common/wm.h.
No game assets, network services, accounts, profiles or saves are used.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get("CXX") or shutil.which("g++") or "C:/msys64/ucrt64/bin/g++.exe"
request = (root / "libraries/sim7/src/wm/wmsp_request.c").read_text()
system = (root / "libraries/sim7/src/wm/wmsp_system.c").read_text()


def function(source, name):
    start = source.index("void " + name + "(")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


helpers = request[request.index("static void WmspSimPublishParentInfo("):request.index("void WMSP_SetParentParam(")]
scans = request[request.index("/* The simulated transport returns"):request.index("void WMSP_EndScan(")]
ex = request[request.index("static BOOL WmspSimScanSsidMatches("):request.index("void WMSP_StartScanEx(")] + function(request, "WMSP_StartScanEx")
source = "\n".join(function(system, n) for n in ["WMSP_ResetSizeVars", "WMSP_SetParentMaxSize", "WMSP_SetChildMaxSize"])
source += "\n" + helpers + scans + ex
source += "\n" + "\n".join(function(request, n) for n in ["WMSP_SetParentParam", "WMSP_StartParent", "WMSP_EndParent", "WMSP_StartConnectEx", "WMSP_EndMP", "WMSP_SetGameInfo"])

fixture = r'''
#include <nitro.h>
#include <simulator/sim_net.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <type_traits>
static_assert(sizeof(WMGameInfo)==128 && sizeof(WMBssDesc)==192,"actual SDK descriptor layout changed");
static_assert(sizeof(((WMGameInfo*)nullptr)->userGameInfo[0])==2,"must use actual u16 payload storage");
using OSMessage=void*;using OSIntrMode=int;using WlCmdCfm=int;using WlParamSetCfm=int;
#define WL_CMDLABEL_MODE_CHILD 1
#define WL_CMDLABEL_MODE_PARENT 2
#define WL_CMDLABEL_WEP_NO 0
struct WMSPWork {WMArm7Buf* wm7buf;};
static WMStatus status{};static WMArm7Buf arm7{};static WMSPWork work{&arm7};
static std::vector<WMBssDesc> parents;
static bool provider=true,joinOk=true;static int aid=3,forcedCount=-1;
static SIM_Net_MPState_t role=SIM_NET_MPSTATE_OFF;
static SIM_Net_ParentInfo_t metadata{};static std::vector<u8> userInfo;static unsigned updates=0;
static std::array<u8,6> joinedMac{};static std::array<u8,24> joinedSsid{};
struct alignas(32) CallbackGuard {u8 before[32];alignas(32)u8 bytes[512];u8 after[32];};
static CallbackGuard callback;
static std::vector<std::array<u8,512>> replies;
static void GuardCheck(){for(auto b:callback.before)assert(b==0x69);for(auto b:callback.after)assert(b==0x96);}
static void Reset(){status={};arm7={};parents.clear();replies.clear();forcedCount=-1;provider=true;joinOk=true;aid=3;role=SIM_NET_MPSTATE_OFF;updates=0;metadata={};userInfo.clear();std::memset(&callback,0,sizeof(callback));std::memset(callback.before,0x69,32);std::memset(callback.after,0x96,32);}
static WMStatus* WMSP_GetStatusStructure(){return &status;}
static WMSPWork* WMSP_GetSystemWork(){return &work;}
static void* WMSP_GetBuffer4Callback2Wm9(){GuardCheck();std::memset(callback.bytes,0xa5,sizeof(callback.bytes));return callback.bytes;}
static void WMSP_ReturnResult2Wm9(void* p){GuardCheck();assert(p==callback.bytes);std::array<u8,512> copy;std::memcpy(copy.data(),p,copy.size());replies.push_back(copy);}
static void MI_CpuCopy8(const void* from,void* to,size_t n){std::memcpy(to,from,n);}
static void MI_CpuClear8(void* to,size_t n){std::memset(to,0,n);}
static OSIntrMode OS_DisableInterrupts(){return 0;}
static OSIntrMode OS_RestoreInterrupts(OSIntrMode mode){return mode;}
static void ChangeWmState(WMStatus* target,int state){target->state=static_cast<u16>(state);}
static void SDL_Delay(unsigned){}
extern "C" int SIM_Net_Scan(WMBssDesc* to){assert(parents.size()<=SIM_NET_MAX_PLAYERS);std::memcpy(to,parents.data(),parents.size()*sizeof(WMBssDesc));return forcedCount>=0?forcedCount:static_cast<int>(parents.size());}
extern "C" BOOL SIM_Net_HasProvider(){return provider;}
extern "C" void SIM_Net_SetParentInfo(const SIM_Net_ParentInfo_t* info){metadata=*info;}
extern "C" void SIM_Net_SetGameInfo(int,int,u8* bytes,int length){updates++;userInfo.clear();if(length)userInfo.assign(bytes,bytes+length);}
extern "C" void SIM_Net_SetMPState(SIM_Net_MPState_t next){role=next;}
extern "C" BOOL SIM_Net_JoinMP(u8* mac,u8* ssid){std::memcpy(joinedMac.data(),mac,6);std::memcpy(joinedSsid.data(),ssid,24);return joinOk;}
extern "C" int SIM_Net_GetAID(){return aid;}
template<class T>static T Reply(){assert(!replies.empty());T result;std::memcpy(&result,replies.back().data(),sizeof(result));return result;}
#include "under_test.inc"
template<size_t N>struct alignas(32) Guard {u8 before[32];alignas(32)u8 bytes[N];u8 after[32];Guard(){std::memset(before,0x3c,32);std::memset(bytes,0xcd,N);std::memset(after,0xc3,32);}void Check()const{for(auto v:before)assert(v==0x3c);for(auto v:after)assert(v==0xc3);}};
static WMBssDesc Parent(unsigned id,unsigned length=112){WMBssDesc d{};d.length=sizeof(d)/2;d.bssid[0]=2;d.bssid[5]=static_cast<u8>(id);d.channel=1;d.ssidLength=5;std::memcpy(d.ssid,"Alpha",5);d.gameInfoLength=static_cast<u16>(16+length);d.gameInfo.magicNumber=WM_GAMEINFO_MAGIC_NUMBER;d.gameInfo.ver=WM_GAMEINFO_VERSION_NUMBER;d.gameInfo.platform=WM_GAMEINFO_PLATFORM_ID_NITRO;d.gameInfo.ggid=0x12345678;d.gameInfo.tgid=0x4321;d.gameInfo.attribute=WM_ATTR_FLAG_ENTRY;d.gameInfo.parentMaxSize=192;d.gameInfo.childMaxSize=38;d.gameInfo.userGameInfoLength=static_cast<u8>(length);auto* p=reinterpret_cast<u8*>(d.gameInfo.userGameInfo);for(unsigned i=0;i<length;i++)p[i]=static_cast<u8>(i*37+id);return d;}
int main(){
 Reset();status.state=WM_STATE_IDLE;Guard<192> single;WMStartScanReq scan{};scan.channel=1;scan.scanBuf=reinterpret_cast<WMBssDesc*>(single.bytes);std::memset(scan.bssid,255,6);parents={Parent(1),Parent(2,111),Parent(3)};parents[2].channel=2;
 unsigned seen=0;for(unsigned i=0;i<6;i++){WMSP_StartScan(&scan);single.Check();auto cb=Reply<WMStartScanCallback>();assert(cb.state==WM_STATECODE_PARENT_FOUND&&cb.errcode==WM_ERRCODE_SUCCESS&&cb.channel==1&&cb.linkLevel==WM_LINK_LEVEL_3);assert(cb.macAddress[5]==1||cb.macAddress[5]==2);seen|=1U<<cb.macAddress[5];const auto& expected=parents[cb.macAddress[5]-1];assert(cb.ssidLength==5&&std::memcmp(cb.ssid,expected.ssid,32)==0&&cb.gameInfoLength==expected.gameInfoLength);assert(std::memcmp(&cb.gameInfo,&expected.gameInfo,expected.gameInfoLength)==0);assert(std::memcmp(single.bytes,&expected,sizeof(expected))==0);if(expected.gameInfoLength==127)assert(reinterpret_cast<u8*>(&cb.gameInfo)[127]==0);}
 assert(seen==6);std::memcpy(scan.bssid,parents[1].bssid,6);WMSP_StartScan(&scan);assert(Reply<WMStartScanCallback>().macAddress[5]==2);scan.bssid[5]=99;WMSP_StartScan(&scan);assert(Reply<WMStartScanCallback>().state==WM_STATECODE_PARENT_NOT_FOUND&&Reply<WMStartScanCallback>().gameInfoLength==0);single.Check();
 std::memset(scan.bssid,255,6);parents[0].gameInfoLength=129;parents[1].gameInfoLength=16;parents[2].ssidLength=33;WMSP_StartScan(&scan);assert(Reply<WMStartScanCallback>().state==WM_STATECODE_PARENT_NOT_FOUND);scan.channel=0;WMSP_StartScan(&scan);assert(Reply<WMStartScanCallback>().errcode==WM_ERRCODE_INVALID_PARAM);single.Check();
 Reset();status.state=WM_STATE_IDLE;Guard<1024> multiple;WMStartScanExReq ex{};ex.scanBuf=reinterpret_cast<WMBssDesc*>(multiple.bytes);ex.scanBufSize=1024;ex.channelList=0xffff;ex.scanType=WM_SCANTYPE_PASSIVE;std::memset(ex.bssid,255,6);for(unsigned i=0;i<16;i++)parents.push_back(Parent(i+1,i==1?111:112));
 WMSP_StartScanEx(&ex);multiple.Check();auto cb=Reply<WMStartScanExCallback>();assert(cb.errcode==WM_ERRCODE_SUCCESS&&cb.bssDescCount==5&&cb.channelList==0x3fff);for(unsigned i=0;i<5;i++){assert(cb.bssDesc[i]==reinterpret_cast<WMBssDesc*>(multiple.bytes)+i&&cb.linkLevel[i]==WM_LINK_LEVEL_3);assert(std::memcmp(cb.bssDesc[i],&parents[i],192)==0);}for(unsigned i=960;i<1024;i++)assert(multiple.bytes[i]==0xcd);for(unsigned i=5;i<16;i++)assert(cb.bssDesc[i]==nullptr&&cb.linkLevel[i]==0);
 ex.scanBufSize=191;WMSP_StartScanEx(&ex);assert(Reply<WMStartScanExCallback>().errcode==WM_ERRCODE_INVALID_PARAM);multiple.Check();ex.scanBufSize=193;WMSP_StartScanEx(&ex);assert(Reply<WMStartScanExCallback>().bssDescCount==1);multiple.Check();
 ex.scanBufSize=1024;parents.resize(3);std::memcpy(parents[1].ssid,"Alphabet",8);parents[1].ssidLength=8;std::memcpy(parents[2].ssid,"Beta",4);parents[2].ssidLength=4;ex.ssidLength=5;std::memcpy(ex.ssid,"Alpha",5);WMSP_StartScanEx(&ex);assert(Reply<WMStartScanExCallback>().bssDescCount==1);ex.scanType=WM_SCANTYPE_PASSIVE_CUSTOM;ex.ssidMatchLength=3;WMSP_StartScanEx(&ex);assert(Reply<WMStartScanExCallback>().bssDescCount==2);std::memcpy(ex.bssid,parents[1].bssid,6);WMSP_StartScanEx(&ex);assert(Reply<WMStartScanExCallback>().bssDescCount==1&&Reply<WMStartScanExCallback>().bssDesc[0]->bssid[5]==2);multiple.Check();
 parents.clear();ex.ssidLength=0;std::memset(ex.bssid,255,6);WMSP_StartScanEx(&ex);assert(Reply<WMStartScanExCallback>().bssDescCount==0);provider=false;WMSP_StartScanEx(&ex);assert(Reply<WMStartScanExCallback>().bssDescCount==1&&std::memcmp(ex.scanBuf->ssid,"SimAP",5)==0);multiple.Check();
 Reset();status.state=WM_STATE_IDLE;auto advertised=Parent(7);status.pparam.userGameInfo=advertised.gameInfo.userGameInfo;status.pparam.userGameInfoLength=112;status.pparam.ggid=advertised.gameInfo.ggid;status.pparam.tgid=advertised.gameInfo.tgid;status.pparam.parentMaxSize=192;status.pparam.childMaxSize=38;status.pparam.maxEntry=1;status.pparam.entryFlag=1;status.pparam.CS_Flag=1;status.pparam.beaconPeriod=222;status.pparam.channel=6;
 WMSP_StartParent(nullptr);auto parent=Reply<WMStartParentCallback>();assert(parent.errcode==WM_ERRCODE_SUCCESS&&parent.parentSize==192&&parent.childSize==38&&status.mp_sendSize==192+WM_HEADER_PARENT_MAX_SIZE&&status.mp_recvSize==38+WM_HEADER_CHILD_MAX_SIZE);assert(role==SIM_NET_MPSTATE_PARENT&&metadata.parentMaxSize==192&&metadata.childMaxSize==38&&metadata.maxEntry==1&&metadata.beaconPeriod==222&&metadata.channel==6&&metadata.attributes==(WM_ATTR_FLAG_ENTRY|WM_ATTR_FLAG_CS)&&updates==1&&userInfo.size()==112&&std::memcmp(userInfo.data(),advertised.gameInfo.userGameInfo,112)==0);
 status.state=WM_STATE_MP_PARENT;status.mp_flag=TRUE;WMSP_EndMP(nullptr);assert(status.state==WM_STATE_PARENT&&role==SIM_NET_MPSTATE_PARENT&&!status.mp_flag);WMSP_EndParent(nullptr);assert(status.state==WM_STATE_IDLE&&role==SIM_NET_MPSTATE_OFF&&!status.beaconIndicateFlag&&!status.mp_parentSize&&!status.mp_childSize);
 status.pparam.parentMaxSize=40;status.pparam.childMaxSize=10;status.pparam.KS_Flag=1;WMSP_StartParent(nullptr);assert(status.mp_parentSize==40+WM_SIZE_KS_PARENT_DATA+WM_SIZE_MP_PARENT_PADDING&&status.mp_childSize==10+WM_SIZE_KS_CHILD_DATA+WM_SIZE_MP_CHILD_PADDING);assert(metadata.parentMaxSize==40&&metadata.childMaxSize==10&&(metadata.attributes&WM_ATTR_FLAG_KS));status.state=WM_STATE_IDLE;status.pparam.userGameInfoLength=113;WMSP_StartParent(nullptr);assert(Reply<WMStartParentCallback>().errcode==WM_ERRCODE_INVALID_PARAM);
 Reset();status.state=WM_STATE_IDLE;WMStartConnectReq connect{};connect.pInfo=&advertised;std::memset(connect.ssid,0x4b,24);advertised.gameInfo.attribute=WM_ATTR_FLAG_ENTRY|WM_ATTR_FLAG_KS;advertised.gameInfo.parentMaxSize=100;advertised.gameInfo.childMaxSize=20;WMSP_StartConnectEx(&connect);auto connected=Reply<WMStartConnectCallback>();assert(replies.size()==2&&connected.state==WM_STATECODE_CONNECTED&&connected.aid==3&&std::memcmp(connected.macAddress,advertised.bssid,6)==0);assert(status.curr_tgid==advertised.gameInfo.tgid&&status.mode==WL_CMDLABEL_MODE_CHILD&&status.child_bitmap==1&&status.mp_readyBitmap==1&&std::memcmp(status.parentMacAddress,advertised.bssid,6)==0&&std::memcmp(joinedMac.data(),advertised.bssid,6)==0);assert(connected.parentSize==100+WM_SIZE_KS_PARENT_DATA+WM_SIZE_MP_PARENT_PADDING&&connected.childSize==20+WM_SIZE_KS_CHILD_DATA+WM_SIZE_MP_CHILD_PADDING&&status.mp_sendSize==connected.childSize+WM_HEADER_CHILD_MAX_SIZE&&status.mp_recvSize==connected.parentSize+WM_HEADER_PARENT_MAX_SIZE);
 status.state=WM_STATE_MP_CHILD;status.mp_flag=TRUE;WMSP_EndMP(nullptr);assert(status.state==WM_STATE_CHILD&&role==SIM_NET_MPSTATE_CHILD&&!status.mp_flag);
 status.state=WM_STATE_IDLE;advertised.gameInfo.attribute=0;WMSP_StartConnectEx(&connect);assert(Reply<WMStartConnectCallback>().errcode==WM_ERRCODE_NO_ENTRY);advertised.gameInfo.attribute=WM_ATTR_FLAG_ENTRY;joinOk=false;WMSP_StartConnectEx(&connect);assert(Reply<WMStartConnectCallback>().errcode==WM_ERRCODE_FAILED&&status.state==WM_STATE_IDLE);joinOk=true;aid=0;WMSP_StartConnectEx(&connect);assert(Reply<WMStartConnectCallback>().errcode==WM_ERRCODE_FAILED&&status.state==WM_STATE_IDLE);advertised.gameInfoLength=129;WMSP_StartConnectEx(&connect);assert(Reply<WMStartConnectCallback>().errcode==WM_ERRCODE_INVALID_PARAM);GuardCheck();
 std::puts("Actual SDK WM scan/connect/parent functions passed: 112/111-byte u16-backed metadata, single-buffer and ScanEx canaries/capacity/pointer tables/filters, no community SimAP, parent limits/KS sizes/MAC and role lifecycle.");
}
'''

scratch = root / ".cache" / "community-tests"
scratch.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="community-wm-", dir=scratch) as directory:
    temp = Path(directory)
    stubs = {
        "nitro.h": '#pragma once\n#include <nitro/wm/common/wm.h>\n',
        "nitro/os.h": '#pragma once\n#include <nitro/types.h>\ntypedef u64 OSTick;typedef struct {int opaque;} OSMutex;\n',
        "enet/enet.h": '#pragma once\ntypedef struct ENetHost ENetHost;typedef struct ENetPeer ENetPeer;\n',
        "SDL2/SDL.h": '#pragma once\ntypedef struct SDL_mutex SDL_mutex;typedef struct SDL_sem SDL_sem;\n',
    }
    for name, text in stubs.items():
        path = temp / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    (temp / "under_test.inc").write_text(source)
    (temp / "fixture.cpp").write_text(fixture)
    exe = temp / ("fixture.exe" if os.name == "nt" else "fixture")
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-Wno-unknown-pragmas", "-DSDK_PORT", "-DSDK_BUILD_LINUX", "-I"+str(temp), "-I"+str(root / "include"), str(temp / "fixture.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
