![chatra_logo](https://raw.githubusercontent.com/chatra-lang/resources/master/logo/chatra_logo_for_light_bg_small.png "Chatra")

# About “Chatra”
Chatraは誰でも簡単に書けることを重視して開発された軽量スクリプト言語です。  
独立したインタプリタとしても使えるほか、プログラムに組み込んでマクロ言語として使うこともできます。

プログラムへの組み込みを容易にするため、ChatraのインタプリタはC++11標準機能のみを使って書かれています。
外部ライブラリにも依存しておらず、ビルドも簡単です。  
特別な工夫なしで複数の独立した仮想マシンを動かすことができ、またそれぞれの仮想マシン内部で複数のスクリプトを同時に実行することもできます。

## Chatra is simple
Chatraの文法はとてもシンプルです。  
少なくとも何か１つのプログラミング言語に習熟していれば、読むことはできます。

```Text
class HelloWorld
    def someMethod(to: 'Earth')
        log('Hello,' + to + '!')

instance = HelloWorld()
for in Range(5)
    instance.someMethod('World')
```

そしてChatraはシンプルであるだけではなく、現代的な言語が持っているべき多くの機能を備えています。

難しく言うと、Chatraはクラスタイプの動的型付きオブジェクト指向言語で、
マルチスレッドサポート・コンカレントガベージコレクション・クロージャ・参照ベースの排他制御といった言語上の特徴があります。
とはいえ簡単であることがChatraの核であり価値ですので、こういう面倒くさい話は忘れて構わないでしょう。
誰かにかっこよく説明したい場合にだけ思い出してください。

Chatraの文法はC/C++, Java, Python, Swiftといった言語に影響を受けています。
これらの言語を学んだことがある人なら、すぐにでもChatraを使いこなすことができるはずです。

## Chatra is serializable
シンプルであることに次ぐ第二の特徴は、「保存可能」であるということです。  
Chatraのスクリプトを実行する仮想マシン(Runtimeと呼びます)は、いつでも全状態をバイトストリームに保存することができ、
あとで復元して再開することができます。
スマートフォンアプリのような、頻繁に停止・再開を伴うような状況で役に立ちます。  
条件さえ満たせば、別マシンで再開することさえ可能です(厳密には条件があります)。

note: 以下のコードはChatraを組み込んだプログラムに組み込む人にとって有用な情報ですが、
Chatraのスクリプトを書く人にとっては多分無関係な話です。

```C++
// Runtimeを生成して、"test_script"を実行します。
auto host = std::make_shared<Host>();
auto runtime1 = cha::Runtime::newInstance(host); 
auto packageId = runtime1->loadPackage(cha::Script("test_script", "..."));
runtime1->run(packageId);

// … スクリプトを実行 ...

// Runtimeの状態をstreamに保存して、シャットダウンします。
std::vector<uint8_t> stream = runtime1->shutdown(true);

// 保存したstreamからRuntimeを復元します。
runtime2 = cha::Runtime::newInstance(host, stream);

// … 実行再開 ...
```

“Chatra”の名前は日本語の「茶トラ」から来ています。茶トラは単純で甘えっ子とされていて、この言語の表層的な特性をよく表しています。

![red_tabby](https://raw.githubusercontent.com/chatra-lang/resources/master/images/red_tabby.jpg "Chatra \(red tabby\) is simple")
![calico](https://raw.githubusercontent.com/chatra-lang/resources/master/images/calico.jpg "Mike \(calico\) is not simple")

# Getting Started
(Sorry under construction)  
言語仕様の概要を説明したドキュメントがあります。  
詳細はこちらです。ただし、まだ十分に整備されていません。  
プログラムに組み込む方法については、こちらを参照してください。

現在のところ、ChatraはC++で書かれたソースコードの形で提供されています。こちらのガイドに従ってビルドしてください。
ソースコードには、独立したインタプリタとして使うためのフロントエンドも含まれています。
フロントエンドの使い方についてはこちらを参照してください。

# Chatra is in alpha stage
Chatraの開発はまだ始まったばかりです。  
言語として足りない機能はまだまだ多く、これから順次追加していく予定です。
少なくとも以下の重要な機能が足りていないか、不十分な実装になっています:
- 標準ライブラリ
- デバッガサポート
- Remote Procedure Callサポート
- 実行の高速化
- アノテーションとリフレクション
- 多言語対応
- hot-fixのサポート
- 正確かつ充実したドキュメンテーション
- 広範かつ網羅されたテスト

なおバージョン番号が1.0に達するまでの間は、旧バージョンとの互換性を無視した変更が加えられる可能性があります
(バージョン間の互換性を保つことよりも、機能を早く実装することが優先されます)。
あなたのプロジェクトにChatraを組み込む場合は、バージョン間の差分に十分注意してください。

# Contributing
Chatraでは皆様のどのような形での参加も歓迎しています!  
どんなに小さな変更でもかまいません。コードを書く以外にも、あらゆる種類の参加をお待ちしています。
- 不具合の指摘
- 言語テストパターンの追加
- 機能のリクエスト
- ドキュメントの拡充、翻訳、修正
- サイトの構築と管理
- 各種デザイン
- 感想とか感触とか

なにかありましたら、~~こちらの手順~~で投稿をお願いします。
(ドキュメントは作成中です。githubを通じてPRを受け付けます)

# License
Apache License v2でリリースされています。

# Version History
0.1.0  最初のリリース
