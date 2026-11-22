# ビルド手順（tk2-rv64-virt）

**検証済み**: QEMU `virt` / RV64 / M-mode / `make TK2=1 qemu`（公式 T-Kernel 2.0 リンク + M5）。

設計正本（同梱）: [`spec/TK2-RV64-SPEC-v3.2.md`](spec/TK2-RV64-SPEC-v3.2.md)  
仕様の読み方: [`spec/README.md`](spec/README.md)  
※ 兄弟ディレクトリで仕様を編集する場合: `../tk2-RV64-spec-rev3.1/`

## 必要環境

| 項目 | 内容 |
|------|------|
| OS | Ubuntu 22.04+ / WSL2 |
| コンパイラ | `riscv64-unknown-elf-gcc` |
| QEMU | `qemu-system-riscv64`（`qemu-system-misc` パッケージ） |
| 上流 TK2 | `B-Free-master/archive/tkernel_2-master`（正本） |

```bash
sudo apt update
sudo apt install -y gcc-riscv64-unknown-elf qemu-system-misc git
```

## ディレクトリ例

```
Desktop/
  tk2-rv64-virt/                              ← 本リポジトリ（RV64 ポート）
  B-Free-master/archive/tkernel_2-master/      ← 公式 T-Kernel 2.0（正本）
  tk2-RV64-spec-rev3.1/                       ← RV64 仕様書（編集正本・任意）
```

```bash
cd ~/tamazusa-rv64-and-a64
```

## ビルドモード

| モード | コマンド | 出力 | 用途 |
|--------|----------|------|------|
| ブリングアップ | `make` | `tk2-rv64.elf` | Phase 1–4（UART / CLINT / 協調スケジューラ） |
| **公式 TK2** | `make TK2=1` | `build/tk2-rv64-tk.elf` | 公式 `tkernel_2` リンク + M5 テスト（**推奨**） |

### 上流ソースの確認

```bash
make tk2-check
# または
make TK2=1 TKERNEL2_ROOT=/mnt/c/path/to/tkernel_2
```

公式ツリーが無い場合:

```bash
make clone-tkernel2
```

### ブリングアップ（TK2 未リンク）

```bash
make clean && make
make qemu
```

終了: **Ctrl+A** → **x**

### 公式 TK2 リンク + QEMU

```bash
make clean && make TK2=1
make TK2=1 qemu
```

## 成功時の UART（抜粋）

```
[virt_rv64] QEMU virt RV64
[board_init] UART OK
[board_init] CLINT timer OK
[board_init] IRQ enabled
Hello RV64 TK2
Running at M-mode
[M5b] init cpu... OK
...
[M5] tk_sta_tsk OK, dispatching...
[task_a] start
[task_b] start
ABABABAB
[task_a] done
[task_b] done
[M5] COMPLETE (returned to idle)
[M5b] kernel idle (no runnable tasks)
```

先頭の交互文字が `B` から始まる（`BABABABA`）ことはあります。

## よくある問題

| 現象 | 対処 |
|------|------|
| `basic.h` not found | `TKERNEL2_ROOT` を正本パスに合わせる（`make tk2-check`） |
| `offset.h` 生成失敗 | `make clean && make TK2=1` |
| `riscv64-unknown-elf-gcc` 無し | `sudo apt install gcc-riscv64-unknown-elf` |
| QEMU が止まらない | Ctrl+A → x |
| 古い ELF | `make clean` 後に再ビルド |

## 関連ドキュメント

| ファイル | 内容 |
|----------|------|
| [README.md](README.md) | ドキュメント全体のナビ |
| [spec/README.md](spec/README.md) | 仕様書の読む順序 |
| [TKERNEL2_INTEGRATION.md](TKERNEL2_INTEGRATION.md) | `TK2=1` の結合構成（ファイル一覧） |
| [COPYRIGHT_AND_PUBLISHING.en.md](COPYRIGHT_AND_PUBLISHING.en.md) | 著作権・公開（英語正本） |
| [COPYRIGHT.ja.md](COPYRIGHT.ja.md) | 著作権・公開（日本語概要） |

別 CPU の参考: `../tk2-aarch64`（`make M5B=1 run`）。RV64 は **`TK2=1`** です。

## 構成 B（S-mode + OpenSBI）

```bash
make clean && make TK2=1 SMODE=1
make TK2=1 SMODE=1 qemu
```

