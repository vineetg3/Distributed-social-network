#include <ctime>

#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/duration.pb.h>
#include <chrono>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <stdlib.h>
#include <unistd.h>
#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>
#include <glog/logging.h>     // for LOG
#include <glog/raw_logging.h> // for RAW_LOG
#include <stack>

#include "coordinator.grpc.pb.h"

using csce438::Confirmation;
using csce438::CoordService;
using csce438::ID;
using csce438::Path;
using csce438::PathAndData;
using csce438::ReplyStatus;
using csce438::ServerInfo;
using csce438::ServerList;
using google::protobuf::Duration;
using google::protobuf::Timestamp;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
using namespace std;
// using csce438::ServerList;
// using csce438::SynchService;//used for Mp2.2

#define log(severity, msg) \
  LOG(severity) << msg;    \
  google::FlushLogFiles(google::severity);

struct zNode
{
  int serverID;
  std::string hostname;
  std::string port;
  std::string type;
  std::time_t last_heartbeat;
  bool missed_heartbeat;
  bool isActive();
};

// potentially thread safe
std::mutex v_mutex;
std::vector<zNode *> cluster1;
std::vector<zNode *> cluster2;
std::vector<zNode *> cluster3;
std::vector<zNode *> sync_servers(4, NULL);
std::unordered_map<std::string, int> paths1; // for cluster 1
std::unordered_map<std::string, int> paths2; // for cluster 2
std::unordered_map<std::string, int> paths3; // for cluster 3
stack<int> lastMasterServer1;
stack<int> lastMasterServer2;
stack<int> lastMasterServer3;

// func declarations
int findServer(std::vector<zNode *> v, int id);
std::time_t getTimeNow();
void checkHeartbeat();
string znodeToString(zNode *z);
bool eitherCreateOrHB = true;

std::vector<zNode *> &getCluster(int idx)
{
  switch (idx)
  {
  case 1:
  {
    return cluster1;
    break;
  }
  case 2:
  {
    return cluster2;
    break;
  }
  case 3:
  {
    return cluster3;
    break;
  }
  }
  return cluster1;
}

std::unordered_map<std::string, int> &getPath(int idx)
{
  switch (idx)
  {
  case 1:
  {
    return paths1;
    break;
  }
  case 2:
  {
    return paths2;
    break;
  }
  case 3:
  {
    return paths3;
    break;
  }
  }
  return paths1;
}

int getLastMasterServer(int idx)
{
  switch (idx)
  {
  case 1:
  {
    return lastMasterServer1.empty() ? -1 : lastMasterServer1.top();
    break;
  }
  case 2:
  {
    return lastMasterServer2.empty() ? -1 : lastMasterServer2.top();
    break;
  }
  case 3:
  {
    return lastMasterServer3.empty() ? -1 : lastMasterServer3.top();
    break;
  }
  }
  return 1;
}

void pushMasterServer(int idx, int masterID)
{
  switch (idx)
  {
  case 1:
  {
    return lastMasterServer1.push(masterID);
    break;
  }
  case 2:
  {
    return lastMasterServer2.push(masterID);
    break;
  }
  case 3:
  {
    return lastMasterServer3.push(masterID);
    break;
  }
  }
}

zNode *getZNode(int server_id, int cluster_id)
{
  std::vector<zNode *> &cluster = getCluster(cluster_id);
  for (int i = 0; i < cluster.size(); i++)
  {
    if (cluster[i]->serverID == server_id)
      return cluster[i];
  }
  return NULL;
}

string generateMasterPath(int cluster_idx)
{
  return "ls/cluster" + to_string(cluster_idx) + "/" + "master";
}

string generateSlavePath(int cluster_idx)
{
  return "ls/cluster" + to_string(cluster_idx) + "/" + "slave";
}

bool zNode::isActive()
{
  bool status = false;
  if (!missed_heartbeat)
  {
    status = true;
  }
  else if (difftime(getTimeNow(), last_heartbeat) < 10)
  {
    status = true;
  }
  return status;
}

