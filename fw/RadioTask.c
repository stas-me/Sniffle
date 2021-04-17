/*
 * Written by Sultan Qasim Khan
 * Copyright (c) 2016-2021, NCC Group plc
 * Released as open source under GPLv3
 */

/***** Includes *****/
#include <stdlib.h>
#include <stdbool.h>
#include <xdc/std.h>
#include <xdc/runtime/System.h>

#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Semaphore.h>

/* Drivers */
#include <ti/drivers/rf/RF.h>
#include <ti/drivers/PIN.h>

#include "csa2.h"
#include "adv_header_cache.h"
#include "debug.h"
#include "conf_queue.h"
#include "TXQueue.h"

#include <RadioTask.h>
#include <RadioWrapper.h>
#include <PacketTask.h>
#include <DelayHopTrigger.h>
#include <DelayStopTrigger.h>
#include <AuxAdvScheduler.h>

#include <debug.h>
#include <string.h>

#include <messenger.h>

/***** Defines *****/
#define RADIO_TASK_STACK_SIZE 1024
#define RADIO_TASK_PRIORITY   3

#define RADIO_EVENT_ALL                     0xFFFFFFFF
#define RADIO_EVENT_VALID_PACKET_RECEIVED   (uint32_t)(1 << 0)
#define RADIO_EVENT_INVALID_PACKET_RECEIVED (uint32_t)(1 << 1)

#define BLE_ADV_AA 0x8E89BED6

#define ARR_SZ(x) (sizeof(x) / sizeof(x[0]))

// more states will be added later, eg. auxiliary advertising channel
typedef enum
{
    STATIC,
    ADVERT_SEEK,
    ADVERT_HOP,
    DATA,
    PAUSED,
    INITIATING,
    MASTER,
    SLAVE,
    ADVERTISING,
    SCANNING
} SnifferState;

/***** Variable declarations *****/
static Task_Params radioTaskParams;
Task_Struct radioTask; /* not static so you can see in ROV */
static uint8_t radioTaskStack[RADIO_TASK_STACK_SIZE];
static uint8_t mapping_table[37];

static volatile SnifferState snifferState = STATIC;
static SnifferState sniffDoneState = STATIC;

static uint8_t statChan = 37;
static PHY_Mode statPHY = PHY_1M;
static uint32_t statCRCI = 0x555555;

static struct RadioConfig rconf;
static uint32_t accessAddress = BLE_ADV_AA;
static uint8_t curUnmapped;
static uint8_t hopIncrement;
static uint32_t crcInit;
static uint32_t nextHopTime;
static uint32_t connEventCount;
static uint32_t empty_hops = 0;
static bool use_csa2;
static bool ll_encryption;

static volatile bool gotLegacy;
static volatile bool firstPacket;
static uint32_t anchorOffset[4];
static uint32_t aoInd = 0;

static uint32_t timestamp37 = 0;
static uint32_t lastAdvTicks = 0;
static uint32_t advInterval[9];
static uint32_t aiInd = 0;
static bool postponed = false;
static bool followConnections = true;
static bool instaHop = true;

// bit 0 is M->S, bit 1 is S->M
static uint8_t moreData;

static bool advHopEnabled = false;
static bool auxAdvEnabled = false;

// MAC addresses need to be 16 bit aligned for radio core, hence type
static bool ourAddrRandom = false;
static bool peerAddrRandom = false;
static uint16_t ourAddr[3];
static uint16_t peerAddr[3];

static uint8_t connReqLLData[22];

static uint8_t s_advLen;
static uint8_t s_advData[31];
static uint8_t s_scanRspLen;
static uint8_t s_scanRspData[31];
static uint16_t s_advIntervalMs = 100;

uint8_t g_pkt_dir = 0;

// target offset before anchor point to start listing on next data channel
// 0.5 ms @ 4 Mhz
#define AO_TARG 2000

// be ready some microseconds before aux advertisement is received
#define AUX_OFF_TARG_USEC 600

// don't bother listening for fewer than this many ticks
// radio will get stuck if end time is in past
#define LISTEN_TICKS_MIN 2000

/***** Prototypes *****/
static void radioTaskFunction(UArg arg0, UArg arg1);
static void computeMap1(uint64_t map);
static void handleConnFinished(void);
static void reactToDataPDU(const BLE_Frame *frame);
static void reactToAdvExtPDU(const BLE_Frame *frame, uint8_t advLen);
static void handleConnReq(PHY_Mode phy, uint32_t connTime, uint8_t *llData,
        bool isAuxReq);
static void reactToTransmitted(dataQueue_t *pTXQ, uint32_t numEntries);

/***** Function definitions *****/
void RadioTask_init(void)
{
    Task_Params_init(&radioTaskParams);
    radioTaskParams.stackSize = RADIO_TASK_STACK_SIZE;
    radioTaskParams.priority = RADIO_TASK_PRIORITY;
    radioTaskParams.stack = &radioTaskStack;
    Task_construct(&radioTask, radioTaskFunction, &radioTaskParams, NULL);
}

static int _compare(const void *a, const void *b)
{
    return *(int32_t *)a - *(int32_t *)b;
}

// not technically correct for even sized arrays but it doesn't matter here
static uint32_t median(uint32_t *arr, size_t sz)
{
    qsort(arr, sz, sizeof(uint32_t), _compare);
    return arr[sz >> 1];
}

static uint32_t percentile25(uint32_t *arr, size_t sz)
{
    qsort(arr, sz, sizeof(uint32_t), _compare);
    return arr[sz >> 2];
}

