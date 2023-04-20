// Copyright (c) 2023. ByteDance Inc. All rights reserved.

#pragma once

#include <deque>
#include <memory>
#include "congestioncontrol.hpp"
#include "basefw/base/log.h"
#include "packettype.h"
#include "base/hash.h"

class SessionStreamCtlHandler
{
public:
    virtual void OnPiecePktTimeout(const basefw::ID& peerid, const std::vector<int32_t>& spns) = 0;

    virtual bool DoSendDataRequest(const basefw::ID& peerid, const std::vector<int32_t>& spns) = 0;
};

/// PacketSender is a simple traffic control module, in TCP or Quic, it is called Pacer.
/// Decide if we can send pkt at this time
class PacketSender
{
public:
    bool CanSend(uint32_t cwnd, uint32_t downloadingPktCnt)
    ;
    // {

    //     auto rt = false;
    //     if (cwnd > downloadingPktCnt)
    //     {
    //         rt = true;
    //     }
    //     else
    //     {
    //         rt = false;
    //     }
    //     SPDLOG_TRACE("cwnd:{},downloadingPktCnt:{},rt: {}", cwnd, downloadingPktCnt, rt);
    //     return rt;
    // }

    uint32_t MaySendPktCnt(uint32_t cwnd, uint32_t downloadingPktCnt)
    ;
    // {
    //     SPDLOG_TRACE("cwnd:{},downloadingPktCnt:{}", cwnd, downloadingPktCnt);
    //     if (cwnd >= downloadingPktCnt)
    //     {
    //         return std::min(cwnd - downloadingPktCnt, 8U);
    //     }
    //     else
    //     {
    //         return 0U;
    //     }
    // }

};

/// SessionStreamController is the single session delegate inside transport module.
/// This single session contains three part, congestion control module, loss detection module, traffic control module.
/// It may be used to send data request in its session and receive the notice when packets has been sent
class SessionStreamController: public std::enable_shared_from_this<SessionStreamController>
{
public:

    SessionStreamController();
    ~SessionStreamController();
    void StartSessionStreamCtl(const basefw::ID& sessionId, RenoCongestionCtlConfig& ccConfig,
            std::weak_ptr<SessionStreamCtlHandler> ssStreamHandler);

    // [SM]
    void StartSessionStreamCtl(const basefw::ID& sessionId, std::weak_ptr<SessionStreamCtlHandler> ssStreamHandler,
        CongestionCtlType ccType, void *pConfig = nullptr
    );

    void StopSessionStreamCtl();
    basefw::ID GetSessionId();
    bool CanSend();
    uint32_t CanRequestPktCnt();
    /// send ONE datarequest Pkt, requestting for the data pieces whose id are in spns
    bool DoRequestdata(const basefw::ID& peerid, const std::vector<int32_t>& spns);
    void OnDataRequestPktSent(const std::vector<SeqNumber>& seqs,
            const std::vector<DataNumber>& dataids, Timepoint sendtic);

    void OnDataPktReceived(uint32_t seq, int32_t datapiece, Timepoint recvtic)
    ;
    // {
    //     if (!isRunning)
    //     {
    //         return;
    //     }
    //     // find the sending record
    //     auto rtpair = m_inflightpktmap.PktIsInFlight(seq, datapiece);
    //     auto inFlight = rtpair.first;
    //     auto inflightPkt = rtpair.second;
    //     if (inFlight)
    //     {

    //         auto oldsrtt = m_rttstats.smoothed_rtt();
    //         // we don't have ack_delay in this simple implementation.
    //         auto pkt_rtt = recvtic - inflightPkt.sendtic;
    //         m_rttstats.UpdateRtt(pkt_rtt, Duration::Zero(), Clock::GetClock()->Now());
    //         auto newsrtt = m_rttstats.smoothed_rtt();

    //         auto oldcwnd = m_congestionCtl->GetCWND();

    //         AckEvent ackEvent;
    //         ackEvent.valid = true;
    //         ackEvent.ackPacket.seq = seq;
    //         ackEvent.ackPacket.pieceId = datapiece;
    //         ackEvent.sendtic = inflightPkt.sendtic;
    //         LossEvent lossEvent; // if we detect loss when ACK event, we may do loss check here.
    //         m_congestionCtl->OnDataAckOrLoss(ackEvent, lossEvent, m_rttstats);

    //         auto newcwnd = m_congestionCtl->GetCWND();
    //         // mark as received
    //         m_inflightpktmap.OnPacktReceived(inflightPkt, recvtic);
    //     }
    //     else
    //     {
    //         SPDLOG_WARN(" Recv an pkt with unknown seq:{}", seq);
    //     }

    // }

    void OnLossDetectionAlarm()
    ;
    // {
    //     DoAlarmTimeoutDetection();
    // }

