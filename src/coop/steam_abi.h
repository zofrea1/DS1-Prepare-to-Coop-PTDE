// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#pragma once

// The parts of the Steamworks ABI that Dark Souls: Prepare to Die Edition uses for multiplayer, declared to
// match its steam_api.dll (interfaces SteamMatchMaking009 and SteamNetworking005). Checked against the
// game: every vtable offset it calls (matchmaking 4, 5, 9, 10, 13, 14, 15, 20, 35; networking 0, 2, 3, 5)
// and the sizes it copies out of LobbyCreated_t (16), LobbyEnter_t (24) and LobbyMatchList_t (4).
// Compiled by MSVC like the game, so `this` in ECX, CSteamID returned through a hidden pointer, and the
// overloaded CCallbackBase::Run in the same vtable order.

#include <cstdint>

typedef uint64_t SteamAPICall_t;
typedef uint32_t AppId_t;
typedef uint32_t SNetSocket_t;
typedef uint32_t SNetListenSocket_t;
typedef int32_t HSteamUser;
const SteamAPICall_t k_uAPICallInvalid = 0;

enum ELobbyComparison {
    k_ELobbyComparisonEqualToOrLessThan = -2,
    k_ELobbyComparisonLessThan = -1,
    k_ELobbyComparisonEqual = 0,
    k_ELobbyComparisonGreaterThan = 1,
    k_ELobbyComparisonEqualToOrGreaterThan = 2,
    k_ELobbyComparisonNotEqual = 3,
};
enum ELobbyDistanceFilter { k_ELobbyDistanceFilterWorldwide = 3 };
enum ELobbyType { k_ELobbyTypePrivate = 0, k_ELobbyTypeFriendsOnly = 1, k_ELobbyTypePublic = 2, k_ELobbyTypeInvisible = 3 };
enum EChatEntryType { k_EChatEntryTypeInvalid = 0, k_EChatEntryTypeChatMsg = 1 };
enum EP2PSend {
    k_EP2PSendUnreliable = 0,
    k_EP2PSendUnreliableNoDelay = 1,
    k_EP2PSendReliable = 2,
    k_EP2PSendReliableWithBuffering = 3,
};
enum ESNetSocketConnectionType { k_ESNetSocketConnectionTypeNotConnected = 0 };

#pragma pack(push, 1)
class CSteamID {
public:
    CSteamID() : id_(0) {}
    explicit CSteamID(uint64_t id) : id_(id) {}
    uint64_t ConvertToUint64() const { return id_; }

private:
    uint64_t id_;
};
#pragma pack(pop)

// Callback structs: Windows builds of the SDK pack them to 8.
#pragma pack(push, 8)
struct SteamServersConnected_t { enum { k_iCallback = 101 }; };
struct SteamServersDisconnected_t { enum { k_iCallback = 103 }; int32_t m_eResult; };
struct LobbyEnter_t {
    enum { k_iCallback = 504 };
    uint64_t m_ulSteamIDLobby;
    uint32_t m_rgfChatPermissions;
    bool m_bLocked;
    uint32_t m_EChatRoomEnterResponse;
};
struct LobbyDataUpdate_t {
    enum { k_iCallback = 505 };
    uint64_t m_ulSteamIDLobby;
    uint64_t m_ulSteamIDMember;
    uint8_t m_bSuccess;
};
struct LobbyChatUpdate_t {
    enum { k_iCallback = 506 };
    uint64_t m_ulSteamIDLobby;
    uint64_t m_ulSteamIDUserChanged;
    uint64_t m_ulSteamIDMakingChange;
    uint32_t m_rgfChatMemberStateChange;
};
struct LobbyChatMsg_t {
    enum { k_iCallback = 507 };
    uint64_t m_ulSteamIDLobby;
    uint64_t m_ulSteamIDUser;
    uint8_t m_eChatEntryType;
    uint32_t m_iChatID;
};
struct LobbyMatchList_t { enum { k_iCallback = 510 }; uint32_t m_nLobbiesMatching; };
struct LobbyCreated_t { enum { k_iCallback = 513 }; int32_t m_eResult; uint64_t m_ulSteamIDLobby; };
struct P2PSessionRequest_t { enum { k_iCallback = 1202 }; CSteamID m_steamIDRemote; };
struct P2PSessionConnectFail_t { enum { k_iCallback = 1203 }; CSteamID m_steamIDRemote; uint8_t m_eP2PSessionError; };
struct P2PSessionState_t {
    uint8_t m_bConnectionActive;
    uint8_t m_bConnecting;
    uint8_t m_eP2PSessionError;
    uint8_t m_bUsingRelay;
    int32_t m_nBytesQueuedForSend;
    int32_t m_nPacketsQueuedForSend;
    uint32_t m_nRemoteIP;
    uint16_t m_nRemotePort;
};
#pragma pack(pop)

