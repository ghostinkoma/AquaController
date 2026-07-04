# GitHub Personal Access Token (PAT) 運用メモ

このプロジェクトのリポジトリ: https://github.com/ghostinkoma/AquaController.git

## 失効すべき過去のトークン（チャット履歴に露出済み）

以下はこれまでの会話で共有され、**リポジトリへの書き込みに使用済み、または使用を試みた**トークンです。
残っている場合は GitHub 側で必ず Revoke（失効）してください。チャット全文はローカルに保存されるため、
トークン文字列自体が生き続けている限りリスクが残ります。

| # | 種別 | 先頭 | 状態 |
|---|------|------|------|
| 1 | Classic PAT | `ghp_fLEs...` | 使用済み（push成功）。失効推奨 |
| 2 | Fine-grained PAT | `github_pat_11AE6454...` | 使用試行（403 Permission denied で失敗）。**権限不足のため要修正 or 失効** |

> 権限確認・失効場所: GitHub → 右上アイコン → **Settings** → 左メニュー **Developer settings**
> → **Personal access tokens** → **Tokens (classic)** または **Fine-grained tokens**

## 新しいトークンを発行する手順（推奨: Classic トークン）

Fine-grained トークンは既定でリポジトリへのアクセス権がゼロのため、設定を誤ると
今回のような `403 Permission to ... denied` になります。運用がシンプルな **Classic トークン**
を推奨します。

1. https://github.com/settings/tokens/new を開く（ログイン状態で）
2. **Note**: 用途がわかる名前を入力（例: `AquaController-push-2026`）
3. **Expiration**: 短め（7日 or 30日）を推奨。長期間有効なトークンは漏洩時のリスクが大きい
4. **Select scopes**: `repo` にチェック（サブ項目は自動で全部入る）
5. **Generate token** → 表示された `ghp_...` をコピー（**この画面を閉じると二度と表示されない**）

### Fine-grained トークンを使う場合（上級者向け）

1. https://github.com/settings/tokens?type=beta を開く
2. **Repository access** → 「Only select repositories」→ `AquaController` を選択
3. **Permissions** → **Repository permissions** → **Contents** を `Read and write` に設定
   （これが漏れると今回と同じ 403 になります）
4. **Generate token**

## push の実行

```powershell
cd D:\hobby\AquaController
git push origin main
```

認証を求められたら:
- **Username**: GitHub のユーザー名
- **Password**: 発行したトークン（パスワード欄にそのまま貼り付け）

## 運用上の注意（今後のため）

- **トークンをチャットや平文ファイルに貼らない**。貼った場合は使用後に必ず失効する
- 一度使ったトークンは**都度失効**し、次回は新規発行する運用でよい（短命トークン戦略）
- どうしても繰り返し使う場合は **Expiration を設定**し、失効管理を GitHub 側に任せる
- Git の認証情報キャッシュ（Windows の資格情報マネージャー）にトークンが残ることがあるため、
  不要になったら `control panel > 資格情報マネージャー > 全般の資格情報` から `git:https://github.com` を削除する