- QEMU は `-bios default`（内蔵 OpenSBI）で起動し、カーネルは `0x80200000` に置かれる。
- 成功の目印: OpenSBI のバナー → `Domain0 Next Mode : S-mode` → `Running at S-mode` → `[M5] COMPLETE` → `[M6] PASS` → `[M7] PASS` → `[TEST] ALL PASS`
- `[M6] PASS` は M-mode（`make TK2=1 qemu`）でも出る。500ms のタイマ待ちとシステム時刻を確認するテスト。
- `[M7] PASS` は割込みによるタスク切替（プリエンプション）の確認。
- `SMODE` を切り替えると、Makefile が自動でオブジェクトを消して作り直す。

## テスト後の自動終了（TEST_EXIT）

- 既定（`TEST_EXIT=1`）: M5・M6 のあと `[TEST] ALL PASS` を出して QEMU が自動で終了する。
  - 成功なら QEMU の終了コード 0、失敗なら 1（`echo $?` で確認できる）。
  - M-mode は QEMU の終了用デバイス（sifive_test, 0x100000）、S-mode は SBI SRST で終了。
- 終了させずに idle で待たせたいとき: `make TK2=1 TEST_EXIT=0 qemu`（止めるには別の端末で `pkill -f qemu-system-riscv64`）

## 構成 B-2（Sv39 MMU）

- `SMODE=1` のとき Sv39 ページングが既定で有効（`MMU=1`）。カーネルは恒等マップ（仮想 = 物理）で動く。
- 成功の目印: `[board_init] Sv39 MMU OK satp=0x8000...` → … → `[M8] PASS` → `[TEST] ALL PASS`
- ページングなしの S-mode と比べたいとき: `make TK2=1 SMODE=1 MMU=0 qemu`
- `MMU=1` は M-mode（`SMODE=0`）では使えない（Makefile がエラーで止める）。
- MMU=1 では M9（U-mode のユーザープログラム）も走る。成功の目印: `[user] hello from U-mode (via ecall)` → `[M9] PASS`
- 続いて M10（ユーザープログラムの同時実行）が走る。成功の目印: `[M10] SYS_PUTS order (task id): 5 6 5 6 5 6` → `[M10] PASS`
- 続いて M11（タスクごとのユーザー空間）が走る。成功の目印: `[M11] exit A=0x11 B=0x22` → `[M11] PASS`
- 続いて M12（ELF のユーザープログラム）が走る。成功の目印: `[elf] hello from an ELF user program (.data/.bss OK)` → `[M12] PASS`
- ユーザープログラム（`user/`）はカーネルとは別に `build/user/hello.elf` としてビルドされ、カーネルに埋め込まれる（M12 用）。

## 仮想ディスク（M13、全構成）

- `make TK2=1 ...` は `build/disk.img` も作る。中身は tar（ustar）で、`hello.elf` と `user/disk/readme.txt` が入る（GNU tar を使う）。
- `make ... qemu` はこのディスクを virtio-blk として付ける（`-global virtio-mmio.force-legacy=false` で新しい形式にする）。
- 成功の目印: `[M13] readme.txt: B-Free disk test: ...` → `[M13] PASS`
- QEMU を手で起動するときは、次を付ける:
  `-global virtio-mmio.force-legacy=false -drive file=build/disk.img,if=none,format=raw,id=hd0,readonly=on -device virtio-blk-device,drive=hd0`
  付けないと `[virtio-blk] no disk` と出て M13 が不合格になる。
- ディスクは読み取り専用で付ける（`readonly=on`）。前の QEMU が残っていても「write lock」の衝突にならない。
- MMU=1 では続いて M14（ファイルとプログラム起動のシステムコール）が走る。成功の目印: `[fsdemo] all file/spawn checks done` → `[M14] PASS`
- ディスクには `fsdemo.elf`（`user/fsdemo`）も入る。

## シェル（M15、MMU=1）

- テスト M15 は、用意した入力でシェル（`sh.elf`）を動かして出力を確かめる。成功の目印: `[M15] PASS`
- **`make TK2=1 SMODE=1 qemu` は、テストのあとシェルが起動してキーボード入力を待つ。** `tamazusa>` と出たら打てる:
  - `help` 使い方、`ls` ファイル一覧、`echo 文字列`、`cat readme.txt`、`hello`（ディスクの hello.elf を起動）
  - `exit`（または行の先頭で Ctrl-D）でシェルが終わり、QEMU も自動で終わる
- テストだけで自動で終わらせたいとき: `make TK2=1 SMODE=1 RUN_SH=0 qemu`
- M16（UART の受信割込み）は、UART を折り返し（ループバック）にして自分で送った文字を受け取る。成功の目印: `[M16] PASS`。テスト中の数ミリ秒、画面への表示が止まるのは折り返しのため。
- M17（ディスクの完了割込み）。成功の目印: `[M17] PASS`
- Makefile はヘッダの依存関係を `.d` に書き出す（`-MMD -MP`）。ヘッダを変えると、使っている所が自動で作り直される。この設定より前に作った `build/` には `.d` がないので、一度だけ `make clean` してから作り直す。

