# Tamazusa（tamazusa-rv64-and-a64）

T-Kernel 2.0 の上で動く Tamazusa OS の、**RISC-V（RV64）版と AArch64 版**。同じソースの木から、2つの CPU 向けに作る。Linux 互換層（musl・busybox・bash・ssh）、T88-BASIC、USB、ext2、画面とデスクトップの入口まで入っている。

| CPU・機器 | 状況 | ビルドと実行 |
|-----------|------|--------------|
| RISC-V（QEMU `virt`） | M5〜M39 合格（本線） | `make TK2=1` / `make TK2=1 qemu` |
| AArch64（QEMU `virt`・GICv3） | M5〜M39 合格 | `make -f Makefile.a64` / `make -f Makefile.a64 run` |
| Orange Pi 4 Pro（AArch64） | 起動の入口（M5〜M12）まで。実機は未確認 | `make -f Makefile.a64 BOARD=a733`（[手順](docs/dev/A733-起動手順.md)） |
| Xperia 1 II（AArch64） | 調査の段階 | — |

**動作の確認は、いまのところ QEMU の上だけ**。実機では動かしていない。実行ファイルは CPU ごとに別（RISC-V 用の `bash.elf` を AArch64 で動かすことはできない）。ソース、T88-BASIC のプログラム（`.BAS`）は、どちらの CPU でも同じ。

AArch64 版は 2026年11月16日から、同じ木に CPU とボードの部分を足して作っている。最初に一度、`tools/build_musl_a64.sh`・`build_busybox_a64.sh`・`build_bash_a64.sh`・`build_dropbear_a64.sh`（RISC-V は `*_rv64.sh`）で、Linux 用のプログラムを用意する。M16 は QEMU 9.1 より前では SKIP になる。

