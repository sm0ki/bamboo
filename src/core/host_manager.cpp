#include "host_manager.hpp"
#include "helpers.hpp"
#include "api.hpp"
#include "constants.hpp"
#include "logger.hpp"
#include "header_chain.hpp"
#include "../external/http.hpp"
#include <iostream>
#include <thread>
#include <mutex>
#include <future>
#include <cstdio>
using namespace std;

#define ADD_PEER_BRANCH_FACTOR 10
#define HEADER_VALIDATION_HOST_COUNT 8
#define RANDOM_GOOD_HOST_COUNT 9
#define HOST_MIN_FRESHNESS 180 * 60 // 3 hours

/*
    Fetches the public IP of the node
*/

bool isValidIPv4(string & ip) {
   unsigned int a,b,c,d;
   return sscanf(ip.c_str(),"%d.%d.%d.%d", &a, &b, &c, &d) == 4;
}

string HostManager::computeAddress() {
    if (this->ip == "") {
        bool found = false;
        vector<string> lookupServices = { "checkip.amazonaws.com", "icanhazip.com", "ifconfig.co", "wtfismyip.com/text", "ifconfig.io" };

        for(auto& lookupService : lookupServices) {
            string cmd = "curl -s4 " + lookupService;
            string rawUrl = exec(cmd.c_str());
            string ip = rawUrl.substr(0, rawUrl.size() - 1);
            if (isValidIPv4(ip)) {
                this->address = "http://" + ip  + ":" + to_string(this->port);
                found = true;
                break;
            }
        }

        if (!found) {
            Logger::logError("IP discovery", "Could not determine current IP address");
        }
    } else {
        this->address = this->ip + ":" + to_string(this->port);
    }
    return this->address;
}

/*
    This thread periodically updates all neighboring hosts with the 
    nodes current IP 
*/  
void peer_sync(HostManager& hm) {
    while(true) {
        for(auto host : hm.hosts) {
            try {
                pingPeer(host, hm.computeAddress(), std::time(0), hm.version);
            } catch (...) { }
        }
        std::this_thread::sleep_for(std::chrono::minutes(5));
    }
}

HostManager::HostManager(json config) {
    this->name = config["name"];
    this->port = config["port"];
    this->ip = config["ip"];
    this->version = BUILD_VERSION;
    this->computeAddress();

    // check if a blacklist file exists
    std::ifstream blacklist("blacklist.txt");
    if (blacklist.good()) {
        std::string line;
        while (std::getline(blacklist, line)) {
            if (line[0] != '#') {
                string blocked = line;
                if (blocked[blocked.size() - 1] == '/') {
                    blocked = blocked.substr(0, blocked.size() - 1);
                }
                this->blacklist.insert(blocked);
                Logger::logStatus("Ignoring host " + blocked);
            }
        }
    }

    this->disabled = false;
    for(auto h : config["hostSources"]) {
        this->hostSources.push_back(h);
    }
    if (this->hostSources.size() == 0) {
        string localhost = "http://localhost:3000";
        this->hosts.push_back(localhost);
        this->hostPingTimes[localhost] = std::time(0);
        this->peerClockDeltas[localhost] = 0;
    } else {
        this->refreshHostList();
    }
    
}

void HostManager::startPingingPeers() {
    if (this->syncThread.size() > 0) throw std::runtime_error("Peer ping thread exists.");
    this->syncThread.push_back(std::thread(peer_sync, ref(*this)));
}

string HostManager::getAddress() {
    return this->address;
}

// Only used for tests
HostManager::HostManager() {
    this->disabled = true;
}

uint64_t HostManager::getNetworkTimestamp() {
    // find deltas of all hosts that pinged recently
    vector<int32_t> deltas;
    for (auto pair : this->hostPingTimes) {
        uint64_t lastPingAge = std::time(0) - pair.second;
        // only use peers that have pinged in the last hour
        if (lastPingAge < HOST_MIN_FRESHNESS) { 
            deltas.push_back(this->peerClockDeltas[pair.first]);
        }
    }
    
    if (deltas.size() == 0) return std::time(0);

    std::sort(deltas.begin(), deltas.end());
    
    // compute median
    uint64_t medianTime;
    if (deltas.size() % 2 == 0) {
        int32_t avg = (deltas[deltas.size()/2] + deltas[deltas.size()/2 - 1])/2;
        medianTime = std::time(0) + avg;
    } else {
        int32_t delta = deltas[deltas.size()/2];
        medianTime = std::time(0) + delta;
    }

    return medianTime;
}   



/*
    Asks nodes for their current POW and chooses the best
*/
string HostManager::getGoodHost() {
    Bigint bestWork = 0;
    string bestHost = this->currPeers[0]->getHost();
    this->lock.lock();
    for(auto h : this->currPeers) {
        if (h->getTotalWork() > bestWork) {
            bestWork = h->getTotalWork();
            bestHost = h->getHost();
        }
    }
    this->lock.unlock();
    return bestHost;
}


/*
    Returns N unique random hosts that have pinged us
*/
set<string> HostManager::sampleFreshHosts(int count) {
    vector<string> freshHosts;
    for (auto pair : this->hostPingTimes) {
        uint64_t lastPingAge = std::time(0) - pair.second;
        // only return peers that have pinged
        if (lastPingAge < HOST_MIN_FRESHNESS) { 
            freshHosts.push_back(pair.first);
        }
    }

    int numToPick = min(count, (int)freshHosts.size());
    set<string> sampledHosts;
    while(sampledHosts.size() < numToPick) {
        string host = freshHosts[rand()%freshHosts.size()];
        sampledHosts.insert(host);
    }
    return sampledHosts;
}