static bool enoughTimeForAdvHopCheck()
{
    uint8_t chan;
    PHY_Mode phy;
    uint32_t cur_t = RF_getCurrentTime();
    uint32_t etime = AuxAdvScheduler_next(cur_t, &chan, &phy);

    if (chan != 0xFF)
        return false; // aux PDU time!

    // upcoming aux pkt, no time for check
    if (etime - LISTEN_TICKS_MIN - cur_t - (rconf.hopIntervalTicks * 8) >= 0x80000000)
        return false;

    return true;
}

static void stateTransition(SnifferState newState)
{
    BLE_Frame frame;
    uint8_t buf;

    // first update state
    snifferState = newState;

    // now send a message to host indicating it
    buf = (uint8_t)newState;
    frame.timestamp = 0;
    frame.rssi = 0;
    frame.channel = 42; // indicates state message
    frame.phy = PHY_1M;
    frame.pData = &buf;
    frame.length = 1;

    // Does thread safe copying into queue
    indicatePacket(&frame);
}

// no side effects
static inline uint8_t getCurrChan()
{
    if (use_csa2)
        return csa2_computeChannel(connEventCount);
    else
        return mapping_table[curUnmapped];
}

// performs channel hopping "housekeeping"
static void afterConnEvent(bool slave)
{
    // we're done if we got lost
    // the +3 on slaveLatency is to tolerate occasional missed packets
    if (empty_hops > rconf.slaveLatency + 3)
        handleConnFinished();

    curUnmapped = (curUnmapped + hopIncrement) % 37;
    connEventCount++;
    if (rconf_dequeue(connEventCount & 0xFFFF, &rconf))
    {
        nextHopTime += rconf.offset * 5000;
        if (use_csa2)
            csa2_computeMapping(accessAddress, rconf.chanMap);
        else
            computeMap1(rconf.chanMap);
    }
    nextHopTime += rconf.hopIntervalTicks;

    // slaves need to adjust for master clock drift
    if (slave && (connEventCount & (ARR_SZ(anchorOffset) - 1)) == 0)
    {
        uint32_t medAnchorOffset = median(anchorOffset, ARR_SZ(anchorOffset));
        nextHopTime += medAnchorOffset - AO_TARG;
    }
}

