setwd('/Users/shuoyang/vivid64/')
library(ggplot2)

cwnd.ts = read.csv('cwnd.txt', header=TRUE, sep='\t')
cwnd.ts$time = cwnd.ts$time * 1000
png('cwnd.png')
# Basic line plot with points
ggplot(data=cwnd.ts, aes(x=time, y=cwndsize, group=1)) +
  xlab('time(ms)') + ylab('cwnd size(segment)') +
  ggtitle('AIMD') + 
  geom_line() +
  scale_y_continuous(breaks=seq(0, 100, 10))
  #scale_x_continuous(breaks=seq(0, 35000, 5000))
#geom_point()
dev.off()

rttrto.ts = read.csv('rttrto.txt', header=TRUE, sep='\t')
rttrtoest.ts = read.csv('rttrtoSamples.txt', header=TRUE, sep='\t')
timeout.ts = read.csv('timeout.txt', header=TRUE, sep='\t')

#png('timeout.png')
#ggplot(data=timeout.ts, aes(x=time, y=number)) +
#  geom_point() + ylab('# of timed out packets') + 
#  xlab('time(ms)') + ylab('number of timeouts')
  #scale_x_continuous(breaks=seq(0, 35000, 5000))
#dev.off()

png('rttrto.png')
ggplot(data=rttrto.ts) +
  geom_line(aes(x=segno, y=rtt, color='rtt')) +
  geom_line(aes(x=segno, y=rto, color='rto')) +
  scale_color_manual(values=c('rtt'='red','rto'='green')) +
  xlab('segment #') + ylab('rtt/rto (ms)') + 
  scale_y_continuous(breaks=seq(0, 1000, 100))
  #scale_x_continuous(breaks=seq(0, 35000, 5000))
dev.off()

png('rttrtoSample.png')
ggplot(data=rttrtoest.ts) +
  geom_line(aes(x=segno, y=rttvar, color='rttvar')) +
  geom_line(aes(x=segno, y=srtt, color='srtt')) +
  geom_line(aes(x=segno, y=rto, color='rto')) +
  scale_color_manual(values=c('rttvar'='blue','srtt'='red','rto'='green')) +
  xlab('segment #') + ylab('rttvar/srtt/rto (ms)') + 
  scale_y_continuous(breaks=seq(0, 200, 50))
#scale_x_continuous(breaks=seq(0, 35000, 5000))
dev.off()

