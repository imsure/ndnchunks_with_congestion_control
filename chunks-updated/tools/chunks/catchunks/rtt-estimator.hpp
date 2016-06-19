/**
 * Copyright (c) 2015,  Arizona Board of Regents.
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
 * @author Shuo Yang <shuoyang@email.arizona.edu>
 */

#ifndef NDN_TOOLS_CHUNKS_CATCHUNKS_RTT_ESTIMATOR_HPP
#define NDN_TOOLS_CHUNKS_CATCHUNKS_RTT_ESTIMATOR_HPP

#include "core/common.hpp"
#include "options.hpp"

namespace ndn {
namespace chunks {

typedef time::duration<double, time::milliseconds::period> duration_in_ms;

class RttEstimatorOptions : public Options
{
public:
  explicit
  RttEstimatorOptions(const Options& options = Options())
    : Options(options)
    , alpha(0.125)
    , beta(0.25)
    , k(4)
    , minRto(50) // 50 ms
    , maxRto(4000) // 4s
    , rtoBackoffMultiplier(2)
  {
  }

public:
  double alpha; // parameter for RTT estimation
  double beta; // parameter for RTT variation calculation
  int k; // parameter for RTO computation
  double minRto; // lower bound of RTO (not used currently)
  double maxRto; // upper bound of RTO
  int rtoBackoffMultiplier;
};

/**
 * @brief RTT Estimator.
 *
 * This class implements the "Mean--Deviation" RTT estimator, as discussed in RFC6298.
 */
class RttEstimator
{
public:
  typedef RttEstimatorOptions Options;


public:
  /**
   * @brief create a RTT Estimator
   *
   * Configures the RTT Estimator with the default parameters.
   */
  explicit
  RttEstimator(const Options& options = Options());

  ~RttEstimator();

  void
  rttMeasurementFirstTime(uint64_t segno,
                          const time::steady_clock::TimePoint& timeSent,
                          double rto);

  /**
   * @brief Add a new RTT measurement to the estimator for the given received segment.
   *
   * @param segno the segment number of the received segmented Data
   * @param timeSent the time when the corresponding Interest was sent
   * @param rto (in ms) estimated RTO for the segment at the time it was sent out;
   *        this parameter is for keeping a history of RTT measurement,
   *        so may not be used unless keepStats flag is true.
   */
  void
  rttMeasurement(uint64_t segno,
                 const time::steady_clock::TimePoint& timeSent,
                 double rto);

  /**
   * @brief returns the estimated RTO value in ms.
   */
  double
  estimatedRto();

  void
  rtoBackoff();

  void
  writeStats();

private:

private:
  const Options m_options;
  double m_sRtt; // smoothed round-trip time, in milliseconds
  double m_rttVar; // round-trip time variation, in milliseconds
  double m_rto; // retransmission timeout, in milliseconds

  time::steady_clock::TimePoint m_mostRecentRttEstimation;

  /*
   * a vector of rtt/rto history for each segment (not including retransmitted ones).
   */
  std::vector<std::pair<uint64_t, std::pair<double, double>>> m_rttrtoHistory;

  /*
   * a vector of rtt estimation samples for each sampled segment.
   * first element of the pair is the segment number, second element is a vector
   * of <m_rttVar, m_sRtt, m_rto>.
   */
  std::vector<std::pair<uint64_t, std::vector<double>>> m_rttrtoSamples;
};

} // namespace chunks
} // namespace ndn

#endif // NDN_TOOLS_CHUNKS_CATCHUNKS_PIPELINE_INTERESTS_HPP
