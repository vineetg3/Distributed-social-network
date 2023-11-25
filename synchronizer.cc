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
#include <glog/logging.h>     // for LOG
#include <glog/raw_logging.h> // for RAW_LOG

#include "synchronizer.grpc.pb.h"
#include "coordinator.grpc.pb.h"

namespace fs = std::filesystem;

using csce438::AllUsers;
using csce438::Confirmation;
using csce438::Msg;

using csce438::CoordService;
using csce438::FlwRel;
using csce438::ID;
using csce438::ServerInfo;
using csce438::ServerList;
using csce438::SynchService;

// using csce438::TLFL;
using google::protobuf::Duration;
using google::protobuf::Timestamp;
using grpc::ClientContext;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
using namespace std;

#define log(severity, msg) \
    LOG(severity) << msg;  \
    google::FlushLogFiles(google::severity);

struct zNode
{
    int clusterID;
    std::string hostname;
    std::string port;
    std::string type;
};

int synchID = 1;
std::vector<std::string> get_lines_from_file(std::string);
void run_synchronizer(std::string, std::string, std::string, int);
std::vector<std::string> get_all_managed_users_func(int);
std::vector<std::string> get_tl_or_fl(int, int, bool);
std::vector<std::string> getMatchingFilePaths(int, string);
std::vector<std::string> getSortedUniqueStrings(const std::vector<std::string> &inputVector);
int sync_global_users(std::vector<zNode *> &sync_servers, vector<std::unique_ptr<SynchService::Stub>> &fs_stubs, int syncId);
void overwriteToFile(const std::string &relativeFilePath, const std::vector<std::string> &data);
set<string> get_all_follow_relations();
int sync_follow(std::vector<zNode *> &sync_servers, vector<std::unique_ptr<SynchService::Stub>> &fs_stubs, int syncId);
map<string, set<string>> generateFlwMp(vector<string> &flwrels);
std::vector<std::string> splitString(const std::string &input, char delimiter);
std::string removeFileName(const std::string &input);

class SynchServiceImpl final : public SynchService::Service
{
    Status GetManagedUsers(ServerContext *context, const Msg *msg, AllUsers *allusers) override
    {
        std::cout << "Got GetManagedUsersfrom: " << msg->data() << std::endl;
        std::vector<std::string> list = get_all_managed_users_func(synchID);
        // package list
        for (auto s : list)
        {
            allusers->add_users(s);
        }

        // return list
        return Status::OK;
    }

    Status GetFL(ServerContext *context, const Msg *msg, FlwRel *flwrels) override
    {
        std::cout << "Got GetFL from: " << msg->data() << std::endl;
        set<string> rels = get_all_follow_relations();
        for (string s : rels)
        {
            flwrels->add_rel(s);
        }

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
            cout << "I am part of cluster: " << synchID << endl;
            break;
        default:
            std::cerr << "Invalid Command Line Argument\n";
        }
    }
    std::string log_file_name = std::string("synchronizer");
    google::InitGoogleLogging(log_file_name.c_str());
    FLAGS_logtostderr = true;
    FLAGS_alsologtostderr = true;
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
        sleep(4);
        // Get other FS servers
        std::vector<zNode *> sync_servers;
        vector<std::unique_ptr<SynchService::Stub>> fs_stubs;
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
            // fs_stubs[server_info.clusterid()] = std::unique_ptr<SynchService::Stub>(SynchService::NewStub(grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials())));
            fs_stubs.push_back(unique_ptr<SynchService::Stub>(SynchService::NewStub(grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials()))));
        }

        log(INFO, "**** Beginning global users sync *****");
        int global_users_sync_result = sync_global_users(sync_servers, fs_stubs, synchID);
        if (global_users_sync_result == 0)
        {
            log(INFO, "GLOBAL USER SYNC Failed.");
        }
        else
        {
            log(INFO, "GLOBAL USER SYNC Completed.");
        }
        log(INFO, "**** End global users sync *****");

        log(INFO, "**** Beginning Follower sync *****");
        int followers_sync_result = sync_follow(sync_servers, fs_stubs, synchID);
        if (followers_sync_result == 0)
        {
            log(INFO, "FOLLOW SYNC Failed.");
        }
        else
        {
            log(INFO, "FOLLOW SYNC Completed.");
        }
        log(INFO, "**** End Follower sync *****");

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
        cout << endl
             << endl;
    }
    //     return;
}
//////*********  SYNC FOLLOW RELATIONS **********/////////

