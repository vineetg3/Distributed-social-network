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
#include <algorithm>
#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>
#include <regex>

#include "synchronizer.grpc.pb.h"
#include "coordinator.grpc.pb.h"

namespace fs = std::filesystem;

using csce438::AllUsers;
using csce438::Confirmation;
using csce438::Msg;

using csce438::CoordService;
using csce438::ID;
using csce438::ServerInfo;
using csce438::ServerList;
using csce438::SynchService;
// using csce438::TLFL;
using google::protobuf::Duration;
using google::protobuf::Timestamp;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ClientContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
using namespace std;

int synchID = 1;
std::vector<std::string> get_lines_from_file(std::string);
void run_synchronizer(std::string, std::string, std::string, int);
std::vector<std::string> get_all_users_func(int);
std::vector<std::string> get_tl_or_fl(int, int, bool);
std::vector<std::string> getMatchingDirectories(const std::string &baseDirectory, const std::string &regexPattern);

struct zNode
{
    int clusterID;
    std::string hostname;
    std::string port;
    std::string type;
};

class SynchServiceImpl final : public SynchService::Service
{
    Status GetManagedUsers(ServerContext *context, const Msg *msg, AllUsers *allusers) override
    {
        // std::cout<<"Got GetAllUsers"<<std::endl;
        std::vector<std::string> list = get_all_users_func(stoi(msg->data()));
        // package list
        for (auto s : list)
        {
            allusers->add_users(s);
        }

        // return list
        return Status::OK;
    }

    //     Status GetTLFL(ServerContext* context, const ID* id, TLFL* tlfl){
    //         //std::cout<<"Got GetTLFL"<<std::endl;
    //         int clientID = id->id();

    //         std::vector<std::string> tl = get_tl_or_fl(synchID, clientID, true);
    //         std::vector<std::string> fl = get_tl_or_fl(synchID, clientID, false);

    //         //now populate TLFL tl and fl for return
    //         for(auto s:tl){
    //             tlfl->add_tl(s);
    //         }
    //         for(auto s:fl){
    //             tlfl->add_fl(s);
    //         }
    //         tlfl->set_status(true);

    //         return Status::OK;
    //     }

    //     Status ResynchServer(ServerContext* context, const ServerInfo* serverinfo, Confirmation* c){
    //         std::cout<<serverinfo->type()<<"("<<serverinfo->serverid()<<") just restarted and needs to be resynched with counterpart"<<std::endl;
    //         std::string backupServerType;

    //         // YOUR CODE HERE

    //         return Status::OK;
    //     }
};

void RunServer(std::string coordIP, std::string coordPort, std::string port_no, int synchID)
{
    // localhost = 127.0.0.1
    std::string server_address("127.0.0.1:" + port_no);
    SynchServiceImpl service;
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

    std::thread t1(run_synchronizer, coordIP, coordPort, port_no, synchID);
    /*
    TODO List:
      -Implement service calls
      -Set up initial single heartbeat to coordinator
      -Set up thread to run synchronizer algorithm
    */

    // Wait for the server to shutdown. Note that some other thread must be
    // responsible for shutting down the server for this call to ever return.
    server->Wait();
}

int main(int argc, char **argv)
{

    int opt = 0;
    std::string coordIP;
    std::string coordPort;
    std::string port = "3029";

    while ((opt = getopt(argc, argv, "h:k:p:i:")) != -1)
    {
        switch (opt)
        {
        case 'h':
            coordIP = optarg;
            break;
        case 'k':
            coordPort = optarg;
            break;
        case 'p':
            port = optarg;
            break;
        case 'i':
            synchID = std::stoi(optarg);
            break;
        default:
            std::cerr << "Invalid Command Line Argument\n";
        }
    }
    RunServer(coordIP, coordPort, port, synchID);
    return 0;
}

