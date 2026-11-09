# Sessions: decisions log

The app is called **Sessions**.

Planning document. No code yet. Each entry records what was decided and why. Open questions are at the bottom.

## Purpose (2026-11-09)

- **Finding conversations, not reading them.** The usual question is "I had a session about X: was it Codex or Claude, and which session?" The answer is a card; the conversation itself stays in its own app.
- **Copying sessions between tools and accounts** is the one action the app takes on other tools' data, for example a session started in Codex by accident, continued in Claude. Copy only: no moving, archiving or deleting. The original is never touched.
- Copying client work between the Sembit and personal Claude organizations is fine; there is no practical difference between them.
- This PC only.

## Sources: six targets

| # | Target | How it is told apart on disk | Account tag |
|---|---|---|---|
| 1 | Claude Code desktop app | `.claude\projects\**.jsonl`, `entrypoint: "claude-desktop"`; sidebar entry under `%APPDATA%\Claude\claude-code-sessions\<account>\<organization>\` | organization: **Sembit Team** or **Personal Ultra** |
| 2 | Claude Code command line | same folder, `entrypoint: "cli"` | account in use (to confirm how it is recorded) |
| 3 | ChatGPT desktop, including Codex | `.codex\sessions` and `archived_sessions`, `originator: "Codex Desktop"` | `creator_account_id` in each session |
| 4 | Codex command line | same folders, command-line originator (none on disk yet) | `creator_account_id` |
| 5 | Antigravity desktop app | `.gemini\antigravity\conversations\*.pb` | to confirm |
| 6 | Antigravity command line (`agy`) | `.gemini\antigravity-cli\conversations\*.db` (SQLite) | to confirm |

Not indexed: Cursor, ChatGPT web chats, Claude.ai web chats, Cowork.

Format notes:
- Almost all Codex sessions (3,379 of 3,486) are stored Zstandard-compressed (`.jsonl.zst`), so reading Codex needs a Zstandard decoder.
- Antigravity desktop `.pb` files measure 8.00 bits per byte of entropy, have no header and are not block-aligned: they are encrypted, not plain protobuf. Antigravity's language server is a Go program that saves them through a `proto_saver.WithEncryptionKey` helper and uses AES-GCM (an authenticated encryption mode), so these are almost certainly AES-GCM-encrypted protobuf. Windows Credential Manager holds an entry named `gemini:antigravity`, which is the likely home of the key (unconfirmed; its value has not been read).
- Antigravity desktop titles and stats are readable without decryption: `%APPDATA%\Antigravity\User\globalStorage\state.vscdb`, key `antigravityUnifiedStateSync.trajectorySummaries`, holds a base64 protobuf with one entry per conversation (id, title such as "Explain Repository Functionality", step count, timestamps). All 11 desktop conversations are listed there. Each conversation's `brain\<id>\` folder also holds readable Markdown artifacts (plans, reviews).
- The same `state.vscdb` also holds Antigravity's sign-in token, so the app reads only the summaries key and never copies that file elsewhere.
- The command-line `.db` files are ordinary SQLite.
- Account and organization names are shown in the Settings panel.

## What is indexed

- What Shaun and the AI said. Tool calls, command output and file contents are left out (the distinction is kept in the schema in case it matters later).
- Full text over all of it, plus semantic (meaning-based) search.

## Platform and dependencies

- Windows 11, native C++20 and WinUI 3 (Windows App SDK), following CardFX's structure: `build.ps1`, `run.ps1`, PowerShell test wrappers, README as the main documentation.
- **Low dependency, not no dependency.** Every library is proposed to Shaun before it is used. Approved so far:
  - **SQLite**, compiled into the executable (single C file, public domain). Keyword search uses its built-in full-text search (FTS5).
  - **A small SQLite vector add-on** (for example sqlite-vec, a single C file), compiled in.
  - **Zstandard decoder** (single-file decompressor, BSD licence), compiled in, for Codex's compressed sessions (approved 2026-11-09).
- **Embeddings run locally** through Windows Machine Learning (part of the Windows App SDK already used for WinUI 3) on the GPU (NVIDIA GeForce RTX 3070, 8 GB). A small open embedding model ships as a file next to the executable. Its tokenizer is written in-house.

## Storage and updating

- One installed folder, **`D:\Apps\Sessions`** (not under Dropbox; C: is nearly full): the executable, the embedding model file and a single database file holding both the keyword index and the vectors.
- Nothing runs while the app is closed: no service, no background CPU.
- While open, the app watches the six source folders for changes. On launch, it catches up by comparing file sizes and modification times with what the database last saw. The NTFS change journal and a Windows service are not used.
- Speed comes from the index itself, not from how changes are detected: search queries run against the local database, so they stay fast no matter how the app notices new files.
- No backups or undo for copied sessions.

## Interface

- One screen, no navigation, one translucent pane. Follows the Windows light or dark setting; light mode is the main testing target.
- Every session is a small card in one vertical list, sorted by **last activity** by default.
- A card shows: title, tool icon (six distinct icons), account, date and message count. No folder.
- Clicking a card opens the session in its original app. Opening straight to that session is a stretch goal. Claude, Codex and Antigravity all register their own app links (`claude:`, `codex:`, `antigravity:`); whether they accept a session id still needs research.
- Settings: a small gear next to the status indicator opens a side panel listing each account, where it can be renamed and its part of the index rebuilt.
- A filter section at the top (tool, account, tags), and Shaun's own tags on sessions.
- A small status indicator in the upper right: scanning, ingesting new sessions, or caught up (green check).

## Look and icon

- Accent colour: **deep teal `#0F766E`** (not CardFX's purple), with a new icon.
- The icon is rendered in Blender (installed) at each exact output size: the six tools' logos on a stack of glass-like card planes. The same render is used for the splash screen. The metaphor throughout is cards.
- The icon uses other companies' logos, so it is for this private app only.

## Copying a session (research track)

The hardest part: creating a real session in the destination that its app accepts and can continue.
- **Into Claude Code:** write a new transcript (`.claude\projects\<folder>\<new id>.jsonl`). For the desktop app, also add a sidebar entry in the chosen organization's folder. Transcripts are not tied to an account, so Claude-to-Claude copies between organizations should be simple.
- **Into Codex:** write a new session file and register it in Codex's own SQLite index (`state_5.sqlite`, `session_index.jsonl`).
- Across tools, the tool calls do not translate directly (different formats and ids). The likely approach is to copy what was said and turn tool activity into short text notes. To be tested on throwaway sessions.

## Findings

Machine: Intel Core i9-13900 (24 cores), 64 GB RAM, NVIDIA GeForce RTX 3070 (8 GB). C: has about 20 GB free; D: has about 1.5 TB free.

Sizes: Claude Code ~1 GB of transcripts (180 top-level sessions plus subagents); Codex ~4.8 GB compressed (~3,500 files); Antigravity desktop 11 files; Antigravity command line 14 databases.

## Open questions

1. Antigravity desktop full text (decryption approved 2026-11-09). Ruled out so far: the `gemini:antigravity` credential is only Antigravity's sign-in token (JSON with access and refresh tokens), not a key; a scan of the language server for a built-in 16- or 32-byte key (4-byte aligned, protobuf-looking first block) found nothing. Next step: disassemble `proto_saver.DiskSaver.Save` and the caller that passes `WithEncryptionKey` to see where the key comes from, and whether the data is compressed before encryption. This needs a decompiler (for example Ghidra, free, needs Java), which is not installed. Until then, desktop Antigravity is indexed from titles, summaries and brain artifacts.