TRON Forum 公式の BSP ではない。
上流はビルド時に別途リンクする [T-Kernel 2.0](https://github.com/tron-forum/tkernel_2)（本ツリーに同梱しない）。

**Author:** Hideyuki Kishida  
**Licenses:** [LICENSE](LICENSE) (MIT。T88-BASIC を除く) / T88-BASIC は PolyForm Noncommercial 1.0.0（商用利用不可、[user/windows/LICENSE.txt](user/windows/LICENSE.txt)） / [NOTICE](NOTICE) (T-Kernel 2.0, T-License 2.2) / [docs/licenses/OFL-1.1.txt](docs/licenses/OFL-1.1.txt) (画面の文字 `kernel/gui/font16.c`。GNU Unifont から作成、SIL Open Font License 1.1)

---

## クイックスタート（公式 TK2 + M5、推奨）

**ホスト:** Linux または WSL2 + `riscv64-unknown-elf-gcc` + `qemu-system-riscv64`

```bash
cd tamazusa-rv64-and-a64                       # ← この README があるディレクトリ
make tk2-check                                 # 正本 tkernel_2 のパス確認
make clean && make TK2=1
make TK2=1 qemu
```

QEMU 終了: **Ctrl+A** → **x**

**成功の目印:** `ABABABAB`（または `BABABABA`）→ `[M5] COMPLETE` → `[M5b] kernel idle`

詳細ログ: [docs/BUILD.ja.md](docs/BUILD.ja.md)  
ドキュメント案内: [docs/README.md](docs/README.md)

---

## ビルドモード（このリポジトリだけ）

| モード | コマンド | 説明 |
|--------|----------|------|
| ブリングアップ | `make` / `make qemu` | Phase 1–4（TK2 未リンク、`tk2-rv64.elf`） |
| **TK2 + M5** | `make TK2=1` / `make TK2=1 qemu` | 公式 `tkernel_2` リンク + デュアルタスクテスト（構成 A: M-mode） |
| **TK2 + M5（S-mode）** | `make TK2=1 SMODE=1` / `make TK2=1 SMODE=1 qemu` | 構成 B: OpenSBI → S-mode（`0x80200000`）、`build/tk2-rv64-tk-smode.elf` |

---

## T-Kernel 2.0 ソース（正本・共通）

AArch64 ポートと **同じ上流** を使いますが、**ビルドは各リポジトリで別々**です。

```
C:\path\to\tkernel_2
```

```bash
make TK2=1 TKERNEL2_ROOT=/mnt/c/path/to/tkernel_2
```

---

## ドキュメント（すべて RV64 専用）

**まず:** [docs/README.md](docs/README.md)（何を読むか一覧）

| やりたいこと | 文書 |
|--------------|------|
| ビルド・QEMU | [docs/BUILD.ja.md](docs/BUILD.ja.md) |
| リリース・同梱物 | [docs/RELEASE.ja.md](docs/RELEASE.ja.md) |
| 仕様（v3.2→v3.3→v3.4） | [docs/spec/](docs/spec/) |
| TK2 結合のファイル構成 | [docs/TKERNEL2_INTEGRATION.md](docs/TKERNEL2_INTEGRATION.md) |
| 著作権 | [docs/COPYRIGHT_AND_PUBLISHING.en.md](docs/COPYRIGHT_AND_PUBLISHING.en.md)（正本） |
| Orange Pi 4 Pro の手順 | [docs/dev/A733-起動手順.md](docs/dev/A733-起動手順.md) |

仕様は **v3.2 + v3.3 + v3.4 を併用**（v3.2 だけでは不足）。

---

## やりたいこと別

| やりたいこと | コマンド例 |
|--------------|------------|
| RISC-V で QEMU virt | `make TK2=1 qemu` |
| AArch64 で QEMU virt | `make -f Makefile.a64 run` |
| RISC-V の設計仕様を読む | [docs/spec/](docs/spec/)（v3.2→v3.3→v3.4） |
| 公式 TK2 ソースだけ欲しい | [tron-forum/tkernel_2](https://github.com/tron-forum/tkernel_2) を clone |

--------------|----------------|------------|
| **RV64** で QEMU virt | **tk2-rv64-virt**（ここ） | `make TK2=1 qemu` |
| **AArch64** で QEMU virt | `tk2-aarch64` | `make M5B=1 run` |
| RV64 の設計仕様を読む | 同梱 [docs/spec/](docs/spec/) または `tk2-RV64-spec-rev3.1` | v3.2→v3.3→v3.4 |
| 公式 TK2 ソースだけ欲しい | `B-Free-master/archive/tkernel_2-master` | clone のみ |

---

## マイルストーン（RV64）

| Phase | 状態 | 内容 |
|-------|------|------|
| 1–3 | 完了 | UART / CLINT / 協調コンテキスト切替 |
| 4 / TK2=1 | 完了 | 公式 TK2 リンク、M5（`ABAB` + `COMPLETE`） |
| 構成 B-1（S-mode + OpenSBI） | 完了 | `make TK2=1 SMODE=1 qemu` で M5 合格 |
| 構成 B-2 第1段（Sv39 恒等マップ + 書込み保護） | 完了 | `make TK2=1 SMODE=1 qemu`（MMU は既定で有効）、M8 合格 |
| 構成 B-2 第2段（U-mode とシステムコール） | 完了 | M9 合格（ユーザープログラム 1 本、同期実行） |
| 構成 B-2 第3段（タスクごとのカーネルスタック） | 完了 | M10 合格（ユーザープログラム 2 本の同時実行、システムコール中の待ち、U-mode 実行中のタスク切替） |
| 構成 B-2 第4段（タスクごとのユーザー空間） | 完了 | M11 合格（同じ仮想アドレスが別の物理ページ、切替時に satp を入れ替え） |
| 構成 B-2 第5段（ELF の読み込み） | 完了 | M12 合格（C のユーザープログラム `user/hello` を ELF として読み込み、必要なページだけ割り当て、終了時に全解放） |
| 仮想ディスク（virtio-blk）と tar のファイル | 完了 | M13 合格（`build/disk.img` から readme.txt と hello.elf を読み、ELF をディスクから起動） |
| ファイルとプログラム起動のシステムコール | 完了 | M14 合格（ユーザープログラムが open / read / close でファイルを読み、spawn でディスクの別プログラムを起動して終了コードを受け取る） |
| シェル（キーボード入力） | 完了 | M15 合格（`tamazusa>` で ls / cat / echo / ディスクのプログラムの起動。テストのあとキーボードから使える） |
| UART の受信割込み（PLIC） | 完了 | M16 合格（キー入力を割込みで受け取り、待っているタスクを起こす。10ms おきの確認をやめた） |
| ディスクの完了割込み | 完了 | M17 合格（読み込みの完了を割込みで受け取り、待つ間はほかのタスクが動ける） |
| Tamazusa 移植 第1段（Linux 互換層） | 完了 | M18 合格（RISC-V の Linux 用に musl でコンパイルしたプログラムがそのまま動く）。計画は `docs/dev/Tamazusa移植計画.ja.md` |
| 浮動小数点・ベクトルのレジスタの保存 | 完了 | M19 合格（タスクの切替で、浮動小数点 32 本とベクトル 32 本のレジスタを退避・復元） |
| Tamazusa 移植 第2段: ファイル | 完了 | M20 合格（Linux 用のプログラムが fopen・stat・lseek・readdir などでディスクのファイルを読む） |
| Tamazusa 移植 第2段: プロセス | 完了 | M21 合格（fork・wait・パイプ・execve。子は T-Kernel のタスクで動く） |
| USB のキーボード（HID） | 完了 | M31 合格（QEMU の窓で打った文字がシェルと T88-BASIC に届く。JIS 配列） |
| USB のマウス・タブレット（HID） | 完了 | M32 合格（QEMU の窓のマウスの位置とボタンが /dev/mouse と T88-BASIC の MOUSE() に届く） |
| USB メモリ | 完了 | M35 合格（Bulk-Only Transport・SCSI。/dev/sda として読み書き。ファイルシステムは次の段階） |
| 書き込めるディスク /data | 完了 | M36 合格（2 台目のディスクの ext2。cd・相対のパス・リンク・許可。/data のプログラムも動く。日付は RTC から） |
| デスクトップの入口 | 完了 | M37 合格（画面全体 1024×768 をカーネルで組み立てる。上の帯と時計、端末の窓（シリアルの文字を VT100 として映す。漢字は GNU Unifont）、画面の窓（/dev/fb0）、マウスの矢印、窓の移動と重なり） |
| Wayland の下回り | 完了 | M38 合格（memfd と mmap の共有・mremap、UNIX ソケットと fd の受け渡し（SCM_RIGHTS）・SO_PEERCRED、epoll・timerfd・eventfd、/dev/input/event0・1（evdev）） |
| 機器の安全の決まり 6・7 | 完了 | M39・M26 合格（動かすのは root と io のグループだけ、ネットワークから・ssh からは読めるが動かせない。操作の記録を /dev/iolog と /data/log/io.log に） |
| AArch64 版 段階 A1 | 完了 | M5〜M7 合格（`make -f Makefile.a64 run`。QEMU virt・GICv3。起動・汎用タイマ・PSCI・プリエンプション） |
| AArch64 版 段階 A2 | 完了 | M8〜M12 合格（MMU・EL0 のユーザープログラム・svc・タスクごとの空間・ELF の読み込み） |
| AArch64 版 段階 A3 | 完了 | M13・M18〜M24 合格（仮想ディスク・Linux 互換層・musl・busybox・bash・シグナル・浮動小数点と NEON の退避） |
| AArch64 版 段階 A4 | 完了 | M14〜M17・M25〜M39 合格（ネットワーク・ssh・T88-BASIC・USB・ext2・画面とデスクトップ・機器の安全の決まり） |
| 実機 Orange Pi 4 Pro | 準備 | `make -f Makefile.a64 BOARD=a733` で `build-a733/Image`（M5〜M12）。手順は `docs/dev/A733-起動手順.md` |
| USB のホスト（xHCI）の土台 | 完了 | M30 合格（PCIe で xHCI を見つけ、つながったキーボードとタブレットに住所を付けて記述子を読む。入力はまだ） |
| 入出力を SoM に依存させない | 完了 | M33・M34 合格（名前の口と /etc/io.conf・上限・起動時は止まっている。USB の I/O ボード（USB シリアル + Firmata、模擬のボードで確認）。デバイスツリーで選ぶ SoM のピン（DesignWare・SiFive の GPIO、SiFive の PWM。偽のレジスタで確認）） |
| T88-BASIC の拡張・機器の制御 | 完了 | M28 合格（複数行の IF・SELECT CASE・DO LOOP・SUB・FUNCTION・ON TIMER。仮想の機器 /dev/io を SENSOR・PWM・SERVO・ESTOP・WATCHDOG で動かす。例は /bas/GREEN.BAS・ARM.BAS） |
| T88-BASIC 段階 4・画面 | 完了 | M28 合格（PSET・LINE・CIRCLE・PAINT・GET/PUT など 640×400・16 色。virtio-gpu の /dev/fb0 で QEMU の窓に出る） |
| T88-BASIC 段階 3・音の装置 | 完了 | M28 合格（PLAY の MML 6 声・BEEP。virtio-sound の /dev/dsp で鳴る。WSLg なら音が出る） |
| T88-BASIC 段階 2 | 完了 | M28 合格（ファイル: OPEN・PRINT #・INPUT #・ランダムファイル・SAVE・LOAD・KILL・NAME、エラー処理: ON ERROR GOTO・RESUME、Shift_JIS のプログラムの LOAD） |
| 書き込めるフォルダ /tmp | 完了 | M29 合格（メモリの上。作る・書く・消す・名前を変える。電源を切ると消える） |
| T88-BASIC 段階 1 | 完了 | M28 合格（N88-BASIC 互換のインタプリタ。`basic` で起動。行番号・FOR・GOSUB・配列・DATA・PRINT USING・LOCATE・COLOR・INKEY$ など。サンプルは `/bas`） |
| リアルタイム性 | 測定 | M27 合格（Linux のプログラムが重い仕事をしていても、優先度の高い T-Kernel のタスクは QEMU の上で最大 0.2 ミリ秒ほどで動く。ディスクのキャッシュでテスト全体が 8 分から 17 秒に） |
| ssh・sshd | 完了 | M26 合格（Dropbear。パソコンから `ssh -p 2222 root@localhost` でログイン、疑似端末つきの bash。パスワードは tamazusa） |
| ネットワーク | 完了 | M25 合格（virtio-net・IPv4・TCP・UDP・ソケット。ping・wget・httpd が動く。TCP は x86_64 版のものを使う） |
| Tamazusa 移植 第3段: bash | 完了 | M24 合格（GNU bash 5.2 が動き、テストのあとのシェルも bash。#! のスクリプト・/bin/ls なども使える） |
| Tamazusa 移植 第3段: シグナル | 完了 | M23 合格（Ctrl-C・trap・kill・ジョブ（&・jobs）。busybox は全コマンド約 400） |
| Tamazusa 移植 第2段: busybox | 完了 | M22 合格（busybox の ash が動き、ls・cat・パイプ・外のプログラムが使える。テストのあとキーボードから `tamazusa#` で使える） |
