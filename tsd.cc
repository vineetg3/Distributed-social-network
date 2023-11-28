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
std::thread tlUpdatesThread;
string root_folder = "";
string my_role = "";
string connection_str_slave = "";

void sendHeartBeat();
bool createFolderIfNotExists(const std::string &folderPath);
void listDirectoryContents(const std::string &directoryPath);
void loadClientDB();
void createClusterFiles();
bool fileExists(const std::string &filePath);
std::vector<std::string> get_lines_from_file(std::string filename);
bool lineExistsInFile(const std::string &filePath, const std::string &targetLine);
std::string removeFileName(const std::string &input);
void appendToFile(const std::string &filePath, const std::vector<std::string> &lines);
std::unique_ptr<SNSService::Stub> getSlaveStub(string connection_str);
std::vector<std::string> get_lines_from_file_from_linenum(std::string filename, int linenum);
google::protobuf::Timestamp *stringToTimestamp(const std::string &str);
std::vector<std::vector<std::string>> get_TLmessages_from_linenum(std::string username, int linenum);
void sendTLUpdates();
std::mutex tl_mutex;

struct Client
{
  std::string username;
  bool connected = true;
  int following_file_size = 0;
  std::vector<string> *client_followers;
  std::vector<string> *client_following; // redundant
  ServerReaderWriter<Message, Message> *stream = nullptr;
  bool operator==(const Client &c1) const
  {
    return (username == c1.username);
  }
  int tl_line_num = 0;
};
void sendTLBackLogUpdates(Client *client);

vector<string> managed_users;
vector<string> global_users;

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

Message MakeMessage(string ts, const std::string &username, const std::string &msg)
{
  Message m;
  m.set_username(username);
  m.set_msg(msg);
  google::protobuf::Timestamp *timestamp = stringToTimestamp(ts);
  m.set_allocated_timestamp(timestamp);
  return m;
}

class SNSServiceImpl final : public SNSService::Service
{

  Status List(ServerContext *context, const Request *request, ListReply *list_reply) override
  {
    if (my_role == "master")
    {
      if (connection_str_slave.length() != 0)
      {
        // slave exists. hence forward requests
        std::unique_ptr<SNSService::Stub> stubPtr = getSlaveStub(connection_str_slave);
        ClientContext cc;
        ListReply lr;
        stubPtr->List(&cc, *request, &lr);
      }
    }
    loadClientDB();
    Client *curUser;
    string name = request->username();
    curUser = getUser(name);
    Client c;
    std::cout << "Followers array size " << curUser->client_followers->size() << std::endl;
    std::cout << "All users array size " << global_users.size() << std::endl;
    for (int i = 0; i < global_users.size(); i++)
    {
      list_reply->add_all_users(global_users[i]);
    }

    for (int i = 0; i < curUser->client_followers->size(); i++)
    {
      std::cout << curUser->client_followers->at(i) << std::endl;
      list_reply->add_followers(curUser->client_followers->at(i));
    }
    return Status::OK;
  }

  Status Follow(ServerContext *context, const Request *request, Reply *reply) override
  {
    if (my_role == "master")
    {
      if (connection_str_slave.length() != 0)
      {
        // slave exists. hence forward requests
        std::unique_ptr<SNSService::Stub> stubPtr = getSlaveStub(connection_str_slave);
        ClientContext cc;
        Reply rp;
        stubPtr->Follow(&cc, *request, &rp);
      }
    }
    loadClientDB();
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
    if (find(global_users.begin(), global_users.end(), userNameToFollow) == global_users.end())
    {
      reply->set_msg("FAILURE_INVALID_USERNAME: User " + userNameToFollow + " does not exist.");
      return Status::OK;
    }

    // check if curUser is following userToFollow,If not add to following list
    // int flwFlag = 0;
    // for (int i = 0; i < curUser->client_following->size(); i++)
    // {
    //   // implicit conversion of ptr userToFollow to reference
    //   if (curUser->client_following->at(i) == userToFollow)
    //   {
    //     flwFlag++;
    //   }
    // }
    if (userToFollow != NULL)
    {
      // if user to follow is part of current cluster, cache it into memory.
      // Persistance will happen by sync server.
      userToFollow->client_followers->push_back(name);
    }

    appendToFile("./" + root_folder + "/follow_relations.txt", {name + " " + userNameToFollow});

    reply->set_msg(name + " is following " + userNameToFollow);
    return Status::OK;
  }