/*
    Returns any N unique random hosts. 
    Used when hosts don't ping us (Miner)
*/
set<string> HostManager::sampleAllHosts(int count) {
    int numToPick = min(count, (int)this->hosts.size());
    set<string> sampledHosts;
    while(sampledHosts.size() < numToPick) {
        string host = this->hosts[rand()%this->hosts.size()];
        sampledHosts.insert(host);
    }
    return sampledHosts;
}


/*
    Adds a peer to the host list, 
*/
void HostManager::addPeer(string addr, uint64_t time, string version) {
    if (version != this->version) return;

    // check if host is in blacklist
    if (this->blacklist.find(addr) != this->blacklist.end()) return;

    // check if we already have this peer host
    auto existing = std::find(this->hosts.begin(), this->hosts.end(), addr);
    if (existing != this->hosts.end()) {
        this->hostPingTimes[addr] = std::time(0);
        // record how much our system clock differs from theirs:
        this->peerClockDeltas[addr] = std::time(0) - time;
        return;
    } 

    // check if the host is reachable:
    try {
        string name = getName(addr);
    } catch(...) {
        // if not exit
        return;
    }
    
    // add to our host list
    this->hosts.push_back(addr);
    // pick random neighbor hosts and forward the addPeer request to them:
    set<string> neighbors = this->sampleFreshHosts(ADD_PEER_BRANCH_FACTOR);
    vector<future<void>> reqs;
    string _version = this->version;
    for(auto neighbor : neighbors) {
        reqs.push_back(std::async([neighbor, addr, _version](){
            if (neighbor == addr) return;
            try {
                pingPeer(neighbor, addr, std::time(0), _version);
            } catch(...) {
                Logger::logStatus("Could not add peer " + addr + " to " + neighbor);
            }
        }));
    }

    for(int i =0 ; i < reqs.size(); i++) {
        reqs[i].get();
    }   
}


bool HostManager::isDisabled() {
    return this->disabled;
}


/*
    Downloads an initial list of peers and validates connectivity to them
*/
void HostManager::refreshHostList() {
    if (this->hostSources.size() == 0) return;
    
    Logger::logStatus("Finding peers...");

    set<string> fullHostList;

    // Iterate through all host sources merging into a combined peer list
    for (int i = 0; i < this->hostSources.size(); i++) {
        try {
            string hostUrl = this->hostSources[i];
            http::Request request{hostUrl};
            const auto response = request.send("GET","",{},std::chrono::milliseconds{TIMEOUT_MS});
            json hostList = json::parse(std::string{response.body.begin(), response.body.end()});
            for(auto host : hostList) {
                fullHostList.insert(string(host));
            }
        } catch (...) {
            continue;
        }
    }

    if (fullHostList.size() == 0) return;

    // iterate through all listed peer hosts
    vector<std::thread> threads;
    std::mutex lock;
    for(auto hostJson : fullHostList) {
        // if we've already added this host skip
        string hostUrl = string(hostJson);
        auto existing = std::find(this->hosts.begin(), this->hosts.end(), hostUrl);
        if (existing != this->hosts.end()) continue;

        // if host is in blacklist skip:
        if (this->blacklist.find(hostUrl) != this->blacklist.end()) continue;

        // otherwise try connecting to the host to confirm it's up
        HostManager & hm = *this;
        threads.emplace_back(
            std::thread([hostUrl, &hm, &lock](){
                try {
                    string hostName = getName(hostUrl);
                    lock.lock();
                    hm.hosts.push_back(hostUrl);
                    hm.hostPingTimes[hostUrl] = std::time(0);
                    lock.unlock();
                    Logger::logStatus(GREEN + "[ CONNECTED ] " + RESET  + hostUrl);
                } catch (...) {
                    Logger::logStatus(RED + "[ UNREACHABLE ] " + RESET  + hostUrl);
                }
            })
        );
    }
    for (auto& th : threads) th.join();

    this->syncHeadersWithPeers();
}

void HostManager::syncHeadersWithPeers() {
    // free existing peers
    this->lock.lock();
    for(auto peer : this->currPeers) {
        delete peer;
    }
    this->currPeers.empty();
    
    set<string> hosts = this->sampleAllHosts(RANDOM_GOOD_HOST_COUNT);

    for (auto h : hosts) {
        this->currPeers.push_back(new HeaderChain(h));
    }
    this->lock.unlock();
}


/*
    Returns a list of all peer hosts
*/
vector<string> HostManager::getHosts(bool includeSelf) {
    vector<string> ret;
    for (auto pair : this->hostPingTimes) {
        uint64_t lastPingDelta = std::time(0) - pair.second;
        // only return peers that have pinged in the last hour
        if (lastPingDelta < HOST_MIN_FRESHNESS) { 
            ret.push_back(pair.first);
        }
    }
    if (includeSelf) ret.push_back(this->address);
    return ret;
}

size_t HostManager::size() {
    return this->hosts.size();
}

/*
    Returns a random host
*/
std::pair<string,uint64_t> HostManager::getRandomHost() {
    set<string> hosts = this->sampleAllHosts(1);
    if (hosts.size() == 0) {
        return std::pair<string, uint64_t>("", 0);
    }
    string host = *hosts.begin();
    uint64_t chainLength = getCurrentBlockCount(host);
    return std::pair<string, uint64_t>(host, chainLength);
}