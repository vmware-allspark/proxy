module istio.io/proxy

go 1.12

replace cloud.google.com/go/meshtelemetry/v1alpha1 v0.0.0 => ./test/envoye2e/stackdriver_plugin/edges

require (
	cloud.google.com/go/meshtelemetry/v1alpha1 v0.0.0
	github.com/cncf/udpa/go v0.0.0-20200327203949-e8cd3a4bb307
	github.com/d4l3k/messagediff v1.2.2-0.20180726183240-b9e99b2f9263
	github.com/envoyproxy/go-control-plane v0.9.5
	github.com/ghodss/yaml v1.0.0
	github.com/golang/protobuf v1.4.1
	github.com/kr/pretty v0.1.0 // indirect
	github.com/prometheus/client_model v0.2.0
	github.com/prometheus/common v0.9.1
	golang.org/x/net v0.0.0-20200324143707-d3edc9973b7e // indirect
	golang.org/x/sys v0.0.0-20200331124033-c3d80250170d // indirect
	golang.org/x/text v0.3.2 // indirect
	google.golang.org/genproto v0.0.0-20200526211855-cb27e3aa2013
	google.golang.org/grpc v1.28.0
	google.golang.org/protobuf v1.25.0 // indirect
	gopkg.in/check.v1 v1.0.0-20180628173108-788fd7840127 // indirect
	gopkg.in/yaml.v2 v2.2.8 // indirect
)
