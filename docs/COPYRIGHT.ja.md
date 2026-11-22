# 著作権・公開ガイド（tk2-rv64-virt）

**対象:** RISC-V RV64 向け T-Kernel 2.0 コミュニティポート（QEMU `virt`）  
**作者:** Hideyuki Kishida  
**英語版（正本）:** [COPYRIGHT_AND_PUBLISHING.en.md](COPYRIGHT_AND_PUBLISHING.en.md)

> 公開前チェックリストおよび詳細は **英語版（正本）** を参照してください。  
> 本ファイルは日本語での概要です。

## 法的ファイル

| ファイル | 内容 |
|----------|------|
| [LICENSE](../LICENSE) | 本リポジトリのポートコード — **MIT** |
| [NOTICE](../NOTICE) | リンクする T-Kernel 2.0 — **T-License 2.2** |

## 誰が何を持つか

| 素材 | 著作権 / ライセンス |
|------|---------------------|
| RV64 ポート（`kernel/`, `include/`, `docs/` 等） | Hideyuki Kishida（MIT） |
| ビルド時にリンクする `tkernel_2` | TRON Forum 等（T-License 2.2） |
| 生成バイナリ `tk2-rv64-tk.elf` | 上記の組み合わせ |

## 注意事項

- T-Kernel 2.0 全体の著作権を主張しない。
- `tkernel_2` をリポジトリに同梱する場合は T-License 2.2 に従う（本プロジェクトは **同梱せず `TKERNEL2_ROOT` で参照**）。
- TRON Forum 公式 BSP ではないと明記する。

## 上流ソース

| 項目 | 内容 |
|------|------|
| 正本 | `B-Free-master/archive/tkernel_2-master` |
| 公開 clone | https://github.com/tron-forum/tkernel_2 |
| 公式 CPU ポート | **ARM em1d のみ**（RV64 はコミュニティ研究） |

## tk2-aarch64 との関係

`tk2-aarch64` は **別 CPU・別リポジトリ** です。本文書は **RV64 専用** です。

## ライセンスの適用範囲（まとめ）

| 対象 | ライセンス | 備考 |
|------|-----------|------|
| カーネル、ポートコード、`docs/` など（本リポジトリの独自部分） | MIT（`LICENSE`） | |
| T88-BASIC（`user/windows/`、`user/linux/basic.c` ほか） | PolyForm Noncommercial 1.0.0 | 商用利用不可。研究・教育・個人利用のみ。全文は `user/windows/LICENSE.txt` と `docs/licenses/` |
| 画面フォント（GNU Unifont） | SIL OFL 1.1 | `docs/licenses/OFL-1.1.txt` |
| T-Kernel 2.0（`tkernel_2`） | T-License 2.2 | 同梱せず `TKERNEL2_ROOT` で参照 |
| musl / Dropbear | MIT 系 | ディスクイメージに入れる場合のみ |
| BusyBox（GPL v2）/ bash（GPL v3） | GPL | ディスクイメージに入れる場合のみ。別プログラムとして扱う |

## 配布するときの決まり

- ソースの配布物（GitHub など）には `toolchain/` を含めない（`.gitignore` 済み）。
- ディスクイメージや ISO を配るときは、含めた GPL 部品のライセンス文と対応するソースの入手先（または同梱）を付ける。
- T88-BASIC は PolyForm のままにする。MIT と GPL のコードを、T88-BASIC と一つのプログラムに結合しない。
- 来歴の記録は `docs/T88-BASIC-拡張.ja.md` の第8章を参照。
- NEC の ROM・ダンプ・マニュアルの文章は含まない。参照資料のファイルは配布物に入れない。
