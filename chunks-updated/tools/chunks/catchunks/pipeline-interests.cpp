/**
 * Copyright (c) 2016,  Regents of the University of California,
 *                      Colorado State University,
 *                      University Pierre & Marie Curie, Sorbonne University.
 *
 * This file is part of ndn-tools (Named Data Networking Essential Tools).
 * See AUTHORS.md for complete list of ndn-tools authors and contributors.
 *
 * ndn-tools is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * ndn-tools is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * ndn-tools, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 *
 * See AUTHORS.md for complete list of ndn-cxx authors and contributors.
 *
 * @author Wentao Shang
 * @author Steve DiBenedetto
 * @author Andrea Tosatto
 *
 * modified by Shuo Yang <shuoyang@email.arizona.edu>
 */

#include "pipeline-interests.hpp"
#include "data-fetcher.hpp"
#include <fstream>

namespace ndn {
namespace chunks {

// Ethernet header (16) + IP header (20) + UDP header (8)
// + NDN overhead (381, name, signature...)
const int header_overhead = 425;

PipelineInterests::PipelineInterests(Face& face, RttEstimator& rttestimator,
                                     const Options& options)
  : m_face(face)
  , m_rttEstimator(rttestimator)
  , m_scheduler(m_face.getIoService())
  , m_nextSegmentNo(0)
  , m_lastSegmentNo(0)
  , m_excludeSegmentNo(0)
  , m_options(options)
  , m_hasFinalBlockId(false)
  , m_segmentSize(0)
  , m_hasError(false)
  , m_hasFailure(false)
  , m_highData(0)
  , m_highInterest(0)
  , m_recPoint(0)
  , m_inFlight(0)
  , m_numOfSegmentReceived(0)
  , m_numOfLossEvent(0)
  , m_numOfSegmentRetransmitted(0)
  , m_eventCheckRto(m_scheduler)
  , m_eventRecordCwnd(m_scheduler)
{
  m_segmentFetchers.resize(m_options.maxPipelineSize);
  m_cwnd = m_options.initCwnd;
  m_ssthresh = m_options.initSsthresh;

  if (m_options.isVerbose) {
    std::cerr << "Interest Pipeplie starts with following parameters:" << std::endl;
    std::cerr << "\tinitial cwnd = " << m_options.initCwnd << std::endl;
    std::cerr << "\tinitial slow start threshold = " << m_options.initSsthresh << std::endl;
    std::cerr << "\tMD factor = " << m_options.mdCoef << std::endl;
    std::cerr << "\tAI step = " << m_options.aiStep << std::endl;
    std::cerr << "\tinitial RTO = " << m_options.initialRto << std::endl;
    std::cerr << "\tRTO check interval = " << m_options.rtoCheckInterval << "ms" << std::endl;
    std::cerr << "\tinitial RTO = " << m_options.initialRto << std::endl;
    std::cerr << "\tMax retries on timeout = "
              << m_options.maxRetriesOnTimeoutOrNack << std::endl;
  }
}

PipelineInterests::~PipelineInterests()
{
}

void
PipelineInterests::runWithExcludedSegment(const Data& data, DataCallback onData,
                                          FailureCallback onFailure)
{
  BOOST_ASSERT(onData != nullptr);

  // record the start time of running pipeline
  m_startTime = time::steady_clock::now();

  m_onData = std::move(onData);
  m_onFailure = std::move(onFailure);
  m_numOfSegmentReceived++; // count the excluded segment

  Name dataName = data.getName();
  m_prefix = dataName.getPrefix(-1);
  m_excludeSegmentNo = dataName[-1].toSegment();

  if (!data.getFinalBlockId().empty()) {
    m_hasFinalBlockId = true;
    m_lastSegmentNo = data.getFinalBlockId().toSegment();
  }
  else { // Name must contain a final block ID so that consumer knows when download finishes
    fail("Name of Data packet: " + data.getName().toUri() +
         " must a final block ID!");
  }

  // schedule the event to check retransmission timer.
  m_eventCheckRto = m_scheduler.scheduleEvent(time::milliseconds(m_options.rtoCheckInterval),
                                              bind(&PipelineInterests::checkRto, this));

  if (m_options.keepStats) {
    // schedule the event to keep track the history of cwnd size
    m_eventRecordCwnd =
      m_scheduler.scheduleEvent(time::milliseconds(m_options.cwndRecordInterval),
                                bind(&PipelineInterests::recordCwnd, this));
  }

  sendFirstInterest();
}

void
PipelineInterests::checkRto()
{
  uint64_t timeout_count = 0;
  bool timeout_found = false;

  for (auto it = m_segmentInfoMap.begin(); it != m_segmentInfoMap.end(); ++it) {
    shared_ptr<SegmentInfo> seg_info = it->second;
    if (seg_info->state != inRetxQueue && // donot check segments currently in the retx queue
        seg_info->state != retransmitReceived) { // or already-received retransmitted segments
      duration_in_ms time_elapsed = time::steady_clock::now() - seg_info->timeSent;
      if (time_elapsed.count() > seg_info->rto) { // timer expired?
        uint64_t timedout_seg = it->first;
        m_retxQueue.push(timedout_seg); // put on retx queue
        seg_info->state = inRetxQueue; // update status
        timeout_found = true;
        timeout_count++;
      }
    }
  }

  if (timeout_found) {
    handleTimeout(timeout_count);
  }

  // schedule the next event after predefined interval
  m_scheduler.scheduleEvent(time::milliseconds(m_options.rtoCheckInterval),
                            bind(&PipelineInterests::checkRto, this));
}

void
PipelineInterests::sendFirstInterest()
{
  // get around the excluded segment
  if (m_nextSegmentNo == m_excludeSegmentNo)
    m_nextSegmentNo++;

  if (m_hasFinalBlockId && m_nextSegmentNo > m_lastSegmentNo)
    return;

  // Send the first interest to initialize RTT measurement
  if (m_options.isVerbose)
    std::cerr << "Requesting the first segment #" << m_nextSegmentNo << std::endl;

  Interest interest(Name(m_prefix).appendSegment(m_nextSegmentNo));
  interest.setInterestLifetime(m_options.interestLifetime);
  interest.setMustBeFresh(m_options.mustBeFresh);
  interest.setMaxSuffixComponents(1);

  auto interestId = m_face.expressInterest(interest,
                                           bind(&PipelineInterests::handleDataFirstSegment,
                                                this, _1, _2),
                                           bind(&PipelineInterests::handleNack, this, _1, _2),
                                           bind(&PipelineInterests::handleLifeTimeExpiration,
                                                this, _1));

  SegmentState state = firstTimeSent;
  auto segInfo = shared_ptr<SegmentInfo>(new SegmentInfo(interestId,
                                                         state,
                                                         m_options.initialRto,
                                                         time::steady_clock::now()));

  m_segmentInfoMap[m_nextSegmentNo] = segInfo;
  m_inFlight++;
  m_nextSegmentNo++;
}

void
PipelineInterests::cancel()
{
  m_segmentInfoMap.clear();
  m_scheduler.cancelAllEvents();
  m_face.getIoService().stop();
}

void
PipelineInterests::fail(const std::string& reason)
{
  std::cerr << "Interest Pipeline failed because: " << reason << std::endl;
  m_hasError = true;
  m_hasFailure = true;
  if (m_onFailure)
    m_face.getIoService().post([this, reason] { m_onFailure(reason); });

  cancel();
}

void
PipelineInterests::handleDataFirstSegment(const Interest& interest, const Data& data)
{
  BOOST_ASSERT(data.getName().equals(interest.getName()));

  uint64_t recv_segno = data.getName()[-1].toSegment();
  if (m_highData < recv_segno) {
    m_highData = recv_segno; // record the highest segment number of data received so far
  }

  m_segmentSize = data.getContent().value_size(); // get segment size
  shared_ptr<SegmentInfo> seg_info = m_segmentInfoMap[recv_segno];

  if (m_options.isVerbose) {
    duration_in_ms rtt = time::steady_clock::now() - seg_info->timeSent;
    std::cerr << "Received the first segment #" << recv_segno
              << ", rtt=" << rtt.count() << "ms"
              << ", rto=" << seg_info->rto << "ms" << std::endl;
  }

  // initiate RTT measurement
  m_rttEstimator.rttMeasurementFirstTime(recv_segno,
                                         seg_info->timeSent,
                                         seg_info->rto);
  m_onData(interest, data);

  if (m_inFlight > 0) m_inFlight--;

  m_numOfSegmentReceived++;
  m_segmentInfoMap.erase(recv_segno);
  adjustCwnd();

  if (allSegmentsReceived() == true)
    cancel();
  else
    schedulePackets();
}

void
PipelineInterests::schedulePackets()
{
  int available_window_size = static_cast<int>(floor(m_cwnd - m_inFlight));

  while (available_window_size > 0) {
    if (m_retxQueue.size()) { // do retransmission first
      uint64_t retx_segno = m_retxQueue.front();
      m_retxQueue.pop();

      auto it = m_segmentInfoMap.find(retx_segno);
      if (it != m_segmentInfoMap.end()) {
        // the segment is still in the map, it means that it needs to be retransmitted
        sendInterest(retx_segno, true);
      }
      else {
        continue;
      }
    }
    else { // send next segment
      sendInterest(m_nextSegmentNo, false);
      m_nextSegmentNo++;
    }
    available_window_size--;
  }
}

void
PipelineInterests::sendInterest(uint64_t segno, bool is_retransmission)
{
  // get around the excluded segment
  if (m_nextSegmentNo == m_excludeSegmentNo)
    m_nextSegmentNo++;

  if (m_hasFinalBlockId && m_nextSegmentNo > m_lastSegmentNo)
    return;

  if (m_options.isVerbose) {
    if (is_retransmission)
      std::cerr << "Retransmitting the segment #" << segno << std::endl;
    else
      std::cerr << "Requesting the segment #" << segno << std::endl;
  }

  if (is_retransmission) {
    auto it = m_retxCountMap.find(segno);
    if (it == m_retxCountMap.end()) { // not found, first retransmission
      m_retxCountMap[segno] = 1;
    } else {
      if (m_retxCountMap[segno] > m_options.maxRetriesOnTimeoutOrNack) {
        fail("Reached the maximum number of timeout retries (" +
             to_string(m_options.maxRetriesOnTimeoutOrNack) +
             ") while retrieving segment " + to_string(segno));
        return;
      }
      m_retxCountMap[segno] += 1;

      if (m_options.isVerbose) {
        std::cerr << "# of retries for the segment #" << segno
                  << " is "<< m_retxCountMap[segno] << std::endl;
      }
    }

    m_face.removePendingInterest(m_segmentInfoMap[segno]->interestId);
  }

  Interest interest(Name(m_prefix).appendSegment(segno));
  interest.setInterestLifetime(m_options.interestLifetime);
  interest.setMustBeFresh(m_options.mustBeFresh);
  interest.setMaxSuffixComponents(1);

  auto interestId = m_face.expressInterest(interest,
                                           bind(&PipelineInterests::handleData, this, _1, _2),
                                           bind(&PipelineInterests::handleNack, this, _1, _2),
                                           bind(&PipelineInterests::handleLifeTimeExpiration,
                                                this, _1));

  SegmentState state;
  if (is_retransmission) {
    m_segmentInfoMap[segno]->state = retransmitted;
    m_segmentInfoMap[segno]->rto = m_rttEstimator.estimatedRto();
    m_segmentInfoMap[segno]->timeSent = time::steady_clock::now();
    m_numOfSegmentRetransmitted++;
  }
  else {
    m_highInterest = segno;
    state = firstTimeSent;

    auto segInfo = shared_ptr<SegmentInfo>(new SegmentInfo(interestId,
                                                           state,
                                                           m_rttEstimator.estimatedRto(),
                                                           time::steady_clock::now()));
    m_segmentInfoMap[segno] = segInfo;
  }

  m_inFlight++;
}

void
PipelineInterests::adjustCwnd()
{
  if (m_cwnd < m_ssthresh) {
    m_cwnd += m_options.aiStep; // additive increase
  } else {
    m_cwnd += m_options.aiStep/floor(m_cwnd); // congestion avoidance
  }
}

bool
PipelineInterests::allSegmentsReceived()
{
  if (m_numOfSegmentReceived - 1 >= m_lastSegmentNo)
    return true;
  else
    return false;
}

void
PipelineInterests::handleData(const Interest& interest, const Data& data)
{
  BOOST_ASSERT(data.getName().equals(interest.getName()));

  uint64_t recv_segno = data.getName()[-1].toSegment();
  if (m_highData < recv_segno) {
    m_highData = recv_segno;
  }

  shared_ptr<SegmentInfo> seg_info = m_segmentInfoMap[recv_segno];
  BOOST_ASSERT(seg_info != nullptr);
  if (seg_info->state == retransmitReceived) {
    m_segmentInfoMap.erase(recv_segno);
    return; // ignore already-received segment
  }

  if (m_options.isVerbose) {
    duration_in_ms rtt = time::steady_clock::now() - seg_info->timeSent;
    std::cerr << "Received the segment #" << recv_segno
              << ", rtt=" << rtt.count() << "ms"
              << ", rto=" << seg_info->rto << "ms" << std::endl;
  }

  // for segments in retransmission queue, no need to decrement m_inFligh since
  // it's already been decremented when segments timed out.
  if (seg_info->state != inRetxQueue && m_inFlight > 0) {
    m_inFlight--;
  }

  m_numOfSegmentReceived++;
  adjustCwnd();
  m_onData(interest, data);

  if (seg_info->state == firstTimeSent ||
      seg_info->state == inRetxQueue) { // donot sample RTT for retransmitted segments
    m_rttEstimator.rttMeasurement(recv_segno, seg_info->timeSent, seg_info->rto);
    m_segmentInfoMap.erase(recv_segno); // remove the entry associated with the received segment
  }
  else { // retransmission
    seg_info->state = retransmitReceived;
  }

  if (allSegmentsReceived() == true) {
    printSummary();
    if (m_options.keepStats) {
      writeStats();
      m_rttEstimator.writeStats();
    }
    cancel();
  }
  else {
    schedulePackets();
  }
}

void
PipelineInterests::handleNack(const Interest& interest, const lp::Nack& nack)
{
  if (m_options.isVerbose)
    std::cerr << "Received Nack with reason " << nack.getReason()
              << " for Interest " << interest << std::endl;

  switch (nack.getReason()) {
  case lp::NackReason::DUPLICATE: {
    break; // ignore duplicates
  }
  case lp::NackReason::CONGESTION: { // treated the same as timeout for now
    uint64_t segno = interest.getName()[-1].toSegment();
    m_segmentInfoMap[segno]->state = inRetxQueue; // update state
    handleTimeout(1);
    break;
  }
  default: {
    m_hasError = true;
    fail("Could not retrieve data for " + interest.getName().toUri() +
         ", reason: " + boost::lexical_cast<std::string>(nack.getReason()));
    break;
  }
  }
}

void
PipelineInterests::handleLifeTimeExpiration(const Interest& interest)
{
  uint64_t segno = interest.getName()[-1].toSegment();
  m_retxQueue.push(segno); // put on retx queue
  m_segmentInfoMap[segno]->state = inRetxQueue; // update state
  handleTimeout(1);
}

void
PipelineInterests::handleTimeout(uint64_t timeout_count)
{
  if (m_highData > m_recPoint) { // react to only one timeout per RTT (TCP SACK)
    m_recPoint = m_highInterest;
    m_ssthresh = std::max(2.0, m_cwnd * m_options.mdCoef); // multiplicative decrease
    m_cwnd = m_ssthresh; // fast recovery
    m_rttEstimator.rtoBackoff();

    m_numOfLossEvent++;

    if (m_options.isVerbose) {
      std::cerr << "Packets loss event happened. cwnd = " << m_cwnd
                << ", ssthresh = " << m_ssthresh << std::endl;
    }
  }

  if (m_inFlight >= timeout_count)
    m_inFlight = m_inFlight - timeout_count;
  else
    m_inFlight = 0;

  schedulePackets();
}

void
PipelineInterests::recordCwnd()
{
  time::steady_clock::TimePoint time_now = time::steady_clock::now();
  duration_in_ms time_passed = time_now - m_startTime;
  m_cwndTimeSeries.push_back(std::make_pair(time_passed.count()/1000, m_cwnd));
  m_scheduler.scheduleEvent(time::milliseconds(m_options.cwndRecordInterval),
                            bind(&PipelineInterests::recordCwnd, this));
}

void
PipelineInterests::printSummary()
{
  time::steady_clock::TimePoint time_now = time::steady_clock::now();
  duration_in_ms time_passed = time_now - m_startTime;
  double throughput_kpbs =
    (m_numOfSegmentReceived * 8 * (m_segmentSize + header_overhead)) / time_passed.count();

  std::cerr << "All segments have been received. Canceling the pipleline\n";
  std::cerr << "Total # of segments received: " << m_numOfSegmentReceived << std::endl;
  std::cerr << "Time used: " << time_passed.count() << " ms" << std::endl;
  std::cerr << "Segment size: " << m_segmentSize << " byte" << std::endl;
  std::cerr << "Total # of packet loss burst: " << m_numOfLossEvent << std::endl;
  std::cerr << "Packet loss rate: "
            << static_cast<double>(m_numOfLossEvent) / static_cast<double>(m_numOfSegmentReceived)
            << std::endl;
  std::cerr << "Total # of retransmitted segments: " << m_numOfSegmentRetransmitted
            << std::endl;
  std::cerr << "Effective throughput: " << throughput_kpbs << " kbps" << std::endl;
}

void
PipelineInterests::writeStats()
{
  std::ofstream fs_cwnd("cwnd.txt");

  // header
  fs_cwnd << "time";
  fs_cwnd << '\t';
  fs_cwnd << "cwndsize\n";

  for (size_t i = 0; i < m_cwndTimeSeries.size(); ++i) {
    fs_cwnd << m_cwndTimeSeries[i].first;
    fs_cwnd << '\t';
    fs_cwnd << m_cwndTimeSeries[i].second;
    fs_cwnd << '\n';
  }
}

} // namespace chunks
} // namespace ndn