class CoordServiceImpl final : public CoordService::Service
{

  grpc::Status Heartbeat(ServerContext *context, const ServerInfo *serverinfo, Confirmation *confirmation) override
  {
    std::cout << "Got Heartbeat! " << serverinfo->type() << "(" << serverinfo->serverid() << ")" << std::endl;

    // Your code here
    // Heartbeat only happens after create path. So path is guarenteed to be present.

    int server_id = serverinfo->serverid();
    int cluster_id = serverinfo->clusterid();
    zNode *node = getZNode(server_id, cluster_id);
    node->last_heartbeat = getTimeNow();
    node->missed_heartbeat = false;
    cout << getTimeNow() << "INFO: "
         << "Heartbeat from server_id " << to_string(server_id) << " cluster id " << to_string(cluster_id) << endl;
    cout << "INFO: cluster sizes" << cluster1.size() << " " << cluster2.size() << " " << cluster3.size() << endl;
    string msterPth = generateMasterPath(cluster_id);
    string slavePth = generateSlavePath(cluster_id);
    std::vector<zNode *> cluster = getCluster(cluster_id);
    std::unordered_map<std::string, int> path = getPath(cluster_id);
    if (path.find(msterPth) != path.end())
    {
      if (cluster[path.at(msterPth)]->serverID == server_id)
      {
        confirmation->set_role("master");
        if (path.find(slavePth) != path.end())
        {
          // slave exists, forward details to master
          zNode *zn = cluster[path.at(slavePth)];
          confirmation->set_data(zn->hostname + ":" + zn->port);
        }
      }
      else
      {
        // current HB is not the master
        confirmation->set_role("slave");
      }
    }
    return grpc::Status::OK;
  }

  // function returns the server information for requested client id
  // this function assumes there are always 3 clusters and has math
  // hardcoded to represent this.
  grpc::Status GetServer(ServerContext *context, const ID *id, ServerInfo *serverinfo) override
  {
    std::cout << "Got GetServer for clientID: " << id->id() << std::endl;
    int clusterID = ((id->id() - 1) % 3) + 1;

    // Your code here
    // If server is active, return serverinfo
    unordered_map<string, int> &path = getPath(clusterID);
    std::vector<zNode *> &cluster = getCluster(clusterID);
    auto it = path.find(generateMasterPath(clusterID));
    int serverIdx = 0;
    if (it != path.end())
    {
      // master exists
      serverIdx = it->second; // get index of server in cluster vector
      zNode *zn = cluster[serverIdx];
      serverinfo->set_serverid(zn->serverID);
      serverinfo->set_clusterid(clusterID);
      serverinfo->set_hostname(zn->hostname);
      serverinfo->set_port(zn->port);
      serverinfo->set_type(zn->type);
    }
    else
    {
      // master isn't available
      serverinfo->set_serverid(-1);
    }
    return grpc::Status::OK;
  }

