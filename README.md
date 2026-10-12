## なんとなくの機能紹介



th123OIViewerの機能を一部移植してSWRSToysのモジュールにしたものです



天則観のデータベースから、キャラクターセレクト中/観戦中に検出したプロファイルとの戦績等を表示します（F10で表示と非表示を切り替えられます）



タイトル画面 (SceneIDが2) では、IP:Portをキーボードで手入力し、コピーすることができます



天則観が起動していないとき/天則観が非想天則を検知できていない（可能性がある）とき、に警告を表示します



---

#### 各バージョンの特徴



v1では、DBファイルが(th123.exeがあるディレクトリ)/th4_5888/Default.dbでないと戦績を読み込めません



v2では、iniファイルで各種設定が可能です...設定の手間は少しだけかかります
[先発](https://github.com/yamachiyannn/th123OIViewer)のCombinedPlayers.dbを読み込んで連携することも可能になります



v3では、非想天則のウィンドウに直接焼きこむ方式でオーバーレイします
UIに一部相違点がありますが、機能面ではv2と同じです



---

#### 導入方法



OIOverlayフォルダをModules/に配置してください



機能を有効化するには、次の一行をSWRSToys.iniに書き加えてください



```
OIOverlay=Modules/OIOverlay/OIOverlay.dll
```



---

#### ダウンロードリンク



[v1](https://github.com/yamachiyannn/OIOverlay/releases/download/v1/OIOverlay_v1.zip) / [v2](https://github.com/yamachiyannn/OIOverlay/releases/download/v2/OIOverlay_v2.zip) / [v3](https://github.com/yamachiyannn/OIOverlay/releases/download/v3/OIOverlay_v3.zip)



---

#### クレジット



https://github.com/SokuDev/ModTemplate



https://github.com/ocornut/imgui



---

更新: 20261012

