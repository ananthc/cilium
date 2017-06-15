// Copyright 2016-2017 Authors of Cilium
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package endpointmanager

import (
	"fmt"
	"sync"

	"github.com/cilium/cilium/pkg/endpoint"

	log "github.com/Sirupsen/logrus"
)

var (
	// Mutex protects Endpoints and endpointsAux
	Mutex sync.RWMutex

	// Endpoints is the global list of endpoints indexed by ID. Mutex must
	// be held to read and write.
	//
	// FIXME: This is currently exported, as more code moves from daemon
	// into pkg/endpoint, we might be able to unexport this
	Endpoints    = map[uint16]*endpoint.Endpoint{}
	endpointsAux = map[string]*endpoint.Endpoint{}
)

// LookupCiliumIDLocked looks up endpoint by endpoint ID with Mutex held
func LookupCiliumIDLocked(id uint16) *endpoint.Endpoint {
	if ep, ok := Endpoints[id]; ok {
		return ep
	}
	return nil
}

// LookupCiliumID looks up endpoint by endpoint ID
func LookupCiliumID(id uint16) *endpoint.Endpoint {
	Mutex.Lock()
	defer Mutex.Unlock()

	return LookupCiliumIDLocked(id)
}

func lookupDockerEndpointLocked(id string) *endpoint.Endpoint {
	if ep, ok := endpointsAux[endpoint.NewID(endpoint.DockerEndpointPrefix, id)]; ok {
		return ep
	}
	return nil
}

// LookupDockerID looks up endpoint by Docker ID
func LookupDockerID(id string) *endpoint.Endpoint {
	Mutex.Lock()
	defer Mutex.Unlock()

	return lookupDockerIDLocked(id)
}

func lookupDockerIDLocked(id string) *endpoint.Endpoint {
	if ep, ok := endpointsAux[endpoint.NewID(endpoint.ContainerIdPrefix, id)]; ok {
		return ep
	}
	return nil
}

func linkContainerID(ep *endpoint.Endpoint) {
	endpointsAux[endpoint.NewID(endpoint.ContainerIdPrefix, ep.DockerID)] = ep
}

// LinkContainerID links an endpoint and makes it searchable by Docker ID.
// Mutex must be held
func LinkContainerID(ep *endpoint.Endpoint) {
	linkContainerID(ep)
}

// Insert inserts the endpoint into the global maps
func Insert(ep *endpoint.Endpoint) {
	Endpoints[ep.ID] = ep

	if ep.DockerID != "" {
		linkContainerID(ep)
	}

	if ep.DockerEndpointID != "" {
		endpointsAux[endpoint.NewID(endpoint.DockerEndpointPrefix, ep.DockerEndpointID)] = ep
	}
}

// RemoveLocked removes the endpoint from the global maps. Mutex must be held
func RemoveLocked(ep *endpoint.Endpoint) {
	delete(Endpoints, ep.ID)

	if ep.DockerID != "" {
		delete(endpointsAux, endpoint.NewID(endpoint.ContainerIdPrefix, ep.DockerID))
	}

	if ep.DockerEndpointID != "" {
		delete(endpointsAux, endpoint.NewID(endpoint.DockerEndpointPrefix, ep.DockerID))
	}
}

// Lookup looks up the endpoint by prefix id
func Lookup(id string) (*endpoint.Endpoint, error) {
	Mutex.RLock()
	defer Mutex.RUnlock()

	return LookupLocked(id)
}

// LookupLocked looks up the endpoint by prefix id with the Mutex already held
func LookupLocked(id string) (*endpoint.Endpoint, error) {
	prefix, eid, err := endpoint.ParseID(id)
	if err != nil {
		return nil, err
	}

	switch prefix {
	case endpoint.CiliumLocalIdPrefix:
		n, _ := endpoint.ParseCiliumID(id)
		return LookupCiliumIDLocked(uint16(n)), nil

	case endpoint.CiliumGlobalIdPrefix:
		return nil, fmt.Errorf("Unsupported id format for now")

	case endpoint.ContainerIdPrefix:
		return lookupDockerIDLocked(eid), nil

	case endpoint.DockerEndpointPrefix:
		return lookupDockerEndpointLocked(eid), nil

	default:
		return nil, fmt.Errorf("Unknown endpoint prefix %s", prefix)
	}
}

// TriggerPolicyUpdates calls TriggerPolicyUpdates for each endpoint and
// regenerates as required. During this process, the endpoint list is locked
// and cannot be modified.
func TriggerPolicyUpdates(owner endpoint.Owner) {
	Mutex.RLock()

	for k := range Endpoints {
		go func(ep *endpoint.Endpoint) {
			policyChanges, err := ep.TriggerPolicyUpdates(owner)
			if err != nil {
				log.Warningf("Error while handling policy updates for endpoint %s\n", err)
				ep.LogStatus(endpoint.Policy, endpoint.Failure, err.Error())
			} else {
				ep.LogStatusOK(endpoint.Policy, "Policy regenerated")
			}
			if policyChanges {
				ep.Regenerate(owner)
			}
		}(Endpoints[k])
	}
	Mutex.RUnlock()
}