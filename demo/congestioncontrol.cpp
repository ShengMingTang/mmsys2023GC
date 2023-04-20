// Copyright (c) 2023. ByteDance Inc. All rights reserved.
#include <cstdint>
#include <chrono>
#include "utils/thirdparty/quiche/rtt_stats.h"
#include "basefw/base/log.h"
#include "utils/rttstats.h"
#include "utils/transporttime.h"
#include "utils/defaultclock.hpp"
#include "sessionstreamcontroller.hpp"
#include "packettype.h"
#include "congestioncontrol.hpp"

std::string LossEvent::DebugInfo() const
{
    std::stringstream ss;
    ss << "valid: " << valid << " "
        << "lossPackets:{";
    for (const auto& pkt: lossPackets)
    {
        ss << pkt;
    }

    ss << "} "
        << "losttic: " << losttic.ToDebuggingValue() << " ";
    return ss.str();
}

std::string AckEvent::DebugInfo() const
{
    std::stringstream ss;
    ss << "valid: " << valid << " "
        << "ackpkt:{"
        << "seq: " << ackPacket.seq << " "
        << "dataid: " << ackPacket.pieceId << " "
        << "} "
        << "sendtic: " << sendtic.ToDebuggingValue() << " "
        << "losttic: " << losttic.ToDebuggingValue() << " ";
    return ss.str();
}

void DefaultLossDetectionAlgo::DetectLoss(const InFlightPacketMap& downloadingmap, Timepoint eventtime, const AckEvent& ackEvent,
        uint64_t maxacked, LossEvent& losses, RttStats& rttStats) 
{
    SPDLOG_TRACE("inflight: {} eventtime: {} ackEvent:{} ", downloadingmap.DebugInfo(),
            eventtime.ToDebuggingValue(), ackEvent.DebugInfo());
    /** RFC 9002 Section 6
     * */
    Duration maxrtt = std::max(rttStats.previous_srtt(), rttStats.latest_rtt());
    if (maxrtt == Duration::Zero())
    {
        SPDLOG_DEBUG(" {}", maxrtt == Duration::Zero());
        maxrtt = rttStats.SmoothedOrInitialRtt();
    }
    Duration loss_delay = maxrtt + (maxrtt * (5.0 / 4.0));
    loss_delay = std::max(loss_delay, Duration::FromMicroseconds(1));
    SPDLOG_TRACE(" maxrtt: {}, loss_delay: {}", maxrtt.ToDebuggingValue(), loss_delay.ToDebuggingValue());
    for (const auto& pkt_itor: downloadingmap.inflightPktMap)
    {
        const auto& pkt = pkt_itor.second;
        if (Timepoint(pkt.sendtic + loss_delay) <= eventtime)
        {
            losses.lossPackets.emplace_back(pkt);
        }
    }
    if (!losses.lossPackets.empty())
    {
        losses.losttic = eventtime;
        losses.valid = true;
        SPDLOG_DEBUG("losses: {}", losses.DebugInfo());
    }
}

DefaultLossDetectionAlgo::~DefaultLossDetectionAlgo() 
{
}


 RenoCongestionContrl::RenoCongestionContrl(const RenoCongestionCtlConfig& ccConfig)
{
    m_ssThresh = ccConfig.ssThresh;
    m_minCwnd = ccConfig.minCwnd;
    m_maxCwnd = ccConfig.maxCwnd;
    SPDLOG_DEBUG("m_ssThresh:{}, m_minCwnd:{}, m_maxCwnd:{} ", m_ssThresh, m_minCwnd, m_maxCwnd);
}
RenoCongestionContrl::~RenoCongestionContrl() 
{
    SPDLOG_DEBUG("");
}

CongestionCtlType RenoCongestionContrl::GetCCtype() 
{
    return CongestionCtlType::reno;
}

void RenoCongestionContrl::OnDataSent(const InflightPacket& sentpkt) 
{
    SPDLOG_TRACE("");
}

void RenoCongestionContrl::OnDataAckOrLoss(const AckEvent& ackEvent, const LossEvent& lossEvent, RttStats& rttstats) 
{
    SPDLOG_TRACE("ackevent:{}, lossevent:{}", ackEvent.DebugInfo(), lossEvent.DebugInfo());
    if (lossEvent.valid)
    {
        OnDataLoss(lossEvent);
    }

    if (ackEvent.valid)
    {
        OnDataRecv(ackEvent);
    }

}

