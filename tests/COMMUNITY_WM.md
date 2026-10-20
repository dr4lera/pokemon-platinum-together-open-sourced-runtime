# Native simulated WM metadata and discovery

The community provider ABI is version 2. Its `setGameInfo` callback receives a
borrowed `const SIM_Net_ParentInfo_t*` alongside the existing byte payload.
The provider copies the metadata during that call. It contains the real SDK
parent/child packet limits, beacon period, channel, maximum child entries and
ENTRY/MB/KS/CS attributes. `SIM_Net_SetParentInfo` caches these values under a
mutex. The unchanged public `SIM_Net_SetGameInfo` supplies a copy, and installing
a provider preserves metadata advertised before transport selection.

`WMSP_SetParentParam`, `WMSP_StartParent`, and `WMSP_SetGameInfo` publish actual
SDK parent parameters. Parent startup initializes the SDK's packet-size helpers
with the SDK's KS padding. Child connection copies the selected parent's MAC
and TGID and initializes packet-size helpers from its beacon. Ending MP retains
the SDK Parent/Child association role; ending Parent withdraws the advertisement.

Ordinary WM scan has exactly one output `WMBssDesc`. The simulated transport's
array is collected in bounded scratch storage, then one matching descriptor is
copied to that output. Repeated wildcard scans rotate parents. The callback
includes the full bounded game information, sender MAC, SSID, channel and a
virtual link level. The descriptor and callback agree on `gameInfoLength`.

The actual SDK declares `WMGameInfo::userGameInfo` as `u16[56]`, while its length
counts **bytes**. Copying metadata preserves exactly 112 bytes, or 111 bytes
with a zeroed callback tail. A typed element-copy that widens 112 bytes to 112
u16 values is incorrect and exceeds the descriptor. Byte comparisons and buffer
canaries verify this boundary.

Extended scan uses at most `scanBufSize / sizeof(WMBssDesc)` full descriptor
slots and initializes every returned pointer and link level. It filters BSSID,
requested channels and exact or custom-prefix SSIDs. A zero descriptor channel
is accepted as the legacy virtual transport's unspecified channel. The legacy
synthetic `SimAP` is never returned when an application provider is installed.

The legacy ENet peer structs and protocol remain unchanged. They do not carry
the newly added parent metadata; native community transport is required for
these fields to be transmitted. Legacy descriptors now have a defined complete
length and version, but their unavailable parent parameters remain zero.

Run `python tests/community_wm_test.py` from this runtime checkout. The fixture
compiles the actual changed functions and actual SDK size helpers against the
actual `include/nitro/wm/common/wm.h` declarations for WMGameInfo, WMBssDesc,
WMParentParam, WMStatus, requests and callbacks. Only OS/transport execution is
substituted. It checks single-buffer and extended-scan canaries, 112/111-byte
payloads, capacity/pointer tables/filtering, no community SimAP, parent metadata,
KS packet-size calculation, connected MAC/TGID, refused joins and role teardown.
It uses no game assets, accounts, saves, public services or native game process.
These source/call-site tests do not establish completed in-game battle/trade.
