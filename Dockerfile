# --- build BPF ---
FROM debian:bookworm-slim AS bpf
RUN apt-get update && apt-get install -y clang llvm bpftool linux-libc-dev linux-headers-amd64 libbpf-dev
COPY bpf /src
WORKDIR /src
RUN clang -O2 -target bpf -I/usr/include/x86_64-linux-gnu -c tail_lifter.bpf.c -o tail_lifter.bpf.o

# --- build Go controller ---
FROM golang:1.22 AS go
WORKDIR /app
COPY go.mod go.sum ./
RUN go mod download
COPY cmd/tail-lifter/main.go ./main.go
COPY hack ./hack
RUN CGO_ENABLED=0 GOOS=linux go build -o tail-lifter ./main.go

# --- runtime ---
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y iproute2 bpftool && rm -rf /var/lib/apt/lists/*
COPY --from=bpf /src/tail_lifter.bpf.o /opt/
COPY --from=go /app/tail-lifter /usr/bin/
COPY --chmod=755 hack/install.sh /install.sh
ENTRYPOINT ["/install.sh"]