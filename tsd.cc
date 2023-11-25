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

#define log(severity, msg) \
  LOG(severity) << msg;    \
  google::FlushLogFiles(google::severity);

#include "sns.grpc.pb.h"
#include "coordinator.grpc.pb.h"

using csce438::Confirmation;
using csce438::CoordService;
using csce438::ListReply;
using csce438::Message;
using csce438::Path;
using csce438::PathAndData;
using csce438::Reply;
using csce438::ReplyStatus;
using csce438::Request;
using csce438::ServerInfo;
using csce438::SNSService;
using google::protobuf::Duration;
using google::protobuf::Timestamp;
using grpc::Channel;
using grpc::ClientContext;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;

using namespace std;
namespace fs = std::filesystem;

int clusterID;
int serverID;
string coordinatorIP;
string coordinatorPort;
string myPort;
int masterID = -1;
std::unique_ptr<CoordService::Stub> coord_stub_;
bool isRegWithCoord = false;
std::thread hb;
string root_folder = "";

void sendHeartBeat();
bool createFolderIfNotExists(const std::string &folderPath);
void listDirectoryContents(const std::string &directoryPath);
void loadClientDB();

struct Client
{
  std::string username;
  bool connected = true;
  int following_file_size = 0;
  std::vector<Client *> *client_followers;
  std::vector<Client *> *client_following;
  ServerReaderWriter<Message, Message> *stream = 0;
  bool operator==(const Client &c1) const
  {
    return (username == c1.username);
  }
};

// Vector that stores every client that has been created
std::vector<Client *> client_db;

/// @brief Get's the user from client_db given username.
/// @param username
/// @return
Client *getUser(string username)
{
  Client *c;
  for (int i = 0; i < client_db.size(); i++)
  {
    // check if user name is in client db
    c = client_db[i];
    if (c->username == username)
    {
      return c;
    }
  }
  return NULL;
}

Message MakeMessage(const std::string &username, const std::string &msg)
{
  Message m;
  m.set_username(username);
  m.set_msg(msg);
  google::protobuf::Timestamp *timestamp = new google::protobuf::Timestamp();
  timestamp->set_seconds(time(NULL));
  timestamp->set_nanos(0);
  m.set_allocated_timestamp(timestamp);
  return m;
}

class SNSServiceImpl final : public SNSService::Service
{

  Status List(ServerContext *context, const Request *request, ListReply *list_reply) override
  {
    Client *curUser;
    string name = request->username();
    curUser = getUser(name);
    Client c;
    std::cout << "Followers array size " << curUser->client_followers->size() << std::endl;
    std::cout << "All users array size " << client_db.size() << std::endl;
    for (int i = 0; i < client_db.size(); i++)
    {
      list_reply->add_all_users(client_db[i]->username);
    }
    for (int i = 0; i < curUser->client_followers->size(); i++)
    {
      std::cout << curUser->client_followers->at(i)->username.length() << std::endl;
      list_reply->add_followers(curUser->client_followers->at(i)->username);
    }
    return Status::OK;
  }

  Status Follow(ServerContext *context, const Request *request, Reply *reply) override
  {
    Client *curUser;
    Client *userToFollow;
    string name = request->username();
    string userNameToFollow = request->arguments(0);

    // If both current user and user to follow are the same throw error
    if (name == userNameToFollow)
    {
      reply->set_msg("FAILURE_ALREADY_EXISTS: Cannot follow yourself.");
      return Status::OK;
    }
    curUser = getUser(name);
    userToFollow = getUser(userNameToFollow);

    // If user to follow doesn't exist throw error
    if (userToFollow == NULL)
    {
      reply->set_msg("FAILURE_INVALID_USERNAME: User " + userNameToFollow + " does not exist.");
      return Status::OK;
    }

    // check if curUser is following userToFollow,If not add to following list
    int flwFlag = 0;
    for (int i = 0; i < curUser->client_following->size(); i++)
    {
      // implicit conversion of ptr userToFollow to reference
      if (curUser->client_following->at(i) == userToFollow)
      {
        flwFlag++;
      }
    }
    if (!flwFlag)
    {
      curUser->client_following->push_back(userToFollow);
    }

    // add curUser to userToFollow's followers list
    flwFlag = 0;
    for (int i = 0; i < userToFollow->client_followers->size(); i++)
    {
      if (userToFollow->client_followers->at(i) == curUser)
      {
        flwFlag++;
      }
    }
    if (!flwFlag)
    {
      userToFollow->client_followers->push_back(curUser);
    }

    reply->set_msg(name + " is following " + userNameToFollow);
    return Status::OK;
  }