static_assert(sizeof(LobbyEnter_t) == 24, "LobbyEnter_t layout");
static_assert(sizeof(LobbyCreated_t) == 16, "LobbyCreated_t layout");
static_assert(sizeof(LobbyChatUpdate_t) == 32, "LobbyChatUpdate_t layout");
static_assert(sizeof(P2PSessionState_t) == 20, "P2PSessionState_t layout");

class CCallbackBase {
public:
    CCallbackBase() : m_nCallbackFlags(0), m_iCallback(0) {}
    virtual void Run(void* pvParam) = 0;
    virtual void Run(void* pvParam, bool bIOFailure, SteamAPICall_t hSteamAPICall) = 0;
    int GetICallback() { return m_iCallback; }
    virtual int GetCallbackSizeBytes() = 0;

    uint8_t m_nCallbackFlags;
    int m_iCallback;
};

class ISteamMatchmaking009 {
public:
    virtual int GetFavoriteGameCount() = 0;
    virtual bool GetFavoriteGame(int iGame, AppId_t* pnAppID, uint32_t* pnIP, uint16_t* pnConnPort,
                                 uint16_t* pnQueryPort, uint32_t* punFlags, uint32_t* pRTime32LastPlayedOnServer) = 0;
    virtual int AddFavoriteGame(AppId_t nAppID, uint32_t nIP, uint16_t nConnPort, uint16_t nQueryPort,
                                uint32_t unFlags, uint32_t rTime32LastPlayedOnServer) = 0;
    virtual bool RemoveFavoriteGame(AppId_t nAppID, uint32_t nIP, uint16_t nConnPort, uint16_t nQueryPort,
                                    uint32_t unFlags) = 0;
    virtual SteamAPICall_t RequestLobbyList() = 0;
    virtual void AddRequestLobbyListStringFilter(const char* pchKeyToMatch, const char* pchValueToMatch,
                                                 ELobbyComparison eComparisonType) = 0;
    virtual void AddRequestLobbyListNumericalFilter(const char* pchKeyToMatch, int nValueToMatch,
                                                    ELobbyComparison eComparisonType) = 0;
    virtual void AddRequestLobbyListNearValueFilter(const char* pchKeyToMatch, int nValueToBeCloseTo) = 0;
    virtual void AddRequestLobbyListFilterSlotsAvailable(int nSlotsAvailable) = 0;
    virtual void AddRequestLobbyListDistanceFilter(ELobbyDistanceFilter eLobbyDistanceFilter) = 0;
    virtual void AddRequestLobbyListResultCountFilter(int cMaxResults) = 0;
    virtual void AddRequestLobbyListCompatibleMembersFilter(CSteamID steamIDLobby) = 0;
    virtual CSteamID GetLobbyByIndex(int iLobby) = 0;
    virtual SteamAPICall_t CreateLobby(ELobbyType eLobbyType, int cMaxMembers) = 0;
    virtual SteamAPICall_t JoinLobby(CSteamID steamIDLobby) = 0;
    virtual void LeaveLobby(CSteamID steamIDLobby) = 0;
    virtual bool InviteUserToLobby(CSteamID steamIDLobby, CSteamID steamIDInvitee) = 0;
    virtual int GetNumLobbyMembers(CSteamID steamIDLobby) = 0;
    virtual CSteamID GetLobbyMemberByIndex(CSteamID steamIDLobby, int iMember) = 0;
    virtual const char* GetLobbyData(CSteamID steamIDLobby, const char* pchKey) = 0;
    virtual bool SetLobbyData(CSteamID steamIDLobby, const char* pchKey, const char* pchValue) = 0;
    virtual int GetLobbyDataCount(CSteamID steamIDLobby) = 0;
    virtual bool GetLobbyDataByIndex(CSteamID steamIDLobby, int iLobbyData, char* pchKey, int cchKeyBufferSize,
                                     char* pchValue, int cchValueBufferSize) = 0;
    virtual bool DeleteLobbyData(CSteamID steamIDLobby, const char* pchKey) = 0;
    virtual const char* GetLobbyMemberData(CSteamID steamIDLobby, CSteamID steamIDUser, const char* pchKey) = 0;
    virtual void SetLobbyMemberData(CSteamID steamIDLobby, const char* pchKey, const char* pchValue) = 0;
    virtual bool SendLobbyChatMsg(CSteamID steamIDLobby, const void* pvMsgBody, int cubMsgBody) = 0;
    virtual int GetLobbyChatEntry(CSteamID steamIDLobby, int iChatID, CSteamID* pSteamIDUser, void* pvData,
                                  int cubData, EChatEntryType* peChatEntryType) = 0;
    virtual bool RequestLobbyData(CSteamID steamIDLobby) = 0;
    virtual void SetLobbyGameServer(CSteamID steamIDLobby, uint32_t unGameServerIP, uint16_t unGameServerPort,
                                    CSteamID steamIDGameServer) = 0;
    virtual bool GetLobbyGameServer(CSteamID steamIDLobby, uint32_t* punGameServerIP, uint16_t* punGameServerPort,
                                    CSteamID* psteamIDGameServer) = 0;
    virtual bool SetLobbyMemberLimit(CSteamID steamIDLobby, int cMaxMembers) = 0;
    virtual int GetLobbyMemberLimit(CSteamID steamIDLobby) = 0;
    virtual bool SetLobbyType(CSteamID steamIDLobby, ELobbyType eLobbyType) = 0;
    virtual bool SetLobbyJoinable(CSteamID steamIDLobby, bool bLobbyJoinable) = 0;
    virtual CSteamID GetLobbyOwner(CSteamID steamIDLobby) = 0;
    virtual bool SetLobbyOwner(CSteamID steamIDLobby, CSteamID steamIDNewOwner) = 0;
    virtual bool SetLinkedLobby(CSteamID steamIDLobby, CSteamID steamIDLobbyDependent) = 0;
};