static void radioTaskFunction(UArg arg0, UArg arg1)
{
    SnifferState lastState = snifferState;

    RadioWrapper_init();

    while (1)
    {
        // zero empty_hops on state change to avoid possible confusion
        if (snifferState != lastState)
        {
            empty_hops = 0;
            lastState = snifferState;
        }

        g_pkt_dir = 0;

        if (snifferState == STATIC)
        {
            if (auxAdvEnabled)
            {
                uint8_t chan;
                PHY_Mode phy;
                uint32_t aa = BLE_ADV_AA;
                uint32_t cur_t = RF_getCurrentTime();
                uint32_t etime = AuxAdvScheduler_next(cur_t, &chan, &phy);
                if (etime - LISTEN_TICKS_MIN - cur_t >= 0x80000000)
                    continue; // pointless to listen for tiny period, may stall radio with etime in past
                if (chan == 0xFF)
                {
                    chan = statChan;
                    phy = statPHY;
                    aa = accessAddress;
                }
                RadioWrapper_recvFrames(phy, chan, aa, statCRCI, etime, indicatePacket);
            } else {
                /* receive forever (until stopped) */
                RadioWrapper_recvFrames(statPHY, statChan, accessAddress, statCRCI, 0xFFFFFFFF,
                        indicatePacket);
            }
        } else if (snifferState == ADVERT_SEEK) {
            /* if we get no legacy advertisements for 3 seconds, and we're also interested in
             * extended advertising, then just jump to ADVERT_HOP with an assumed legacy ad
             * hop interval. If legacy advertising starts later, we can correct the hopping then.
             */
            gotLegacy = false;
            if (auxAdvEnabled)
                DelayStopTrigger_trig(3 * 1000000);

            firstPacket = true;
            RadioWrapper_recvAdv3(200, 22*4000, indicatePacket);
            firstPacket = false;

            // break out early if we cancelled
            if (snifferState != ADVERT_SEEK) continue;

            // Timeout case
            if (!gotLegacy && auxAdvEnabled) {
                // assume 700 us hop interval
                rconf.hopIntervalTicks = 700 * 4;
                connEventCount = 0;
                dprintf("No legacy ads, jumping to ADVERT_HOP");
                stateTransition(ADVERT_HOP);
                continue;
            }

            if (aiInd == ARR_SZ(advInterval))
            {
                // two hops from 37 -> 39, four ticks per microsecond, 4 / 2 = 2
                rconf.hopIntervalTicks = percentile25(advInterval, ARR_SZ(advInterval)) * 2;

                // If hop interval is over 11 ms (* 4000 ticks/ms), something is wrong
                // Hop interval under 400 us is also wrong
                if ((rconf.hopIntervalTicks > 11*4000) || (rconf.hopIntervalTicks < 400*4))
                {
                    // try again
                    advHopSeekMode();
                    continue;
                }

                // DEBUG
                dprintf("hop us %lu", rconf.hopIntervalTicks >> 2);

                connEventCount = 0;
                stateTransition(ADVERT_HOP);
            }
        } else if (snifferState == ADVERT_HOP) {
            // hop between 37/38/39 targeting a particular MAC
            gotLegacy = false; // used to avoid spurious increments of connEventCount
            postponed = false;
            if ((connEventCount & 0x1F) == 0x1F && enoughTimeForAdvHopCheck())
            {
                bool interval_changed = false;

                // occasionally check that hopIntervalTicks is correct
                // do this by sniffing for an ad on 39 after 37
                firstPacket = true;
                aiInd = 0;
                RadioWrapper_recvAdv3(200, rconf.hopIntervalTicks * 4, indicatePacket);

                // break out early if we cancelled
                if (snifferState != ADVERT_HOP) continue;

                if (!gotLegacy)
                    continue; // wrong advertising set, try again

                // make sure hop interval didn't change too much (more than 5 us change)
                if (!firstPacket)
                {
                    int32_t delta_interval = rconf.hopIntervalTicks - (advInterval[0] * 2);
                    if (delta_interval < 0) delta_interval = -delta_interval;
                    if (delta_interval > 20) interval_changed = true;
                }

                // return to ADVERT_SEEK if we got lost
                if (!firstPacket && !interval_changed) {
                    empty_hops = 0;

                    // DEBUG
                    dprintf("hop confirmed");
                } else {
                    empty_hops++;
                }

                firstPacket = false;
                if (empty_hops >= 3) {
                    dprintf("adv hop interval changed, retrying");
                    advHopSeekMode();
                    continue;
                }

            } else {
                if (auxAdvEnabled)
                {
                    uint8_t chan;
                    PHY_Mode phy;
                    uint32_t cur_t = RF_getCurrentTime();
                    uint32_t etime = AuxAdvScheduler_next(cur_t, &chan, &phy);
                    if (etime - LISTEN_TICKS_MIN - cur_t >= 0x80000000)
                        continue; // pointless to listen for tiny period, may stall radio with etime in past
                    if (chan != 0xFF)
                    {
                        RadioWrapper_recvFrames(phy, chan, BLE_ADV_AA, 0x555555, etime,
                                indicatePacket);
                        continue; // don't touch connEventCount
                    } else {
                        // we need to force cancel recvAdv3 eventually
                        DelayStopTrigger_trig((etime - RF_getCurrentTime()) >> 2);
                        RadioWrapper_recvAdv3(rconf.hopIntervalTicks - 200, 8000, indicatePacket);
                    }
                } else {
                    RadioWrapper_recvAdv3(rconf.hopIntervalTicks - 200, 8000, indicatePacket);
                }
            }

            // state might have changed to DATA, in which case we must not mess with
            // connEventCount
            if (snifferState == ADVERT_HOP && gotLegacy)
                connEventCount++;
        } else if (snifferState == PAUSED) {
            Task_sleep(100);
        } else if (snifferState == DATA) {
            uint8_t chan = getCurrChan();
            firstPacket = true;
            moreData = 0x3;
            RadioWrapper_recvFrames(rconf.phy, chan, accessAddress, crcInit,
                    nextHopTime, indicatePacket);

            if (!firstPacket) empty_hops = 0;
            else empty_hops++;

            afterConnEvent(true);
        } else if (snifferState == INITIATING) {
            uint32_t connTime;
            PHY_Mode connPhy;
            g_pkt_dir = 1;
            int status = RadioWrapper_initiate(statPHY, statChan, 0xFFFFFFFF,
                    indicatePacket, ourAddr, ourAddrRandom, peerAddr, peerAddrRandom,
                    connReqLLData, &connTime, &connPhy);
            if (snifferState != INITIATING)
                continue; // initiating state was cancelled
            if (status < 0) {
                handleConnFinished();
                continue;
            }

            use_csa2 = (status >= 1) ? true : false;
            handleConnReq(connPhy, 0, connReqLLData, status >= 2);
            nextHopTime = connTime - AO_TARG + rconf.hopIntervalTicks;
            RadioWrapper_resetSeqStat();

            stateTransition(MASTER);
        } else if (snifferState == MASTER) {
            dataQueue_t txq, txq2;
            uint32_t numSent;
            uint8_t chan = getCurrChan();
            TXQueue_take(&txq);
            txq2 = txq; // copy the queue since TX will update current entry pointer
            firstPacket = false; // no need for anchor offset calcs, since we're master
            g_pkt_dir = 1;

            uint32_t curHopTime = nextHopTime - rconf.hopIntervalTicks + AO_TARG;

            int status = RadioWrapper_master(rconf.phy, chan, accessAddress,
                    crcInit, nextHopTime, indicatePacket, &txq, curHopTime, &numSent);

            if (snifferState != MASTER)
            {
                // quickly break out due to cancellation
                TXQueue_flush(numSent);
                continue;
            } else {
                reactToTransmitted(&txq2, numSent);
                TXQueue_flush(numSent);
            }

            if (status != 0) empty_hops++;
            else empty_hops = 0;

            // Sleep till next event (till anchor offset before next anchor point)
            // 10us per tick for sleep, 0.25 us per radio tick
            uint32_t rticksRemaining = nextHopTime - RF_getCurrentTime();
            if (rticksRemaining < 0x7FFFFFFF && rticksRemaining > 2000)
                Task_sleep(rticksRemaining / 40);

            afterConnEvent(false);
        } else if (snifferState == SLAVE) {
            dataQueue_t txq, txq2;
            uint32_t numSent;
            uint8_t chan = getCurrChan();
            TXQueue_take(&txq);
            txq2 = txq; // copy the queue since TX will update current entry pointer
            firstPacket = true; // for anchor offset calculations

            int status = RadioWrapper_slave(rconf.phy, chan, accessAddress,
                    crcInit, nextHopTime, indicatePacket, &txq, 0, &numSent);

            if (snifferState != SLAVE)
            {
                // quickly break out due to cancellation
                TXQueue_flush(numSent);
                continue;
            } else {
                reactToTransmitted(&txq2, numSent);
                TXQueue_flush(numSent);
            }

            if (status != 0) empty_hops++;
            else empty_hops = 0;

            // Sleep till next event (till anchor offset before next anchor point)
            // 10us per tick for sleep, 0.25 us per radio tick
            uint32_t rticksRemaining = nextHopTime - RF_getCurrentTime();
            if (rticksRemaining < 0x7FFFFFFF && rticksRemaining > 2000 &&
                    !(ll_encryption && instaHop))
                Task_sleep(rticksRemaining / 40);

            afterConnEvent(true);
        } else if (snifferState == ADVERTISING) {
            // slightly "randomize" advertisement timing as per spec
            uint32_t sleep_ms = s_advIntervalMs + (RF_getCurrentTime() & 0x7);
            RadioWrapper_advertise3(indicatePacket, ourAddr, ourAddrRandom,
                    s_advData, s_advLen, s_scanRspData, s_scanRspLen);
            // don't sleep if we had a connection established
            if (snifferState == ADVERTISING)
                Task_sleep(sleep_ms * 100); // 100 kHz ticks
        }
    }
}