/////
uint32_t RenoCongestionContrl::GetCWND() 
{
    SPDLOG_TRACE(" {}", m_cwnd);
    return m_cwnd;
}

bool RenoCongestionContrl::InSlowStart()
{
    bool rt = false;
    if (m_cwnd < m_ssThresh)
    {
        rt = true;
    }
    else
    {
        rt = false;
    }
    SPDLOG_TRACE(" m_cwnd:{}, m_ssThresh:{}, InSlowStart:{}", m_cwnd, m_ssThresh, rt);
    return rt;
}
bool RenoCongestionContrl::LostCheckRecovery(Timepoint largestLostSentTic)
{
    SPDLOG_DEBUG("largestLostSentTic:{},lastLagestLossPktSentTic:{}",
            largestLostSentTic.ToDebuggingValue(), lastLagestLossPktSentTic.ToDebuggingValue());
    /** If the largest sent tic of this loss event,is bigger than the last sent tic of the last lost pkt
     * (plus a 10ms correction), this session is in Recovery phase.
     * */
    if (lastLagestLossPktSentTic.IsInitialized() &&
        (largestLostSentTic + Duration::FromMilliseconds(10) > lastLagestLossPktSentTic))
    {
        SPDLOG_DEBUG("In Recovery");
        return true;
    }
    else
    {
        // a new timelost
        lastLagestLossPktSentTic = largestLostSentTic;
        SPDLOG_DEBUG("new loss");
        return false;
    }

}

void RenoCongestionContrl::ExitSlowStart()
{
    SPDLOG_DEBUG("m_ssThresh:{}, m_cwnd:{}", m_ssThresh, m_cwnd);
    m_ssThresh = m_cwnd;
}


void RenoCongestionContrl::OnDataRecv(const AckEvent& ackEvent)
{
    SPDLOG_DEBUG("ackevent:{},m_cwnd:{}", ackEvent.DebugInfo(), m_cwnd);
    if (InSlowStart())
    {
        /// add 1 for each ack event
        m_cwnd += 1;

        if (m_cwnd >= m_ssThresh)
        {
            ExitSlowStart();
        }
        SPDLOG_DEBUG("new m_cwnd:{}", m_cwnd);
    }
    else
    {
        /// add cwnd for each RTT
        m_cwndCnt++;
        m_cwnd += m_cwndCnt / m_cwnd;
        if (m_cwndCnt == m_cwnd)
        {
            m_cwndCnt = 0;
        }
        SPDLOG_DEBUG("not in slow start state,new m_cwndCnt:{} new m_cwnd:{}",
                m_cwndCnt, ackEvent.DebugInfo(), m_cwnd);

    }
    m_cwnd = BoundCwnd(m_cwnd);

    SPDLOG_DEBUG("after RX, m_cwnd={}", m_cwnd);
}
void RenoCongestionContrl::OnDataLoss(const LossEvent& lossEvent)
{
    SPDLOG_DEBUG("lossevent:{}", lossEvent.DebugInfo());
    Timepoint maxsentTic{ Timepoint::Zero() };

    for (const auto& lostpkt: lossEvent.lossPackets)
    {
        maxsentTic = std::max(maxsentTic, lostpkt.sendtic);
    }

    /** In Recovery phase, cwnd will decrease 1 pkt for each lost pkt
     *  Otherwise, cwnd will cut half.
     * */
    if (InSlowStart())
    {
        // loss in slow start, just cut half
        m_cwnd = m_cwnd / 2;
        m_cwnd = BoundCwnd(m_cwnd);

    }
    else //if (!LostCheckRecovery(maxsentTic))
    {
        // Not In slow start and not inside Recovery state
        // Cut half
        m_cwnd = m_cwnd / 2;
        m_cwnd = BoundCwnd(m_cwnd);
        m_ssThresh = m_cwnd;
        // enter Recovery state
    }
    SPDLOG_DEBUG("after Loss, m_cwnd={}", m_cwnd);
}