class ISteamNetworking005 {
public:
    virtual bool SendP2PPacket(CSteamID steamIDRemote, const void* pubData, uint32_t cubData, EP2PSend eP2PSendType,
                               int nChannel) = 0;
    virtual bool IsP2PPacketAvailable(uint32_t* pcubMsgSize, int nChannel) = 0;
    virtual bool ReadP2PPacket(void* pubDest, uint32_t cubDest, uint32_t* pcubMsgSize, CSteamID* psteamIDRemote,
                               int nChannel) = 0;
    virtual bool AcceptP2PSessionWithUser(CSteamID steamIDRemote) = 0;
    virtual bool CloseP2PSessionWithUser(CSteamID steamIDRemote) = 0;
    virtual bool CloseP2PChannelWithUser(CSteamID steamIDRemote, int nChannel) = 0;
    virtual bool GetP2PSessionState(CSteamID steamIDRemote, P2PSessionState_t* pConnectionState) = 0;
    virtual bool AllowP2PPacketRelay(bool bAllow) = 0;
    virtual SNetListenSocket_t CreateListenSocket(int nVirtualP2PPort, uint32_t nIP, uint16_t nPort,
                                                  bool bAllowUseOfPacketRelay) = 0;
    virtual SNetSocket_t CreateP2PConnectionSocket(CSteamID steamIDTarget, int nVirtualPort, int nTimeoutSec,
                                                   bool bAllowUseOfPacketRelay) = 0;
    virtual SNetSocket_t CreateConnectionSocket(uint32_t nIP, uint16_t nPort, int nTimeoutSec) = 0;
    virtual bool DestroySocket(SNetSocket_t hSocket, bool bNotifyRemoteEnd) = 0;
    virtual bool DestroyListenSocket(SNetListenSocket_t hSocket, bool bNotifyRemoteEnd) = 0;
    virtual bool SendDataOnSocket(SNetSocket_t hSocket, void* pubData, uint32_t cubData, bool bReliable) = 0;
    virtual bool IsDataAvailableOnSocket(SNetSocket_t hSocket, uint32_t* pcubMsgSize) = 0;
    virtual bool RetrieveDataFromSocket(SNetSocket_t hSocket, void* pubDest, uint32_t cubDest,
                                        uint32_t* pcubMsgSize) = 0;
    virtual bool IsDataAvailable(SNetListenSocket_t hListenSocket, uint32_t* pcubMsgSize, SNetSocket_t* phSocket) = 0;
    virtual bool RetrieveData(SNetListenSocket_t hListenSocket, void* pubDest, uint32_t cubDest,
                              uint32_t* pcubMsgSize, SNetSocket_t* phSocket) = 0;
    virtual bool GetSocketInfo(SNetSocket_t hSocket, CSteamID* pSteamIDRemote, int* peSocketStatus,
                               uint32_t* punIPRemote, uint16_t* punPortRemote) = 0;
    virtual bool GetListenSocketInfo(SNetListenSocket_t hListenSocket, uint32_t* pnIP, uint16_t* pnPort) = 0;
    virtual ESNetSocketConnectionType GetSocketConnectionType(SNetSocket_t hSocket) = 0;
    virtual int GetMaxPacketSize(SNetSocket_t hSocket) = 0;
};