// Channel Selection Algorithm #1
static void computeMap1(uint64_t map)
{
    uint8_t i, numUsedChannels = 0;
    uint8_t remapping_table[37];

    // count bits for numUsedChannels and generate remapping table
    for (i = 0; i < 37; i++)
    {
        if (map & (1ULL << i))
        {
            remapping_table[numUsedChannels] = i;
            numUsedChannels += 1;
        }
    }

    // generate the actual map
    for (i = 0; i < 37; i++)
    {
        if (map & (1ULL << i))
            mapping_table[i] = i;
        else {
            uint8_t remappingIndex = i % numUsedChannels;
            mapping_table[i] = remapping_table[remappingIndex];
        }
    }
}

static inline bool isDataState(SnifferState state)
{
    switch (state)
    {
    case DATA:
    case MASTER:
    case SLAVE:
        return true;
    default:
        return false;
    }
}

// change radio configuration based on a packet received
void reactToPDU(const BLE_Frame *frame)
{
    if (!isDataState(snifferState) || frame->channel >= 37)
    {
        // Advertising PDU
        uint8_t pduType;
        uint8_t ChSel;
        //bool TxAdd;
        //bool RxAdd;
        uint8_t advLen;

        // advertisements must have a header at least
        if (frame->length < 2)
            return;

        // decode the advertising header
        pduType = frame->pData[0] & 0xF;
        ChSel = frame->pData[0] & 0x20 ? 1 : 0;
        //TxAdd = frame->pData[0] & 0x40 ? true : false;
        //RxAdd = frame->pData[0] & 0x80 ? true : false;
        advLen = frame->pData[1];

        // make sure length is coherent
        if (frame->length - 2 < advLen)
            return;

        /* for advertisements, jump along and track intervals if needed
         *
         * ADV_EXT_IND is excluded from triggering a hop for two reasons:
         * 1. It's pointless, as the actual advertising data and connection
         *    establishment occur on the aux channel, and 37/38/39 aux pointers
         *    are just redundant.
         * 2. For devices that do both legacy and extended advertising, the hop
         *    period between 37/38/39 is different for the legacy and extended
         *    advertising sets. They are advertised independently, not interleaved
         *    in practice. We only want to get the hop interval for the legacy ads.
         */
        if (pduType == ADV_IND ||
            pduType == ADV_DIRECT_IND ||
            pduType == ADV_NONCONN_IND ||
            pduType == ADV_SCAN_IND)
        {
            // advertisement interval tracking
            if (firstPacket)
            {
                if (frame->channel == 37)
                    timestamp37 = frame->timestamp;
                else if ((frame->channel == 39))
                {
                    // microseconds from 37 to 39 advertisement
                    advInterval[aiInd++] = (frame->timestamp*4 - timestamp37*4) >> 2;
                    firstPacket = false;
                }
            }

            // Hop to 38 (with a delay) after we get an anchor advertisement on 37
            if ( (frame->channel == 37) &&
                ((snifferState == ADVERT_HOP) || (snifferState == ADVERT_SEEK)) )
            {
                /* Packet timestamps represent the start of the packet.
                 * I'm not sure if it's the time of the preamble or time of the access address.
                 *
                 * Regardless, the time it takes from advertisement start to advertisement
                 * end (frame duration) is approximately (frame->length + 8)*8 microseconds.
                 *
                 * The latency in 4 MHz radio ticks between end of transmission and now is:
                 * RF_getCurrentTime() - ((frame->timestamp << 2) + (frame->length + 8)*32)
                 * I've measured this to be typically around 165 us
                 *
                 * There's a 150 us inter-frame separation as per BLE spec.
                 * A scan request needs approximately 176 us of transmission time.
                 * A connection request needs approximately 352 us of transmission time.
                 *
                 * If endTrigger fires during receipt of a packet, it will still be received
                 * to completion.
                 *
                 * If nothing was received around T_IFS, an advertisement will be sent on the
                 * next channel in typically 200-300 us after T_IFS. This exact time (let's
                 * call it turnaround time) can be calculated as:
                 * hop interval - frame duration - 150 us T_IFS
                 *
                 * To be sure there's no scan request or conn request, we need to wait this
                 * long after the timestamp of our advertisement on 37:
                 * frame duration + 150 us T_IFS + 176 us scan request + 165 us latency
                 * = frame duration + 491 us
                 *
                 * If there's a connection request on 38, and there was no scan on 37, the
                 * connection request will start the following amount of time after 37 timestamp:
                 * frame duration + hop interval + 150 us T_IFS
                 *
                 * Our listener needs to be running on 38 before this. There's also software
                 * latency in the delay trigger, and latency in tuning/configuring the radio.
                 * Let's say this combined latency is 240 us. It's a bit tricky to measure, but
                 * it really does seem this long.
                 *
                 * When following connections, at latest, we must hop 240 us (aforementioned
                 * latency) before the scan or connect request on 38. That is at:
                 * timestamp_37 + frame duration + hop interval + 150 us T_IFS - 240 us latency
                 * = timestamp_37 + frame duration + hop interval - 90 us
                 *
                 * Usually, hop interval - 90 us > 491 us, so we can just use a fixed hop delay
                 * after the end of the advertisement on 37. Instead of setting the timer at 491
                 * us after the advert end, we set it at 530 us to give us some time to postpone
                 * the radio trigger.
                 */
                uint32_t timeRemaining;

                if (snifferState == ADVERT_SEEK) {
                    timeRemaining = 0;
                } else {
                    // we do the math in 4 MHz radio ticks so that the timestamp integer overflow works
                    uint32_t targHopTime;

                    // we should hop around 530 us (2120 radio ticks) after frame end
                    // this should give us enough time to postpone hop if necessary
                    targHopTime = frame->timestamp*4 + (frame->length + 8)*32 + 2120;

                    timeRemaining = targHopTime - RF_getCurrentTime();
                    if (timeRemaining >= 0x80000000)
                        timeRemaining = 0; // should not happen given typical latency
                    else
                        timeRemaining >>= 2; // convert to microseconds from radio ticks
                }

                // Let them no we got a legacy adv on 37 (to increment connEventCount)
                gotLegacy = true;

                DelayHopTrigger_trig(timeRemaining);
            }
        }

        // hop interval gets temporarily stretched by 400 us if a scan request is received,
        // since the advertiser needs to respond
        if (pduType == SCAN_REQ && frame->channel == 37 && snifferState == ADVERT_HOP && !postponed)
        {
            DelayHopTrigger_postpone(400);
            postponed = true;
        }

        /* for connectable advertisements, save advertisement headers to the cache
         * Connectable types are:
         * ADV_IND (0x0), ADV_DIRECT_IND (0x1), and ADV_EXT_IND (0x7)
         *
         * ADV_EXT_IND is special (BT5 specific) and its connectability depends on
         * AdvMode. ADV_EXT_IND advertisements don't contain the AdvA field and
         * instead require you to look at a secondary advertising channel.
         * The actual AdvA will be in an AUX_ADV_IND PDU in the secondary channel.
         *
         * For now, I haven't implemented support for secondary advertising
         * channels, so I'll just ignore ADV_EXT_IND.
         */
        if (pduType == ADV_IND ||
            pduType == ADV_DIRECT_IND)
        {
            adv_cache_store(frame->pData + 2, frame->pData[0]);
            return;
        }

        // react to extended advert PDUs, but don't distract in the ADVERT_SEEK state
        if (pduType == ADV_EXT_IND && auxAdvEnabled && snifferState != ADVERT_SEEK)
        {
            reactToAdvExtPDU(frame, advLen);
            return;
        }

        // handle CONNECT_IND or AUX_CONNECT_REQ (0x5)
        // TODO: deal with AUX_CONNECT_RSP (wait for it? require it? need to decide)
        if ((pduType == CONNECT_IND) && followConnections)
        {
            bool isAuxReq = frame->channel < 37;

            // make sure body length is correct
            if (advLen != 34)
                return;

            if (snifferState == ADVERTISING) {
                use_csa2 = ChSel ? true : false;
            } else {
                // Use CSA#2 if both initiator and advertiser support it
                // AUX_CONNECT_REQ always uses CSA#2, ChSel is RFU
                use_csa2 = isAuxReq ? true : false;
                if (!isAuxReq && ChSel)
                {
                    // check if advertiser supports it
                    uint8_t adv_hdr = adv_cache_fetch(frame->pData + 8);
                    if (adv_hdr != 0xFF && (adv_hdr & 0x20))
                        use_csa2 = true;
                }
            }

            // use_csa2 needs to be set before calling this
            handleConnReq(frame->phy, frame->timestamp << 2, frame->pData + 14,
                    isAuxReq);

            if (snifferState == ADVERTISING)
            {
                RadioWrapper_resetSeqStat();
                stateTransition(SLAVE);
            } else {
                stateTransition(DATA);
            }
            RadioWrapper_stop();
        }
    } else {
        reactToDataPDU(frame);
    }
}

