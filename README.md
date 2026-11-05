# clinic-alert — 診察室の非常ボタン

診察室の机の下に隠したボタンを押すと、受付の端末が警報を出し、**LINE WORKS のトークルーム**にも通知します。

```
[診察室] ATOM Lite＋押しボタン ─┐
[診察室] ATOM Lite＋押しボタン ─┼─ ESP-NOW（無線・ルーター不要）─▶ [受付] Core2 / CoreS3
[処置室] ATOM Lite＋押しボタン ─┘                                   ・画面が赤く点滅、警報音
                                                                   ・Wi-Fi ─▶ LINE WORKS に通知
```

## 動作
| 場面 | 送信機（診察室） | 受信機（受付） | LINE WORKS |
|---|---|---|---|
| ボタンを1秒長押し | 音は出さない。受付に届くとLEDが一瞬緑に光る | 赤く点滅＋警報音 | 🚨「第1診察室で非常ボタンが押されました」 |
| 受付で画面をタップ | — | オレンジ「対応中」、音が止まる | 🏃「受付で対応を開始しました」 |
| 受付で画面を2秒長押し | — | 待機画面に戻る | ✅「解除されました」 |
| ボタンの電源が抜けた・故障 | 通信できないと5秒ごとにLEDが赤く点滅 | 「応答なし」と表示 | ⚠️「応答がありません」 |
| 受付のBボタンを2秒長押し | — | — | 🔔 テスト送信 |

- 受付に届くまで送信機は最大3分間、自動で再送します。
- 通信はESP-NOWという端末同士の直接通信なので、**Wi-Fiルーターが止まっても受付での警報は動きます**（LINE WORKSへの通知だけが止まります）。
- 送信機は受信機の Wi-Fi チャンネルを自動で探します。

## 用意するもの
| 物 | 数 | 備考 |
|---|---|---|
| M5Stack ATOM Lite | 部屋の数 | 送信機 |
| 押しボタン（モーメンタリ型） | 部屋の数 | 大きめのものが押しやすい。Grove ケーブル付きの「M5Stack Dual Button Unit」や「Key Unit」でも可 |
| M5Stack Core2 または CoreS3 | 1 | 受信機 |
| USB 電源アダプタとケーブル | 台数分 | **電池ではなく常時給電で使ってください** |

### 配線（送信機）
押しボタンの片側を ATOM Lite の Grove 端子の **G26**、もう片側を **GND** につなぎます。
Grove ケーブルなら：黄 = G26 / 黒 = GND（白・赤は使いません）。
ATOM Lite 本体のボタンでも発報できます（不要なら `USE_INTERNAL_BUTTON 0`）。

## LINE WORKS の準備（Incoming Webhook）
1. LINE WORKS 管理者画面で **Incoming Webhook** アプリを追加する
2. 通知を受けたいトークルーム（例：「スタッフ全員」）に Incoming Webhook の Bot を招待する
3. トークルームのメニュー →「チャンネルID」をコピーする
4. Incoming Webhook の Webhook リスト画面で、そのチャンネルIDを入力して Webhook URL を追加する
5. 発行された URL を `include/config.h` の `LINEWORKS_WEBHOOK_URL` に貼り付ける

> Webhook URL を知っている人は誰でもそのトークルームに投稿できます。URL は人に見せたりGitHubに上げたりしないでください（`config.h` は `.gitignore` 済みです）。

## 開発環境の準備（VS Code + PlatformIO）
1. VS Code の拡張機能で **PlatformIO IDE** を検索してインストールし、VS Code を再起動
2. このリポジトリを clone して VS Code で開く
   ```bash
   git clone https://github.com/kiwiman0706-beep/clinic-alert.git
   ```
3. 設定ファイルを作る
   ```bash
   # Mac
   cp include/config.example.h include/config.h
   # Windows (PowerShell)
   Copy-Item include\config.example.h include\config.h
   ```
4. `include/config.h` を編集する（`CLINIC_NET_ID`、Wi-Fi、Webhook URL、部屋の名前）

コマンドで操作する場合は PlatformIO の `pio` コマンドを使います（VS Code 下部のツールバーからも同じことができます）。

## 書き込み
### 受信機（Core2）
```bash
pio run -e receiver -t upload        # CoreS3 の場合は -e receiver_cores3
```
### 送信機（ATOM Lite）— 全部屋同じファームウェア
```bash
pio run -e button -t upload
pio device monitor -e button
```
シリアルモニタで部屋IDを入力します（`config.h` の `ROOM_NAMES` の順番、0から）。
```
room 0      ← 第1診察室
status      ← 現在の設定と通信状態を表示
```
設定は電源を切っても残ります。ボタン1台ごとに書き込みと部屋IDの設定をしてください。

## HTTPS の証明書（推奨）
`LINEWORKS_ROOT_CA` が空のままでも送信はできますが、通信相手が本物の LINE WORKS かを確認しません。
次のコマンド（Mac / Windows の Git Bash）でチェーンを表示し、**一番最後の証明書**（ルート側）の
`-----BEGIN CERTIFICATE-----` から `-----END CERTIFICATE-----` までを貼り付けてください。
```bash
openssl s_client -showcerts -connect webhook.worksmobile.com:443 </dev/null
```
```cpp
static const char LINEWORKS_ROOT_CA[] = R"(-----BEGIN CERTIFICATE-----
...
-----END CERTIFICATE-----
)";
```
ルート証明書が変わると送信に失敗します。その場合はシリアルモニタにエラーが出るので、同じ手順で貼り直してください。

## 設置後の確認（必ず行ってください）
- [ ] 各部屋のボタンを実際の設置場所で押し、受付とLINE WORKSの両方に届くか
- [ ] 受付の待機画面で全部屋が「正常」になっているか
- [ ] 送信機のUSBを抜いて、約2分後に「応答なし」が出るか
- [ ] 受付のBボタン長押しで LINE WORKS にテスト送信が届くか
- [ ] **月1回程度の定期テスト**（日付を決めて、全員で手順を確認）

## 注意
- これは院内スタッフに知らせるための自作システムです。**警察や警備会社への通報の代わりにはなりません。** 必要に応じて警備会社の非常通報サービスと組み合わせてください。
- 壁や距離によって電波が届きにくい場所があります。必ず設置場所で試してください。届きにくい場合は受信機の位置を変えるか、受信機を増やしてください。
- 受信機の Wi-Fi は 2.4GHz のみ対応です。
- LINE WORKS の Incoming Webhook は、ドメイン全体で 120 回/分の送信上限があります。
- 送信内容の形式（`{"body":{"text":"..."}}`）は LINE WORKS の公開情報に基づいています。届かない場合はシリアルモニタの HTTP エラー内容と、[公式ドキュメント](https://developers.worksmobile.com/jp/docs/webhook)を確認してください。
