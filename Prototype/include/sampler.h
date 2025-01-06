#ifndef SAMPLER_H
#define SAMPLER_H

#include "node.h"
//#include "skiplist.h" 
#include "hotLog.h"
#include "valuelist.h"


class Sampler {
public:
    Sampler(size_t sampling_interval, size_t window_size);

    // Perform sampling and update node statuses
    void performSampling(ValueList &valueList); //dereference valueList?
    void calculateAccessRatios(ValueList &valueList);
    void markHotNodes(ValueList &valueList, const std::vector<double>& access_ratios, double max_ratio);
    void resetSampling(ValueList &valueList);
    void logHot(uint32_t node_id, Val_t value);
    void processHotNode(uint32_t node_id);

    // extrapolate
    std::vector<std::pair<Val_t, size_t>> predictFutureInserts(const uint32_t& node_id);

private:
    size_t _sampling_interval; // Interval at which sampling is triggered
    size_t _current_iteration; // Tracks iterations for periodic sampling
    //log
    HotLog _hot_log;
};

#endif // SAMPLER_H
