The latest branch is mp2.2

Compile the code using the provided makefile:

    make

To clear the directory (and remove .txt files):
   
    make clean

To run the server without glog messages (port number is optional): 

    ./tsd <-p port>
    
To run the server with glog messages: 

    GLOG_logtostderr=1 ./tsd <-p port>


To run the client without glog messages (port number and host address are optional): 

    ./tsc <-h host_addr -p port> -u user1
    
To run the server with glog messages: 

    GLOG_logtostderr=1 ./tsc <-h host_addr -p port> -u user1

# Explanation

The architecture incorporates Master-Slave pairs for each cluster, with redundant processes to ensure seamless operation in case of failures. The Coordinator process manages client requests and redirects them to active servers, while Follower Synchronizer processes handle updates to user timelines across clusters. Communication between components utilizes gRPC for efficient inter-process communication. The system includes fault detection mechanisms and automatic failover to ensure continuous service availability. The project encompasses development tasks such as implementing client-coordinator interaction, managing master-slave coordination, and synchronizing follower information across clusters. It also emphasizes robust logging practices using the glog library for effective debugging and monitoring.

## File Structure

Every server (both the slave and master) have their own directory made on bootup.

The directory is named server_{cluster_id}\_{server_id}. 

Inside the directory are user folders and 4 files namely:

- cluster_tl.txt
    This file contains all the posts done by all the users in the current cluster
    Here is a sample message by user U1. The first line of a post contains the followers of the user.
    Format is:
    *a b c
    T 2023
    U u1
    W "hello world"*
    Where, a,b,c are followers of user U1.
    This file is pulled by the home Sync Server and broadcasted  to all Sync Servers, including itself. When a sync server gets the message, it checks the first line of each post to find if the string contains users  it's currently managing. If so, it pushes the post to all the servers in the current cluster.
    
- follow_relations.txt
    This file contains the follow relations mentioned by clients in the current cluster. 
    Each line in the file is in the format "a b", where it means a follows b.
- global_users.txt
	It contains all the users of the system.
- managed_users.txt
	It contains all the users of the cluster.
## Sync Server

The sync server syncs timelines, followers and global users.

## Global users

Each cluster maintains a managed users text file(managed_users.txt), which gets populated by the master server. On Sync the managed users of a cluster are combined, removed duplicates, and sent to  the calling Sync servers. 

The sync servers would then persist to global_users.txt(present in both master and slave).

I used a "pull" approach. I.e the current server asks all the other servers to send the managed users.

## Follow relations

As mentioned, follow_relations.txt contains relations. These are picked up from all the servers in the clusters, removed duplicates and sent. The receiving sync server, would then check each line of relation and if the user is following a managed user, the user is kept under the managed user folder(present in both master and slave). More specifically the followers.txt file in that directory.

I used a "pull" approach. I.e the current server asks all the other servers to send the relations.

## Timeline

As mentioned before, cluster_tl.txt is used to post all the clusters messages. I.e whenever a post comes the master immediately persists the message to cluster_tl.txt.
These posts are broadcasted and the concerning sync server picks the message and persists(appends) to the user's timeline file (both master and slave).
After the information is pushed, the cluster_tl.txt is flushed!

Both the cluster_txt files in master and slave are checked for size and the bigger one is picked up(as it would logically be the latest, as the files are always flushed.).

I used a push approach here for easier flushing of timeline files.

------


> [!NOTE]
> - Please allow 20-25 seconds after commands like List,Follow and posting to Timeline for results to be updated as Sync server needs to sync. **This applies to within cluster as well.**
> - For Follow to work, the List command needs to display the user to follow.
>-  My code works with the delays provided in the the test case file.

------

## The server and files **interaction**

As mentioned, sync server periodically syncs the information across clusters.
Now, whenever a request comes and on boot up, information is picked up from the file system and kept in memory - very similar to previous MPs.

A small change was to have a tl_line_num variable in memory, which points to the last line read. Therefore only new messages are sent back in timeline mode.

## Master and slave servers

On boot-up, both servers try to create ls/cluster{clusterID}/master path. Whoever does it first becomes master, the other becomes slave.  
This information is sent back to the concerning servers.
The roles of the current server, is sent back with each heartbeat.
This way a server knows if its the master/slave if the other server dies.
Also, the master is sent the slave connection string(host:port), if slave exists in the HB.

On bootup, if a server is slave, it copies the files from the master directly, by simply copying and renaming the folder to its serverID.

Master forwards the requests to slave by calling the function as a client would do to master. The request is kept at top of the master functions.
The Login,Follow, List functions are forwarded.

However, i decided not to forward Timeline function because, sync server would need to find unique posts, in both master and slave cluster_tl files, which is not required, as i'm picking up the larger of the files.


# Commands

Please use these commands:
In a cluster , please try to turn on the synchronizer after the other servers are turned on - its preferable not a necessity.

Coordinator:
FLAGS_alsologtostderr=true ./coordinator -p 9000

Cluster 1:

GLOG_logtostderr=1 ./tsd -c 1 -s 1 -h localhost -k 9000 -p 2011
GLOG_logtostderr=1 ./tsd -c 1 -s 2 -h localhost -k 9000 -p 2012
GLOG_logtostderr=1  ./synchronizer -h localhost -k 9000 -p 9001 -i 1

Cluster 2:
GLOG_logtostderr=1 ./tsd -c 2 -s 1 -h localhost -k 9000 -p 2021 (this is the server that is killed in test case 3)
GLOG_logtostderr=1 ./tsd -c 2 -s 2 -h localhost -k 9000 -p 2022 
GLOG_logtostderr=1  ./synchronizer -h localhost -k 9000 -p 9002 -i 2

Cluster 3:
GLOG_logtostderr=1 ./tsd -c 3 -s 1 -h localhost -k 9000 -p 2031
GLOG_logtostderr=1 ./tsd -c 3 -s 2 -h localhost -k 9000 -p 2032
GLOG_logtostderr=1  ./synchronizer -h localhost -k 9000 -p 9003 -i 3

Users:
GLOG_logtostderr=1 ./tsc -h localhost -k 9000 -u 1
GLOG_logtostderr=1 ./tsc -h localhost -k 9000 -u 2
GLOG_logtostderr=1 ./tsc -h localhost -k 9000 -u 3
GLOG_logtostderr=1 ./tsc -h localhost -k 9000 -u 5