  grpc::Status Create(ServerContext *context, const PathAndData *pdata, Confirmation *status) override
  {
    v_mutex.lock();
    zNode *newZNode = new zNode;
    newZNode->serverID = pdata->serverid();
    newZNode->hostname = pdata->hostname();
    newZNode->port = pdata->port();
    newZNode->missed_heartbeat = false;
    newZNode->last_heartbeat = getTimeNow();
    if (pdata->path() == "/master")
    {
      // cout << "Creating Master Path for: server id,cluster id " << pdata->serverid() << " " << pdata->clusterid() << endl;
      // current server is contending for master

      unordered_map<string, int> &path = getPath(pdata->clusterid());
      string master_path = generateMasterPath(pdata->clusterid());
      std::vector<zNode *> &cluster = getCluster(pdata->clusterid());

      // send last master id
      status->set_data(to_string(getLastMasterServer(pdata->clusterid())));

      // We need to check if master is already registered and if the master is active.
      if (path.find(master_path) != path.end())
      {
        // check if it is active
        if (cluster[path[master_path]]->isActive())
        {
          // the current Create request for master failed
          status->set_role("slave");
          cout << "Master is already active . Master is: " << to_string(cluster[path[master_path]]->serverID) << endl;
          // TODO: register as slave
          //  checking if server already is saved in cluster vector because of previous events.
          // Would ideally be present as registration to /servers is done first.
          int newIdx = -1;
          for (int i = 0; i < cluster.size(); i++)
          {
            if (cluster[i]->serverID == pdata->serverid())
            {
              newIdx = i;
              cluster[i] = newZNode; // should ideally free previous reference. But it's ok for the MP
            }
          }
          if (newIdx == -1)
          {
            cluster.push_back(newZNode);
            newIdx = cluster.size() - 1;
          }
          string slave_path = generateSlavePath(pdata->clusterid());
          path[slave_path] = newIdx;
          log(INFO, "Slave Path created: " << slave_path
                                           << " with serverID: " << cluster[newIdx]->serverID << " clusterID: " << pdata->clusterid() + " port: " << cluster[newIdx]->port);
          v_mutex.unlock();
          return grpc::Status::OK;
        }
      }

      // checking if server already is saved in cluster vector because of previous events.
      // Would ideally be present as registration to /servers is done first.
      int newIdx = -1;
      for (int i = 0; i < cluster.size(); i++)
      {
        if (cluster[i]->serverID == pdata->serverid())
        {
          newIdx = i;
          cluster[i] = newZNode; // should ideally free previous reference. But it's ok for the MP
        }
      }
      if (newIdx == -1)
      {
        cluster.push_back(newZNode);
        newIdx = cluster.size() - 1;
      }

      path[master_path] = newIdx; // registering new master. The previous master is dereferenced.
      pushMasterServer(pdata->clusterid(), pdata->serverid());
      log(INFO, "Master Path created: " << master_path << " with serverID: " << cluster[newIdx]->serverID << " clusterID: " << pdata->clusterid() + " port: " << cluster[newIdx]->port);
      // status->set_status(to_string(cluster[path[master_path]]->serverID));
      status->set_role("master");
    }
    v_mutex.unlock();
    return grpc::Status::OK;
  }

  // Path can be of /slave or /master. Only these two allowed
  grpc::Status Exists(ServerContext *context, const Path *req_path, ReplyStatus *status) override
  {
    unordered_map<string, int> &path = getPath(req_path->clusterid());
    std::vector<zNode *> &cluster = getCluster(req_path->clusterid());
    if (req_path->path() == "/master") // not really used
    {
      string master_path = generateMasterPath(req_path->clusterid());
      // We need to check if master is already registered and if the master is active.
      if (path.find(master_path) != path.end())
      {
        // check if it is active
        if (cluster[path[master_path]]->isActive())
        {
          // the current Create request for master failed
          status->set_status("Master id:" + to_string(path[master_path]));
          return grpc::Status::OK;
        }
      }
    }
    else if (req_path->path() == "/slave") // return slave details if it exists
    {
      string slave_path = generateMasterPath(req_path->clusterid());
      // TODO MP2.2
      if (path.find(slave_path) != path.end())
      {
        std::vector<zNode *> cluster = getCluster(req_path->clusterid());
        std::unordered_map<std::string, int> path = getPath(req_path->clusterid());
        // slave exists, forward details to master
        zNode *zn = cluster[path.at(slave_path)];
        status->set_status(zn->hostname + ":" + zn->port);
      }
    }
    return grpc::Status::OK;
  }

  grpc::Status RegFS(ServerContext *context, const ServerInfo *server_info, ReplyStatus *status) override
  {
    zNode *newZNode = new zNode;
    newZNode->hostname = server_info->hostname();
    newZNode->port = server_info->port();
    newZNode->missed_heartbeat = false;
    newZNode->last_heartbeat = getTimeNow();
    sync_servers[server_info->clusterid()] = newZNode;
    status->set_status("success");
    log(INFO, "Registered sync server " << newZNode->port << " " << server_info->clusterid());
    return grpc::Status::OK;
  }