void run_synchronizer(std::string coordIP, std::string coordPort, std::string port, int synchID)
{
    //     //setup coordinator stub
    //     //std::cout<<"synchronizer stub"<<std::endl;
    std::string target_str = coordIP + ":" + coordPort;
    std::unique_ptr<CoordService::Stub> coord_stub_;
    coord_stub_ = std::unique_ptr<CoordService::Stub>(CoordService::NewStub(grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials())));
    std::cout << "MADE STUB" << std::endl;

    ServerInfo msg;
    Confirmation c;
    grpc::ClientContext context;

    msg.set_clusterid(synchID);
    msg.set_hostname("127.0.0.1");
    msg.set_port(port);
    msg.set_type("follower");

    //     //send init heartbeat

    grpc::Status status = coord_stub_->RegFS(&context, msg, &c);

    //

    //     //TODO: begin synchronization process
    while (true)
    {
        // change this to 30 eventually
        sleep(10);
        // Get other FS servers
        std::vector<zNode *> sync_servers;
        vector<std::unique_ptr<SynchService::Stub>> fs_stubs(4,NULL);
        grpc::ClientContext context;
        ID id;
        ServerList serverlist;
        grpc::Status status = coord_stub_->GetFS(&context, id, &serverlist);
        for (const ServerInfo &server_info : serverlist.servers())
        {
            // Access individual objects in the repeated field
            zNode *newZNode = new zNode;
            newZNode->hostname = server_info.hostname();
            newZNode->port = server_info.port();
            newZNode->clusterID = server_info.clusterid();
            sync_servers.push_back(newZNode);
            cout << "Sync server details: " << newZNode->port << " " << newZNode->clusterID << endl;
            string target_str = server_info.hostname() + ":" + server_info.port();
            fs_stubs[server_info.clusterid()] = std::unique_ptr<SynchService::Stub>(SynchService::NewStub(grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials())));

        }


        //         //1. synch all users file
        //             //get list of all followers

        //             // YOUR CODE HERE
        //             //set up stub
        //             //send each a GetAllUsers request
        //             //aggregate users into a list
        //             //sort list and remove duplicates

        //             // YOUR CODE HERE

        //             //for all the found users
        //             //if user not managed by current synch
        //             // ...

        //             // YOUR CODE HERE

        // 	    //force update managed users from newly synced users
        //             //for all users
        //             for(auto i : aggregated_users){
        //                 //get currently managed users
        //                 //if user IS managed by current synch
        //                     //read their follower lists
        //                     //for followed users that are not managed on cluster
        //                     //read followed users cached timeline
        //                     //check if posts are in the managed tl
        //                     //add post to tl of managed user

        //                      // YOUR CODE HERE
        //                     }
        //                 }
        //             }
    }
    //     return;
}


void sync_global_users(std::vector<zNode *>& sync_servers,vector<std::unique_ptr<SynchService::Stub>>& fs_stubs, int syncId){
    vector<string> received_users;
    for(auto& stub_: fs_stubs){
        ClientCont
        stub_->
    }
}


std::vector<std::string> get_lines_from_file(std::string filename)
{
    std::vector<std::string> users;
    std::string user;
    std::ifstream file;
    file.open(filename);
    if (file.peek() == std::ifstream::traits_type::eof())
    {
        // return empty vector if empty file
        // std::cout<<"returned empty vector bc empty file"<<std::endl;
        file.close();
        return users;
    }
    while (file)
    {
        getline(file, user);

        if (!user.empty())
            users.push_back(user);
    }

    file.close();

    // std::cout<<"File: "<<filename<<" has users:"<<std::endl;
    /*for(int i = 0; i<users.size(); i++){
      std::cout<<users[i]<<std::endl;
    }*/

    return users;
}

bool file_contains_user(std::string filename, std::string user)
{
    std::vector<std::string> users;
    // check username is valid
    users = get_lines_from_file(filename);
    for (int i = 0; i < users.size(); i++)
    {
        // std::cout<<"Checking if "<<user<<" = "<<users[i]<<std::endl;
        if (user == users[i])
        {
            // std::cout<<"found"<<std::endl;
            return true;
        }
    }
    // std::cout<<"not found"<<std::endl;
    return false;
}

std::vector<std::string> get_all_users_func(int synchID)
{
    const std::string baseDirectory = fs::current_path().string();

    vector<string> dirs = getMatchingDirectories(baseDirectory, "server_" + to_string(synchID) + "_[0-9]+");

    for (auto &dir : dirs)
    {
        dir += "/managed_users.txt";
    }
    std::set<std::string> stringSet;
    // take longest list and package into AllUsers message
    for (int i = 0; i < dirs.size(); i++)
    {
        std::vector<std::string> list = get_lines_from_file(dirs[i]);
        stringSet.insert(list.begin(), list.end());
    }
    std::vector<std::string> users(stringSet.begin(), stringSet.end());
    return users;
}

std::vector<std::string> getMatchingDirectories(const std::string &baseDirectory, const std::string &regexPattern)
{
    // returns
    //     ./server_1_123
    //     ./server_1_456

    const std::regex pattern(regexPattern);

    std::vector<std::string> matchingDirectories;

    try
    {
        for (const auto &entry : fs::directory_iterator(baseDirectory))
        {
            if (fs::is_directory(entry) && std::regex_match(entry.path().filename().string(), pattern))
            {
                matchingDirectories.push_back(fs::relative(entry.path(), baseDirectory).string());
            }
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
    }

    return matchingDirectories;
}

// std::vector<std::string> get_tl_or_fl(int synchID, int clientID, bool tl){
//     std::string master_fn = "./master"+std::to_string(synchID)+"/"+std::to_string(clientID);
//     std::string slave_fn = "./slave"+std::to_string(synchID)+"/" + std::to_string(clientID);
//     if(tl){
//         master_fn.append("_timeline");
//         slave_fn.append("_timeline");
//     }else{
//         master_fn.append("_follow_list");
//         slave_fn.append("_follow_list");
//     }

//     std::vector<std::string> m = get_lines_from_file(master_fn);
//     std::vector<std::string> s = get_lines_from_file(slave_fn);

//     if(m.size()>=s.size()){
//         return m;
//     }else{
//         return s;
//     }

// }