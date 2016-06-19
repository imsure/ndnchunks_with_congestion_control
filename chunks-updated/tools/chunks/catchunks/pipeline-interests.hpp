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

#ifndef NDN_TOOLS_CHUNKS_CATCHUNKS_PIPELINE_INTERESTS_HPP
#define NDN_TOOLS_CHUNKS_CATCHUNKS_PIPELINE_INTERESTS_HPP

#include "core/common.hpp"
#include "options.hpp"
#include "rtt-estimator.hpp"
#include <queue>

namespace ndn {
namespace chunks {

class DataFetcher;

class PipelineInterestsOptions : public Options
{
public:
  explicit
  PipelineInterestsOptions(const Options& options = Options())
    : Options(options)
    , maxPipelineSize(1)
    , initCwnd(1)
    , initSsthresh(200)
    , mdCoef(0.5)
    , aiStep(1)
    , cwndRecordInterval(10) // 10ms
    , rtoCheckInterval(10) // 10ms
    , initialRto(1000) // 1000ms
  {
  }

public:
  size_t maxPipelineSize;
  double initCwnd; // initial congestion window size
  double initSsthresh; // initial slow start threshold
  double mdCoef; // multiplicative decrease coefficient
  double aiStep; // additive increase step (unit: segment)
  int cwndRecordInterval; // time interval (in ms) for recording cwnd size
  int rtoCheckInterval; // time interval (in ms) for checking retransmission timer
  double initialRto; // initial RTO value
};

enum SegmentState {
  firstTimeSent, inRetxQueue,
  retransmitted, retransmitReceived,
};

/**
 * @brief Wraps up information that's necessary for segment transmission
 */
class SegmentInfo
{
public:
  /**
   * @brief initialize an segment's information
   */
  explicit
  SegmentInfo(const PendingInterestId *interest_id,
              SegmentState state, double rto,
              time::steady_clock::TimePoint time_sent)
    : interestId(interest_id)
    , state(state)
    , rto(rto)
    , timeSent(time_sent)
  {
  }

public:
  /*
   * The pending interest ID returned by ndn::Face::expressInterest.
   * It can be used with removePendingInterest before retransmitting this Interest.
   */
  const PendingInterestId *interestId;
  SegmentState state;
  double rto;
  time::steady_clock::TimePoint timeSent;
};


/**
 * @brief Service for retrieving Data via an Interest pipeline
 *
 * Retrieves all segmented Data under the specified prefix by maintaining
 * a dynamic congestion window based on TCP SACK Conservative Loss Recovery Algorithm.
 *
 * Provides retrieved Data on arrival with no ordering guarantees. Data is delivered to the
 * PipelineInterests' user via callback immediately upon arrival.
 */
class PipelineInterests
{
public:
  typedef PipelineInterestsOptions Options;
  typedef function<void(const std::string& reason)> FailureCallback;

public:
  /**
   * @brief create a PipelineInterests service
   *
   * Configures the pipelining service without specifying the retrieval namespace. After this
   * configuration the method runWithExcludedSegment must be called to run the Pipeline.
   */
  explicit
  PipelineInterests(Face& face, RttEstimator& rttestimator,
                    const Options& options = Options());

  ~PipelineInterests();

  /**
   * @brief fetch all the segments between 0 and lastSegment of the specified prefix
   *
   * Starts the pipeline of size defined inside the options. The pipeline retrieves all the segments
   * until the last segment is received, @p data is excluded from the retrieving.
   *
   * @param data a segment of the segmented Data to retrive; data.getName() must end with a segment
   *        number
   * @param onData callback for every segment correctly received, must not be empty
   * @param onfailure callback called if an error occurs
   */
  void
  runWithExcludedSegment(const Data& data, DataCallback onData, FailureCallback onFailure);

  /**
   * @brief stop all fetch operations
   */
  void
  cancel();

private:
  /**
   * @brief send out the first Interest by the pipeline to initialize
   * RTT/RTO estimation.
   */
  void
  sendFirstInterest();