  grpc::Status GetFS(ServerContext *context, const ID *id, ServerList *list) override
  {
    for (int i = 1; i <= 3; i++)
    {
      if (sync_servers[i] == NULL)
      {
        cout << "Sync server " << i << " is null." << endl;
        continue;
      }
      ServerInfo *si = list->add_servers();
      si->set_hostname(sync_servers[i]->hostname);
      si->set_port(sync_servers[i]->port);
      si->set_clusterid(i);
    }
    return grpc::Status::OK;
  }
};

void RunServer(std::string port_no)
{
  // start thread to check heartbeats
  std::thread hb(checkHeartbeat);
  // localhost = 127.0.0.1
  std::string server_address("127.0.0.1:" + port_no);
  CoordServiceImpl service;
  // grpc::EnableDefaultHealthCheckService(true);
  // grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  ServerBuilder builder;
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to an *synchronous* service.
  builder.RegisterService(&service);
  // Finally assemble the server.
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << std::endl;

  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();
}

int main(int argc, char **argv)
{

  std::string port = "3010";
  int opt = 0;
  while ((opt = getopt(argc, argv, "p:")) != -1)
  {
    switch (opt)
    {
    case 'p':
      port = optarg;
      break;
    default:
      std::cerr << "Invalid Command Line Argument\n";
    }
  }
  std::string log_file_name = std::string("coordinator");
  google::InitGoogleLogging(log_file_name.c_str());
  FLAGS_logtostderr = true;
  FLAGS_alsologtostderr = true;
  RunServer(port);
  return 0;
}

// invalidates node periodically.
void checkHeartbeat()
{
  while (true)
  {
    // log(INFO, "Heartbeat check");
    // cout << "hb check" << endl;
    // check servers for heartbeat > 10
    // if true turn missed heartbeat = true
    //  Your code below
    v_mutex.lock();
    for (int i = 1; i <= 3; i++)
    {
      std::vector<zNode *> &cluster = getCluster(i);
      // log(INFO, "size of cluster" << i << " " << cluster.size());
      for (int j = 0; j < cluster.size(); j++)
      {
        auto &s = cluster[j];
        if (difftime(getTimeNow(), s->last_heartbeat) > 10)
        {
          if (!s->missed_heartbeat)
          {
            log(INFO, "Hearbeat missed for zNode\n"
                          << znodeToString(s));
            // heartbeat has been missed with more than 10 seconds delay. so  make heartbeat missed
            s->missed_heartbeat = true;
            s->last_heartbeat = getTimeNow();
            std::unordered_map<std::string, int> &path = getPath(i);
            // delete path as heartbeat is missed. TODO: create seperate thread safe function for paths map
            string msterPth = generateMasterPath(i);
            string slavePth = generateSlavePath(i);
            if (path.at(msterPth) == j) // THIS IS IMPORTANT check. Master path might exist which doesnt point to dead server. lets not delete that
            {
              if (path.find(slavePth) != path.end())
              {
                // slave exists. assign slave as master
                path[msterPth] = path[slavePth];
                path.erase(slavePth);
                std::vector<zNode *> cluster = getCluster(i);
                pushMasterServer(i, cluster[path[msterPth]]->serverID);
                log(INFO, "Master erased. New master for cluster " << i << " is: " << cluster[path[msterPth]]->serverID);
              }
              else
              {
                log(INFO, "Master erased for cluster " << i);

                path.erase(msterPth); // soft deletion as current server is at master. For Mp2.2 delete registration path as well.
              }
            }
            // TODO MP2.2 remove path /servers
          }
          else
          {
          }
        }
      }
    }
    v_mutex.unlock();

    sleep(3);
  }
}

std::time_t getTimeNow()
{
  return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}

string znodeToString(zNode *zn)
{
  std::stringstream ss;
  ss << "zNode: serverID=" << zn->serverID
     << ", hostname=" << zn->hostname
     << ", port=" << zn->port
     << ", type=" << zn->type
     << ", last_heartbeat=" << std::ctime(&zn->last_heartbeat);
  return ss.str();
}