uint8_t diff = 0;
char messageString[512];
static void reactToDataPDU(const BLE_Frame *frame)
{
    uint8_t LLID;
    //uint8_t NESN, SN;
    uint8_t MD;
    uint8_t datLen;
    uint8_t opcode;
    uint16_t nextInstant;
    const struct RadioConfig *last_rconf;
    struct RadioConfig next_rconf;

    uint8_t windowSize;
    uint16_t interval;

    /* clock synchronization
     * first packet on each channel is anchor point
     * this is only used in DATA and SLAVE states
     */
    if (firstPacket)
    {
        // compute anchor point offset from start of receive window
        anchorOffset[aoInd] = (frame->timestamp << 2) + rconf.hopIntervalTicks - nextHopTime;
        aoInd = (aoInd + 1) & (ARR_SZ(anchorOffset) - 1);
        firstPacket = false;
    }

    if (snifferState == DATA)
        g_pkt_dir ^= 1;

    // data channel PDUs should at least have a 2 byte header
    if (frame->length < 2)
        return;

    // decode the header
    LLID = frame->pData[0] & 0x3;
    //NESN = frame->pData[0] & 0x4 ? 1 : 0;
    //SN = frame->pData[0] & 0x8 ? 1 : 0;
    MD = frame->pData[0] & 0x10 ? 1 : 0;
    datLen = frame->pData[1];
    opcode = frame->pData[2];

    if (!MD)
        moreData &= ~(1 << g_pkt_dir);

    if (ll_encryption && instaHop && !moreData && snifferState == DATA)
        RadioWrapper_stop();

    // We only care about LL Control PDUs
    if (LLID != 0x3)
        return;

    // make sure length is coherent
    if (frame->length - 2 != datLen)
        return;

    last_rconf = rconf_latest();
    if (!last_rconf)
        last_rconf = &rconf;

    // for now, we lack decryption support, so encrypted LL control opcode is random
    // don't react to encrypted control PDUs we can't decipher
    if (ll_encryption)
    {
        if (datLen == 9) // 1 byte opcode + 4 byte CtrData + 4 byte MIC
        {
            // special case: this must be a LL_PHY_UPDATE_IND due to length
            // usually this means switching to 2M PHY mode
            // usually the switch is 6-10 instants from now
            // thus, we'll make an educated guess
            next_rconf.chanMap = last_rconf->chanMap;
            next_rconf.offset = 0;
            next_rconf.hopIntervalTicks = last_rconf->hopIntervalTicks;
            next_rconf.phy = PHY_2M;
            next_rconf.slaveLatency = last_rconf->slaveLatency;
            nextInstant = (connEventCount + 7) & 0xFFFF;
            rconf_enqueue(nextInstant, &next_rconf);
        }
        return;
    }

    switch (opcode)
    {
    case 0x00: // LL_CONNECTION_UPDATE_IND
        if (datLen != 12) break;
        next_rconf.chanMap = last_rconf->chanMap;
        next_rconf.offset = *(uint16_t *)(frame->pData + 4);
        next_rconf.hopIntervalTicks = *(uint16_t *)(frame->pData + 6) * 5000;
        next_rconf.phy = last_rconf->phy;
        next_rconf.slaveLatency = *(uint16_t *)(frame->pData + 6);
        nextInstant = *(uint16_t *)(frame->pData + 12);
        rconf_enqueue(nextInstant, &next_rconf);

        windowSize = (uint8_t)(frame->pData[3]);
        interval = *(uint16_t *)(frame->pData + 6);
        strcpy(messageString, "t:cu|ins:");
        strcat(messageString, convertIntegerToChar(nextInstant - (connEventCount % 65536)));
        strcat(messageString, "|ws:");
        strcat(messageString, convertIntegerToChar(windowSize));
        strcat(messageString, "|wo:");
        strcat(messageString, convertIntegerToChar(next_rconf.offset));
        strcat(messageString, "|int:");
        strcat(messageString, convertIntegerToChar(interval));
        dprintf(messageString);
        break;
    case 0x01: // LL_CHANNEL_MAP_IND
        if (datLen != 8) break;
        next_rconf.chanMap = 0;
        memcpy(&next_rconf.chanMap, frame->pData + 3, 5);
        next_rconf.offset = 0;
        next_rconf.hopIntervalTicks = last_rconf->hopIntervalTicks;
        next_rconf.phy = last_rconf->phy;
        next_rconf.slaveLatency = last_rconf->slaveLatency;
        nextInstant = *(uint16_t *)(frame->pData + 8);
        rconf_enqueue(nextInstant, &next_rconf);

        strcpy(messageString, "t:cm|ins:");
        strcat(messageString, convertIntegerToChar(nextInstant - (connEventCount % 65536)));
        strcat(messageString, "|m:");
        strcat(messageString, convertIntegerToChar(next_rconf.chanMap));
        dprintf(messageString);
        break;
    case 0x02: // LL_TERMINATE_IND
        if (datLen != 2) break;
        handleConnFinished();
        break;
    case 0x05: // LL_START_ENC_REQ
        ll_encryption = true;
        break;
    case 0x18: // LL_PHY_UPDATE_IND
        if (datLen != 5) break;
        next_rconf.chanMap = last_rconf->chanMap;
        next_rconf.offset = 0;
        next_rconf.hopIntervalTicks = last_rconf->hopIntervalTicks;
        // we don't handle different M->S and S->M PHYs, assume both match
        strcpy(messageString, "t:pu|phy:");
        switch (frame->pData[3] & 0x7)
        {
        case 0x1:
            next_rconf.phy = PHY_1M;
            strcat(messageString, "1m|ins:");
            break;
        case 0x2:
            next_rconf.phy = PHY_2M;
            strcat(messageString, "2m|ins:");
            break;
        case 0x4:
            next_rconf.phy = PHY_CODED_S8;
            strcat(messageString, "cod|ins:");
            break;
        default:
            strcat(messageString, "def|ins:");
            next_rconf.phy = last_rconf->phy;
            break;
        }
        next_rconf.slaveLatency = last_rconf->slaveLatency;
        nextInstant = *(uint16_t *)(frame->pData + 5);
        rconf_enqueue(nextInstant, &next_rconf);

        strcat(messageString, convertIntegerToChar(nextInstant - (connEventCount % 65536)));
        dprintf(messageString);
        break;
    case 0x24: // LL_POWER_CONTROL_RSP
        strcpy(messageString, "t:pcr");
        dprintf(messageString);
        break;
    case 0x25: //  LL_POWER_CHANGE_IND
        strcpy(messageString, "t:pci");
        dprintf(messageString);
        break;
    default:
        strcpy(messageString, "t:");
        strcat(messageString, convertIntegerToChar((uint8_t)(opcode)));
        strcat(messageString, "|len:");
        strcat(messageString, convertIntegerToChar(datLen));
        dprintf(messageString);
        break;
    }
}

