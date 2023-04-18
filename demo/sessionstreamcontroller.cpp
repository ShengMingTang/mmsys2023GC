// Copyright (c) 2023. ByteDance Inc. All rights reserved.

#include <deque>
#include <memory>
#include "congestioncontrol.hpp"
#include "basefw/base/log.h"
#include "packettype.h"
#include "sessionstreamcontroller.hpp"

/// PacketSender is a simple traffic control module, in TCP or Quic, it is called Pacer.
/// Decide if we can send pkt at this time
bool PacketSender::CanSend(uint32_t cwnd, uint32_t downloadingPktCnt)
{

        auto rt = false;
        if (cwnd > downloadingPktCnt)
        {
            rt = true;
        }
        else
        {
            rt = false;
        }
        SPDLOG_TRACE("cwnd:{},downloadingPktCnt:{},rt: {}", cwnd, downloadingPktCnt, rt);
        return rt;
    }

uint32_t PacketSender::MaySendPktCnt(uint32_t cwnd, uint32_t downloadingPktCnt)
{
    SPDLOG_TRACE("cwnd:{},downloadingPktCnt:{}", cwnd, downloadingPktCnt);
    if (cwnd >= downloadingPktCnt)
    {
        return std::min(cwnd - downloadingPktCnt, 8U);
    }
    else
    {
        return 0U;
    }
}


SessionStreamController::SessionStreamController()
{
    SPDLOG_TRACE("");
};

SessionStreamController::~SessionStreamController()
{
    SPDLOG_TRACE("");
    StopSessionStreamCtl();
};

void SessionStreamController::StartSessionStreamCtl(const basefw::ID& sessionId, RenoCongestionCtlConfig& ccConfig,
        std::weak_ptr<SessionStreamCtlHandler> ssStreamHandler)
{
    if (isRunning)
    {
        SPDLOG_WARN("isRunning = true");
        return;
    }
    isRunning = true;
    m_sessionId = sessionId;
    m_ssStreamHandler = ssStreamHandler;
    // cc
    m_ccConfig = ccConfig;
    m_congestionCtl.reset(new RenoCongestionContrl(m_ccConfig));

    // send control
    m_sendCtl.reset(new PacketSender());

    //loss detection
    m_lossDetect.reset(new DefaultLossDetectionAlgo());

    // set initial smothed rtt
    m_rttstats.set_initial_rtt(Duration::FromMilliseconds(200));

};

// [SM]
void SessionStreamController::StartSessionStreamCtl(const basefw::ID& sessionId, std::weak_ptr<SessionStreamCtlHandler> ssStreamHandler,
    CongestionCtlType ccType, void *pConfig
)
{
    if (isRunning)
    {
        SPDLOG_WARN("isRunning = true");
        return;
    }
    isRunning = true;
    m_sessionId = sessionId;
    m_ssStreamHandler = ssStreamHandler;
    // cc
    // [SM]
    // m_ccConfig = ccConfig;
    // m_congestionCtl.reset(new RenoCongestionContrl(m_ccConfig));
    if(ccType == CongestionCtlType::reno || ccType == CongestionCtlType::reno_AIAD || ccType == CongestionCtlType::bbr) {
        m_ccConfig = *(RenoCongestionCtlConfig*)pConfig;
        if(ccType == CongestionCtlType::reno) {
            m_congestionCtl.reset(new RenoCongestionContrl(m_ccConfig));
        }
        else if(ccType == CongestionCtlType::reno_AIAD) {
            m_congestionCtl.reset(new RenoAIADCongestionContrl(m_ccConfig));
        }
        else if(ccType == CongestionCtlType::bbr) {
            SPDLOG_DEBUG("BBR is used");
            m_congestionCtl.reset(new BBRCongestionControl(m_ccConfig, shared_from_this()));
        }
        else {
            SPDLOG_DEBUG("CCtype not recognized: {}", ccType);
            m_congestionCtl.reset(nullptr);
            return;
        }
    }
    else {
        SPDLOG_DEBUG("CCtype not defined: {}", ccType);
    }

    // send control
    m_sendCtl.reset(new PacketSender());

    //loss detection
    m_lossDetect.reset(new DefaultLossDetectionAlgo());

    // set initial smothed rtt
    m_rttstats.set_initial_rtt(Duration::FromMilliseconds(200));

}

