// Copyright 2020 Istio Authors
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

package driver

import (
	"context"
	"fmt"
	"log"

	"google.golang.org/grpc"
	"google.golang.org/grpc/status"

	"istio.io/proxy/test/envoye2e/env"
	"istio.io/proxy/test/envoye2e/env/grpc_echo"
)

type GrpcServer struct {
	grpcBackend *env.GRPCServer
}

var _ Step = &GrpcServer{}

func (g *GrpcServer) Run(p *Params) error {
	g.grpcBackend = env.NewGRPCServer(p.Ports.BackendPort)
	log.Printf("Starting GRPC echo server")
	errCh := g.grpcBackend.Start()
	if err := <-errCh; err != nil {
		return fmt.Errorf("not able to start GRPC server: %v", err)
	}
	return nil
}

func (g *GrpcServer) Cleanup() {
	g.grpcBackend.Stop()
}

var _ Step = &GrpcCall{}

type GrpcCall struct {
	ReqCount   int
	WantStatus *status.Status
}

func (g *GrpcCall) Run(p *Params) error {
	proxyAddr := fmt.Sprintf("127.0.0.1:%d", p.Ports.ClientPort)
	conn, err := grpc.Dial(proxyAddr, grpc.WithInsecure(), grpc.WithBlock())
	if err != nil {
		return fmt.Errorf("could not establish client connection to gRPC server: %v", err)
	}
	defer conn.Close()
	client := grpc_echo.NewEchoClient(conn)

	for i := 0; i < g.ReqCount; i++ {
		_, grpcErr := client.Echo(context.Background(), &grpc_echo.EchoRequest{ReturnStatus: g.WantStatus.Proto()})
		fromErr, ok := status.FromError(grpcErr)
		if ok && fromErr.Code() != g.WantStatus.Code() {
			return fmt.Errorf("failed GRPC call: %#v (code: %v)", grpcErr, fromErr.Code())
		}
		fmt.Printf("successfully called GRPC server and get status code %+v\n", fromErr)
	}
	return nil
}

func (g *GrpcCall) Cleanup() {}
