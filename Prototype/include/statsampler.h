#ifndef SAMPLING_H
#define SAMPLING_H

#include <unordered_map>
#include <mutex>
#include <deque>
#include <vector>
#include <iostream>
#include <limits>
#include <cmath>

// TODO: Forward declaration of Key_t (replace with actual type)
typedef uint64_t Key_t;

constexpr size_t window_size = 5;

struct SamplingEntry {
    uint32_t nodeId;
    bool isHot;
    int totalAccess;
    std::deque<Key_t> log;
    std::mutex lock;

    SamplingEntry() : nodeId(0), isHot(false), totalAccess(0) {}
    SamplingEntry(uint32_t id): nodeId(id), isHot(false), totalAccess(0) {}

    void logInsert(Key_t key);
    void clearLog();
    void incTotalAccess();
    int getTotalAccess();
    void resetTotalAccess();
    void resetSample();
};

// Linear regression model over insert log
inline std::pair<double, double> regressionModel(const std::deque<Key_t>& log) {
    if (log.empty()) return {0.0, 0.0};

    size_t logSize = log.size();
    double meanX = static_cast<double>(logSize - 1) / 2.0;
    double sumY = 0.0, numerator = 0.0, denominator = 0.0;

    for (size_t i = 0; i < logSize; ++i) {
        double centeredX = static_cast<double>(i) - meanX;
        double valueY = static_cast<double>(log[i]);
        sumY += valueY;
        numerator += centeredX * valueY;
        denominator += centeredX * centeredX;
    }

    double meanY = sumY / logSize;
    double slope = (denominator == 0.0) ? 0.0 : numerator / denominator;
    double intercept = meanY - slope * meanX;

    return {slope, intercept};
}

inline std::vector<Key_t> linearPrediction(const std::deque<Key_t>& log) {
    if (log.empty()) return {};
    auto [slope, intercept] = regressionModel(log);

    size_t lastIndex = log.size() - 1;
    size_t numPredictions = log.size() / 2;

    std::vector<Key_t> predictions;
    predictions.reserve(numPredictions);
    for (size_t i = 1; i <= numPredictions; ++i) {
        predictions.push_back(slope * (lastIndex + i) + intercept);
    }

    return predictions;
}

// SamplingEntry Methods
inline void SamplingEntry::logInsert(Key_t key) {
    log.push_back(key);
    //std::cout << "log size: " << log.size() << std::endl;

    if (log.size() >= ::window_size) {
        auto pred = linearPrediction(log);

        //std::cout << "Logged values for node: " << nodeId << ": ";
        //for (const auto& val : log) std::cout << val << " ";
        //std::cout << "\nPredicted values: ";
        //for (const auto& val : pred) std::cout << val << " ";
        //std::cout << std::endl;

        log.clear();
        isHot = false;
    }
}

inline void SamplingEntry::clearLog() {
    //std::lock_guard<std::mutex> guard(lock);
    log.clear();
}

inline void SamplingEntry::incTotalAccess() {
    //std::lock_guard<std::mutex> guard(lock);
    totalAccess++;
}

inline int SamplingEntry::getTotalAccess() {
    //std::lock_guard<std::mutex> guard(lock);
    return totalAccess;
}

inline void SamplingEntry::resetTotalAccess() {
    //std::lock_guard<std::mutex> guard(lock);
    totalAccess = 0;
}

inline void SamplingEntry::resetSample() {
    resetTotalAccess();
}

class SamplingTable {
private:
    std::unordered_map<uint32_t, SamplingEntry> table;
    static constexpr int HOTSPOT_R = 1;
    static constexpr size_t window_size = 10;
    size_t sampling_interval = 100000;
    size_t total_request = 0;
    size_t sampled_inserts = 0;
    std::mutex tableLock;

public:
    SamplingEntry& getEntry(uint32_t nodeId);
    void sampleInsert(uint32_t nodeId, Key_t key);
    void logInsert(uint32_t nodeId, Key_t key);
    bool isHotNode(uint32_t nodeId);
    void setHotNode(uint32_t nodeId);
    void resetHotNode(uint32_t nodeId);
    void updateHotNodes();
    void resetSampling();
    void performSampling();
    void calculateAccessRatios();
};

inline SamplingEntry& SamplingTable::getEntry(uint32_t nodeId) {
    if (table.find(nodeId) == table.end()) {
        table.emplace(std::piecewise_construct,
                      std::forward_as_tuple(nodeId),
                      std::forward_as_tuple(nodeId));
    }
    return table[nodeId];
}

inline void SamplingTable::sampleInsert(uint32_t nodeId, Key_t key) {
    total_request++;
    SamplingEntry& entry = getEntry(nodeId);
    if (entry.isHot) entry.logInsert(key);

    if (total_request % HOTSPOT_R == 0) {
        sampled_inserts++;
        entry.incTotalAccess();
    }
    performSampling();
}

inline void SamplingTable::logInsert(uint32_t nodeId, Key_t key) {
    SamplingEntry& entry = getEntry(nodeId);
    entry.logInsert(key);
}

inline bool SamplingTable::isHotNode(uint32_t nodeId) {
    return getEntry(nodeId).isHot;
}

inline void SamplingTable::setHotNode(uint32_t nodeId) {
    SamplingEntry& entry = getEntry(nodeId);
    entry.isHot = true;
    //std::cout << "set hot node: " << nodeId << std::endl;
}

inline void SamplingTable::resetHotNode(uint32_t nodeId) {
    SamplingEntry& entry = getEntry(nodeId);
    entry.isHot = false;
}

inline void SamplingTable::updateHotNodes() {
    //std::lock_guard<std::mutex> guard(tableLock);
    for (auto& [nodeId, entry] : table) {
        entry.isHot = (entry.totalAccess > 10);
        if (!entry.isHot) entry.clearLog();
    }
}

inline void SamplingTable::resetSampling() {
    for (auto& [nodeId, entry] : table) {
        entry.resetSample();
    }
    total_request = 0;
}

inline void SamplingTable::performSampling() {
    if (total_request % sampling_interval == 0) {
        calculateAccessRatios();
        resetSampling();
    }
}

inline void SamplingTable::calculateAccessRatios() {
    if (total_request == 0) return;

    uint32_t hot_node = UINT32_MAX;
    double max_ratio = 0.0;

    for (auto& [nodeId, entry] : table) {
        double ratio = static_cast<double>(entry.totalAccess) / static_cast<double>(sampled_inserts);
        if (ratio > max_ratio) {
            max_ratio = ratio;
            hot_node = nodeId;
        }
    }

    //std::cout << "total_request: " << total_request << " sampled: " << sampled_inserts << std::endl;

    if (hot_node != UINT32_MAX) setHotNode(hot_node);

    for (auto& [nodeId, entry] : table) {
        if (nodeId != hot_node && entry.isHot) {
            resetHotNode(nodeId);
            entry.clearLog();
        }
    }
    sampled_inserts = 0;
}

inline SamplingTable samplingTable;

#endif // SAMPLING_H