void SessionStreamController::StopSessionStreamCtl()
{
    if (isRunning)
    {
        isRunning = false;
    }
    else
    {
        SPDLOG_WARN("isRunning = false");
    }
}

basefw::ID SessionStreamController::GetSessionId()
{
    if (isRunning)
    {
        return m_sessionId;
    }
    else
    {
        return {};
    }
};

bool SessionStreamController::CanSend()
{
    SPDLOG_TRACE("");
    if (!isRunning)
    {
        return false;
    }

    return m_sendCtl->CanSend(m_congestionCtl->GetCWND(), GetInFlightPktNum());
};

uint32_t SessionStreamController::CanRequestPktCnt()
{
    SPDLOG_TRACE("");
    if (!isRunning)
    {
        return false;
    }
    return m_sendCtl->MaySendPktCnt(m_congestionCtl->GetCWND(), GetInFlightPktNum());
};

/// send ONE datarequest Pkt, requestting for the data pieces whose id are in spns
bool SessionStreamController::DoRequestdata(const basefw::ID& peerid, const std::vector<int32_t>& spns)
{
    SPDLOG_TRACE("peerid = {}, spns = {}", peerid.ToLogStr(), spns);
    if (!isRunning)
    {
        return false;
    }
    if (!CanSend())
    {
        SPDLOG_WARN("CanSend = false");
        return false;
    }

    if (spns.size() > CanRequestPktCnt())
    {
        SPDLOG_WARN("The number of request data pieces {} exceeds the freewnd {}", spns.size(), CanRequestPktCnt());
        return false;
    }
    auto handler = m_ssStreamHandler.lock();
    if (handler)
    {
        return handler->DoSendDataRequest(peerid, spns);
    }
    else
    {
        SPDLOG_WARN("SessionStreamHandler is null");
        return false;
    }

}

void SessionStreamController::OnDataRequestPktSent(const std::vector<SeqNumber>& seqs,
        const std::vector<DataNumber>& dataids, Timepoint sendtic)
{
    SPDLOG_TRACE("seq = {}, dataid = {}, sendtic = {}",
            seqs,
            dataids, sendtic.ToDebuggingValue());
    if (!isRunning)
    {
        return;
    }
    auto seqidx = 0;
    for (auto datano: dataids)
    {
        DataPacket p;
        p.seq = seqs[seqidx];
        p.pieceId = datano;
        // add to downloading queue
        m_inflightpktmap.AddSentPacket(p, sendtic);

        // inform cc algo that a packet is sent
        InflightPacket sentpkt;
        sentpkt.seq = seqs[seqidx];
        sentpkt.pieceId = datano;
        sentpkt.sendtic = sendtic;
        m_congestionCtl->OnDataSent(sentpkt);
        seqidx++;
    }

}

void SessionStreamController::OnDataPktReceived(uint32_t seq, int32_t datapiece, Timepoint recvtic)
{
    if (!isRunning)
    {
        return;
    }
    // find the sending record
    auto rtpair = m_inflightpktmap.PktIsInFlight(seq, datapiece);
    auto inFlight = rtpair.first;
    auto inflightPkt = rtpair.second;
    if (inFlight)
    {

        auto oldsrtt = m_rttstats.smoothed_rtt();
        // we don't have ack_delay in this simple implementation.
        auto pkt_rtt = recvtic - inflightPkt.sendtic;
        m_rttstats.UpdateRtt(pkt_rtt, Duration::Zero(), Clock::GetClock()->Now());
        auto newsrtt = m_rttstats.smoothed_rtt();

        auto oldcwnd = m_congestionCtl->GetCWND();

        AckEvent ackEvent;
        ackEvent.valid = true;
        ackEvent.ackPacket.seq = seq;
        ackEvent.ackPacket.pieceId = datapiece;
        ackEvent.sendtic = inflightPkt.sendtic;
        LossEvent lossEvent; // if we detect loss when ACK event, we may do loss check here.
        m_congestionCtl->OnDataAckOrLoss(ackEvent, lossEvent, m_rttstats);

        auto newcwnd = m_congestionCtl->GetCWND();
        // mark as received
        m_inflightpktmap.OnPacktReceived(inflightPkt, recvtic);
    }
    else
    {
        SPDLOG_WARN(" Recv an pkt with unknown seq:{}", seq);
    }

}

