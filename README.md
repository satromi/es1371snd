# es1371snd:超漢字向けES1371(VMware AudioPCI)サウンドドライバ

es1371sndは、超漢字V / B-right/V(BTRON3, i386)に「サウンド」デバイスを提供するカーネル拡張(kerext)ドライバです。VMwareが既定でエミュレートするEnsoniq ES1371 / Creative AudioPCIを対象としています。

同梱のリファレンスドライバintelsounddrvはIntel ICH AC97用で、PCI ID・レジスタ・DMAエンジンがまったく異なるES1371では動きません。本ドライバを常駐させると、BeFCやPCM Playerのようなアプリが`opn_dev("サウンド", D_UPDATE)`と`wri_dev()`でPCMを再生できるようになります。

- 対象環境:超漢字V / B-right/V(BTRON3, i386)、VMware上のES1371(PCI 0x1274:0x1371。CT5880の0x5880も同じ手順で動きます)
- 出力:48kHz/16bit/ステレオ固定。デバイス本来のレートなのでSRCは1:1動作となり、レートごとの再設定が要りません
- ミキシング:オープナ(タスク)ごとにPCMリングを持ち、最大4本のストリームを割り込みハンドラ内で加算合成します
- 再生経路:16KBの物理連続DMAリングを4分割し、DAC2割り込みごとに再生を終えた4KBをアプリのFIFOから補充します
- ハング対策:レベルトリガのPCI INTxストームを連続回数と累計回数の両方で監視し、検出したらチップ側の割り込み源を全て落とします

---

## ビルド

WSL(Ubuntu)上のbrightvドライバ(kerext)ツールチェーンでビルドします。BeFCなどのアプリ用ツールチェーンとは別物で、intelsounddrvと同じものです。

```sh
bash ./build-es1371.sh
```

スクリプトは`src/`と`pcat/`を`/usr/local/brightv/driver/es1371snd/`へコピーして`pcat`でmakeを走らせ、できたkerextバイナリ`es1371snd`を配置します。

コンソールトレースは既定で無効です。見たい場合は`src/main.c`冒頭の`#undef DEBUG_PRINT`と`#define DEBUG_PRINT(x) ((void)0)`の2行を消してからビルドしてください。呼び出し側はそのまま残してあります。

割り込みを使わないタイマ方式(後述)を試す場合は次を使います。ビルドコピー側の`REFILL_BY_TIMER`だけを1に書き換えるので、リポジトリのソースは既定の0のままです。

```sh
bash ./build-es1371-timer.sh
```

## 超漢字への組み込みと起動

1. `es1371snd`をファイル変換で無変換（31レコード）で取り込み、超漢字の`/SYS`へコピーします。
2. `STARTUP.CMD`に下記のようにif endifで囲んで`kerext es1371snd`を追加します。IMSから一度だけ実行することもできます。
3. トレースを有効にしたビルドなら、システムコンソールにPCI検出→I/O空間→リセット→コーデック→SRC→DMA→割り込みの各段が順に出ます。

```text
if ?/SYS/es1371snd
 kerext es1371snd
endif
```

常駐すると「サウンド」デバイスが登録され、アプリの`opn_dev`が成功するようになります。未常駐なら`opn_dev`が失敗するだけなので、アプリ側は無音のまま動作を続けられます。

## アプリケーションからの使い方

```c
#include <btron/device.h>
#include <device/sound.h>

static TC SND_DEVNAME[] = { 0x2535, 0x2526, 0x2573, 0x2549, 0 };  /* サウンド */

W  a, dd = opn_dev(SND_DEVNAME, D_UPDATE, NULL);
SDPcmMode m = { PCMST48K, PCMFmt16, 1 };

wri_dev(dd, DN_SDPCMMODE, (B *)&m, sizeof(m), &a, NULL);
wri_dev(dd, 0, (B *)pcm, frames * 4, &a, NULL);   /* 16bitステレオPCMを流し込む */
```

`datano`に指定する項目は次のとおりです。

