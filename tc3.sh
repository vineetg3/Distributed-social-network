#!/bin/bash

# Command to run in Terminal 1
(gnome-terminal -- bash -c 'GLOG_logtostderr=1 ./coordinator -p 9090;read') &

# Command to run in Terminal 2
(gnome-terminal -- bash -c 'GLOG_logtostderr=1 ./tsd -c 1 -s 1 -h localhost -k 9090 -p 2000') &

# Command to run in Terminal 3
(gnome-terminal -- bash -c 'GLOG_logtostderr=1 ./tsd -c 2 -s 2 -h localhost -k 9090 -p 2001') &

# Command to run in Terminal 4
(gnome-terminal -- bash -c 'GLOG_logtostderr=1 ./tsc -h localhost -k 9090 -u 1') &

# Command to run in Terminal 5
(gnome-terminal -- bash -c 'GLOG_logtostderr=1 ./tsc -h localhost -k 9090 -u 2') &