void SessionStreamController::OnLossDetectionAlarm()
{
    DoAlarmTimeoutDetection();
}

void SessionStreamController::InformLossUp(LossEvent& loss)
{
    if (!isRunning)
    {
        return;
    }
    auto handler = m_ssStreamHandler.lock();
    if (handler)
    {
        std::vector<int32_t> lossedPieces;
        for (auto&& pkt: loss.lossPackets)
        {
            lossedPieces.emplace_back(pkt.pieceId);
        }
        handler->OnPiecePktTimeout(m_sessionId, lossedPieces);
    }
}

void SessionStreamController::DoAlarmTimeoutDetection()
{
    if (!isRunning)
    {
        return;
    }
    ///check timeout
    Timepoint now_t = Clock::GetClock()->Now();
    AckEvent ack;
    LossEvent loss;
    m_lossDetect->DetectLoss(m_inflightpktmap, now_t, ack, -1, loss, m_rttstats);
    if (loss.valid)
    {
        // [SM]
        m_loss = (double)loss.lossPackets.size() / (double)GetInFlightPktNum();

        for (auto&& pkt: loss.lossPackets)
        {
            m_inflightpktmap.RemoveFromInFlight(pkt);
        }
        m_congestionCtl->OnDataAckOrLoss(ack, loss, m_rttstats);
        InformLossUp(loss);
    }
}

Duration SessionStreamController::GetRtt()
{
    Duration rtt{ Duration::Zero() };
    if (isRunning)
    {
        rtt = m_rttstats.smoothed_rtt();
    }
    SPDLOG_TRACE("rtt = {}", rtt.ToDebuggingValue());
    return rtt;
}

uint32_t SessionStreamController::GetInFlightPktNum()
{
    return m_inflightpktmap.InFlightPktNum();
}


basefw::ID SessionStreamController::GetSessionID()
{
    return m_sessionId;
}

// [SM]
uint32_t SessionStreamController::GetCWND()
{
    return m_congestionCtl->GetCWND();
}

double SessionStreamController::GetLossRate()
{
    return m_loss;
}



//     // [SM]
//     void StartSessionStreamCtl(const basefw::ID& sessionId, std::weak_ptr<SessionStreamCtlHandler> ssStreamHandler,
//         CongestionCtlType ccType, void *pConfig = nullptr
//     )
//     {
//         if (isRunning)
//         {
//             SPDLOG_WARN("isRunning = true");
//             return;
//         }
//         isRunning = true;
//         m_sessionId = sessionId;
//         m_ssStreamHandler = ssStreamHandler;
//         // cc
//         // [SM]
//         // m_ccConfig = ccConfig;
//         // m_congestionCtl.reset(new RenoCongestionContrl(m_ccConfig));
//         if(ccType == CongestionCtlType::reno || ccType == CongestionCtlType::reno_AIAD || ccType == CongestionCtlType::bbr) {
//             m_ccConfig = *(RenoCongestionCtlConfig*)pConfig;
//             if(ccType == CongestionCtlType::reno) {
//                 m_congestionCtl.reset(new RenoCongestionContrl(m_ccConfig));
//             }
//             else if(ccType == CongestionCtlType::reno_AIAD) {
//                 m_congestionCtl.reset(new RenoAIADCongestionContrl(m_ccConfig));
//             }
//             else if(ccType == CongestionCtlType::bbr) {
//                 m_congestionCtl.reset(new BBRCongestionControl(m_ccConfig, shared_from_this()));
//             }
//             else {
//                 m_congestionCtl.reset(nullptr);
//                 return;
//             }
//         }