static void reactToAdvExtPDU(const BLE_Frame *frame, uint8_t advLen)
{
    // First, we parse the Common Extended Advertising Payload
    uint8_t *pAdvA = NULL;
    uint8_t *pTargetA __attribute__((unused)) = NULL;
    uint8_t *pCTEInfo __attribute__((unused)) = NULL;
    uint8_t *pAdvDataInfo __attribute__((unused)) = NULL;
    uint8_t *pAuxPtr = NULL;
    uint8_t *pSyncInfo __attribute__((unused)) = NULL;
    uint8_t *pTxPower __attribute__((unused)) = NULL;
    uint8_t *pACAD __attribute__((unused)) = NULL;
    uint8_t ACADLen __attribute__((unused)) = 0;
    uint8_t *pAdvData __attribute__((unused)) = NULL;
    uint8_t AdvDataLen __attribute__((unused)) = 0;

    uint8_t hdrBodyLen = 0;
    uint8_t advMode;

    // invalid if missing extended header length and AdvMode
    if (advLen < 1)
        return;

    advMode = frame->pData[2] >> 6;
    hdrBodyLen = frame->pData[2] & 0x3F;
    if (advLen < hdrBodyLen + 1)
        return; // inconsistent headers

    // extended header only present if extended header len is non-zero
    // hdrBodyLen must be > 1 if any non ext header header bytes present
    if (hdrBodyLen > 1)
    {
        uint8_t hdrFlags = frame->pData[3];

        // first header field will be at frame->pData + 4
        uint8_t hdrPos = 4;

        // now parse the various header fields
        if (hdrFlags & 0x01)
        {
            pAdvA = frame->pData + hdrPos;
            hdrPos += 6;
        }
        if (hdrFlags & 0x02)
        {
            pTargetA = frame->pData + hdrPos;
            hdrPos += 6;
        }
        if (hdrFlags & 0x04)
        {
            pCTEInfo = frame->pData + hdrPos;
            hdrPos += 1;
        }
        if (hdrFlags & 0x08)
        {
            pAdvDataInfo = frame->pData + hdrPos;
            hdrPos += 2;
        }
        if (hdrFlags & 0x10)
        {
            pAuxPtr = frame->pData + hdrPos;
            hdrPos += 3;
        }
        if (hdrFlags & 0x20)
        {
            pSyncInfo = frame->pData + hdrPos;
            hdrPos += 18;
        }
        if (hdrFlags & 0x40)
        {
            pTxPower = frame->pData + hdrPos;
            hdrPos += 1;
        }
        if (hdrPos - 3 < hdrBodyLen)
        {
            pACAD = frame->pData + hdrPos;
            ACADLen = hdrBodyLen - (hdrPos - 3);
            hdrPos += ACADLen;
        }

        if (hdrPos - 2 > advLen)
            return; // inconsistent headers, parsing error

        pAdvData = frame->pData + hdrPos;
        AdvDataLen = advLen - (hdrPos - 2);
    }

    if (pAdvA)
    {
        bool TxAdd = frame->pData[0] & 0x40 ? true : false;
        if (!macOk(pAdvA, TxAdd))
            return; // rejected by MAC filter
    }

    // If we have a connectable AUX_ADV_IND, store AdvA in the cache
    if (pAdvA && advMode == 1)
        adv_cache_store(pAdvA, frame->pData[0]);

    /* TODO: handle periodic advertising
     * It's more complicated than I initially realized
     * It's sort of half way between data and advertising
     * It uses a custom access address and channel map
     * I suspect it uses CSA#2 for hopping, but the spec is not obvious on this
     * There's also a procedure for sync transfer from a link layer (data) connection
     * In short: I don't have time to implement it today.
     * The scheduler will need to be reworked to deal with this all.
     */

    // Add AUX_ADV_INDs to the schedule
    if (pAuxPtr)
    {
        uint8_t chan = pAuxPtr[0] & 0x3F;
        PHY_Mode phy = pAuxPtr[2] >> 5 < 3 ? pAuxPtr[2] >> 5 : PHY_2M;
        uint16_t offsetUsecMultiplier = (pAuxPtr[0] & 0x80) ? 300 : 30;
        uint16_t auxOffset = pAuxPtr[1] + ((pAuxPtr[2] & 0x1F) << 8);
        uint32_t auxOffsetUs = auxOffset * offsetUsecMultiplier;

        // account for being ready in advance
        if (auxOffsetUs < AUX_OFF_TARG_USEC)
            auxOffsetUs = 0;
        else
            auxOffsetUs -= AUX_OFF_TARG_USEC;

        // multiply by 4 to convert from usec to radio ticks
        uint32_t radioTimeStart = (frame->timestamp + auxOffsetUs) * 4;

        // wait for a little longer than the expected aux packet start time
        // it will actually remain till packet completion if a packet is detected
        uint32_t auxPeriod;
        if (phy == PHY_1M)
            auxPeriod = (AUX_OFF_TARG_USEC + 3000) * 4;
        else if (phy == PHY_2M)
            auxPeriod = (AUX_OFF_TARG_USEC + 2000) * 4;
        else // (phy == PHY_CODED_S8 || phy == PHY_CODED_S2)
            auxPeriod = (AUX_OFF_TARG_USEC + 20000) * 4;
        AuxAdvScheduler_insert(chan, phy, radioTimeStart, auxPeriod);

        // schedule a scheduler invocation in 5 ms or sooner if needed
        uint32_t ticksToStart = radioTimeStart - RF_getCurrentTime();
        if (ticksToStart > 0x80000000) ticksToStart = 0; // underflow
        if (ticksToStart < 5000 * 4)
            DelayStopTrigger_trig(ticksToStart >> 2);
        else
            DelayStopTrigger_trig(5000);
    }
}

