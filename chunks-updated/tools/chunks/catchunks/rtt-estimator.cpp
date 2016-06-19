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

#include "core/common.hpp"
#include "options.hpp"
#include "rtt-estimator.hpp"
#include <fstream>

namespace ndn {
namespace chunks {

RttEstimator::RttEstimator(const Options& options)
  : m_options(options)
{
  if (m_options.isVerbose) {
    std::cerr << "Rtt Estimator starts with following parameters:" << std::endl;
    std::cerr << "\talpha = " << m_options.alpha << std::endl;
    std::cerr << "\tbeta = " << m_options.beta << std::endl;
    std::cerr << "\tk = " << m_options.k << std::endl;
    std::cerr << "\tmax rto = " << m_options.maxRto << "ms" << std::endl;
  }

  m_mostRecentRttEstimation = time::steady_clock::now();
}

RttEstimator::~RttEstimator()
{
}

void
RttEstimator::rttMeasurementFirstTime(uint64_t segno,
                                      const time::steady_clock::TimePoint& timeSent,
                                      double rto)
{
  // initialize SRTT and RTTVAR
  duration_in_ms measured_rtt = time::steady_clock::now() - timeSent;
  m_sRtt = measured_rtt.count();
  m_rttVar = m_sRtt / 2;
  m_rto = m_sRtt + m_options.k * m_rttVar;

  if (m_options.keepStats) {
    m_rttrtoHistory.push_back(std::make_pair(segno,
                                             std::make_pair(measured_rtt.count(),
                                                            rto)));

    std::vector<double> rtt_data;
    rtt_data.push_back(m_rttVar);
    rtt_data.push_back(m_sRtt);
    rtt_data.push_back(m_rto);
    m_rttrtoSamples.push_back(std::make_pair(segno, rtt_data));
  }
}

void
RttEstimator::rttMeasurement(uint64_t segno,
                             const time::steady_clock::TimePoint& timeSent,
                             double rto)
{
  time::steady_clock::TimePoint time_now = time::steady_clock::now();
  duration_in_ms time_passed = time_now - m_mostRecentRttEstimation;
  duration_in_ms measured_rtt = time_now - timeSent;
  double rtt = measured_rtt.count();

  if (m_options.keepStats) {
    m_rttrtoHistory.push_back(std::make_pair(segno, std::make_pair(rtt, rto)));
  }

  if (time_passed.count() > m_sRtt) { // take one sample per RTT
    m_rttVar = (1-m_options.beta) * m_rttVar + m_options.beta * std::abs(m_sRtt-rtt);
    m_sRtt = (1-m_options.alpha) * m_sRtt + m_options.alpha * rtt;

    m_rto = m_sRtt + m_options.k * m_rttVar;
    if (m_rto > m_options.maxRto) m_rto = m_options.maxRto;

    m_mostRecentRttEstimation = time::steady_clock::now(); // update time

    if (m_options.keepStats) {
      std::vector<double> rtt_data;
      rtt_data.push_back(m_rttVar);
      rtt_data.push_back(m_sRtt);
      rtt_data.push_back(m_rto);
      m_rttrtoSamples.push_back(std::make_pair(segno, rtt_data));
    }
  }
}

double
RttEstimator::estimatedRto()
{
  return m_rto;
}

void
RttEstimator::rtoBackoff()
{
  m_rto = std::min(m_options.maxRto,
                   m_rto * m_options.rtoBackoffMultiplier);
}

void
RttEstimator::writeStats()
{
  std::ofstream fs_rttrto("rttrto.txt");

  // header
  fs_rttrto << "segno\t";
  fs_rttrto << "rtt\t";
  fs_rttrto << "rto\n";

  for (size_t i = 0; i < m_rttrtoHistory.size(); ++i) {
    fs_rttrto << m_rttrtoHistory[i].first;
    fs_rttrto << '\t';
    fs_rttrto << m_rttrtoHistory[i].second.first;
    fs_rttrto << '\t';
    fs_rttrto << m_rttrtoHistory[i].second.second;
    fs_rttrto << '\n';
  }

  std::ofstream fs_rttrtoSamples("rttrtoSamples.txt");

  // header
  fs_rttrtoSamples << "segno\t";
  fs_rttrtoSamples << "rttvar\t";
  fs_rttrtoSamples << "srtt\t";
  fs_rttrtoSamples << "rto\n";

  for (size_t i = 0; i < m_rttrtoSamples.size(); ++i) {
    fs_rttrtoSamples << m_rttrtoSamples[i].first;
    fs_rttrtoSamples << '\t';
    fs_rttrtoSamples << m_rttrtoSamples[i].second[0];
    fs_rttrtoSamples << '\t';
    fs_rttrtoSamples << m_rttrtoSamples[i].second[1];
    fs_rttrtoSamples << '\t';
    fs_rttrtoSamples << m_rttrtoSamples[i].second[2];
    fs_rttrtoSamples << '\n';
  }
}

} // namespace chunks
} // namespace ndn