    void InformLossUp(LossEvent& loss)
    ;
    // {
    //     if (!isRunning)
    //     {
    //         return;
    //     }
    //     auto handler = m_ssStreamHandler.lock();
    //     if (handler)
    //     {
    //         std::vector<int32_t> lossedPieces;
    //         for (auto&& pkt: loss.lossPackets)
    //         {
    //             lossedPieces.emplace_back(pkt.pieceId);
    //         }
    //         handler->OnPiecePktTimeout(m_sessionId, lossedPieces);
    //     }
    // }

    void DoAlarmTimeoutDetection()
    ;
    // {
    //     if (!isRunning)
    //     {
    //         return;
    //     }
    //     ///check timeout
    //     Timepoint now_t = Clock::GetClock()->Now();
    //     AckEvent ack;
    //     LossEvent loss;
    //     m_lossDetect->DetectLoss(m_inflightpktmap, now_t, ack, -1, loss, m_rttstats);
    //     if (loss.valid)
    //     {
    //         // [SM]
    //         m_loss = (double)loss.lossPackets.size() / (double)GetInFlightPktNum();

    //         for (auto&& pkt: loss.lossPackets)
    //         {
    //             m_inflightpktmap.RemoveFromInFlight(pkt);
    //         }
    //         m_congestionCtl->OnDataAckOrLoss(ack, loss, m_rttstats);
    //         InformLossUp(loss);
    //     }
    // }

    Duration GetRtt()
    ;
    // {
    //     Duration rtt{ Duration::Zero() };
    //     if (isRunning)
    //     {
    //         rtt = m_rttstats.smoothed_rtt();
    //     }
    //     SPDLOG_TRACE("rtt = {}", rtt.ToDebuggingValue());
    //     return rtt;
    // }

    uint32_t GetInFlightPktNum()
    ;
    // {
    //     return m_inflightpktmap.InFlightPktNum();
    // }


    basefw::ID GetSessionID()
    ;
    // {
    //     return m_sessionId;
    // }

    // [SM]
    uint32_t GetCWND()
    ;
    // {
    //     return m_congestionCtl->GetCWND();
    // }
    double GetLossRate()
    ;
    // {
    //     return m_loss;
    // }

private:
    bool isRunning{ false };

    basefw::ID m_sessionId;/** The remote peer id defines the session id*/
    basefw::ID m_taskid;/**The file id downloading*/
    RenoCongestionCtlConfig m_ccConfig;
    std::unique_ptr<CongestionCtlAlgo> m_congestionCtl;
    std::unique_ptr<LossDetectionAlgo> m_lossDetect;
    std::weak_ptr<SessionStreamCtlHandler> m_ssStreamHandler;
    InFlightPacketMap m_inflightpktmap;

    std::unique_ptr<PacketSender> m_sendCtl;
    RttStats m_rttstats;

    // [SM]
    double m_loss;
};



// [SM]
// cc bbr
class BBRCongestionControl : public CongestionCtlAlgo
{
public:
    
    explicit BBRCongestionControl(const RenoCongestionCtlConfig& ccConfig, std::weak_ptr<SessionStreamController> shared_ptr)
    ;
    // {
    //     m_ssThresh = ccConfig.ssThresh;
    //     m_minCwnd = ccConfig.minCwnd;
    //     m_maxCwnd = ccConfig.maxCwnd;
    //     m_sessionhandler = shared_ptr;
    //     SPDLOG_DEBUG("m_ssThresh:{}, m_minCwnd:{}, m_maxCwnd:{} ", m_ssThresh, m_minCwnd, m_maxCwnd);
    // }

    uint32_t GetInFlightPktNum()
    ;
    // {
    //     if (auto observe = m_sessionhandler.lock()) {
    //         return observe->GetInFlightPktNum();
    //     } else {
    //         return UINT32_MAX;
    //     }
    // }

    ~BBRCongestionControl() override
    ;
    // {
    //     SPDLOG_DEBUG("");
    // }

    CongestionCtlType GetCCtype() override
    ;
    // {
    //     return CongestionCtlType::bbr;
    // }

    void OnDataSent(const InflightPacket& sentpkt) override
    ;
    // {
    //     SPDLOG_TRACE("");
    // }

    void OnDataAckOrLoss(const AckEvent& ackEvent, const LossEvent& lossEvent, RttStats& rttstats) override
    ;
    // {
    //     SPDLOG_TRACE("ackevent:{}, lossevent:{}", ackEvent.DebugInfo(), lossEvent.DebugInfo());
    //     if (lossEvent.valid)
    //     {
    //         OnDataLoss(lossEvent);
    //     }

    //     if (ackEvent.valid)
    //     {
    //         OnDataRecv(ackEvent);
    //     }

    // }

    /////
    uint32_t GetCWND() override
    ;
    // {
    //     SPDLOG_TRACE(" {}", m_cwnd);
    //     return m_cwnd;
    // }

    //    virtual uint32_t GetFreeCWND() = 0;

    float CurrBwEstimate(uint64_t acked_bytes, double inflight_bytes, uint64_t rtt);

private:

