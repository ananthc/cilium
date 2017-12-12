#!/bin/bash

# This script must be launched with Jenkins only. 
export WORKSPACE="/Users/ianvernon/go/src/github.com/cilium/cilium/"
cd ${WORKSPACE}

echo "Starting runtime test VM"

NO_PROVISION=1 ./contrib/vagrant/start.sh

echo "Starting Envoy runtime test VM"
NO_PROVISION=1 CILIUM_USE_ENVOY=1 ./contrib/vagrant/start.sh

echo "Starting K8s VMs"
cd tests/k8s
VAGRANT_DEFAULT_PROVIDER=virtualbox vagrant up --provider=virtualbox --no-provision
