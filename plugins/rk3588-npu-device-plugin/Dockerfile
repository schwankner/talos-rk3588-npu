FROM golang:1.23-alpine AS builder

WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download

COPY . .
RUN CGO_ENABLED=0 GOARCH=arm64 GOOS=linux \
    go build -ldflags="-s -w" -trimpath -o /rk3588-npu-device-plugin .

FROM scratch
COPY --from=builder /rk3588-npu-device-plugin /rk3588-npu-device-plugin
ENTRYPOINT ["/rk3588-npu-device-plugin"]