int sync_follow(std::vector<zNode *> &sync_servers, vector<std::unique_ptr<SynchService::Stub>> &fs_stubs, int syncId)
{
    // receive all follow rels. Unique the list.
    //  create hashmap
    //  Get Managed users, for each managed user, get contents from followers file, union it with hashmap
    //  and overwrite to file.
    vector<string> all_rels;
    for (auto &stub_ : fs_stubs)
    {
        ClientContext cc;
        Msg msg;
        msg.set_data(to_string(syncId));
        grpc::Status status;
        FlwRel flwrel;
        status = stub_->GetFL(&cc, msg, &flwrel);
        if (!status.ok())
            return 0;
        for (const string &str : flwrel.rel())
        {
            all_rels.push_back(str);
        }
    }
    vector<string> uniq_rels = getSortedUniqueStrings(all_rels);
    map<string, set<string>> userFlwMp = generateFlwMp(uniq_rels);
    std::vector<std::string> managed_users = get_all_managed_users_func(syncId);
    vector<string> server_paths = getMatchingFilePaths(syncId, "");

    // foreach managed user, get followers info present in slave and master
    //  combine and unique all of followers, and keep it back.
    for (string muser : managed_users)
    {
        vector<string> all_flwrs;
        for (string spath : server_paths)
        {
            string filename = spath + "/" + muser + "/followers.txt";
            vector<string> cur_all_flwrs = get_lines_from_file(filename);
            all_flwrs.insert(all_flwrs.end(), cur_all_flwrs.begin(), cur_all_flwrs.end());
        }
        userFlwMp[muser].insert(all_flwrs.begin(), all_flwrs.end());
        for (string spath : server_paths)
        {
            string filename = spath + "/" + muser + "/followers.txt";
            overwriteToFile(filename, std::vector<string>(userFlwMp[muser].begin(), userFlwMp[muser].end()));
        }
    }
    return 1;
}

set<string> get_all_follow_relations()
{
    // returns all the following relations u1 u2 , u1 follows u2. from both servers in cluster.
    //  "u1 u2" unique strings are returned.
    vector<string> fps = getMatchingFilePaths(synchID, "/follow_relations.txt");
    set<string> rels;
    for (string &fp : fps)
    {
        vector<string> lines = get_lines_from_file(fp);
        rels.insert(lines.begin(), lines.end());
    }
    cout << "Follow relations: " << endl;
    for (string line : rels)
    {
        cout << line << endl;
    }
    return rels;
}

//////*********  SYNC MANAGED USERS **********/////////

int sync_global_users(std::vector<zNode *> &sync_servers, vector<std::unique_ptr<SynchService::Stub>> &fs_stubs, int syncId)
{
    vector<string> received_users;
    for (auto &stub_ : fs_stubs)
    {
        ClientContext cc;
        Msg msg;
        msg.set_data(to_string(syncId));
        grpc::Status status;
        AllUsers all_users;
        status = stub_->GetManagedUsers(&cc, msg, &all_users);
        if (!status.ok())
            return 0;
        for (const std::string &str : all_users.users())
        {
            received_users.push_back(str);
        }
    }
    cout << "Received users" << endl;
    for (auto str : received_users)
    {
        cout << str << endl;
    }
    vector<string> unique_users = getSortedUniqueStrings(received_users);
    vector<string> fps = getMatchingFilePaths(syncId, "/global_users.txt");
    for (string &fp : fps)
    {
        cout << "over write to file: " << fp << endl;
        overwriteToFile(fp, unique_users);
    }
    return 1;
}

