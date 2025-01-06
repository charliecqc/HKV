#ifndef HOTLOG_H
#define HOTLOG_H

#include <deque>
#include <unordered_map>
#include <string>
#include <vector>
#include <cmath>
#include <utility>
#include <numeric>
#pragma once

class HotLog {
private:
    size_t _window_size; //maximum size of sliding window
    //TODO: does it have to be a map - I am manahing only one hot node currently
    //add logic to set the width of the map to the number of hot nodes?
    std::unordered_map<uint32_t, std::deque<Val_t>> _node_logs; 

public:
    HotLog(size_t window_size) : _window_size(window_size) {}

    void logValue(uint32_t node_id, Val_t value) {
        auto& log = _node_logs[node_id];
        // TODO: what if the node is not in the map?
        if (log.size() == _window_size) {
            log.pop_front(); // remove the old value
            //TODO: when we start the sliding window may be empty - with fix size 
            //dont need to pop the first element
        }
        log.push_back(value);

        if (log.size() == _window_size) {
            auto pred = linearPrediction(log);

            std::cout << "logged values for node: " << node_id << ": ";
            for (const auto& val : log) {
                std::cout << val << " ";
            }
            std::cout << "Predicted values for node " << node_id << ": ";
            for (const auto& val : pred) {
            std::cout << val << " ";
            }
            clearAllLogs();
        }


    }

    std::deque<Val_t> getLogs(const uint32_t node_id) const {
        auto it = _node_logs.find(node_id);
        if (it == _node_logs.end()) {
            return {};
        }
        return it->second;
    } // used for extrapolation

    void clearLogs(const uint32_t& node_id) {
        _node_logs[node_id].clear();
    }

    void clearAllLogs() {
        _node_logs.clear();
    }

    //interpolation
    std::pair<double, double> regressionModel (const std::deque<Val_t>& log) {
        size_t n = log.size();
        if (n < 2) {
            return {0.0, log.empty() ? 0.0 : log.front()}; //not enough data
        }
        // means of x and y
        double mean_x = (n-1) / 2; //indices 0 to n-1
        double mean_y = std::accumulate(log.begin(), log.end(), 0) / n;

        //slope
        double numerator = 0.0, denominator = 0.0;
        for (size_t i = 0; i < n; ++i) {
            double x = i - mean_x;
            numerator += x * (log[i] - mean_y);
            denominator += x * x;
        }

        double slope = numerator / denominator;
        double intercept = mean_y - slope * mean_x;

        return {slope, intercept};

        //std::vector
        //what is this?
    }

    std::vector<Val_t> linearPrediction(const std::deque<Val_t>& log) {
        auto [slope, intercept] = regressionModel(log);
        
        std::vector<Val_t> pred;
        size_t last_index = log.size() - 1;
        size_t num_pred = log.size()/2; //predict half
        //TODO: review
        for (size_t i=1; i <= num_pred; ++i) {
            pred.push_back(slope * (last_index + i) + intercept);
        }
        return pred;
    }
};

#endif // HOTLOG_H