  Status UnFollow(ServerContext *context, const Request *request, Reply *reply) override
  {
    Client *curUser;
    Client *userToUnFollow;
    string name = request->username();
    string userNameToUnFollow = request->arguments(0);

    /// If both current user and user to follow are the same throw error.
    if (name == userNameToUnFollow)
    {
      reply->set_msg("FAILURE_INVALID_USERNAME: Cannot unfollow yourself.");
      return Status::OK;
    }

    curUser = getUser(name);
    userToUnFollow = getUser(userNameToUnFollow);

    // If user To unfollow does not exist throw error.
    if (userToUnFollow == NULL)
    {
      reply->set_msg("FAILURE_INVALID_USERNAME: User " + userNameToUnFollow + " does not exist.");
      return Status::OK;
    }

    // if curUser is following userToFollow, remove from following array, else return error
    int flwidx = -1;
    for (int i = 0; i < curUser->client_following->size(); i++)
    {
      if (curUser->client_following->at(i) == userToUnFollow)
      {
        flwidx = i;
        break;
      }
    }
    if (flwidx == -1)
    {
      reply->set_msg("FAILURE_NOT_A_FOLLOWER: Failed with not a follower.");
      return Status::OK;
    }

    // remove user to unfollow
    curUser->client_following->erase(curUser->client_following->begin() + flwidx);

    // if userToUnfollow has follower curUser, remove from  array, else return error
    flwidx = -1;
    for (int i = 0; i < userToUnFollow->client_followers->size(); i++)
    {
      if (userToUnFollow->client_followers->at(i) == curUser)
      {
        flwidx = i;
        break;
      }
    }
    if (flwidx == -1)
    {
      reply->set_msg("FAILURE_NOT_A_FOLLOWER: Failed with not a follower.");
      return Status::OK;
    }
    userToUnFollow->client_followers->erase(userToUnFollow->client_followers->begin() + flwidx);

    reply->set_msg(name + " unfollowed " + userNameToUnFollow);
    return Status::OK;
  }

  // RPC Login
  Status Login(ServerContext *context, const Request *request, Reply *reply) override
  {
    cout << "LOGIN FUNC" << endl;
    Client *c;
    string name = request->username();
    c = getUser(name);
    if (c == NULL)
    {
      // client doesn't exist
      c = new Client;
      c->client_followers = new std::vector<Client *>();
      c->client_following = new std::vector<Client *>();
      c->connected = true;
      // Create required files.
      const std::string folder_path = root_folder + "/" + name;
      createFolderIfNotExists(folder_path);
      create_or_check_file(folder_path, name, "tl");
      create_or_check_file(folder_path, name, "follow");
      c->username = name;
      client_db.push_back(c);
      cout << "User " + name + " is connected." << endl;
    }
    else if (c->connected == true) // TODO MP2.2 Make connected as true before each other function call??
    {
      // client exists so set error message and return
      cout << "CLIENT EXISTS!.." << endl;
      reply->set_msg("FAILURE_NOT_EXISTS: User already exists.");
    }
    else if (c->connected == false)
    {
      // This case is when, server picks up client from filesystem, but client hasnt connected yet.
      // So now we connect the client to server and make the client live.
      cout << "MAKING THE CLIENT CONNECTED.." << endl;
      c->connected = true;
    }
    return Status::OK;
  }

  void create_or_check_file(std::string rpath, std::string name, string attr)
  {
    // Open the file in append
    string file_path = rpath + "/" + attr + ".txt";
    std::ofstream outfile(file_path, std::ios::app);
    if (outfile.is_open())
    {
      // Close the file when done
      outfile.close();
      std::cout << file_path + " File created for " + name << std::endl;
    }
    else
    {
      std::cerr << file_path + " Failed to open the file for " + name << std::endl;
    }
  }

