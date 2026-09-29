#ifndef WAVES_H
#define WAVES_H

/*
 * Wave B / C / D automations.
 *
 * Every feature is gated by a wave.* configuration key and is OFF by
 * default.  Each action is logged so the dashboard log shows exactly
 * when (and why) a packet was sent.
 *
 * Payload note: opcodes come from packet_enum.h (verified against the
 * server: bot handlers match packet_map ids exactly).  Request payloads
 * that were not taken from existing verified senders (SendStartBuilding)
 * are best-effort layouts and are validated live via `$probe` +
 * `wave.log_packets` — the server answers REQUEST+1 on success.
 */

#include <stdint.h>
#include <stdbool.h>
#include "connection.h"

/* Master tick: called from BotTick(). */
void WaveTick(Connection *c);

/* $probe <opcode> <hex> — raw packet injection for payload discovery. */
bool ProbePacket(Connection *c, const char *args);

/* $waves — report status of every wave feature. */
void WaveStatusMail(Connection *c, const char *player_name);

/* Called by protection hooks (protocol.c) on incoming attack/scout. */
void WaveShelterRequest(Connection *c, bool from_attack);

/* Manual one-shot triggers used by commands/tests. */
void WaveUpgradeOne(Connection *c);   /* Force one building upgrade now. */
void WaveResearchOne(Connection *c);  /* Force one research start now. */
void WaveClaimRewards(Connection *c); /* Send all configured claim packets. */

/* Response notifications (called from main.c packet switch). */
void WaveNotifyBuildStarted(Connection *c);
void WaveNotifyBuildError(Connection *c, uint8_t code);
void WaveRecvDump(Connection *c, const char *tag,
                  const uint8_t *data, uint16_t size);
void WaveNotifyResearchStarted(Connection *c, const uint8_t *data, uint16_t size);
void WaveNotifyResearchState(Connection *c);
void WaveNotifyResearchComplete(Connection *c, const uint8_t *data,
                                uint16_t size);

/* Receive-side learning dumps (called from main.c packet switch). */
void WaveRecvMissionInfo(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvMissionFlag(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvMissionResult(Connection *c, const char *tag,
                           const uint8_t *data, uint16_t size);
void WaveRecvArenaBoard(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvArenaInfo(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvArenaRefresh(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvArenaChallenge(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvShelter(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvShelterData(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvTrapInfo(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvTrapConstruct(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvPetList(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvMapUpdate(Connection *c, uint16_t opcode, const uint8_t *data, uint16_t size);

/* Wave F: hospital heal + hero roster (1201/2427/2428). */
void WaveRecvHealingTroop(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvTrainingResp(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvAddSoldier(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvTrainingInfo(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvMarchNotAtk(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvFinishTraining(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvTroopMarch(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvTroopReturn(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvTroopHome(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvGatheringEvent(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvGatherReport(Connection *c, const uint8_t *data, uint16_t size);

/* Gather slot tracking + recall-on-attack (protection.recall_on_incoming_
 * attack): sends TROOPRETURN (2417) for every tracked gather slot. */
void GatherRecallAll(Connection *c, const char *why);
/* 2435 BEINGATTACK resync — recalls gather marches after a hit. */
void WaveRecvBeingAttacked(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvHealingComplete(Connection *c, const uint8_t *data,
                             uint16_t size);
void WaveRecvHeroSave(Connection *c, const uint8_t *data, uint16_t size);

/* Alliance discovery (2810 apply / 2858 public info / 2818+2820 search). */
void WaveRecvAllianceApply(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvAlliancePublicInfo(Connection *c, const uint8_t *data,
                                uint16_t size);
void WaveRecvAllianceSearch(Connection *c, const uint8_t *data,
                            uint16_t size);
void WaveRecvAllianceSearchResult(Connection *c, const uint8_t *data,
                                  uint16_t size);

/* Wave E: guild fest (ALLIANCEMOBILIZATION 3632..3644). */
void WaveRecvGuildFestData(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvGuildFestGet(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvGuildFestFinish(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvGuildFestUpdate(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvGuildFestDone(Connection *c, const uint8_t *data, uint16_t size);

/* Wave E: Labyrinth (GAMBLE 7001..7008, event push 3660). */
void WaveRecvGambleInfo(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvMonopolyInfo(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvMonopolyStep(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvArenaPrize(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvGambleStart(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvGamblePrize(Connection *c, const uint8_t *data, uint16_t size);
void WaveRecvGambleUpdateInfo(Connection *c, const uint8_t *data,
                              uint16_t size);

/* Wave E: hero stage quick battle (1805/1806). */
void WaveRecvQuickBattle(Connection *c, const uint8_t *data, uint16_t size);

#endif