uint32_t RenoCongestionContrl::BoundCwnd(uint32_t trySetCwnd)
{
    return std::max(m_minCwnd, std::min(trySetCwnd, m_maxCwnd));
}

 RenoAIADCongestionContrl::RenoAIADCongestionContrl(const RenoCongestionCtlConfig& ccConfig)
{
    m_ssThresh = ccConfig.ssThresh;
    m_minCwnd = ccConfig.minCwnd;
    m_maxCwnd = ccConfig.maxCwnd;
    SPDLOG_DEBUG("m_ssThresh:{}, m_minCwnd:{}, m_maxCwnd:{} ", m_ssThresh, m_minCwnd, m_maxCwnd);
}

RenoAIADCongestionContrl::~RenoAIADCongestionContrl() 
{
    SPDLOG_DEBUG("");
}

CongestionCtlType RenoAIADCongestionContrl::GetCCtype() 
{
    return CongestionCtlType::reno_AIAD;
}

void RenoAIADCongestionContrl::OnDataSent(const InflightPacket& sentpkt) 
{
    SPDLOG_TRACE("");
}

void RenoAIADCongestionContrl::OnDataAckOrLoss(const AckEvent& ackEvent, const LossEvent& lossEvent, RttStats& rttstats) 
{
    SPDLOG_TRACE("ackevent:{}, lossevent:{}", ackEvent.DebugInfo(), lossEvent.DebugInfo());
    if (lossEvent.valid)
    {
        OnDataLoss(lossEvent);
    }

    if (ackEvent.valid)
    {
        OnDataRecv(ackEvent);
    }

}

/////
uint32_t RenoAIADCongestionContrl::GetCWND() 
{
    SPDLOG_TRACE(" {}", m_cwnd);
    return m_cwnd;
}

bool RenoAIADCongestionContrl::InSlowStart()
{
    bool rt = false;
    if (m_cwnd < m_ssThresh)
    {
        rt = true;
    }
    else
    {
        rt = false;
    }
    SPDLOG_TRACE(" m_cwnd:{}, m_ssThresh:{}, InSlowStart:{}", m_cwnd, m_ssThresh, rt);
    return rt;
}

bool RenoAIADCongestionContrl::LostCheckRecovery(Timepoint largestLostSentTic)
{
    SPDLOG_DEBUG("largestLostSentTic:{},lastLagestLossPktSentTic:{}",
            largestLostSentTic.ToDebuggingValue(), lastLagestLossPktSentTic.ToDebuggingValue());
    /** If the largest sent tic of this loss event,is bigger than the last sent tic of the last lost pkt
     * (plus a 10ms correction), this session is in Recovery phase.
     * */
    if (lastLagestLossPktSentTic.IsInitialized() &&
        (largestLostSentTic + Duration::FromMilliseconds(10) > lastLagestLossPktSentTic))
    {
        SPDLOG_DEBUG("In Recovery");
        return true;
    }
    else
    {
        // a new timelost
        lastLagestLossPktSentTic = largestLostSentTic;
        SPDLOG_DEBUG("new loss");
        return false;
    }

}

void RenoAIADCongestionContrl::ExitSlowStart()
{
    SPDLOG_DEBUG("m_ssThresh:{}, m_cwnd:{}", m_ssThresh, m_cwnd);
    m_ssThresh = m_cwnd;
}


void RenoAIADCongestionContrl::OnDataRecv(const AckEvent& ackEvent)
{
    SPDLOG_DEBUG("ackevent:{},m_cwnd:{}", ackEvent.DebugInfo(), m_cwnd);
    if (InSlowStart())
    {
        /// add 1 for each ack event
        m_cwnd += 1;

        if (m_cwnd >= m_ssThresh)
        {
            ExitSlowStart();
        }
        SPDLOG_DEBUG("new m_cwnd:{}", m_cwnd);
    }
    else
    {
        /// add cwnd for each RTT
        m_cwndCnt++;
        m_cwnd += m_cwndCnt / m_cwnd;
        if (m_cwndCnt == m_cwnd)
        {
            m_cwndCnt = 0;
        }
        SPDLOG_DEBUG("not in slow start state,new m_cwndCnt:{} new m_cwnd:{}",
                m_cwndCnt, ackEvent.DebugInfo(), m_cwnd);

    }
    m_cwnd = BoundCwnd(m_cwnd);

    SPDLOG_DEBUG("after RX, m_cwnd={}", m_cwnd);
}