  // Status UnFollow(ServerContext *context, const Request *request, Reply *reply) override
  // {
  //   Client *curUser;
  //   Client *userToUnFollow;
  //   string name = request->username();
  //   string userNameToUnFollow = request->arguments(0);

  //   /// If both current user and user to follow are the same throw error.
  //   if (name == userNameToUnFollow)
  //   {
  //     reply->set_msg("FAILURE_INVALID_USERNAME: Cannot unfollow yourself.");
  //     return Status::OK;
  //   }

  //   curUser = getUser(name);
  //   userToUnFollow = getUser(userNameToUnFollow);

  //   // If user To unfollow does not exist throw error.
  //   if (userToUnFollow == NULL)
  //   {
  //     reply->set_msg("FAILURE_INVALID_USERNAME: User " + userNameToUnFollow + " does not exist.");
  //     return Status::OK;
  //   }

  //   // if curUser is following userToFollow, remove from following array, else return error
  //   int flwidx = -1;
  //   for (int i = 0; i < curUser->client_following->size(); i++)
  //   {
  //     if (curUser->client_following->at(i) == userToUnFollow)
  //     {
  //       flwidx = i;
  //       break;
  //     }
  //   }
  //   if (flwidx == -1)
  //   {
  //     reply->set_msg("FAILURE_NOT_A_FOLLOWER: Failed with not a follower.");
  //     return Status::OK;
  //   }

  //   // remove user to unfollow
  //   curUser->client_following->erase(curUser->client_following->begin() + flwidx);

  //   // if userToUnfollow has follower curUser, remove from  array, else return error
  //   flwidx = -1;
  //   for (int i = 0; i < userToUnFollow->client_followers->size(); i++)
  //   {
  //     if (userToUnFollow->client_followers->at(i) == curUser)
  //     {
  //       flwidx = i;
  //       break;
  //     }
  //   }
  //   if (flwidx == -1)
  //   {
  //     reply->set_msg("FAILURE_NOT_A_FOLLOWER: Failed with not a follower.");
  //     return Status::OK;
  //   }
  //   userToUnFollow->client_followers->erase(userToUnFollow->client_followers->begin() + flwidx);

  //   reply->set_msg(name + " unfollowed " + userNameToUnFollow);
  //   return Status::OK;
  // }

  // RPC Login
  Status Login(ServerContext *context, const Request *request, Reply *reply) override
  {
    if (my_role == "master")
    {
      if (connection_str_slave.length() != 0)
      {
        // slave exists. hence forward requests
        std::unique_ptr<SNSService::Stub> stubPtr = getSlaveStub(connection_str_slave);
        ClientContext cc;
        Reply rp;
        stubPtr->Login(&cc, *request, &rp);
      }
    }
    loadClientDB();
    cout << "LOGIN FUNC" << endl;
    Client *c;
    string name = request->username();
    c = getUser(name);
    if (c == NULL)
    {
      // client doesn't exist
      c = new Client;
      c->client_followers = new std::vector<string>();
      c->client_following = new std::vector<string>();
      c->connected = true;
      // Create required files.
      const std::string folder_path = root_folder + "/" + name;
      createFolderIfNotExists(folder_path);
      create_or_check_file(folder_path, name, "tl");
      create_or_check_file(folder_path, name, "followers");
      create_or_check_file(folder_path, name, "tl_ptr");
      c->username = name;
      client_db.push_back(c);
      appendToFile("./" + root_folder + "/managed_users.txt", {name});
      appendToFile("./" + root_folder + "/global_users.txt", {name});
      cout << "User " + name + " is connected." << endl;
    }
    else if (c->connected == true) // TODO MP2.2 Make connected as true before each other function call??
    {
      // client exists so set error message and return
      cout << "CLIENT EXISTS!.." << endl;
      reply->set_msg("FAILURE_NOT_EXISTS: User already exists.");
      c->stream = nullptr;
    }
    else if (c->connected == false)
    {
      // This case is when, server picks up client from filesystem, but client hasnt connected yet.
      // So now we connect the client to server and make the client live.
      cout << "MAKING THE CLIENT CONNECTED.." << endl;
      c->connected = true;
      c->stream = nullptr;
    }
    return Status::OK;
  }

