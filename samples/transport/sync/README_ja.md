# 独自transportを使う同期APIの使用例

## 概要

このディレクトリの sample は [basic/sync](../../basic/sync) と同じ最小 ZMODEM CLI ですが、transport だけを独自実装します。
source/sink と TTY raw mode は `samples/support/minizm_posix.h` の helper をそのまま使い、差分を transport のみに絞っています。

想定 usage は次の通りです。

```text
minizm-sync send <local-file>
minizm-sync recv <output-dir>
```

## basic との差分

`main` 側の差分はほぼこの 2 行だけです。

```diff
- minizm_posix_fd_link_t link_state;
+ minizm_sync_transport_t link_state;

- minizm_posix_init_stdio_link(&link, &link_state);
+ minizm_sync_transport_init_stdio_link(&link, &link_state);
```

それ以外の source/sink 初期化、option 設定、`xyzm_zmodem_send()` / `xyzm_zmodem_receive()` の呼び出しは basic と同じです。

## transport 側で自前実装するもの

- `send`: `poll(2)` で writable を待ってから `write(2)` を 1 回試みる
- `recv`: `poll(2)` で readable を待ってから `read(2)` を 1 回試みる
- `now_ms`: monotonic clock を返す

`libxyzm` の同期 transport では、この 3 つを `xyzm_link_ops_t` に入れれば動作します。

## ファイル構成

1. [minizm_sync.c](minizm_sync.c) が basic sample とほぼ同じ `main()` を持ちます。
2. [minizm_sync_transport.h](minizm_sync_transport.h) が transport state と初期化関数を定義します。
3. [minizm_sync_transport.c](minizm_sync_transport.c) が `xyzm_link_ops_t` callback を実装します。

## ビルド

```sh
make
```

`make -C ../../.. all` で `libxyzm.a` / `libxyzm.so` がビルド済みであることを前提にしています。