| datano | 方向 | 内容 |
|---|---|---|
| 0 | 書込 | PCM本体(16bitのL/Rインタリーブ)。最初のデータでDAC2を自動起動します。返り値は受理バイト数 |
| 0(長さ0) | 書込 | 次の書込が受理できるバイト数の問い合わせ。DACは起動しません |
| `DN_SDPCMMODE` | 読/書 | レート・フォーマット・ステレオ指定。値は保持しますが、ハードウェアは48kHz/16bit/ステレオ固定です |
| `DN_SDPCMCTL` | 読/書 | `PCM_PLAY`で再生開始、`PCM_STOP`で呼び出したタスクのストリームを解放します。全ストリームが消えた時点でDACを止めます |
| `DN_SDPCMCNT` | 読 | 呼び出したタスクのFIFOに残っている未再生バイト数。遅延の管理に使えます |
| `DN_SDPCMBUFSZ` | 読 | 1チャネルあたりのFIFOサイズ(96000バイト、約0.5秒) |
| `DN_SDPCMINFO` | 読 | 対応レートとフォーマットの一覧 |
| `DN_SDSELOUT`、`DN_SDVOL*` | 読/書 | 受理はしますが動作には反映しません。後述の制限を参照してください |

ストリームはPCMを書き込んだタスクのIDで識別します。書き込むタスクと`cls_dev`するタスクが違う場合は、書き込み側が終了直前に`DN_SDPCMCTL`へ`PCM_STOP`を送るとチャネルを確実に解放できます。

## ファイル構成

| ファイル | 役割 |
|---|---|
| `src/main.c` | ドライバ本体。ES1371レジスタ定義、PCI・コーデック・SRCの初期化、DMAリング、ミキサ、割り込みハンドラ、デバイス要求処理、kerextエントリ |
| `src/device/sound.h` | 「サウンド」デバイスのアプリとドライバ間のプロトコル。公式ヘッダが本SDKに無いため再構成したものです |
| `pcat/Makefile` | brightvドライバツールチェーン用のビルド規則 |
| `build-es1371.sh` | 既定の割り込み方式でビルドして配備します |
| `build-es1371-timer.sh` | `REFILL_BY_TIMER=1`(割り込み不使用)でビルドして配備します |
| `LICENSE` | GPLv2全文 |

## アーキテクチャ

### 起動シーケンス

kerextエントリの`main()`から`esInit()`が次の順に進みます。途中で失敗した場合は、登録済みの割り込みベクタを外してからバッファを解放します。解放済みのリング0コードにベクタが残ると、共有IRQの次の割り込みでOSごと落ちるため、この順序が重要です。

1. `esProbe`:PCIバス上の0x1274:0x1371を検索します。無ければ0x5880を探します
2. `esSecureIO`:BAR0(0x40バイトのI/O領域)を予約し、PCIコマンドレジスタでI/O空間とバスマスタを有効にします
3. `esReset`:ICSCを0にし、bit14のSYNC_RESをパルスしてAC97をウォームリセット、`srcInit`でSRC RAMを48kHz(VFI=16)に設定し、AC97コーデックをアンミュートします
4. `esAllocDMA`:`b_get_mbk(M_DMA|M_SYSTEM|M_RESIDENT)`と`CnvPhysicalAddr`で物理連続16KBを確保し、ミキサ4本分のFIFO(各96000バイト)を`Kcalloc`します
5. `esRegisterInterrupt`:PCIコンフィグのIRQ行に`defIntHdr`でハンドラを登録します

このあと`startDriver`がランデブポートと主タスクを作り、`b_DefDevice`で「サウンド」を登録して要求受付ループに入ります。

### データフロー

```text
アプリの wri_dev(datano 0)
   → オープナごとのSPSCリング(96000B × 最大4本)   … 生産者=アプリのタスク
   → mixInto(): 加算合成して16bitにクランプ        … 消費者=DAC2割り込み
   → DMAリング16KB(4分割、1回に4KBを補充)
   → ES1371 DAC2(物理アドレスを直接読んでループ再生)
```

リングは書込側がhead、割り込み側がtailだけを進めるSPSC構成です。32bit整列したhead/tailはi386では原子的に読み書きできるので、ロックは要りません。スロットの確保と解放だけを`DI`/`EI`で囲み、初期化途中のチャネルを割り込みハンドラが走査しないようにしています。

アクティブなチャネルが1本のときは`memcpy`の高速パスを通り、2本以上のときだけ加算合成します。アンダーラン時はゼロ埋めではなく直前のステレオ標本を保持します。ゼロ埋めは割り込みごとに全振幅のステップを作り、可聴のバズになるためです。

### 割り込みリフィルとタイマリフィル

VMwareのES1371モデルではDAC2の再生位置レジスタが凍結します。0x3Cの上位16bitは0のまま、0x28の上位16bitもSAMP_CTのままです。補充位置をハードウェアから読めないので、割り込みの発生順だけを頼りに「いま再生を終えた1/4」を補充します。4分割にしているのは、割り込みが1個ずれても約32msのガードバンドが残り、1ループ以内に自然復帰させられるからです。DAC2のSAMP_CT(0x28)には、割り込みあたりのステレオフレーム数から1を引いた値を書きます。