  Status Timeline(ServerContext *context,
                  ServerReaderWriter<Message, Message> *stream) override
  {

    Message m;
    while (stream->Read(&m))
    {
      std::string username = m.username();
      Client *c = getUser(username);

      // save the stream to client object, for subsequent writes
      if (m.is_initial() == 1)
        c->stream = stream;

      // If the message is initial, i.e Just started timeline mode, we send back, the latest 20 messages
      // by reading the user_tl file.

      if (m.is_initial() == 1)
      {
        // read 20 latest massages from file currentuser_timeline
        std::vector<std::vector<std::string>> msgs = get_last_20_messages(username);
        for (int i = 0; i < msgs.size(); i++)
        {
          Message m1 = MakeMessage(msgs[i][1], msgs[i][2]);
          stream->Write(m1);
        }
      }
      else
      {
        // loop through the followers list to send messages and append to follower's timeline
        for (int i = 0; i < c->client_followers->size(); i++)
        {
          Client *cc = c->client_followers->at(i);
          // append_to_timeline(c->client_followers->at(i)->username, m.username(), m.timestamp(), m.msg());
          if (cc->stream != nullptr)
            cc->stream->Write(m);
        }
      }
    }
    return Status::OK;
  }

  void append_to_timeline(std::string username, std::string puser, google::protobuf::Timestamp ptime,
                          std::string ppost)
  {

    const std::string file_path = root_folder + "/" + username + "/tl.txt";

    // Open the file in input mode to read the existing content
    std::ifstream infile(file_path);

    if (!infile)
    {
      std::cerr << "Failed to open the file." << std::endl;
      return;
    }

    // Read the existing content line by line into a vector
    std::vector<std::string> lines;
    std::string line;

    while (std::getline(infile, line))
    {
      lines.push_back(line);
    }

    infile.close();

    // Open the file again in output mode to write the updated content
    std::ofstream outfile(file_path);

    if (!outfile)
    {
      std::cerr << "Failed to open the file for writing." << std::endl;
      return;
    }

    // Append new strings at the top of the file
    std::vector<std::string> newStrings = {"T " + google::protobuf::util::TimeUtil::ToString(ptime) + "\n",
                                           "U " + puser + "\n",
                                           "W " + ppost + "\n"};

    for (const std::string &newString : newStrings)
    {
      outfile << newString;
    }

    // Append the existing content after the new strings
    for (const std::string &existingLine : lines)
    {
      outfile << existingLine << std::endl;
    }

    outfile.close();

    std::cout << "Strings appended to the top of the file." << std::endl;
  }

  /// @brief Gets the last 20 messages saved to the user's timeline
  /// @param username
  /// @return
  std::vector<std::vector<std::string>> get_last_20_messages(std::string username)
  {
    const std::string file_path = root_folder + "/" + username + "/tl.txt";

    // Open the file in input mode to read the existing content
    std::ifstream infile(file_path);

    if (!infile)
    {
      std::cerr << "Failed to open the file." << std::endl;
    }

    // Read the existing content line by line into a vector
    std::vector<std::string> lines;
    std::string line;

    while (std::getline(infile, line))
    {
      lines.push_back(line);
    }

    // Messages vector will be populated to the scheme
    //{{timestamp1,username1,msg1},{timestamp2,username2,msg2}..}
    std::vector<std::vector<std::string>> messages;
    int ct = 0;

    // Process every set of 3 lines(timestamp,user,msg) and push to vector
    for (int i = 0; i < lines.size();)
    {
      if (ct == 20)
        break;

      std::vector<std::string> reply_msg;
      for (int j = 0; j < 3; j++)
      {
        if (lines[i + j].back() == '\n')
        {
          lines[i + j].pop_back();
        }
        // slice the required string by removing the identifier i.e T, U, W
        reply_msg.push_back(lines[i + j].substr(2));
      }
      messages.push_back(reply_msg);
      ct++;
      i += 3;
    }
    return messages;
  }

  Status CheckIfAlive(ServerContext *context, const Request *request, Reply *reply) override
  {
    reply->set_msg("Alive");
    return Status::OK;
  }
};

void RunServer(std::string port_no)
{
  std::string server_address = "0.0.0.0:" + port_no;
  SNSServiceImpl service;

  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  // std::cout << "Server listening on " << server_address << std::endl;
  log(INFO, "Server listening on " + server_address);
  server->Wait();
}

void registerAsServer()
{
  // TODO: MP2.2
  isRegWithCoord = true;
}

int registerAsMaster()
{
  log(INFO, "Trying to register as master...");
  ClientContext context;
  PathAndData request;
  ReplyStatus response;
  request.set_serverid(serverID);
  request.set_clusterid(clusterID);
  request.set_hostname("localhost"); // would be gotten from an environment variable
  request.set_port(myPort);
  request.set_path("/master");
  Status status = coord_stub_->Create(&context, request, &response);
  if (status.ok())
  {
    log(INFO, "Master is.." + response.status());
    masterID = stoi(response.status());
    // cout << "master " << masterID << endl;
    // cout << "server " << serverID << endl;
    if (masterID == serverID)
    {
      log(INFO, "I am the master..");
    }
  }
  else
  {
    log(ERROR, "Coordinator error!");
  }
  return masterID;
}