  void create_or_check_file(std::string rpath, std::string name, string attr)
  {
    // Open the file
    string file_path = rpath + "/" + attr + ".txt";

    if (!fileExists(file_path))
    {
      // If the file doesn't exist, create it
      std::ofstream createFile(file_path);
      createFile.close(); // Close the file immediately to ensure it's created
    }
  }

  Status Timeline(ServerContext *context,
                  ServerReaderWriter<Message, Message> *stream) override
  {
    Message m;
    while (stream->Read(&m))
    {
      loadClientDB();
      std::string username = m.username();
      Client *c = getUser(username);

      // save the stream to client object, for subsequent writes
      if (m.is_initial() == 1)
      {
        c->stream = stream;
        c->tl_line_num = 0;
      }

      // If the message is initial, i.e Just started timeline mode, we send back, the latest 20 messages
      // by reading the user_tl file.

      if (m.is_initial() == 1)
      {
        sendTLBackLogUpdates(c);
      }
      if (m.is_initial() != 1)
      {
        // note that intial message is dummy message
        string flwrs = "";
        for (int i = 0; i < c->client_followers->size(); i++)
        {
          if (i == 0)
          {
            flwrs = c->client_followers->at(i);
            continue;
          }
          flwrs += " " + c->client_followers->at(i);
        }
        cout << "Timeline Function, flwrs string for user: " << username << " " << flwrs << endl;

        // write to cluster_tl file
        std::vector<std::string> newStrings = {flwrs,
                                               "T " + google::protobuf::util::TimeUtil::ToString(m.timestamp()),
                                               "U " + m.username(),
                                               "W " + m.msg()};
        appendToFile("./" + root_folder + "/cluster_tl.txt", newStrings);
      }
    }
    return Status::OK;
  }

