#!/usr/bin/env bash
# Собирает и запускает PoC миграции testo на asio-корутины.
# Использует исходники coro и мост ПРЯМО из репозитория (с правками под asio 1.36).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

ASIO_TAG="asio-1-36-0"
echo ">> fetching $ASIO_TAG ..."
curl -sfL "https://github.com/chriskohlhoff/asio/archive/refs/tags/$ASIO_TAG.tar.gz" -o "$WORK/asio.tgz"
mkdir -p "$WORK/asio"
tar xzf "$WORK/asio.tgz" -C "$WORK/asio" --strip-components=1
ASIO_INC="$WORK/asio/asio/include"

INC="-I$ASIO_INC -I$REPO/3rd_party -I$REPO/src/testo"
FLAGS="-std=c++20 -DASIO_STANDALONE"

echo ">> building coro (from repo) ..."
OBJS=""
for f in "$REPO"/3rd_party/coro/*.cpp; do
	case "$f" in *FiberWindows.cpp) continue;; esac
	o="$WORK/$(basename "$f").o"
	g++ $FLAGS $INC -c "$f" -o "$o"
	OBJS="$OBJS $o"
done

echo ">> building PoC ..."
g++ $FLAGS $INC "$HERE/main.cpp"        $OBJS -o "$WORK/poc"        -lpthread
g++ $FLAGS $INC "$HERE/main_cancel.cpp" $OBJS -o "$WORK/poc_cancel" -lpthread

echo ">> running round-trip test:"
"$WORK/poc"
echo ">> running cancellation test:"
"$WORK/poc_cancel"