  /**
   * @brief check RTO for all sent-but-not-acked segments.
   */
  void
  checkRto();

  void
  handleTimeout(uint64_t timeout_count);

  void
  fail(const std::string& reason);

  /**
   * @brief handle the data corresponds to the first Interest.
   */
  void
  handleDataFirstSegment(const Interest& interest, const Data& data);

  void
  handleData(const Interest& interest, const Data& data);

  void
  handleNack(const Interest& interest, const lp::Nack& nack);

  void
  handleLifeTimeExpiration(const Interest& interest);

  void
  handleFail(const std::string& reason, size_t pipeNo);

  /**
   * @brief adjust congestion window size based on AIMD scheme
   */
  void
  adjustCwnd();

  /**
   * @brief return true if all the segments have been received,
   * otherwise return false.
   */
  bool
  allSegmentsReceived();

  void
  schedulePackets();

  /**
   * @param segno the segment # of the to-be-sent Interest
   * @param is_retransmission true if this is a retransmission
   */
  void
  sendInterest(uint64_t segno, bool is_retransmission);

  void
  recordCwnd();

  void
  writeStats();

  void
  printSummary();

private:
  Name m_prefix;
  Face& m_face;
  RttEstimator& m_rttEstimator;
  Scheduler m_scheduler;
  uint64_t m_nextSegmentNo;
  uint64_t m_lastSegmentNo;
  uint64_t m_excludeSegmentNo;
  DataCallback m_onData;
  FailureCallback m_onFailure;
  const Options m_options;
  std::vector<std::pair<shared_ptr<DataFetcher>, uint64_t>> m_segmentFetchers;
  bool m_hasFinalBlockId;
  size_t m_segmentSize;

  /**
   * true if there's a critical error
   */
  bool m_hasError;
  /**
   * true if one or more segmentFetcher failed, if lastSegmentNo is not set this is usually not a
   * fatal error for the pipeline
   */
  bool m_hasFailure;

  /*
   * the highest segment number of the Data packet the consumer has received so far
   */
  uint64_t m_highData;

  /*
   * the highest segment number of the Interests the consumer has sent so far
   */
  uint64_t m_highInterest;

  /*
   * the value of m_highInterest when a packet loss event occurred.
   * It remains fixed until the next packet loss event happens
   */
  uint64_t m_recPoint;

  /*
   * number of segments in flight
   */
  uint64_t m_inFlight;

  uint64_t m_numOfSegmentReceived; // total # of segments received
  uint64_t m_numOfLossEvent; // total # of loss events (timeout burst) occurred
  uint64_t m_numOfSegmentRetransmitted; // total # of segments retransmitted

  time::steady_clock::TimePoint m_startTime; // start time of pipelining

  double m_cwnd; // current congestion window size (in segments)
  double m_ssthresh; // current slow start threshold

  std::queue<uint64_t> m_retxQueue;

  /*
   * the maps keeps all the internal information of the sent
   * but not ackownledged segments
   */
  std::map<uint64_t, shared_ptr<SegmentInfo>> m_segmentInfoMap;

  /*
   * maps segment number to it's retransmission count.
   * if the count reaches to the maximum number of timeout/nack
   * retries, the pipeline should be aborted
   */
  std::map<uint64_t, int> m_retxCountMap;

  /*
   * keeps a time series of congestion window size;
   * first element of the pair is the time (s) since the start of pipeling;
   * second element is the recorded cwnd size (in segments);
   * this is only used when keepStats flag is set to true;
   */
  std::vector<std::pair<double, double>> m_cwndTimeSeries;

  scheduler::ScopedEventId m_eventCheckRto;
  scheduler::ScopedEventId m_eventRecordCwnd;
};

} // namespace chunks
} // namespace ndn

#endif // NDN_TOOLS_CHUNKS_CATCHUNKS_PIPELINE_INTERESTS_HPP
