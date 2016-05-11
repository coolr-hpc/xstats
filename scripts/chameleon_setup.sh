#!/bin/bash

sudo yum update -y
sudo yum install -y vim tmux freeipmi lm_sensors

git clone https://github.com/coolr-hpc/xstats.git

git config --global user.name "Kaicheng Zhang"
git config --global user.email "kaichengz@gmail.com"