// Only the first methods are used (to learn the local player's identity).
class ISteamUser017 {
public:
    virtual HSteamUser GetHSteamUser() = 0;
    virtual bool BLoggedOn() = 0;
    virtual CSteamID GetSteamID() = 0;
};

// The first eight methods, unchanged from SteamFriends013 to 015 (the persona name of a friend or of
// anyone in a shared lobby).
class ISteamFriends014 {
public:
    virtual const char* GetPersonaName() = 0;
    virtual SteamAPICall_t SetPersonaName(const char* pchPersonaName) = 0;
    virtual int GetPersonaState() = 0;
    virtual int GetFriendCount(int iFriendFlags) = 0;
    virtual CSteamID GetFriendByIndex(int iFriend, int iFriendFlags) = 0;
    virtual int GetFriendRelationship(CSteamID steamIDFriend) = 0;
    virtual int GetFriendPersonaState(CSteamID steamIDFriend) = 0;
    virtual const char* GetFriendPersonaName(CSteamID steamIDFriend) = 0;
};

// ISteamUtils as PTDE's steam_api returns it (SteamUtils006): GetCSERIPPort is still slot 7, so GetAppID is 9.
class ISteamUtils006 {
public:
    virtual uint32_t GetSecondsSinceAppActive() = 0;
    virtual uint32_t GetSecondsSinceComputerActive() = 0;
    virtual int GetConnectedUniverse() = 0;
    virtual uint32_t GetServerRealTime() = 0;
    virtual const char* GetIPCountry() = 0;
    virtual bool GetImageSize(int iImage, uint32_t* pnWidth, uint32_t* pnHeight) = 0;
    virtual bool GetImageRGBA(int iImage, uint8_t* pubDest, int nDestBufferSize) = 0;
    virtual bool GetCSERIPPort(uint32_t* unIP, uint16_t* usPort) = 0;
    virtual uint8_t GetCurrentBatteryPower() = 0;
    virtual uint32_t GetAppID() = 0;
};
