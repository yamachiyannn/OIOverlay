## Opponent Info Overlay





#### なんとなくの機能紹介



[th123OIViewer](https://github.com/yamachiyannn/th123OIViewer) の機能を一部移植してSWRSToysのモジュールにしたものです



- 天則観のデータベースから、キャラクターセレクト中/観戦中に検出したプロファイルとの戦績等を表示します（[後述](#導入方法)のToggleKeyで表示と非表示を切り替えられます）



- タイトル画面 (SceneIDが2) では、IP:Portをキーボードで手入力し、コピーすることができます
ToggleKeyを押すことで、直近の連戦履歴を表示することもできます



- 天則観が起動していないとき/天則観が非想天則を検知できていない（可能性がある）とき、に警告を表示します



- 対戦中にリバサルミネスの成功を検知すると、RLカウンターが動きます（ToggleKey一回押しで手動カウントもできます）
RLカウンターが上昇すると、同ディレクトリの "RLnotice.wav" を再生します

- （続）三秒以内に二回押しすると、クリップにしたいと思った時刻を "RLcounter.txt" に追記します（ファイルは自動作成）
対戦終了後、プロファイル名とリバサルミネスの成功数が "RLcounter.txt" に追記されます



- [OIV](https://github.com/yamachiyannn/th123OIViewer)の "CombinedPlayers.db" と連携して、複数のプロファイルのデータをプレイヤーとしてまとめることもできます



---

#### 導入方法



OIOverlayフォルダをModules/に配置してください



機能を有効化するには、次の一行をSWRSToys.iniに書き加えてください



```
OIOverlay=Modules/OIOverlay/OIOverlay.dll
```



OIOverlay.iniでToggleKeyなど必要な事項を設定（詳細はini内の記述を参照）して、導入完了です！



---

#### 必要環境



東方非想天則 / 天則観 / SWRSToys



---

#### ダウンロードリンク



[OIOverlay_v5.zip](https://github.com/yamachiyannn/OIOverlay/releases/download/v5/OIOverlay_v5.zip)



---

#### バージョン別説明/変更履歴



v1では、DBファイルが(th123.exeがあるディレクトリ)/th4_5888/Default.dbでないと戦績を読み込めません



v2では、iniファイルで各種設定が可能です...設定の手間は少しだけかかります
[先発](https://github.com/yamachiyannn/th123OIViewer)のCombinedPlayers.dbを読み込んで連携することも可能になります



v3以降では、非想天則のウィンドウに直接焼きこむ方式でオーバーレイします



v4では、リバサルミネスカウンターが追加されました
DLLファイルと同じ階層にRLnotice.wavを置くとRLカウンターが増えたときに再生します
二回押しで。クリップにしたい部分の時刻をRLcounter.txtにをメモすることもできます

その他、タイトル画面でToggleKey（デフォルトではF10）を押すことで連戦履歴を表示する他、IP入力欄で10800を補完したり情報表示を2ページにしたり、一部不具合を修正しました



v5では、リバサルミネスの自動検知機能が追加されました
ダウンした後10秒以内に光符「ルミネスストライク」が発動し、3秒以内に相手が被弾判定になると自動カウントします
手動でToggleKeyを押してもカウントできます



---

#### クレジット



https://github.com/SokuDev/ModTemplate



https://github.com/ocornut/imgui



---

更新: 20261025