std::vector<std::string> getSortedUniqueStrings(const std::vector<std::string> &inputVector)
{
    // Create a copy of the input vector
    std::vector<std::string> uniqueStrings = inputVector;

    // Sort the vector
    std::sort(uniqueStrings.begin(), uniqueStrings.end());

    // Remove duplicates using std::unique idiom
    uniqueStrings.erase(std::unique(uniqueStrings.begin(), uniqueStrings.end()), uniqueStrings.end());

    return uniqueStrings;
}

//////*********  COMMON FUNCTIONS **********/////////

std::vector<std::string> get_lines_from_file(std::string filename)
{
    std::vector<std::string> users;
    std::string user;
    std::ifstream file;
    file.open(filename);

    // Check if the file exists and can be opened
    if (!file.is_open())
    {
        std::cerr << "File does not exist while trying to get lines: " << filename << ". skipping.." << std::endl;
        return {}; // Return an empty vector
    }
    cout << "Lines from file: " << filename << endl;
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
        {
            users.push_back(user);
            cout << user << endl;
        }
    }

    file.close();

    // std::cout<<"File: "<<filename<<" has users:"<<std::endl;
    /*for(int i = 0; i<users.size(); i++){
      std::cout<<users[i]<<std::endl;
    }*/

    return users;
}

void overwriteToFile(const std::string &relativeFilePath, const std::vector<std::string> &data)
{
    // create intermediate directories if not existing
    std::filesystem::create_directories(removeFileName(relativeFilePath));

    // Open the file for writing
    std::ofstream outputFile(relativeFilePath);

    if (!outputFile.is_open())
    {
        std::cerr << "Error: Unable to open file for writing." << std::endl;
        return;
    }
    cout << "Writing to file: " << relativeFilePath << endl;
    // Write each string from the vector to the file
    for (const std::string &line : data)
    {
        cout << line << endl;
        outputFile << line << std::endl;
    }

    // Close the file
    outputFile.close();

    std::cout << "Data has been written to the file: " << relativeFilePath << std::endl;
}

std::string removeFileName(const std::string &input)
{
    std::string cleanedPath = (input.substr(0, 2) == "./") ? input.substr(2) : input;
    size_t lastSlashPos = cleanedPath.find_last_of('/');

    if (lastSlashPos == std::string::npos)
    {
        // No forward slash found, return the original string
        return input;
    }

    return "./" + cleanedPath.substr(0, lastSlashPos); // Include the last slash in the result
}

std::vector<std::string> getMatchingFilePaths(int syncID, string suffixPath)
{
    // returns
    //     ./server_1_123
    //     ./server_1_456
    const std::string &regexPattern = "server_" + to_string(syncID) + "_[0-9]+";
    const std::string baseDirectory = fs::current_path().string();
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

    for (auto &dir : matchingDirectories)
    {
        dir += suffixPath;
    }

    return matchingDirectories;
}

//////*********  END **********/////////

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

std::vector<std::string> get_all_managed_users_func(int synchID)
{

    vector<string> fps = getMatchingFilePaths(synchID, "/managed_users.txt");

    std::set<std::string> stringSet;
    for (int i = 0; i < fps.size(); i++)
    {
        std::vector<std::string> list = get_lines_from_file(fps[i]);
        stringSet.insert(list.begin(), list.end());
    }
    std::vector<std::string> users(stringSet.begin(), stringSet.end());
    return users;
}

map<string, set<string>> generateFlwMp(vector<string> &flwrels)
{
    map<string, set<string>> mp;

    for (string str : flwrels)
    {
        vector<string> rel = splitString(str, ' ');
        mp[rel[1]].insert(rel[0]);
    }
    return mp;
}

std::vector<std::string> splitString(const std::string &input, char delimiter)
{
    std::vector<std::string> tokens;
    std::istringstream tokenStream(input);
    std::string token;

    while (std::getline(tokenStream, token, delimiter))
    {
        tokens.push_back(token);
    }

    return tokens;
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