void RenoAIADCongestionContrl::OnDataLoss(const LossEvent& lossEvent)
{
    SPDLOG_DEBUG("lossevent:{}", lossEvent.DebugInfo());
    Timepoint maxsentTic{ Timepoint::Zero() };

    for (const auto& lostpkt: lossEvent.lossPackets)
    {
        maxsentTic = std::max(maxsentTic, lostpkt.sendtic);
    }

    /** In Recovery phase, cwnd will decrease 1 pkt for each lost pkt
     *  Otherwise, cwnd will cut half.
     * */
    if (InSlowStart())
    {
        // loss in slow start, just cut half
        m_cwnd = m_cwnd / 2;
        m_cwnd = BoundCwnd(m_cwnd);

    }
    else //if (!LostCheckRecovery(maxsentTic))
    {
        // Not In slow start and not inside Recovery state
        // [SM]
        // Cut half
        // m_cwnd = m_cwnd / 2;
        m_cwnd -= lossEvent.lossPackets.size() / m_cwnd + 1;
        
        m_cwnd = BoundCwnd(m_cwnd);
        m_ssThresh = m_cwnd;
        // enter Recovery state
    }
    SPDLOG_DEBUG("after Loss, m_cwnd={}", m_cwnd);
}


uint32_t RenoAIADCongestionContrl::BoundCwnd(uint32_t trySetCwnd)
{
    return std::max(m_minCwnd, std::min(trySetCwnd, m_maxCwnd));
}


 BBRCongestionControl::BBRCongestionControl(const RenoCongestionCtlConfig& ccConfig, std::weak_ptr<SessionStreamController> shared_ptr)
{
    m_ssThresh = ccConfig.ssThresh;
    m_minCwnd = ccConfig.minCwnd;
    m_maxCwnd = ccConfig.maxCwnd;
    m_sessionhandler = shared_ptr;
    SPDLOG_DEBUG("try get inflight:{}", GetInFlightPktNum());
    SPDLOG_DEBUG("m_ssThresh:{}, m_minCwnd:{}, m_maxCwnd:{} ", m_ssThresh, m_minCwnd, m_maxCwnd);
}

uint32_t BBRCongestionControl::GetInFlightPktNum(){
    if (auto observe = m_sessionhandler.lock()) {
        return observe->GetInFlightPktNum();
    } else {
        return UINT32_MAX;
    }
}

BBRCongestionControl::~BBRCongestionControl() 
{
    SPDLOG_DEBUG("");
}

CongestionCtlType BBRCongestionControl::GetCCtype() 
{
    return CongestionCtlType::bbr;
}

void BBRCongestionControl::OnDataSent(const InflightPacket& sentpkt) 
{
    SPDLOG_TRACE("");
}

void BBRCongestionControl::OnDataAckOrLoss(const AckEvent& ackEvent, const LossEvent& lossEvent, RttStats& rttstats) 
{
    SPDLOG_TRACE("ackevent:{}, lossevent:{}", ackEvent.DebugInfo(), lossEvent.DebugInfo());
    if (lossEvent.valid)
    {
        OnDataLoss(lossEvent);
    }

    if (ackEvent.valid)
    {
        OnDataRecv(ackEvent, rttstats);
    }

}

/////
uint32_t BBRCongestionControl::GetCWND() 
{
    SPDLOG_TRACE(" {}", m_cwnd);
    return m_cwnd;
}

bool BBRCongestionControl::InSlowStart()
{
    bool rt = false;
    if (m_cwnd < m_ssThresh)
    {
        rt = true;
    }
    else
    {
        rt = false;
    }
    SPDLOG_TRACE(" m_cwnd:{}, m_ssThresh:{}, InSlowStart:{}", m_cwnd, m_ssThresh, rt);
    return rt;
}

