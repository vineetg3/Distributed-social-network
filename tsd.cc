/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <ctime>

#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/duration.pb.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <stdlib.h>
#include <unistd.h>
#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>
#include <glog/logging.h>
#define log(severity, msg) \
  LOG(severity) << msg;    \
  google::FlushLogFiles(google::severity);

#include "sns.grpc.pb.h"

using csce438::ListReply;
using csce438::Message;
using csce438::Reply;
using csce438::Request;
using csce438::SNSService;
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

struct Client
{
  std::string username;
  bool connected = true;
  int following_file_size = 0;
  std::vector<Client *> client_followers;
  std::vector<Client *> client_following;
  ServerReaderWriter<Message, Message> *stream = 0;
  bool operator==(const Client &c1) const
  {
    return (username == c1.username);
  }
};

// Vector that stores every client that has been created
std::vector<Client> client_db;

Client *getUser(string username)
{
  Client *c;
  for (int i = 0; i < client_db.size(); i++)
  {
    // check if user name is in client db
    c = &client_db[i];
    if (c->username == username)
    {
      return c;
    }
  }
  return NULL;
}

class SNSServiceImpl final : public SNSService::Service
{

  Status List(ServerContext *context, const Request *request, ListReply *list_reply) override
  {
    Client *curUser;
    string name = request->username();
    curUser = getUser(name);
    Client c;
    for (int i = 0; i < client_db.size(); i++)
    {
      list_reply->add_all_users(client_db[i].username);
    }
    for (int i = 0; i < curUser->client_followers.size(); i++)
    {
      list_reply->add_followers(curUser->client_followers[i]->username);
    }
    return Status::OK;
  }

  Status Follow(ServerContext *context, const Request *request, Reply *reply) override
  {
    Client *curUser;
    Client *userToFollow;
    string name = request->username();
    string userNameToFollow = request->arguments(0);
    curUser = getUser(name);
    userToFollow = getUser(userNameToFollow);
    if (userToFollow == NULL)
    {
      reply->set_msg("FAILURE_INVALID_USERNAME: User " + userNameToFollow + " does not exist.");
      return Status::OK;
    }
    // check if curUser is following userToFollow, else add to following list
    int flwFlag = 0;
    for (int i = 0; i < curUser->client_following.size(); i++)
    {
      // implicit conversion of ptr userToFollow to reference
      if (curUser->client_following[i] == userToFollow)
      {
        flwFlag++;
      }
    }
    if (!flwFlag)
    {
      curUser->client_following.push_back(userToFollow);
    }
    // add curUser to userToFollow's followers list
    flwFlag = 0;
    for (int i = 0; i < userToFollow->client_followers.size(); i++)
    {
      if (userToFollow->client_followers[i] == curUser)
      {
        flwFlag++;
      }
    }
    if (!flwFlag)
    {
      userToFollow->client_followers.push_back(curUser);
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
    curUser = getUser(name);
    userToUnFollow = getUser(userNameToUnFollow);
    if (userToUnFollow == NULL)
    {
      reply->set_msg("FAILURE_INVALID_USERNAME: User " + userNameToUnFollow + " does not exist.");
      return Status::OK;
    }
    // if curUser is following userToFollow, remove from following array, else return error
    int flwidx = -1;
    for (int i = 0; i < curUser->client_following.size(); i++)
    {
      if (curUser->client_following[i] == userToUnFollow)
      {
        flwidx = i;
      }
    }
    if (flwidx == -1)
    {
      reply->set_msg("FAILURE_NOT_A_FOLLOWER: Failed with not a follower.");
      return Status::OK;
    }
    // remove user to unfollow
    curUser->client_following.erase(curUser->client_following.begin() + flwidx);
    // if userToUnfollow has follower curUser, remove from  array, else return error
    flwidx = -1;
    for (int i = 0; i < userToUnFollow->client_followers.size(); i++)
    {
      if (userToUnFollow->client_followers[i] == curUser)
      {
        flwidx = i;
      }
    }
    if (flwidx == -1)
    {
      reply->set_msg("FAILURE_NOT_A_FOLLOWER: Failed with not a follower.");
      return Status::OK;
    }
    userToUnFollow->client_followers.erase(userToUnFollow->client_followers.begin() + flwidx);
    reply->set_msg(name + " unfollowed " + userNameToUnFollow);
    return Status::OK;
  }

  // RPC Login
  Status Login(ServerContext *context, const Request *request, Reply *reply) override
  {
    Client *c;
    string name = request->username();
    c = getUser(name);
    if (c == NULL)
    {
      // client doesn't exist
      c = new Client;
    }
    else
    {
      reply->set_msg("FAILURE_NOT_EXISTS: User already exists.");
      return Status::OK;
    }
    c->username = name;
    client_db.push_back((*c));
    cout << "User " + name + " is connected." << endl;
    return Status::OK;
  }

  Status Timeline(ServerContext *context,
                  ServerReaderWriter<Message, Message> *stream) override
  {

    /*********
    YOUR CODE HERE
    **********/

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
  std::cout << "Server listening on " << server_address << std::endl;
  log(INFO, "Server listening on " + server_address);

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

  std::string log_file_name = std::string("server-") + port;
  google::InitGoogleLogging(log_file_name.c_str());
  log(INFO, "Logging Initialized. Server starting...");
  RunServer(port);

  return 0;
}