    bool InSlowStart()
    ;
    // {
    //     bool rt = false;
    //     if (m_cwnd < m_ssThresh)
    //     {
    //         rt = true;
    //     }
    //     else
    //     {
    //         rt = false;
    //     }
    //     SPDLOG_TRACE(" m_cwnd:{}, m_ssThresh:{}, InSlowStart:{}", m_cwnd, m_ssThresh, rt);
    //     return rt;
    // }

    bool LostCheckRecovery(Timepoint largestLostSentTic)
    ;
    // {
    //     SPDLOG_DEBUG("largestLostSentTic:{},lastLagestLossPktSentTic:{}",
    //             largestLostSentTic.ToDebuggingValue(), lastLagestLossPktSentTic.ToDebuggingValue());
    //     /** If the largest sent tic of this loss event,is bigger than the last sent tic of the last lost pkt
    //      * (plus a 10ms correction), this session is in Recovery phase.
    //      * */
    //     if (lastLagestLossPktSentTic.IsInitialized() &&
    //         (largestLostSentTic + Duration::FromMilliseconds(10) > lastLagestLossPktSentTic))
    //     {
    //         SPDLOG_DEBUG("In Recovery");
    //         return true;
    //     }
    //     else
    //     {
    //         // a new timelost
    //         lastLagestLossPktSentTic = largestLostSentTic;
    //         SPDLOG_DEBUG("new loss");
    //         return false;
    //     }

    // }

    void ExitSlowStart()
    ;
    // {
    //     SPDLOG_DEBUG("m_ssThresh:{}, m_cwnd:{}", m_ssThresh, m_cwnd);
    //     m_ssThresh = m_cwnd;
    // }


    void OnDataRecv(const AckEvent& ackEvent, RttStats& rttstats)
    ;
    // {
    //     SPDLOG_DEBUG("ackevent:{},m_cwnd:{}", ackEvent.DebugInfo(), m_cwnd);
    //     if (InSlowStart())
    //     {
    //         /// add 1 for each ack event
    //         m_cwnd += 1;

    //         if (m_cwnd >= m_ssThresh)
    //         {
    //             ExitSlowStart();
    //         }
    //         SPDLOG_DEBUG("new m_cwnd:{}", m_cwnd);
    //     }
    //     else
    //     {
    //         /// add cwnd for each RTT
    //         m_cwndCnt++;
    //         m_cwnd += m_cwndCnt / m_cwnd;
    //         if (m_cwndCnt == m_cwnd)
    //         {
    //             m_cwndCnt = 0;
    //         }
    //         SPDLOG_DEBUG("not in slow start state,new m_cwndCnt:{} new m_cwnd:{}",
    //                 m_cwndCnt, ackEvent.DebugInfo(), m_cwnd);

    //     }
    //     m_cwnd = BoundCwnd(m_cwnd);

    //     SPDLOG_DEBUG("after RX, m_cwnd={}", m_cwnd);
    // }

    void OnDataLoss(const LossEvent& lossEvent)
    ;
    // {
    //     SPDLOG_DEBUG("lossevent:{}", lossEvent.DebugInfo());
    //     Timepoint maxsentTic{ Timepoint::Zero() };

    //     for (const auto& lostpkt: lossEvent.lossPackets)
    //     {
    //         maxsentTic = std::max(maxsentTic, lostpkt.sendtic);
    //     }

    //     /** In Recovery phase, cwnd will decrease 1 pkt for each lost pkt
    //      *  Otherwise, cwnd will cut half.
    //      * */
    //     if (InSlowStart())
    //     {
    //         // loss in slow start, just cut half
    //         m_cwnd = m_cwnd / 2;
    //         m_cwnd = BoundCwnd(m_cwnd);

    //     }
    //     else //if (!LostCheckRecovery(maxsentTic))
    //     {
    //         // Not In slow start and not inside Recovery state
    //         // [SM]
    //         // Cut half
    //         // m_cwnd = m_cwnd / 2;
    //         m_cwnd -= lossEvent.lossPackets.size() / m_cwnd + 1;
            
    //         m_cwnd = BoundCwnd(m_cwnd);
    //         m_ssThresh = m_cwnd;
    //         // enter Recovery state
    //     }
    //     SPDLOG_DEBUG("after Loss, m_cwnd={}", m_cwnd);
    // }


    uint32_t BoundCwnd(uint32_t trySetCwnd)
    ;
    // {
    //     return std::max(m_minCwnd, std::min(trySetCwnd, m_maxCwnd));
    // }

    uint32_t m_cwnd{ 1 };
    uint32_t m_cwndCnt{ 0 }; /** in congestion avoid phase, used for counting ack packets*/
    Timepoint lastLagestLossPktSentTic{ Timepoint::Zero() };


    uint32_t m_minCwnd{ 1 };
    uint32_t m_maxCwnd{ 64 };
    uint32_t m_ssThresh{ 32 };/** slow start threshold*/

    //[SM]
    std::weak_ptr<SessionStreamController> m_sessionhandler;
};