  // DEPRECATED
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

int registerAsMaster(string &lastMasterID)
{
  log(INFO, "Trying to register as master...");
  ClientContext context;
  PathAndData request;
  Confirmation response;
  request.set_serverid(serverID);
  request.set_clusterid(clusterID);
  request.set_hostname("localhost"); // would be gotten from an environment variable
  request.set_port(myPort);
  request.set_path("/master");
  Status status = coord_stub_->Create(&context, request, &response);
  if (status.ok())
  {
    log(INFO, "I am.." + response.role());
    lastMasterID = response.data();
    isRegWithCoord = true;
    my_role = response.role();
    // masterID = stoi(response.status());
    // // cout << "master " << masterID << endl;
    // // cout << "server " << serverID << endl;
    // if (masterID == serverID)
    // {
    //   log(INFO, "I am the master..");
    // }
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
  string lastMasterID;
  registerAsMaster(lastMasterID);
  if (isRegWithCoord)
  {
    hb = std::thread(sendHeartBeat);
  }
  root_folder = "server_" + to_string(clusterID) + "_" + to_string(serverID);
  if (my_role == "slave")
  {
    // copy files from master if exists
    cout << "last master ID is: " << lastMasterID << endl;
  }
  createFolderIfNotExists(root_folder);
  createClusterFiles();
  loadClientDB();
  tlUpdatesThread = std::thread(sendTLUpdates);
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
  tlUpdatesThread.join();
  return 0;
}

void sendHeartBeat()
{
  while (true)
  {
    sleep(8);
    ServerInfo request;
    ClientContext context;
    Confirmation response;
    // cout << serverID << endl;
    request.set_serverid(serverID);
    request.set_clusterid(clusterID);
    request.set_hostname("localhost"); // would be gotten from an environment variable
    request.set_port(myPort);
    cout << "sending heartbeat.." << endl;
    Status status = coord_stub_->Heartbeat(&context, request, &response);
    if (status.ok())
    {
      log(INFO, "Heartbeat succeeded..");
      if (response.role() == "master")
      {
        my_role = "master";
        connection_str_slave = response.data();
      }
      else
      {
        my_role = "slave";
        connection_str_slave = "";
      }
      cout << "I am.." << my_role << ". Slave connection string :" << connection_str_slave << endl;
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
  global_users = get_lines_from_file("./" + root_folder + "/global_users.txt");
  managed_users = get_lines_from_file("./" + root_folder + "/managed_users.txt");
  for (const auto &entry : fs::directory_iterator(root_folder))
  {
    if (entry.is_directory())
    {
      std::cout << "Directory: " << entry.path().filename().string() << std::endl;
      Client *c = NULL;
      c = getUser(entry.path().filename().string());
      int nc = 0;
      if (c == NULL)
      {
        nc++;
        c = new Client;
      }

      c->username = entry.path().filename().string();
      cout << "In load client DB, entry.path().string() is: " << entry.path().string() << endl;

      // merge current followers with persisted ones
      set<string> flwrs;
      if (nc == 0)
      {
        // if client in memory get current list of followers
        flwrs.insert(c->client_followers->begin(), c->client_followers->end());
      }
      vector<string> flwrs_from_file = get_lines_from_file(entry.path().string() + "/followers.txt");
      flwrs.insert(flwrs_from_file.begin(), flwrs_from_file.end());
      c->client_followers = new vector<string>(flwrs.begin(), flwrs.end());
      c->connected = false;
      if (nc == 1)
      { // push to vector if new client
        client_db.push_back(c);
      }
    }
    else if (entry.is_regular_file())
    {
      // std::cout << "File: " << entry.path().string() << std::endl;
    }
    else
    {
      // std::cout << "Other: " << entry.path().string() << std::endl;
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

void createClusterFiles()
{
  vector<string> fileNames = {"cluster_tl.txt", "global_users.txt", "managed_users.txt", "follow_relations.txt"};
  for (const std::string &fileName : fileNames)
  {
    // Construct the full file path
    std::string filePath = "./" + root_folder + '/' + fileName;
    if (fileExists(filePath))
      continue;

    // Open the file for writing
    std::ofstream file(filePath);

    if (file.is_open())
    {
      // Close the file
      file.close();
      std::cout << "File created: " << filePath << std::endl;
    }
    else
    {
      std::cerr << "Unable to create the file: " << filePath << std::endl;
    }
  }
}

// Timeline updates to read from file

void sendTLBackLogUpdates(Client *client)
{
  tl_mutex.lock();
  std::vector<std::vector<std::string>> msgs = get_TLmessages_from_linenum(client->username, client->tl_line_num);
  for (int i = 0; i < msgs.size(); i++)
  {
    Message m1 = MakeMessage(msgs[i][0], msgs[i][1], msgs[i][2]);
    if (my_role == "master")
    {
      if (client->stream != nullptr)
      {
        try
        {
          cout << "Sending TL updates.." << endl;
          cout << "message is " << m1.username() << " " << m1.msg() << " " << m1.timestamp() << endl;
          if (!client->stream->Write(m1))
          {
            cout << "Stream closed!!!" << endl;
          }
          cout << "after write" << endl;
          client->tl_line_num += 3;
        }
        catch (const exception &e)
        {
          cout << " grpc Error: " << e.what() << endl;
        }
      }
    }
  }
  tl_mutex.unlock();
}

void sendTLUpdates()
{
  while (true)
  {
    // loadClientDB();
    cout << "Checking for TL updates.." << endl;
    for (int i = 0; i < client_db.size(); i++)
    {
      Client *client = client_db[i];
      if (client->stream != nullptr)
      {
        sendTLBackLogUpdates(client);
      }
    }
    sleep(5);
  }
}

google::protobuf::Timestamp *stringToTimestamp(const std::string &str)
{
  std::tm tm = {};
  std::istringstream ss(str);
  ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");

  if (ss.fail())
  {
    // Handle parsing failure
    throw std::invalid_argument("Failed to parse timestamp");
  }

  google::protobuf::Timestamp *timestamp = new google::protobuf::Timestamp();
  timestamp->set_seconds(std::mktime(&tm));
  timestamp->set_nanos(0);

  return timestamp;
}

/// @brief Gets the last 20 messages saved to the user's timeline
/// @param username
/// @return
std::vector<std::vector<std::string>> get_TLmessages_from_linenum(std::string username, int linenum)
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
  int ct = 0;
  while (std::getline(infile, line))
  {
    ct++;
    if (ct > linenum)
    {
      lines.push_back(line);
    }
  }

  // Messages vector will be populated to the scheme
  //{{timestamp1,username1,msg1},{timestamp2,username2,msg2}..}
  std::vector<std::vector<std::string>> messages;

  // Process every set of 3 lines(timestamp,user,msg) and push to vector
  for (int i = 0; i < lines.size();)
  {
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
    i += 3;
  }
  return messages;
}

// Function to check if a file exists
bool fileExists(const std::string &filePath)
{
  struct stat buffer;
  return (stat(filePath.c_str(), &buffer) == 0);
}

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
  while (!file.eof())
  {
    getline(file, user);
    if (!user.empty())
    {
      users.push_back(user);
      cout << user << endl;
    }
  }

  file.close();
  return users;
}

std::vector<std::string> get_lines_from_file_from_linenum(std::string filename, int linenum)
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
  int cur_idx = 0;
  while (!file.eof())
  {
    getline(file, user);
    cur_idx++;
    if (!user.empty() && cur_idx > linenum)
    {
      users.push_back(user);
      cout << user << endl;
    }
  }

  file.close();
  return users;
}

bool lineExistsInFile(const std::string &filePath, const std::string &targetLine)
{
  std::ifstream file(filePath);

  if (file.is_open())
  {
    std::string line;
    while (getline(file, line))
    {
      if (line == targetLine)
      {
        file.close();
        return true; // Line found
      }
    }

    file.close();
  }
  else
  {
    std::cerr << "Unable to open the file: " << filePath << std::endl;
  }

  return false; // Line not found or file not opened
}

// Function to append lines to a file
void appendToFile(const std::string &filePath, const std::vector<std::string> &lines)
{

  // it appends to file. If file is not found, creates it and then appends

  // create intermediate directories if not existing
  std::filesystem::create_directories(removeFileName(filePath));

  if (!fileExists(filePath))
  {
    // If the file doesn't exist, create it
    std::ofstream createFile(filePath);
    createFile.close(); // Close the file immediately to ensure it's created
  }

  // Check if the file exists
  if (fileExists(filePath))
  {
    // Open the file in append mode
    std::ofstream file(filePath, std::ios::app);

    if (file.is_open())
    {
      // Append each line to the file
      for (const std::string &line : lines)
      {
        file << line << "\n";
      }

      // Close the file
      file.close();
      std::cout << "Lines appended to the file: " << filePath << std::endl;
    }
    else
    {
      std::cerr << "Unable to open the file for appending." << std::endl;
    }
  }
  else
  {
    std::cerr << "File does not exist." << std::endl;
  }
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

// Sync between master and slave

std::unique_ptr<SNSService::Stub> getSlaveStub(string connection_str)
{
  std::shared_ptr<Channel> channelForServer = grpc::CreateChannel(connection_str, grpc::InsecureChannelCredentials());
  return SNSService::NewStub(channelForServer);
}