//         // send control
//         m_sendCtl.reset(new PacketSender());

//         //loss detection
//         m_lossDetect.reset(new DefaultLossDetectionAlgo());

//         // set initial smothed rtt
//         m_rttstats.set_initial_rtt(Duration::FromMilliseconds(200));

//     }

//     void StopSessionStreamCtl()
//     {
//         if (isRunning)
//         {
//             isRunning = false;
//         }
//         else
//         {
//             SPDLOG_WARN("isRunning = false");
//         }
//     }

//     basefw::ID GetSessionId()
//     {
//         if (isRunning)
//         {
//             return m_sessionId;
//         }
//         else
//         {
//             return {};
//         }
//     }

//     bool CanSend()
//     {
//         SPDLOG_TRACE("");
//         if (!isRunning)
//         {
//             return false;
//         }

//         return m_sendCtl->CanSend(m_congestionCtl->GetCWND(), GetInFlightPktNum());
//     }

//     uint32_t CanRequestPktCnt()
//     {
//         SPDLOG_TRACE("");
//         if (!isRunning)
//         {
//             return false;
//         }
//         return m_sendCtl->MaySendPktCnt(m_congestionCtl->GetCWND(), GetInFlightPktNum());
//     };

//     /// send ONE datarequest Pkt, requestting for the data pieces whose id are in spns
//     bool DoRequestdata(const basefw::ID& peerid, const std::vector<int32_t>& spns)
//     {
//         SPDLOG_TRACE("peerid = {}, spns = {}", peerid.ToLogStr(), spns);
//         if (!isRunning)
//         {
//             return false;
//         }
//         if (!CanSend())
//         {
//             SPDLOG_WARN("CanSend = false");
//             return false;
//         }

//         if (spns.size() > CanRequestPktCnt())
//         {
//             SPDLOG_WARN("The number of request data pieces {} exceeds the freewnd {}", spns.size(), CanRequestPktCnt());
//             return false;
//         }
//         auto handler = m_ssStreamHandler.lock();
//         if (handler)
//         {
//             return handler->DoSendDataRequest(peerid, spns);
//         }
//         else
//         {
//             SPDLOG_WARN("SessionStreamHandler is null");
//             return false;
//         }

//     }

//     void OnDataRequestPktSent(const std::vector<SeqNumber>& seqs,
//             const std::vector<DataNumber>& dataids, Timepoint sendtic)
//     {
//         SPDLOG_TRACE("seq = {}, dataid = {}, sendtic = {}",
//                 seqs,
//                 dataids, sendtic.ToDebuggingValue());
//         if (!isRunning)
//         {
//             return;
//         }
//         auto seqidx = 0;
//         for (auto datano: dataids)
//         {
//             DataPacket p;
//             p.seq = seqs[seqidx];
//             p.pieceId = datano;
//             // add to downloading queue
//             m_inflightpktmap.AddSentPacket(p, sendtic);

//             // inform cc algo that a packet is sent
//             InflightPacket sentpkt;
//             sentpkt.seq = seqs[seqidx];
//             sentpkt.pieceId = datano;
//             sentpkt.sendtic = sendtic;
//             m_congestionCtl->OnDataSent(sentpkt);
//             seqidx++;
//         }

//     }

//     void OnDataPktReceived(uint32_t seq, int32_t datapiece, Timepoint recvtic)
//     {
//         if (!isRunning)
//         {
//             return;
//         }
//         // find the sending record
//         auto rtpair = m_inflightpktmap.PktIsInFlight(seq, datapiece);
//         auto inFlight = rtpair.first;
//         auto inflightPkt = rtpair.second;
//         if (inFlight)
//         {