`REFILL_BY_TIMER=1`でビルドすると、DAC2割り込みを一切有効にせず、10ms周期のハンドラが`vget_otm()`の経過時間から必要バイト数(1msあたり192バイト)を求めて補充します。割り込み源が無いのでINTxストームは原理的に起こりません。既定は実機で動作を確認済みの割り込み方式で、タイマ方式は予防的な代替として用意しています。

### ハング対策

ES1371のINTxはレベルトリガなので、割り込み要因を落とせないままISRを抜けると即座に再入し、例外も出さずにOS全体がlivelockします。そこで次の3つを入れています。

- `ICSS.INTR`は立っているがDAC2ではない場合、MPWR(ICSCのPDLEV)をクリアします。それが連続32回続いたら、SICのストリーム割り込み許可3本とICSCのDAC/ADC稼働許可を全て落とします
- PCIのall-ones読み(0xFFFFFFFF。D3、I/O無効、デバイス消失のとき)はINTRとI_DAC2が両方立っているように見えるため、strayとして扱います
- アプリからのPCM書込が無いまま累計4096回ISRに入った場合も同じ全遮断を行います。DAC2が止まらないストームは連続カウンタでは捕まらないためです

いずれの場合も音は止まりますがOSは生き残り、次のPCM書込でDAC2が再起動します。`DC_SUSPEND`ではDAC2を停止し、サスペンド中の書込は0バイト受理として無害に捨てます。受付そのものを止めると、GUIロックを保持したまま`wri_dev`するアプリが永久にブロックしてしまうためです。

---

## 実装状況と既知の制限

VMware上の超漢字Vで動作を確認済みで、BeFCとPCM Playerから実際に再生できています。現行版はv0.19です。

- 出力は48kHz/16bit/ステレオ固定です。`DN_SDPCMMODE`の値は保持して読み返せますが、SRCとDAC2の設定は変えません。他のレートを鳴らすときはアプリ側でリサンプルしてください。
- 録音(ADC)は未対応です。SRCのADC系レジスタは初期化だけ行います。
- 音量と入出力選択(`DN_SDVOLMAIN`、`DN_SDVOLPCM`、`DN_SDSELOUT`)は要求としては受理しますが、ハードウェアには反映しません。AC97のマスタとPCM-outはドライバ初期化時の固定値です。音量調整はアプリ側で行ってください。
- DMAリングは16KBです。32KBには物理連続8ページが必要で、この環境の`b_get_mbk`では確保できません。
- 同時再生は4ストリームまでです。5本目以降のオープナはPCMを捨てます。
- 再生位置レジスタが凍結する環境では補充が開回路になるため、割り込みが大きくずれると1/4分の継ぎ目が出ることがあります。
- `src/device/sound.h`は公式ヘッダがSDKに無いため再構成したものです。es1371sndと対応アプリの間では自己整合していますが、他の標準アプリとの互換性が必要なら公式ヘッダに差し替えてください。
- コンソールトレースは既定で無効にしています。再生ごとのログはBTRONのコンソール出力が遅いためアプリのFPSを落とし、その結果FIFOがアンダーランして音が途切れます。

---

## ライセンス

- 本ソフトウェアはGNU General Public License version 2の下で配布します。全文は[`LICENSE`](LICENSE)を参照してください。
- Copyright (C) 2026 satromi
- Portions Copyright (C) 2004 Yurio Miyazawa。kerextのドライバ骨格(ランデブポートと主タスク、`b_DefDevice`登録、`DC_*`と`DN_SD*`の要求振り分け、`GetMemoryForDMA`、`defIntHdr`)はintelsounddrv(GPL-2.0)由来です。GPLv2が必須なのはこのためです。
- ES1371のハードウェア層(BAR0のI/O、SRC、SRC配下のAC97コーデック、DAC2の単一フレームDMA)はOpenBSD/NetBSDの`eap(4)`ドライバ(`eapreg.h`、`eap.c`の`eap1371_*`経路)由来で、NetBSD FoundationによるBSD2条項ライセンス下にあります。Copyright (c) 1998, 1999 The NetBSD Foundation, Inc.(contributed by Lennart Augustsson and Charles M. Hannum)。ライセンス全文は`src/main.c`の冒頭に記載しています。

再配布するときは、ソースでもバイナリでも上記の著作権表示とライセンス文をそのまま同梱してください。
