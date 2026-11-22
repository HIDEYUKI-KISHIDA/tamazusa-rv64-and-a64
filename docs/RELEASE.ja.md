# リリースノート — Tamazusa（tk2-rv64-virt）

**版:** 0.1.0  
**日付:** 2026-11-22  
**検証:** QEMU の上だけ。実機では動かしていない。

---

## 概要

RISC-V RV64 向けの T-Kernel 2.0 コミュニティポート。T-Kernel の上に、Linux 互換層（musl・busybox・bash・ssh）、T88-BASIC、USB、ext2、画面とデスクトップの入口までを載せた。QEMU `virt` で、M5〜M39 の試験が合格している。

同じソースに AArch64 の部分も足してあり、QEMU `virt`（GICv3）で同じ試験が合格している。Orange Pi 4 Pro 向けは、起動の入口（M5〜M12）までで、実機での確認は済んでいない。

TRON Forum 公式の BSP ではない。

---

## 含むもの・含まないもの

| 種別 | 内容 |
|------|------|
| 含む | カーネル、ポートのコード、`user/`（シェル・T88-BASIC・試験用プログラム）、`docs/`、ビルド用スクリプト |
| 含まない | `tkernel_2` 本体（`TKERNEL2_ROOT` で別に指定する） |
| 含まない | `toolchain/`（musl・busybox・bash・Dropbear のソースと作った実行ファイル。`tools/build_*.sh` で作る） |
| 含まない | NEC の ROM・ディスクのダンプ・マニュアルの文章、参照資料のファイル |

ディスクイメージや ISO を配る場合は、中に busybox・bash（GPL）などが入る。配布のときの決まりは [COPYRIGHT.ja.md](COPYRIGHT.ja.md) と [NOTICE](../NOTICE) を見る。

---

## ライセンス

| 対象 | ライセンス |
|------|-----------|
| 本リポジトリの独自部分 | MIT（[LICENSE](../LICENSE)） |
| T88-BASIC | PolyForm Noncommercial 1.0.0（商用利用不可。研究・教育・個人利用） |
| 画面の文字（GNU Unifont） | SIL OFL 1.1 |
| T-Kernel 2.0 | T-License 2.2 |

T88-BASIC は N88-BASIC 互換の独立した実装。NEC の資料を互換のために参照したが、文章・コード・ROM・ダンプは写していない。NEC とは無関係。来歴は [T88-BASIC-拡張.ja.md](T88-BASIC-拡張.ja.md) の第8章。

---

## ビルド（最短）

RISC-V（`riscv64-unknown-elf-gcc` と `qemu-system-riscv64`）:

```bash
make tk2-check
make TK2=1
make TK2=1 qemu
```

成功の目印: `ABABABAB` → `[M5] COMPLETE` → `[M5b] kernel idle`

AArch64（`aarch64-linux-gnu-gcc` と `qemu-system-aarch64`）:

```bash
make -f Makefile.a64 run
```

Linux 用のプログラム（musl・busybox・bash・Dropbear）は、最初に一度 `tools/build_*.sh` で作る。詳細は [BUILD.ja.md](BUILD.ja.md)。

---

## ssh について

- sshd（Dropbear）は、起動時には動かない。使う人が `sshd` を自分で動かす。
- イメージには SSH のホスト鍵を入れていない。`sshd` を最初に動かしたときに、書き込めるディスク `/data/etc/dropbear/` に作る。電源を切っても残る。
- ログインのパスワードは、いまは固定の `tamazusa`。実機やネットワークにつなぐ前に変えること。
- QEMU では、パソコンの `localhost:2222` からだけつながる。

---

## 既知の制限

- 実機での確認は未実施（RISC-V の JH7110・K3、Orange Pi 4 Pro、Xperia 1 II とも）。
- メモリ上の `/tmp` では、ハードリンク（`link()`）がまだ使えない。ssh のホスト鍵を `/tmp` に作れないのは、このため。
- 試験は T-Kernel のポート内の試験。TRON 公式の製品試験一式は未実施。
- Orange Pi 4 Pro は起動の入口（M5〜M12）まで。USB シリアルでの確認待ち。
- Xperia 1 II は調査の段階。実機のデータを読んだだけで、動かしていない。
- 機器の制御（T88-BASIC の ESTOP・PWM など）は、仮想の機器 `/dev/io` での確認だけ。実際の機器をつなぐ前に、安全の決まり（`docs/` の該当章）を確認すること。
- `wait.h` の `offsetof` など、upstream が 32bit 前提のため、コンパイルの警告が出る。

---

## 関連

- 仕様: [spec/README.md](spec/README.md)
- Orange Pi 4 Pro: [dev/A733-起動手順.md](dev/A733-起動手順.md)
