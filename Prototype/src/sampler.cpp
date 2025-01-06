#include "sampler.h"

#include <algorithm>


//TODO: check the rest of insert function in tandemindex.cpp - how to sample rebalance and new vnode insert

Sampler::Sampler(size_t sampling_interval, size_t window_size)
    : _hot_log(window_size), _sampling_interval(sampling_interval), _current_iteration(0) {}

void Sampler::performSampling(ValueList &valueList) {
    // Check if it's time to perform sampling
    if (++_current_iteration % _sampling_interval == 0) { //check condition
        calculateAccessRatios(valueList); 
        resetSampling(valueList); 
        //TODO: set sampling interval + call performSampling [periodic in the background?]
    }
}

void Sampler::calculateAccessRatios(ValueList &valueList) {
    std::vector<double> access_ratios;
    double max_ratio = 0.0; //TODO: replace with median later

    Vnode *startNode = valueList.getHeader();

    for (auto* node = startNode; node != nullptr; node = valueList.getNext(node)) {
        double ratio = static_cast<double>(node->hdr.getTotalAccess()) / valueList.getTotalRequests();
        access_ratios.push_back(ratio);

        // Track the maximum ratio
        if (ratio > max_ratio) {
            max_ratio = ratio;
        }
    }

    markHotNodes(valueList, access_ratios, max_ratio);
}

void Sampler::markHotNodes(ValueList &valueList, const std::vector<double>& access_ratios, double max_ratio) {

    Vnode *startNode = valueList.getHeader(); 
    size_t index = 0;

    for (auto* node = startNode; node != nullptr; node = valueList.getNext(node), ++index) {
        //std::cout << "access_ratio: " << access_ratios[index] << std::endl;
        //std::cout << "max: " << max_ratio << std::endl;

        if (access_ratios[index] == max_ratio) {
            node->hdr.setHot(); // Mark as hot
            std::cout << "Node "<< node->hdr.id << "set as hot with access_ratio: " << access_ratios[index] << std::endl;
        } else {
            node->hdr.setCold(); // Reset others
        }
    }
}

void Sampler::resetSampling(ValueList &valueList) {
    // TODO: add decay
    //std::cout << "Resetting sampling for all nodes..." << std::endl;
    //std::cout << "Total requests before reset: " << valueList.getTotalRequests() << std::endl;

    // Reset global request counter
    valueList.resetTotalRequests();

    // Iterate and reset each node
    Vnode *startNode = valueList.getHeader();
    for (auto* node = startNode; node != nullptr; node = valueList.getNext(node)) {
        //std::cout << "Resetting node with access count: " << node->hdr.getTotalAccess() << std::endl;
        //node->hdr.resetSample(); keep the node hot for now
        node->hdr.resetTotalAccess();
    }
    //std::cout << "Sampling reset completed." << std::endl;
}

void Sampler::logHot(uint32_t node_id, Val_t value) {
    // Log the next accesses to hot node
   //std::cout << "log the value" << value << std::endl; 
    _hot_log.logValue(node_id, value);
    
    /*for (const auto& val : _hot_log.getLogs(node_id)) {
        std::cout << "Logged value: " << val << std::endl;
    }*/

    //interpolate the next access [speculate + SGP]
    //TODO: verify when to reset _hot_log - maybe wehn we call set cold?
}

void Sampler::processHotNode(uint32_t node_id) {
    // hasLog not implemented yet 
    /*
    if (!_hot_log.hasLog(node_id)) {
            return;
        }
    */

   const auto& log = _hot_log.getLogs(node_id);
   //get logs generates a histogram of the values for that node [why did i comment this in hotLog.h?]
   auto pred = _hot_log.linearPrediction(log);

   // for now i will output the predictions
   //TODO: activate SGP based on prediction
   std::cout << "Predicted values for node " << node_id << ": ";
    for (const auto& val : pred) {
         std::cout << val << " ";
    }
}



//iterate through nodes in the value list
//calculate access ratio
//mark nodes with the highest ratio as hot 
//reset access metadata periodically 



// optimization - can also calculate the access ratio and update nodes when a new insert is done 
// have to maintain the average and sliding window logic