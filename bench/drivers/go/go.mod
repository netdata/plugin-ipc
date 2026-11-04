module github.com/netdata/plugin-ipc/bench/drivers/go

go 1.27.0

require github.com/netdata/plugin-ipc/go v0.0.0

require golang.org/x/sys v0.49.0 // indirect

replace github.com/netdata/plugin-ipc/go => ../../../src/go