## RISC-V の Linux 用プログラム（M18、MMU=1）

Tamazusa の Linux 互換層で、RISC-V の Linux 用にコンパイルしたプログラムを動かす。musl を一度だけ用意する:

```
sudo apt install gcc-riscv64-linux-gnu
bash tools/build_musl_rv64.sh
```

- できた `toolchain/musl-rv64/bin/musl-gcc` を Makefile が自動で使い、`user/linux/*.c` を静的リンクしてディスクに入れる
- 成功の目印: `Hello from musl on Tamazusa-RV: 42` … → `[M18] PASS`
- musl を用意していないときは `[M18] SKIP` と出る（不合格にはしない）
- M19（浮動小数点・ベクトルのレジスタの保存）も musl で作るプログラム（`fpvec_test`）を使う。成功の目印: `fpvec 11: 20 rounds, registers kept (FP + vector)` → `[M19] PASS`
- M20（Linux 互換層のファイル）も musl で作るプログラム（`fstest`）を使う。成功の目印: `fstest: all 19 checks passed` → `[M20] PASS`
- M21（Linux 互換層のプロセス）も musl で作るプログラム（`proctest`）を使う。成功の目印: `proctest: all 16 checks passed` → `[M21] PASS`
- M22（busybox の ash）は busybox を一度だけ作っておく: `bash tools/build_busybox_rv64.sh`（musl のあと。GitHub から busybox 1.36.1 を取ってくる）。できた `toolchain/busybox-rv64/busybox` を Makefile がディスクに `busybox.elf` として入れる。成功の目印: `[M22] PASS`。busybox がなければ `[M22] SKIP`
- M23（シグナル）も musl で作るプログラム（`sigtest`）を使う。成功の目印: `sigtest: all checks passed` → `[M23] PASS`
- busybox は約 400 のコマンドの全部入り（`busybox` と打つと一覧が出る）。前に作った人は `rm -rf toolchain/busybox-src` してから `bash tools/build_busybox_rv64.sh` をもう一度
- M24（bash）は bash を一度だけ作っておく: `bash tools/build_bash_rv64.sh`（musl のあと。GNU の配布場所から bash 5.2.37 を取ってくる）。成功の目印: `[M24] PASS`。bash がなければ `[M24] SKIP`
- M25（ソケット）は 127.0.0.1 だけで確かめる（`nettest`）。成功の目印: `nettest: all checks passed` → `[M25] PASS`
- QEMU には仮想 LAN（QEMU のユーザーモード）が付く。Tamazusa は 10.0.2.15、パソコンは 10.0.2.2 に見える。`ping 10.0.2.2`、`wget http://10.0.2.2:8000/`（パソコンで `python3 -m http.server 8000` を動かしておく）。パソコンの 8080 番は Tamazusa の 80 番へ（`httpd -p 80 -h /` を動かして、パソコンのブラウザで http://localhost:8080/readme.txt）
- M26（疑似端末と ssh）: 疑似端末は musl で作るプログラム（`ptytest`）で確かめる。ssh の部分は Dropbear を一度だけ作っておく: `bash tools/build_dropbear_rv64.sh`（musl のあと。GitHub から Dropbear 2024.86 を取り、ホスト鍵も作る）。成功の目印: `[M26] PASS`
- ssh でログインするには: Tamazusa のシェルで `sshd` と打つ（後ろで動く）。パソコンの WSL で `ssh -p 2222 root@localhost`、パスワードは `tamazusa`。初めてのときはホスト鍵の確認に `yes` と答える
- bash があると、テストのあとのシェルは bash になる（`sh` と打てば busybox の ash）
- busybox があると、テストのあと ash が起動する（プロンプト `tamazusa# `）。`exit` で終わる。musl を前に作った人は、Linux のヘッダを写すために `bash tools/build_musl_rv64.sh` をもう一度動かす
- QEMU の CPU は RVA23 にしている（Ubuntu 25.10 以降の RISC-V 用コンパイラは RVA23 を前提にし、Zcb・Zba・ベクトルなどの命令を使うため）。QEMU 10 以降は `-cpu rva23s64`、古い QEMU では主な拡張命令を一つずつ有効にした `rv64` を Makefile が自動で選ぶ。別の CPU を使うときは `make ... QEMU_CPU=rv64 qemu`
- 新しいプログラムは `user/名前/名前.c` に置き、Makefile の `USER_PROGS` に名前を足すと、ディスクに `名前.elf` として入り、シェルから `名前 引数` で起動できる。
- 予期しない例外が起きたときは原因（cause）・場所（epc）・アドレス（tval）を表示し、QEMU を終了コード 1 で終える。