static void handleConnReq(PHY_Mode phy, uint32_t connTime, uint8_t *llData,
        bool isAuxReq)
{
    uint16_t WinOffset, Interval;

    accessAddress = *(uint32_t *)llData;
    hopIncrement = llData[21] & 0x1F;
    crcInit = (*(uint32_t *)(llData + 4)) & 0xFFFFFF;
    ll_encryption = false;

    // start on the hop increment channel
    curUnmapped = hopIncrement;

    rconf.chanMap = 0;
    memcpy(&rconf.chanMap, llData + 16, 5);
    if (use_csa2)
        csa2_computeMapping(accessAddress, rconf.chanMap);
    else
        computeMap1(rconf.chanMap);

    /* see pg 2983 of BT5.2 core spec:
     *  transmitWindowDelay = 1.25 ms for CONNECT_IND
     *                        2.5 ms for AUX_CONNECT_REQ (1M and 2M)
     *                        3.75 ms for AUX_CONNECT_REQ (coded)
     * Radio clock is 4 MHz, so multiply by 4000 ticks/ms
     */
    uint32_t transmitWindowDelay;
    if (!isAuxReq)
        transmitWindowDelay = 5000;
    else if (phy == PHY_CODED_S8 || phy == PHY_CODED_S2)
        transmitWindowDelay = 15000;
    else
        transmitWindowDelay = 10000;
    transmitWindowDelay -= AO_TARG; // account for latency
    WinOffset = *(uint16_t *)(llData + 8);
    Interval = *(uint16_t *)(llData + 10);
    nextHopTime = connTime + transmitWindowDelay + (WinOffset * 5000);
    rconf.hopIntervalTicks = Interval * 5000; // 4 MHz clock, 1.25 ms per unit
    nextHopTime += rconf.hopIntervalTicks;
    rconf.phy = phy;
    rconf.slaveLatency = *(uint16_t *)(llData + 12);
    connEventCount = 0;
    strcpy(messageString, "t:con|int:");
    strcat(messageString, convertIntegerToChar(Interval));
    strcat(messageString, "|m:");
    strcat(messageString, convertIntegerToChar(rconf.chanMap));
    dprintf(messageString);
    rconf_reset();
}

