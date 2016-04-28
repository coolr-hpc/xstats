#!/bin/bash

sudo yum update -y
sudo yum install -y vim tmux freeipmi

git clone https://github.com/coolr-hpc/xstats.git

git config --global user.name "Kaicheng Zhang"
git config --global user.email "kaichengz@gmail.com"