bool BBRCongestionControl::LostCheckRecovery(Timepoint largestLostSentTic)
{
    SPDLOG_DEBUG("largestLostSentTic:{},lastLagestLossPktSentTic:{}",
            largestLostSentTic.ToDebuggingValue(), lastLagestLossPktSentTic.ToDebuggingValue());
    /** If the largest sent tic of this loss event,is bigger than the last sent tic of the last lost pkt
     * (plus a 10ms correction), this session is in Recovery phase.
     * */
    if (lastLagestLossPktSentTic.IsInitialized() &&
        (largestLostSentTic + Duration::FromMilliseconds(10) > lastLagestLossPktSentTic))
    {
        SPDLOG_DEBUG("In Recovery");
        return true;
    }
    else
    {
        // a new timelost
        lastLagestLossPktSentTic = largestLostSentTic;
        SPDLOG_DEBUG("new loss");
        return false;
    }

}

void BBRCongestionControl::ExitSlowStart()
{
    SPDLOG_DEBUG("m_ssThresh:{}, m_cwnd:{}", m_ssThresh, m_cwnd);
    m_ssThresh = m_cwnd;
}

float BBRCongestionControl::CurrBwEstimate(uint64_t acked_bytes, double inflight_bytes, uint64_t rtt){
    // acked_bytes: number of bytes acknowledged in the latest received ACK
    // inflight_bytes: calculate the number of in-flight bytes
    // rtt: calculate the RTT of the latest packet
    double estimated_bandwidth = (inflight_bytes + static_cast<double>(acked_bytes)) / std::max<double>(rtt, 1); // calculate the estimated bandwidth in bytes per second
    return estimated_bandwidth;
}

void BBRCongestionControl::OnDataRecv(const AckEvent& ackEvent, RttStats& rttstats)
{
    SPDLOG_DEBUG("ackevent:{},m_cwnd:{}", ackEvent.DebugInfo(), m_cwnd);

    double BytePerPkt = 1; // can we get the size of the pkt
    uint64_t acked_bytes = 0; // can we get the size of the ack bytes
    double currBw = CurrBwEstimate(acked_bytes ,GetInFlightPktNum()*BytePerPkt, static_cast<uint64_t>(rttstats.latest_rtt().ToMicroseconds()));
    

    if (InSlowStart())
    {
        /// add 1 for each ack event
        m_cwnd += 1;

        if (m_cwnd >= m_ssThresh)
        {
            ExitSlowStart();
        }
        SPDLOG_DEBUG("new m_cwnd:{}", m_cwnd);
    }
    else
    {
        /// add cwnd for each RTT
        m_cwndCnt++;
        m_cwnd += m_cwndCnt / m_cwnd;
        if (m_cwndCnt == m_cwnd)
        {
            m_cwndCnt = 0;
        }
        SPDLOG_DEBUG("not in slow start state,new m_cwndCnt:{} new m_cwnd:{}",
                m_cwndCnt, ackEvent.DebugInfo(), m_cwnd);

    }
    m_cwnd = BoundCwnd(m_cwnd);

    SPDLOG_DEBUG("after RX, m_cwnd={}", m_cwnd);
}

void BBRCongestionControl::OnDataLoss(const LossEvent& lossEvent)
{
    SPDLOG_DEBUG("lossevent:{}", lossEvent.DebugInfo());
    Timepoint maxsentTic{ Timepoint::Zero() };

    for (const auto& lostpkt: lossEvent.lossPackets)
    {
        maxsentTic = std::max(maxsentTic, lostpkt.sendtic);
    }

    /** In Recovery phase, cwnd will decrease 1 pkt for each lost pkt
     *  Otherwise, cwnd will cut half.
     * */
    if (InSlowStart())
    {
        // loss in slow start, just cut half
        m_cwnd = m_cwnd / 2;
        m_cwnd = BoundCwnd(m_cwnd);

    }
    else //if (!LostCheckRecovery(maxsentTic))
    {
        // Not In slow start and not inside Recovery state
        // [SM]
        // Cut half
        // m_cwnd = m_cwnd / 2;
        m_cwnd -= lossEvent.lossPackets.size() / m_cwnd + 1;
        
        m_cwnd = BoundCwnd(m_cwnd);
        m_ssThresh = m_cwnd;
        // enter Recovery state
    }
    SPDLOG_DEBUG("after Loss, m_cwnd={}", m_cwnd);
}


uint32_t BBRCongestionControl::BoundCwnd(uint32_t trySetCwnd)
{
    return std::max(m_minCwnd, std::min(trySetCwnd, m_maxCwnd));
}