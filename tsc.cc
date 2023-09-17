#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <string>
#include <unistd.h>
#include <csignal>
#include <grpc++/grpc++.h>
#include "client.h"

#include "sns.grpc.pb.h"
using csce438::ListReply;
using csce438::Message;
using csce438::Reply;
using csce438::Request;
using csce438::SNSService;
using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::Status;

void sig_ignore(int sig)
{
  std::cout << "Signal caught " + sig;
}

bool isErrorCodeExists(std::string error, std::string code)
{
  return error.find(code) != std::string::npos;
}

std::vector<std::string> splitString(const std::string &input, char delimiter)
{
  std::vector<std::string> result;
  size_t start = 0;
  size_t end = input.find(delimiter);

  while (end != std::string::npos)
  {
    result.push_back(input.substr(start, end - start));
    start = end + 1;
    end = input.find(delimiter, start);
  }

  // Add the remaining part of the string (or the whole string if no delimiter found)
  result.push_back(input.substr(start));

  return result;
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

class Client : public IClient
{
public:
  Client(const std::string &hname,
         const std::string &uname,
         const std::string &p)
      : hostname(hname), username(uname), port(p) {}

protected:
  virtual int connectTo();
  virtual IReply processCommand(std::string &input);
  virtual void processTimeline();

private:
  std::string hostname;
  std::string username;
  std::string port;

  // You can have an instance of the client stub
  // as a member variable.
  std::unique_ptr<SNSService::Stub> stub_;

  IReply Login();
  IReply List();
  IReply Follow(const std::string &username);
  IReply UnFollow(const std::string &username);
  void Timeline(const std::string &username);
};

///////////////////////////////////////////////////////////
//
//////////////////////////////////////////////////////////
int Client::connectTo()
{
  // ------------------------------------------------------------
  // In this function, you are supposed to create a stub so that
  // you call service methods in the processCommand/porcessTimeline
  // functions. That is, the stub should be accessible when you want
  // to call any service methods in those functions.
  // Please refer to gRpc tutorial how to create a stub.
  // ------------------------------------------------------------
  std::shared_ptr<Channel> channel = grpc::CreateChannel(hostname + ":" + port, grpc::InsecureChannelCredentials());
  stub_ = SNSService::NewStub(channel);
  IReply ire = Client::Login();
  if (ire.comm_status != IStatus::SUCCESS)
  {
    return -1;
  }
  return 1;
}

IReply Client::processCommand(std::string &input)
{
  // ------------------------------------------------------------
  // GUIDE 1:
  // In this function, you are supposed to parse the given input
  // command and create your own message so that you call an
  // appropriate service method. The input command will be one
  // of the followings:
  //
  // FOLLOW <username>
  // UNFOLLOW <username>
  // LIST
  // TIMELINE
  // ------------------------------------------------------------

  // ------------------------------------------------------------
  // GUIDE 2:
  // Then, you should create a variable of IReply structure
  // provided by the client.h and initialize it according to
  // the result. Finally you can finish this function by returning
  // the IReply.
  // ------------------------------------------------------------

  // ------------------------------------------------------------
  // HINT: How to set the IReply?
  // Suppose you have "FOLLOW" service method for FOLLOW command,
  // IReply can be set as follow:
  //
  //     // some codes for creating/initializing parameters for
  //     // service method
  //     IReply ire;
  //     grpc::Status status = stub_->FOLLOW(&context, /* some parameters */);
  //     ire.grpc_status = status;
  //     if (status.ok()) {
  //         ire.comm_status = SUCCESS;
  //     } else {
  //         ire.comm_status = FAILURE_NOT_EXISTS;
  //     }
  //
  //      return ire;
  //
  // IMPORTANT:
  // For the command "LIST", you should set both "all_users" and
  // "following_users" member variable of IReply.
  // ------------------------------------------------------------

  IReply ire;

  // parse the input command
  std::vector<std::string> tokens = splitString(input, ' ');
  std::string command = tokens[0];
  if (tokens[0] == "FOLLOW")
  {
    std::string username2 = tokens[1];
    ire = Client::Follow(username2);
  }
  else if (tokens[0] == "UNFOLLOW")
  {
    std::string username2 = tokens[1];
    ire = Client::UnFollow(username2);
  }
  else if (tokens[0] == "LIST")
  {
    ire = Client::List();
  }
  else if (tokens[0] == "TIMELINE")
  {
  }
  return ire;
}

void Client::processTimeline()
{
  Timeline(username);
}

// List Command
IReply Client::List()
{

  IReply ire;
  ListReply list_reply;
  Request request;
  ClientContext contex;
  grpc::Status status;
  request.set_username(username);
  status = stub_->List(&contex, request, &list_reply);
  if (status.ok())
  {
    for (const std::string &user : list_reply.all_users())
    {
      ire.all_users.push_back(user);
    }
    for (const std::string &user : list_reply.followers())
    {
      ire.followers.push_back(user);
    }
  }
  ire.comm_status = IStatus::SUCCESS;
  ire.grpc_status = status;
  return ire;
}

// Follow Command
IReply Client::Follow(const std::string &username2)
{

  IReply ire;
  Request request;
  Reply reply;
  ClientContext contex;
  grpc::Status status;

  request.set_username(username);
  request.add_arguments(username2);
  status = stub_->Follow(&contex, request, &reply);
  if (status.ok())
  {
    std::cout << reply.msg() << std::endl;
    if (isErrorCodeExists(reply.msg(), "FAILURE_INVALID_USERNAME"))
    {
      ire.comm_status = IStatus::FAILURE_INVALID_USERNAME;
    }
    else
    {
      ire.comm_status = IStatus::SUCCESS;
    }
  }
  ire.grpc_status = status;
  return ire;
}

// UNFollow Command
IReply Client::UnFollow(const std::string &username2)
{

  IReply ire;
  Request request;
  Reply reply;
  ClientContext contex;
  grpc::Status status;
  request.set_username(username);
  request.add_arguments(username2);
  status = stub_->UnFollow(&contex, request, &reply);
  if (status.ok())
  {
    std::cout << reply.msg() << std::endl;
    if (isErrorCodeExists(reply.msg(), "FAILURE_INVALID_USERNAME"))
    {
      ire.comm_status = IStatus::FAILURE_INVALID_USERNAME;
    }
    else if (isErrorCodeExists(reply.msg(), "FAILURE_NOT_A_FOLLOWER"))
    {
      ire.comm_status = IStatus::FAILURE_NOT_A_FOLLOWER;
    }
    else
    {
      ire.comm_status = IStatus::SUCCESS;
    }
  }
  ire.grpc_status = status;
  return ire;
}

// Login Command
IReply Client::Login()
{

  IReply ire;
  ClientContext context;
  Request request;
  Reply reply;
  std::cout << username + "HERE" << std::endl;
  request.set_username(username);
  // we need to send the address of context and reply object as gRPC receives it as a pointer with the address
  Status status = stub_->Login(&context, request, &reply);
  std::cout << "" << std::endl;
  if (status.ok())
  {
    std::cout << reply.msg() << std::endl;
    if (isErrorCodeExists(reply.msg(), "FAILURE_NOT_EXISTS"))
    {
      ire.comm_status = IStatus::FAILURE_NOT_EXISTS;
    }
    else
    {
      std::cout << "LOGIN SUCCESSFUL" << std::endl;
      ire.comm_status = IStatus::SUCCESS;
    }
  }
  else
  {
    std::cout << "GRPC ERROR" << std::endl;
  }
  ire.grpc_status = status;
  return ire;
}

// Timeline Command
void Client::Timeline(const std::string &username)
{

  // ------------------------------------------------------------
  // In this function, you are supposed to get into timeline mode.
  // You may need to call a service method to communicate with
  // the server. Use getPostMessage/displayPostMessage functions
  // in client.cc file for both getting and displaying messages
  // in timeline mode.
  // ------------------------------------------------------------

  // ------------------------------------------------------------
  // IMPORTANT NOTICE:
  //
  // Once a user enter to timeline mode , there is no way
  // to command mode. You don't have to worry about this situation,
  // and you can terminate the client program by pressing
  // CTRL-C (SIGINT)
  // ------------------------------------------------------------

  /***
  YOUR CODE HERE
  ***/
}

//////////////////////////////////////////////
// Main Function
/////////////////////////////////////////////
int main(int argc, char **argv)
{

  std::string hostname = "localhost";
  std::string username = "default";
  std::string port = "3010";

  int opt = 0;
  while ((opt = getopt(argc, argv, "h:u:p:")) != -1)
  {
    switch (opt)
    {
    case 'h':
      hostname = optarg;
      break;
    case 'u':
      username = optarg;
      break;
    case 'p':
      port = optarg;
      break;
    default:
      std::cout << "Invalid Command Line Argument\n";
    }
  }

  std::cout << "Logging Initialized. Client starting...";

  Client myc(hostname, username, port);

  myc.run();

  return 0;
}
