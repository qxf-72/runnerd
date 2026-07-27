#!/usr/bin/env bash

set -u

if [[ $# -ne 2 ]]; then
  echo "Usage: $0 <runnerd> <runnerctl>" >&2
  exit 2
fi

runnerd_path=$1
runnerctl_path=$2
client_count=20

# 每次测试使用独立目录和 socket，避免与正在运行的 runnerd 冲突。
if ! test_dir=$(mktemp -d /tmp/runnerd-integration.XXXXXX); then
  echo "failed to create temporary test directory" >&2
  exit 1
fi

socket_path="$test_dir/runnerd.sock"
server_log="$test_dir/runnerd.log"

server_pid=""
client_pids=()

cleanup() {
  # 测试中途失败时，先结束仍在运行的客户端。
  for client_pid in "${client_pids[@]}"; do
    if [[ -n "$client_pid" ]] && kill -0 "$client_pid" 2>/dev/null; then
      kill "$client_pid" 2>/dev/null || true
    fi

    if [[ -n "$client_pid" ]]; then
      wait "$client_pid" 2>/dev/null || true
    fi
  done

  # runnerd 当前没有优雅退出命令，因此测试结束时发送 SIGTERM。
  if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill "$server_pid" 2>/dev/null || true
  fi

  if [[ -n "$server_pid" ]]; then
    wait "$server_pid" 2>/dev/null || true
  fi

  # test_dir 一定来自上面的 mktemp，只删除本次测试创建的目录。
  if [[ -n "$test_dir" && -d "$test_dir" ]]; then
    rm -r -- "$test_dir"
  fi
}

trap cleanup EXIT
trap 'exit 1' INT TERM

"$runnerd_path" --socket "$socket_path" >"$server_log" 2>&1 &
server_pid=$!

# 等待监听 socket 出现，同时检查服务端有没有提前退出。
server_ready=0
for ((attempt = 0; attempt < 100; ++attempt)); do
  if [[ -S "$socket_path" ]]; then
    server_ready=1
    break
  fi

  if ! kill -0 "$server_pid" 2>/dev/null; then
    echo "runnerd exited before creating the socket" >&2
    sed -n '1,80p' "$server_log" >&2
    exit 1
  fi

  sleep 0.02
done

if [[ $server_ready -ne 1 ]]; then
  echo "timed out waiting for runnerd socket" >&2
  exit 1
fi

# 同时启动 20 个客户端，每个客户端分别保存标准输出和错误输出。
for ((index = 1; index <= client_count; ++index)); do
  "$runnerctl_path" --socket "$socket_path" ping \
    >"$test_dir/client-$index.out" \
    2>"$test_dir/client-$index.err" &

  client_pids+=("$!")
done

test_failed=0

for ((index = 1; index <= client_count; ++index)); do
  array_index=$((index - 1))
  client_pid=${client_pids[$array_index]}

  if ! wait "$client_pid"; then
    echo "client $index exited with an error" >&2
    sed -n '1,40p' "$test_dir/client-$index.err" >&2
    test_failed=1
  fi

  # 已经 wait 的 PID 不再交给 cleanup，避免 PID 被系统复用后误伤其他进程。
  client_pids[$array_index]=""

  client_output=$(<"$test_dir/client-$index.out")

  if [[ "$client_output" != "PONG" ]]; then
    echo "client $index returned '$client_output', expected 'PONG'" >&2
    test_failed=1
  fi
done

if ! kill -0 "$server_pid" 2>/dev/null; then
  echo "runnerd exited while handling concurrent clients" >&2
  test_failed=1
fi

if [[ $test_failed -ne 0 ]]; then
  echo "runnerd log:" >&2
  sed -n '1,120p' "$server_log" >&2
  exit 1
fi

echo "all $client_count concurrent clients received PONG"