void connectToCoordinator()
{
  std::shared_ptr<Channel> channel = grpc::CreateChannel(coordinatorIP + ":" + coordinatorPort, grpc::InsecureChannelCredentials());
  coord_stub_ = CoordService::NewStub(channel);
}

void onStartUp()
{
  connectToCoordinator();
  registerAsMaster();
  registerAsServer();
  if (masterID != -1 || isRegWithCoord)
  {
    hb = std::thread(sendHeartBeat);
  }
  root_folder = "server_" + to_string(clusterID) + "_" + to_string(serverID);
  createFolderIfNotExists(root_folder);
  loadClientDB();
}

int main(int argc, char **argv)
{

  std::string port = "3010";

  int opt = 0;
  int argslen = 0;
  while ((opt = getopt(argc, argv, "p:c:s:h:k:")) != -1)
  {
    switch (opt)
    {
    case 'p':
      port = optarg;
      myPort = port;
      break;
    case 'c':
      clusterID = stoi(optarg);
      argslen++;
      break;
    case 's':
      // cout << optarg << endl;
      serverID = stoi(optarg);
      argslen++;
      break;
    case 'h':
      coordinatorIP = optarg;
      argslen++;
      break;
    case 'k':
      coordinatorPort = optarg;
      argslen++;
      break;
    default:
      std::cerr << "Invalid Command Line Argument\n";
    }
  }
  if (argslen != 4)
  {
    std::cerr << "Arguments missing!\n";
    exit(1);
  }

  std::string log_file_name = std::string("server-") + port;
  google::InitGoogleLogging(log_file_name.c_str());
  // FLAGS_logtostderr = true;
  // FLAGS_alsologtostderr=true;
  log(INFO, "Logging Initialized. Server starting...");
  onStartUp();
  RunServer(port);
  hb.join();
  return 0;
}

void sendHeartBeat()
{
  while (true)
  {
    sleep(5);
    ServerInfo request;
    ClientContext context;
    Confirmation response;
    // cout << serverID << endl;
    request.set_serverid(serverID);
    request.set_clusterid(clusterID);
    request.set_hostname("localhost"); // would be gotten from an environment variable
    request.set_port(myPort);
    Status status = coord_stub_->Heartbeat(&context, request, &response);
    if (response.status())
    {
      log(INFO, "Heartbeat succeeded..");
    }
    else
    {
      log(INFO, "Heartbeat failed..");
    }
  }
  return;
}

void listDirectoryContents(const std::string &directoryPath)
{
  for (const auto &entry : fs::directory_iterator(directoryPath))
  {
    if (entry.is_directory())
    {
      std::cout << "Directory: " << entry.path().string() << std::endl;
    }
    else if (entry.is_regular_file())
    {
      std::cout << "File: " << entry.path().string() << std::endl;
    }
    else
    {
      std::cout << "Other: " << entry.path().string() << std::endl;
    }
  }
}

void loadClientDB()
{
  for (const auto &entry : fs::directory_iterator(root_folder))
  {
    if (entry.is_directory())
    {
      std::cout << "Directory: " << entry.path().filename().string() << std::endl;
      Client *c = new Client;
      c->client_followers = new std::vector<Client *>();
      c->client_following = new std::vector<Client *>();
      c->username = entry.path().filename().string();
      c->connected = false;
      client_db.push_back(c);
    }
    else if (entry.is_regular_file())
    {
      std::cout << "File: " << entry.path().string() << std::endl;
    }
    else
    {
      std::cout << "Other: " << entry.path().string() << std::endl;
    }
  }
}

bool createFolderIfNotExists(const std::string &folderPath)
{
  if (!fs::exists(folderPath))
  {
    try
    {
      fs::create_directory(folderPath);
      std::cout << "Folder created: " << folderPath << std::endl;
      return true; // Folder was created
    }
    catch (const std::filesystem::filesystem_error &e)
    {
      std::cerr << "Error creating folder: " << e.what() << std::endl;
      return false; // Error occurred while creating the folder
    }
  }
  else
  {
    std::cout << "Folder already exists: " << folderPath << std::endl;
    return true; // Folder already exists
  }
}