static void handleConnFinished()
{
    stateTransition(sniffDoneState);
    accessAddress = BLE_ADV_AA;
    if (snifferState != PAUSED && advHopEnabled)
        advHopSeekMode();
}

static void reactToTransmitted(dataQueue_t *pTXQ, uint32_t numEntries)
{
    BLE_Frame f;
    uint8_t pduBody[40]; // all control PDUs should be under 40 bytes

    f.timestamp = RF_getCurrentTime() >> 2;
    f.rssi = 0;
    f.channel = getCurrChan();
    f.phy = rconf.phy;
    f.pData = pduBody;

    // don't trip up anchor offset calculations
    firstPacket = false;

    rfc_dataEntryPointer_t *entry = (rfc_dataEntryPointer_t *)pTXQ->pCurrEntry;
    if (entry == NULL) return;

    for (uint32_t i = 0; i < numEntries; i++)
    {
        if (entry->length > sizeof(pduBody) - 1) goto next;
        if (entry->length < 1) goto next;
        if ((entry->pData[0] & 0x3) != 0x3) goto next; // ignore non-control PDUs

        // prepare the BLE_Frame for waht we transmitted
        f.length = entry->length + 1; // add length byte
        f.pData[0] = entry->pData[0];
        f.pData[1] = entry->length - 1;
        memcpy(f.pData + 2, entry->pData + 1, f.pData[1]);

        // now process the frame
        reactToDataPDU(&f);

next:
        if (entry->pNextEntry == NULL) break;
        entry = (rfc_dataEntryPointer_t *)entry->pNextEntry;
    }
}

void setChanAAPHYCRCI(uint8_t chan, uint32_t aa, PHY_Mode phy, uint32_t crcInit)
{
    if (chan > 39)
        return;
    statPHY = phy;
    statChan = chan;
    statCRCI = crcInit & 0xFFFFFF;
    stateTransition(STATIC);
    accessAddress = aa;
    advHopEnabled = false;
    RadioWrapper_stop();
}

void setFollowConnections(bool follow)
{
    followConnections = follow;
}

// The idea behind this mode is that most devices send a single advertisement
// on channel 37, then a single ad on 38, then a single ad on 39, then repeat.
// If we hop along with the target, we have a much better chance of catching
// the CONNECT_IND request. This only works when MAC filtering is active.
void advHopSeekMode()
{
    lastAdvTicks = 0;
    aiInd = 0;
    connEventCount = 0;
    stateTransition(ADVERT_SEEK);
    advHopEnabled = true;
    RadioWrapper_stop();
}

void pauseAfterSniffDone(bool do_pause)
{
    if (do_pause)
        sniffDoneState = PAUSED;
    else
        sniffDoneState = STATIC;
}

void setAuxAdvEnabled(bool enable)
{
    auxAdvEnabled = enable;
    if (!enable)
        AuxAdvScheduler_reset();
}

// this command is useful to get the radio time after a series of config
// commands were handled by the firmware (as a "zero" time)
void sendMarker()
{
    BLE_Frame frame;

    frame.timestamp = RF_getCurrentTime() >> 2;
    frame.rssi = 0;
    frame.channel = 41; // indicates marker message
    frame.phy = PHY_1M;
    frame.pData = NULL;
    frame.length = 0;
    frame.direction = 0;

    // Does thread safe copying into queue
    indicatePacket(&frame);
}

/* Set Sniffle's MAC address for advertising/scanning/initiating */
void setAddr(bool isRandom, void *addr)
{
    ourAddrRandom = isRandom;
    memcpy(ourAddr, addr, 6);
}

/* Enter initiating state */
void initiateConn(bool isRandom, void *_peerAddr, void *llData)
{
    peerAddrRandom = isRandom;
    memcpy(peerAddr, _peerAddr, 6);
    memcpy(connReqLLData, llData, 22);

    stateTransition(INITIATING);
    RadioWrapper_stop();
}

/* Enter advertising state */
void advertise(void *advData, uint8_t advLen, void *scanRspData, uint8_t scanRspLen)
{
    s_advLen = advLen;
    s_scanRspLen = scanRspLen;
    memcpy(s_advData, advData, advLen);
    memcpy(s_scanRspData, scanRspData, scanRspLen);
    stateTransition(ADVERTISING);
    RadioWrapper_stop();
}

/* Set advertising interval (for advertising state) in milliseconds */
void setAdvInterval(uint32_t intervalMs)
{
    s_advIntervalMs = intervalMs;
}

/* Enable hopping to next channel immediately for encrypted conns */
void setInstaHop(bool enable)
{
    instaHop = enable;
}