//             auto oldsrtt = m_rttstats.smoothed_rtt();
//             // we don't have ack_delay in this simple implementation.
//             auto pkt_rtt = recvtic - inflightPkt.sendtic;
//             m_rttstats.UpdateRtt(pkt_rtt, Duration::Zero(), Clock::GetClock()->Now());
//             auto newsrtt = m_rttstats.smoothed_rtt();

//             auto oldcwnd = m_congestionCtl->GetCWND();

//             AckEvent ackEvent;
//             ackEvent.valid = true;
//             ackEvent.ackPacket.seq = seq;
//             ackEvent.ackPacket.pieceId = datapiece;
//             ackEvent.sendtic = inflightPkt.sendtic;
//             LossEvent lossEvent; // if we detect loss when ACK event, we may do loss check here.
//             m_congestionCtl->OnDataAckOrLoss(ackEvent, lossEvent, m_rttstats);

//             auto newcwnd = m_congestionCtl->GetCWND();
//             // mark as received
//             m_inflightpktmap.OnPacktReceived(inflightPkt, recvtic);
//         }
//         else
//         {
//             SPDLOG_WARN(" Recv an pkt with unknown seq:{}", seq);
//         }

//     }

//     void OnLossDetectionAlarm()
//     {
//         DoAlarmTimeoutDetection();
//     }

//     void InformLossUp(LossEvent& loss)
//     {
//         if (!isRunning)
//         {
//             return;
//         }
//         auto handler = m_ssStreamHandler.lock();
//         if (handler)
//         {
//             std::vector<int32_t> lossedPieces;
//             for (auto&& pkt: loss.lossPackets)
//             {
//                 lossedPieces.emplace_back(pkt.pieceId);
//             }
//             handler->OnPiecePktTimeout(m_sessionId, lossedPieces);
//         }
//     }

//     void DoAlarmTimeoutDetection()
//     {
//         if (!isRunning)
//         {
//             return;
//         }
//         ///check timeout
//         Timepoint now_t = Clock::GetClock()->Now();
//         AckEvent ack;
//         LossEvent loss;
//         m_lossDetect->DetectLoss(m_inflightpktmap, now_t, ack, -1, loss, m_rttstats);
//         if (loss.valid)
//         {
//             // [SM]
//             m_loss = (double)loss.lossPackets.size() / (double)GetInFlightPktNum();

//             for (auto&& pkt: loss.lossPackets)
//             {
//                 m_inflightpktmap.RemoveFromInFlight(pkt);
//             }
//             m_congestionCtl->OnDataAckOrLoss(ack, loss, m_rttstats);
//             InformLossUp(loss);
//         }
//     }

//     Duration GetRtt()
//     {
//         Duration rtt{ Duration::Zero() };
//         if (isRunning)
//         {
//             rtt = m_rttstats.smoothed_rtt();
//         }
//         SPDLOG_TRACE("rtt = {}", rtt.ToDebuggingValue());
//         return rtt;
//     }

//     uint32_t GetInFlightPktNum()
//     {
//         return m_inflightpktmap.InFlightPktNum();
//     }


//     basefw::ID GetSessionID()
//     {
//         return m_sessionId;
//     }

//     // [SM]
//     uint32_t GetCWND()
//     {
//         return m_congestionCtl->GetCWND();
//     }
//     double GetLossRate()
//     {
//         return m_loss;
//     }

// private:
//     bool isRunning{ false };

//     basefw::ID m_sessionId;/** The remote peer id defines the session id*/
//     basefw::ID m_taskid;/**The file id downloading*/
//     RenoCongestionCtlConfig m_ccConfig;
//     std::unique_ptr<CongestionCtlAlgo> m_congestionCtl;
//     std::unique_ptr<LossDetectionAlgo> m_lossDetect;
//     std::weak_ptr<SessionStreamCtlHandler> m_ssStreamHandler;
//     InFlightPacketMap m_inflightpktmap;

//     std::unique_ptr<PacketSender> m_sendCtl;
//     RttStats m_rttstats;

//     // [SM]
//     double m_loss;
// };

